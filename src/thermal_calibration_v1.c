#include "thermal_calibration_v1.h"

#include <math.h>
#include <string.h>

#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"

#include "sdk.h"
#include "thermal_ota.h"

/* V1 reserves the first 8 KiB for headers. The legacy header occupies the
 * first 64 bytes of selector 0; selector 1 was previously unused.  New
 * records keep FlashDataInfo first, so selector 0 remains readable by an old
 * firmware when it points to the primary payload slot. */
#define CALIBRATION_SELECTOR_PRIMARY   FLASH_OFS_INFO
#define CALIBRATION_SELECTOR_SECONDARY 0x001000u

#define CALIBRATION_SECONDARY_IMGBG    0x02d000u
#define CALIBRATION_SECONDARY_BADPOINTS 0x037000u
#define CALIBRATION_SECONDARY_GAIN     0x038000u

#define CALIBRATION_RECORD_MAGIC       "T160CAL1"
#define CALIBRATION_RECORD_VERSION     2u
#define CALIBRATION_NEW_RECORD_MARKER  0xa5u
#define CALIBRATION_GAIN_MIN           0.2f
#define CALIBRATION_GAIN_MAX           4.0f

enum calibration_slot {
    CALIBRATION_SLOT_NONE = 0,
    CALIBRATION_SLOT_PRIMARY,
    CALIBRATION_SLOT_SECONDARY,
};

struct __attribute__((packed)) calibration_record {
    struct FlashDataInfo flash;
    char magic[8];
    uint32_t version;
    uint32_t generation;
    uint32_t calibration_id;
    uint32_t payload_crc32;
    uint32_t crc32;
};

struct calibration_slot_layout {
    enum calibration_slot slot;
    uint32_t selector_offset;
    uint32_t imgbg_offset;
    uint32_t badpoints_offset;
    uint32_t gain_offset;
};

static const struct calibration_slot_layout calibration_slots[] = {
    {
        .slot = CALIBRATION_SLOT_PRIMARY,
        .selector_offset = CALIBRATION_SELECTOR_PRIMARY,
        .imgbg_offset = FLASH_OFS_IMGBG,
        .badpoints_offset = FLASH_OFS_BADPTS,
        .gain_offset = FLASH_OFS_GAIN,
    },
    {
        .slot = CALIBRATION_SLOT_SECONDARY,
        .selector_offset = CALIBRATION_SELECTOR_SECONDARY,
        .imgbg_offset = CALIBRATION_SECONDARY_IMGBG,
        .badpoints_offset = CALIBRATION_SECONDARY_BADPOINTS,
        .gain_offset = CALIBRATION_SECONDARY_GAIN,
    },
};

static struct {
    bool upload_active;
    bool reboot_required;
    bool payload_prepared;
    bool boot_has_calibration;
    enum thermal_calibration_v1_result error;
    enum calibration_slot active_slot;
    enum calibration_slot target_slot;
    uint32_t active_generation;
    uint32_t active_calibration_id;
    uint32_t received;
    uint32_t payload_received;
    uint8_t package_header[THERMAL_CALIBRATION_V1_HEADER_BYTES];
    uint8_t page[FLASH_PAGE_SIZE] __attribute__((aligned(4)));
    uint16_t page_used;
    uint32_t current_section_bytes;
    uint8_t current_section;
} calibration;

static_assert(sizeof(struct calibration_record) <= FLASH_PAGE_SIZE,
              "calibration selector record must fit one flash page");
static_assert(CALIBRATION_SELECTOR_SECONDARY + FLASH_SECTOR_SIZE
                  <= FLASH_OFS_IMGBG,
              "selector sectors must not overlap the legacy image table");
static_assert(CALIBRATION_SECONDARY_IMGBG % FLASH_SECTOR_SIZE == 0,
              "secondary background must be sector aligned");
static_assert(CALIBRATION_SECONDARY_BADPOINTS % FLASH_SECTOR_SIZE == 0,
              "secondary bad point table must be sector aligned");
static_assert(CALIBRATION_SECONDARY_GAIN % FLASH_SECTOR_SIZE == 0,
              "secondary gain must be sector aligned");
static_assert(CALIBRATION_SECONDARY_GAIN
                  + THERMAL_CALIBRATION_V1_GAIN_BYTES
                  <= FLASH_CALIBRATION_DATA_SIZE,
              "secondary calibration slot overlaps OTA/settings sectors");
static_assert(THERMAL_CALIBRATION_V1_IMAGE_BYTES
                  == SENSOR_W * SENSOR_H * sizeof(uint16_t),
              "V1 package image size must match the sensor");
static_assert(THERMAL_CALIBRATION_V1_GAIN_BYTES
                  == SENSOR_W * SENSOR_H * sizeof(float),
              "V1 package gain size must match the sensor");
static_assert(THERMAL_CALIBRATION_V1_BADPOINT_BYTES
                  == MAX_BADPT * sizeof(struct BadPt),
              "V1 package bad-point size must match FlashDataInfo");

static uint16_t read_le16(const uint8_t* data)
{
    return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
}

static uint32_t read_le32(const uint8_t* data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8)
           | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void write_le16(uint8_t* data, uint16_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
}

static void write_le32(uint8_t* data, uint32_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
    data[2] = (uint8_t)(value >> 16);
    data[3] = (uint8_t)(value >> 24);
}

static float read_le_float(const uint8_t* data)
{
    const uint32_t bits = read_le32(data);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static void write_le_float(uint8_t* data, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    write_le32(data, bits);
}

static bool calibration_ntc_adu_valid(uint16_t value)
{
    return value > 8u && value < 4088u;
}

static bool calibration_ntc_model_fields_valid(
    uint16_t tag, uint16_t reference_adu, int16_t k_q8, uint16_t limit_adu)
{
    if (tag == 0u && reference_adu == 0u && k_q8 == 0
        && limit_adu == 0u) {
        return true;
    }
    if (tag == THERMAL_CALIBRATION_V1_ROW_REF_MODEL_TAG) {
        return reference_adu >= THERMAL_CALIBRATION_V1_ROW_REF_MIN_ADU
               && reference_adu <= THERMAL_CALIBRATION_V1_ROW_REF_MAX_ADU
               && k_q8 > 0
               && (uint16_t)k_q8
                      <= THERMAL_CALIBRATION_V1_ROW_REF_K_Q8_MAX
               && limit_adu > 0u
               && limit_adu
                      <= THERMAL_CALIBRATION_V1_ROW_REF_LIMIT_MAX;
    }
    if (!calibration_ntc_adu_valid(reference_adu)) return false;
    if (tag == THERMAL_CALIBRATION_V1_STARTUP_MODEL_TAG) {
        const int32_t abs_amplitude =
            k_q8 < 0 ? -(int32_t)k_q8 : (int32_t)k_q8;
        return abs_amplitude
                      <= THERMAL_CALIBRATION_V1_STARTUP_AMPLITUDE_MAX
               && limit_adu >= THERMAL_CALIBRATION_V1_STARTUP_TAU_DS_MIN
               && limit_adu <= THERMAL_CALIBRATION_V1_STARTUP_TAU_DS_MAX;
    }
    const int32_t abs_k = k_q8 < 0 ? -(int32_t)k_q8 : (int32_t)k_q8;
    return tag == THERMAL_CALIBRATION_V1_NTC_MODEL_TAG
           && abs_k <= THERMAL_CALIBRATION_V1_NTC_MODEL_K_Q8_MAX
           && limit_adu > 0u
           && limit_adu <= THERMAL_CALIBRATION_V1_NTC_MODEL_LIMIT_MAX;
}

static bool calibration_ntc_model_header_valid(const uint8_t* header)
{
    if (!header) return false;
    return calibration_ntc_model_fields_valid(
        read_le16(header + THERMAL_CALIBRATION_V1_NTC_MODEL_OFFSET),
        read_le16(header + THERMAL_CALIBRATION_V1_NTC_MODEL_OFFSET + 2u),
        (int16_t)read_le16(
            header + THERMAL_CALIBRATION_V1_NTC_MODEL_OFFSET + 4u),
        read_le16(header + THERMAL_CALIBRATION_V1_NTC_MODEL_OFFSET + 6u));
}

static const struct calibration_slot_layout* calibration_slot_layout(
    enum calibration_slot slot)
{
    for (size_t i = 0; i < sizeof(calibration_slots) / sizeof(calibration_slots[0]);
         i++) {
        if (calibration_slots[i].slot == slot) return &calibration_slots[i];
    }
    return NULL;
}

static const uint8_t* calibration_xip(uint32_t relative_offset)
{
    return (const uint8_t*)(XIP_BASE + FLASH_TARGET_OFFSET + relative_offset);
}

static uint32_t calibration_slot_payload_crc32(
    const struct calibration_slot_layout* slot)
{
    if (!slot) return 0;
    uint32_t crc = thermal_flash_crc32_begin();
    crc = thermal_flash_crc32_update(
        crc, calibration_xip(slot->imgbg_offset),
        THERMAL_CALIBRATION_V1_IMAGE_BYTES);
    crc = thermal_flash_crc32_update(
        crc, calibration_xip(slot->gain_offset),
        THERMAL_CALIBRATION_V1_GAIN_BYTES);
    crc = thermal_flash_crc32_update(
        crc, calibration_xip(slot->badpoints_offset),
        THERMAL_CALIBRATION_V1_BADPOINT_BYTES);
    return thermal_flash_crc32_finish(crc);
}

static bool calibration_flash_header_matches_slot(
    const struct FlashDataInfo* header,
    const struct calibration_slot_layout* slot)
{
    return header && slot && header->has_data == FLASH_HAS_DATA_F
           && header->imgbg_ofs == slot->imgbg_offset
           && header->imgbg_size == THERMAL_CALIBRATION_V1_IMAGE_BYTES
           && header->badpts_ofs == slot->badpoints_offset
           && header->badpts_size == THERMAL_CALIBRATION_V1_BADPOINT_BYTES
           && header->badpts_num == MAX_BADPT
           && header->gain_ofs == slot->gain_offset
           && header->gain_size == THERMAL_CALIBRATION_V1_GAIN_BYTES
           && isfinite(header->mean_diff)
           && header->mean_diff >= THERMAL_CALIBRATION_V1_MEAN_DIFF_MIN
           && isfinite(header->t_ref_low);
}

static bool calibration_record_valid(
    const struct calibration_slot_layout* slot,
    const struct calibration_record* record)
{
    if (!slot || !record
        || memcmp(record->magic, CALIBRATION_RECORD_MAGIC,
                  sizeof(record->magic)) != 0
        || record->version != CALIBRATION_RECORD_VERSION
        || record->flash._pad0[0] != CALIBRATION_NEW_RECORD_MARKER
        || !calibration_flash_header_matches_slot(&record->flash, slot)) {
        return false;
    }
    return thermal_flash_crc32((const uint8_t*)record,
                               offsetof(struct calibration_record, crc32))
               == record->crc32
           && calibration_slot_payload_crc32(slot) == record->payload_crc32;
}

static bool calibration_generation_newer(uint32_t candidate, uint32_t current)
{
    return (int32_t)(candidate - current) > 0;
}

static void calibration_apply_header(const struct FlashDataInfo* header)
{
    if (!header) return;
    flash_header = header;
    flash_imgbg = (const uint16_t*)(flash_target_contents + header->imgbg_ofs);
    flash_gain = (const float*)(flash_target_contents + header->gain_ofs);
    flash_badpt =
        (const struct BadPt*)(flash_target_contents + header->badpts_ofs);
}

static void calibration_restore_legacy_pointers(void)
{
    flash_header = (const struct FlashDataInfo*)(flash_target_contents
                                                  + FLASH_OFS_INFO);
    flash_imgbg = (const uint16_t*)(flash_target_contents + FLASH_OFS_IMGBG);
    flash_gain = (const float*)(flash_target_contents + FLASH_OFS_GAIN);
    flash_badpt =
        (const struct BadPt*)(flash_target_contents + FLASH_OFS_BADPTS);
}

void thermal_calibration_v1_boot_select(void)
{
    calibration_restore_legacy_pointers();
    calibration.active_slot = CALIBRATION_SLOT_NONE;
    calibration.active_generation = 0;
    calibration.active_calibration_id = 0;
    calibration.reboot_required = false;
    calibration.boot_has_calibration = false;

    const struct calibration_slot_layout* primary =
        calibration_slot_layout(CALIBRATION_SLOT_PRIMARY);
    const struct calibration_slot_layout* secondary =
        calibration_slot_layout(CALIBRATION_SLOT_SECONDARY);
    const struct calibration_record* primary_record =
        (const struct calibration_record*)calibration_xip(
            CALIBRATION_SELECTOR_PRIMARY);
    const struct calibration_record* secondary_record =
        (const struct calibration_record*)calibration_xip(
            CALIBRATION_SELECTOR_SECONDARY);

    const bool primary_new =
        calibration_record_valid(primary, primary_record);
    const bool secondary_new =
        calibration_record_valid(secondary, secondary_record);
    const struct calibration_record* selected = NULL;
    enum calibration_slot selected_slot = CALIBRATION_SLOT_NONE;

    if (primary_new && secondary_new) {
        if (calibration_generation_newer(secondary_record->generation,
                                         primary_record->generation)) {
            selected = secondary_record;
            selected_slot = CALIBRATION_SLOT_SECONDARY;
        } else {
            selected = primary_record;
            selected_slot = CALIBRATION_SLOT_PRIMARY;
        }
    } else if (primary_new) {
        selected = primary_record;
        selected_slot = CALIBRATION_SLOT_PRIMARY;
    } else if (secondary_new) {
        selected = secondary_record;
        selected_slot = CALIBRATION_SLOT_SECONDARY;
    } else if (primary_record->flash._pad0[0] == 0u
               && primary_record->flash._pad0[1] == 0u
               && primary_record->flash._pad0[2] == 0u
               && calibration_flash_header_matches_slot(flash_header,
                                                        primary)) {
        /* Legacy V1 record: it has no checksum or selector generation, but
         * its fixed offsets and lengths are still safe to load. */
        calibration.active_slot = CALIBRATION_SLOT_PRIMARY;
        uint8_t header[THERMAL_CALIBRATION_V1_HEADER_BYTES];
        if (thermal_calibration_v1_make_packet_header(header)) {
            calibration.active_calibration_id = thermal_flash_crc32(
                header, THERMAL_CALIBRATION_V1_HEADER_BYTES);
        }
        calibration.boot_has_calibration = true;
        return;
    } else {
        return;
    }

    calibration_apply_header(&selected->flash);
    calibration.active_slot = selected_slot;
    calibration.active_generation = selected->generation;
    calibration.active_calibration_id = selected->calibration_id;
    calibration.boot_has_calibration = true;
}

bool thermal_calibration_v1_boot_has_calibration(void)
{
    return calibration.boot_has_calibration;
}

static const struct calibration_slot_layout* calibration_active_layout(void)
{
    return calibration_slot_layout(calibration.active_slot);
}

static enum calibration_slot calibration_next_slot(void)
{
    return calibration.active_slot == CALIBRATION_SLOT_PRIMARY
               ? CALIBRATION_SLOT_SECONDARY
               : CALIBRATION_SLOT_PRIMARY;
}

static bool calibration_erase_range(uint32_t offset, uint32_t bytes)
{
    const uint32_t begin = offset & ~(FLASH_SECTOR_SIZE - 1u);
    const uint32_t end = (offset + bytes + FLASH_SECTOR_SIZE - 1u)
                         & ~(FLASH_SECTOR_SIZE - 1u);
    for (uint32_t current = begin; current < end;
         current += FLASH_SECTOR_SIZE) {
        if (!thermal_ota_calibration_flash_erase(current)) return false;
    }
    return true;
}

static bool calibration_prepare_payload_slot(
    const struct calibration_slot_layout* slot)
{
    return calibration_erase_range(slot->imgbg_offset,
                                   THERMAL_CALIBRATION_V1_IMAGE_BYTES)
           && calibration_erase_range(slot->badpoints_offset,
                                      THERMAL_CALIBRATION_V1_BADPOINT_BYTES)
           && calibration_erase_range(slot->gain_offset,
                                      THERMAL_CALIBRATION_V1_GAIN_BYTES);
}

static uint32_t calibration_section_offset(
    const struct calibration_slot_layout* slot, uint8_t section)
{
    switch (section) {
    case 0: return slot->imgbg_offset;
    case 1: return slot->gain_offset;
    default: return slot->badpoints_offset;
    }
}

static uint32_t calibration_section_size(uint8_t section)
{
    switch (section) {
    case 0: return THERMAL_CALIBRATION_V1_IMAGE_BYTES;
    case 1: return THERMAL_CALIBRATION_V1_GAIN_BYTES;
    default: return THERMAL_CALIBRATION_V1_BADPOINT_BYTES;
    }
}

static bool calibration_flush_page(void)
{
    if (!calibration.page_used) return true;
    const struct calibration_slot_layout* slot =
        calibration_slot_layout(calibration.target_slot);
    if (!slot) return false;
    for (uint16_t i = calibration.page_used; i < FLASH_PAGE_SIZE; i++) {
        calibration.page[i] = 0xffu;
    }
    const uint32_t offset = calibration_section_offset(slot,
                                                        calibration.current_section)
                            + calibration.current_section_bytes
                            - calibration.page_used;
    if (!thermal_ota_calibration_flash_program(offset, calibration.page)) {
        return false;
    }
    const uint8_t* verify = calibration_xip(offset);
    if (memcmp(verify, calibration.page, FLASH_PAGE_SIZE) != 0) return false;
    calibration.page_used = 0;
    return true;
}

static bool calibration_write_payload(const uint8_t* data, size_t bytes)
{
    while (bytes) {
        const uint32_t section_size =
            calibration_section_size(calibration.current_section);
        if (calibration.current_section_bytes == section_size) {
            if (!calibration_flush_page()) return false;
            calibration.current_section++;
            calibration.current_section_bytes = 0;
            if (calibration.current_section > 2u) return false;
            continue;
        }

        uint32_t room = section_size - calibration.current_section_bytes;
        uint32_t page_room = FLASH_PAGE_SIZE - calibration.page_used;
        uint32_t take = room < page_room ? room : page_room;
        if (take > bytes) take = (uint32_t)bytes;
        memcpy(calibration.page + calibration.page_used, data, take);
        calibration.page_used = (uint16_t)(calibration.page_used + take);
        calibration.current_section_bytes += take;
        calibration.payload_received += take;
        data += take;
        bytes -= take;
        if (calibration.page_used == FLASH_PAGE_SIZE
            && !calibration_flush_page()) {
            return false;
        }
    }
    return true;
}

static bool calibration_package_header_valid(void)
{
    const uint8_t* header = calibration.package_header;
    return memcmp(header, "TNC1", 4) == 0 && read_le16(header + 4) == 1u
           && read_le16(header + 6) == THERMAL_CALIBRATION_V1_HEADER_BYTES
           && read_le16(header + 8) == SENSOR_W
           && read_le16(header + 10) == SENSOR_H
           && read_le16(header + 12) == MAX_BADPT
           && read_le16(header + 14) == 0u
           && read_le32(header + 16) == THERMAL_CALIBRATION_V1_IMAGE_BYTES
           && read_le32(header + 20) == THERMAL_CALIBRATION_V1_GAIN_BYTES
           && read_le32(header + 24) == THERMAL_CALIBRATION_V1_BADPOINT_BYTES
           && isfinite(read_le_float(header + 28))
           && read_le_float(header + 28)
                  >= THERMAL_CALIBRATION_V1_MEAN_DIFF_MIN
           && isfinite(read_le_float(header + 32))
           && calibration_ntc_model_header_valid(header)
           && thermal_flash_crc32(header, 60u) == read_le32(header + 60);
}

static bool calibration_payload_valid(
    const struct calibration_slot_layout* slot,
    struct thermal_calibration_v1_manifest* manifest)
{
    if (!slot || !manifest) return false;
    const uint8_t* image = calibration_xip(slot->imgbg_offset);
    const uint8_t* gain = calibration_xip(slot->gain_offset);
    const uint8_t* badpoints = calibration_xip(slot->badpoints_offset);
    for (uint32_t offset = 0; offset < THERMAL_CALIBRATION_V1_IMAGE_BYTES;
         offset += sizeof(uint16_t)) {
        if (read_le16(image + offset) > 16383u) return false;
    }
    for (uint32_t offset = 0; offset < THERMAL_CALIBRATION_V1_GAIN_BYTES;
         offset += sizeof(float)) {
        const float gain_value = read_le_float(gain + offset);
        if (!isfinite(gain_value) || gain_value < CALIBRATION_GAIN_MIN
            || gain_value > CALIBRATION_GAIN_MAX) {
            return false;
        }
    }
    bool saw_terminator = false;
    for (uint32_t offset = 0; offset < THERMAL_CALIBRATION_V1_BADPOINT_BYTES;
         offset += sizeof(struct BadPt)) {
        const uint8_t y = badpoints[offset];
        const uint8_t x = badpoints[offset + 1u];
        if (y == 0xffu && x == 0xffu) {
            saw_terminator = true;
        } else if (saw_terminator || x >= SENSOR_W || y >= SENSOR_H) {
            return false;
        }
    }

    memset(manifest, 0, sizeof(*manifest));
    manifest->image_crc32 = thermal_flash_crc32(
        image, THERMAL_CALIBRATION_V1_IMAGE_BYTES);
    manifest->gain_crc32 = thermal_flash_crc32(
        gain, THERMAL_CALIBRATION_V1_GAIN_BYTES);
    manifest->badpoint_crc32 = thermal_flash_crc32(
        badpoints, THERMAL_CALIBRATION_V1_BADPOINT_BYTES);
    manifest->payload_crc32 = calibration_slot_payload_crc32(slot);
    return true;
}

static void calibration_make_flash_header(
    const struct calibration_slot_layout* slot,
    const uint8_t package_header[THERMAL_CALIBRATION_V1_HEADER_BYTES],
    struct FlashDataInfo* flash)
{
    memset(flash, 0, sizeof(*flash));
    flash->has_data = FLASH_HAS_DATA_F;
    flash->_pad0[0] = CALIBRATION_NEW_RECORD_MARKER;
    flash->imgbg_ofs = slot->imgbg_offset;
    flash->imgbg_size = THERMAL_CALIBRATION_V1_IMAGE_BYTES;
    flash->badpts_ofs = slot->badpoints_offset;
    flash->badpts_size = THERMAL_CALIBRATION_V1_BADPOINT_BYTES;
    flash->badpts_num = MAX_BADPT;
    flash->gain_ofs = slot->gain_offset;
    flash->gain_size = THERMAL_CALIBRATION_V1_GAIN_BYTES;
    flash->mean_diff = read_le_float(package_header + 28);
    flash->t_ref_low = read_le_float(package_header + 32);
    flash->thermal_ntc_tag = read_le16(
        package_header + THERMAL_CALIBRATION_V1_NTC_MODEL_OFFSET);
    flash->thermal_ntc_reference_adu = read_le16(
        package_header + THERMAL_CALIBRATION_V1_NTC_MODEL_OFFSET + 2u);
    flash->thermal_ntc_k_q8 = (int16_t)read_le16(
        package_header + THERMAL_CALIBRATION_V1_NTC_MODEL_OFFSET + 4u);
    flash->thermal_ntc_limit_adu = read_le16(
        package_header + THERMAL_CALIBRATION_V1_NTC_MODEL_OFFSET + 6u);
}

static bool calibration_commit_selector(
    const struct calibration_slot_layout* slot,
    const struct FlashDataInfo* flash, uint32_t generation,
    uint32_t payload_crc32)
{
    uint8_t page[FLASH_PAGE_SIZE] __attribute__((aligned(4)));
    memset(page, 0xff, sizeof(page));
    struct calibration_record* record = (struct calibration_record*)page;
    record->flash = *flash;
    memcpy(record->magic, CALIBRATION_RECORD_MAGIC, sizeof(record->magic));
    record->version = CALIBRATION_RECORD_VERSION;
    record->generation = generation;
    record->calibration_id = thermal_flash_crc32(
        calibration.package_header,
        THERMAL_CALIBRATION_V1_HEADER_BYTES);
    record->payload_crc32 = payload_crc32;
    record->crc32 = thermal_flash_crc32(
        (const uint8_t*)record,
        offsetof(struct calibration_record, crc32));

    if (!thermal_ota_calibration_flash_erase(slot->selector_offset)
        || !thermal_ota_calibration_flash_program(slot->selector_offset,
                                                   page)) {
        return false;
    }
    const struct calibration_record* verify =
        (const struct calibration_record*)calibration_xip(slot->selector_offset);
    return calibration_record_valid(slot, verify);
}

enum thermal_calibration_v1_result thermal_calibration_v1_begin(
    uint32_t packet_bytes)
{
    if (calibration.upload_active || calibration.reboot_required
        || thermal_ota_active() || thermal_ota_install_pending()) {
        return THERMAL_CALIBRATION_V1_BUSY;
    }
    if (packet_bytes != THERMAL_CALIBRATION_V1_PACKET_BYTES) {
        return THERMAL_CALIBRATION_V1_BAD_SIZE;
    }
    memset(&calibration.package_header, 0, sizeof(calibration.package_header));
    calibration.upload_active = true;
    calibration.payload_prepared = false;
    calibration.error = THERMAL_CALIBRATION_V1_OK;
    calibration.target_slot = calibration_next_slot();
    calibration.received = 0;
    calibration.payload_received = 0;
    calibration.page_used = 0;
    calibration.current_section_bytes = 0;
    calibration.current_section = 0;
    return THERMAL_CALIBRATION_V1_OK;
}

enum thermal_calibration_v1_result thermal_calibration_v1_write(
    const uint8_t* data, size_t bytes)
{
    if (!calibration.upload_active || (!data && bytes)) {
        return THERMAL_CALIBRATION_V1_BUSY;
    }
    if (calibration.error != THERMAL_CALIBRATION_V1_OK) {
        return calibration.error;
    }
    if (bytes > THERMAL_CALIBRATION_V1_PACKET_BYTES - calibration.received) {
        calibration.error = THERMAL_CALIBRATION_V1_BAD_SIZE;
        return calibration.error;
    }

    while (bytes) {
        if (calibration.received < THERMAL_CALIBRATION_V1_HEADER_BYTES) {
            const uint32_t remaining = THERMAL_CALIBRATION_V1_HEADER_BYTES
                                       - calibration.received;
            uint32_t take = remaining < bytes ? remaining : (uint32_t)bytes;
            memcpy(calibration.package_header + calibration.received, data,
                   take);
            calibration.received += take;
            data += take;
            bytes -= take;
            if (calibration.received == THERMAL_CALIBRATION_V1_HEADER_BYTES) {
                if (!calibration_package_header_valid()) {
                    calibration.error = THERMAL_CALIBRATION_V1_BAD_HEADER;
                    return calibration.error;
                }
                const struct calibration_slot_layout* slot =
                    calibration_slot_layout(calibration.target_slot);
                if (!slot || !calibration_prepare_payload_slot(slot)) {
                    calibration.error = THERMAL_CALIBRATION_V1_FLASH_ERROR;
                    return calibration.error;
                }
                calibration.payload_prepared = true;
            }
            continue;
        }

        if (!calibration.payload_prepared
            || !calibration_write_payload(data, bytes)) {
            calibration.error = THERMAL_CALIBRATION_V1_FLASH_ERROR;
            return calibration.error;
        }
        calibration.received += (uint32_t)bytes;
        bytes = 0;
    }
    return THERMAL_CALIBRATION_V1_OK;
}

enum thermal_calibration_v1_result
thermal_calibration_v1_finish_with_precommit(
    struct thermal_calibration_v1_commit* commit,
    thermal_calibration_v1_precommit_fn precommit, void* context)
{
    if (!calibration.upload_active) return THERMAL_CALIBRATION_V1_BUSY;
    if (calibration.error != THERMAL_CALIBRATION_V1_OK) {
        thermal_calibration_v1_abort();
        return calibration.error;
    }
    if (calibration.received != THERMAL_CALIBRATION_V1_PACKET_BYTES
        || calibration.payload_received
               != THERMAL_CALIBRATION_V1_PACKET_BYTES
                      - THERMAL_CALIBRATION_V1_HEADER_BYTES) {
        thermal_calibration_v1_abort();
        return THERMAL_CALIBRATION_V1_BAD_SIZE;
    }
    if (!calibration_flush_page()) {
        thermal_calibration_v1_abort();
        return THERMAL_CALIBRATION_V1_FLASH_ERROR;
    }

    const struct calibration_slot_layout* slot =
        calibration_slot_layout(calibration.target_slot);
    struct thermal_calibration_v1_manifest manifest;
    if (!slot || !calibration_payload_valid(slot, &manifest)
        || manifest.image_crc32 != read_le32(calibration.package_header + 36)
        || manifest.gain_crc32 != read_le32(calibration.package_header + 40)
        || manifest.badpoint_crc32 != read_le32(calibration.package_header + 44)
        || manifest.payload_crc32 != read_le32(calibration.package_header + 48)) {
        thermal_calibration_v1_abort();
        return THERMAL_CALIBRATION_V1_VERIFY_ERROR;
    }

    struct FlashDataInfo flash;
    calibration_make_flash_header(slot, calibration.package_header, &flash);
    const uint32_t generation = calibration.active_generation + 1u;
    if (precommit && !precommit(context)) {
        thermal_calibration_v1_abort();
        return THERMAL_CALIBRATION_V1_FLASH_ERROR;
    }
    if (!calibration_commit_selector(slot, &flash, generation,
                                     manifest.payload_crc32)) {
        thermal_calibration_v1_abort();
        return THERMAL_CALIBRATION_V1_FLASH_ERROR;
    }

    manifest.package_header_crc32 = read_le32(calibration.package_header + 60);
    manifest.flash_header_crc32 = thermal_flash_crc32(
        (const uint8_t*)&flash, sizeof(flash));
    manifest.generation = generation;
    /* The selector stores the ID only after its payload has been validated;
     * derive the same deterministic value before publishing the record. */
    manifest.calibration_id = thermal_flash_crc32(
        calibration.package_header,
        THERMAL_CALIBRATION_V1_HEADER_BYTES);
    manifest.valid = true;
    if (commit) {
        commit->generation = generation;
        commit->calibration_id = manifest.calibration_id;
        commit->payload_crc32 = manifest.payload_crc32;
        commit->package_header_crc32 = manifest.package_header_crc32;
        commit->flash_header_crc32 = manifest.flash_header_crc32;
    }
    calibration.upload_active = false;
    calibration.reboot_required = true;
    return THERMAL_CALIBRATION_V1_OK;
}


enum thermal_calibration_v1_result thermal_calibration_v1_finish(
    struct thermal_calibration_v1_commit* commit)
{
    return thermal_calibration_v1_finish_with_precommit(commit, NULL, NULL);
}
void thermal_calibration_v1_abort(void)
{
    calibration.upload_active = false;
    calibration.payload_prepared = false;
    calibration.page_used = 0;
    calibration.current_section_bytes = 0;
    calibration.current_section = 0;
}

bool thermal_calibration_v1_active(void)
{
    return calibration.upload_active || calibration.reboot_required;
}

bool thermal_calibration_v1_upload_active(void)
{
    return calibration.upload_active;
}

const char* thermal_calibration_v1_result_string(
    enum thermal_calibration_v1_result result)
{
    static const char* const names[] = {
        "ok", "busy", "invalid packet size", "invalid TNC1 header",
        "invalid calibration data", "flash write failed", "readback verification failed",
    };
    return (unsigned)result < sizeof(names) / sizeof(names[0])
               ? names[result]
               : "unknown";
}

bool thermal_calibration_v1_manifest(
    struct thermal_calibration_v1_manifest* manifest)
{
    const struct calibration_slot_layout* slot = calibration_active_layout();
    if (!manifest || !slot
        || !calibration_flash_header_matches_slot(flash_header, slot)
        || !calibration_payload_valid(slot, manifest)) {
        return false;
    }
    uint8_t packet_header[THERMAL_CALIBRATION_V1_HEADER_BYTES];
    if (!thermal_calibration_v1_make_packet_header(packet_header)) {
        return false;
    }
    manifest->package_header_crc32 = read_le32(packet_header + 60);
    manifest->flash_header_crc32 = thermal_flash_crc32(
        (const uint8_t*)flash_header, sizeof(*flash_header));
    manifest->generation = calibration.active_generation;
    manifest->calibration_id = calibration.active_calibration_id;
    manifest->valid = true;
    return true;
}

bool thermal_calibration_v1_make_packet_header(
    uint8_t header[THERMAL_CALIBRATION_V1_HEADER_BYTES])
{
    const struct calibration_slot_layout* slot = calibration_active_layout();
    if (!header || !slot
        || !calibration_flash_header_matches_slot(flash_header, slot)) {
        return false;
    }
    struct thermal_calibration_v1_manifest manifest;
    if (!calibration_payload_valid(slot, &manifest)) return false;
    memset(header, 0, THERMAL_CALIBRATION_V1_HEADER_BYTES);
    memcpy(header, "TNC1", 4);
    write_le16(header + 4, 1u);
    write_le16(header + 6, THERMAL_CALIBRATION_V1_HEADER_BYTES);
    write_le16(header + 8, SENSOR_W);
    write_le16(header + 10, SENSOR_H);
    write_le16(header + 12, MAX_BADPT);
    write_le32(header + 16, THERMAL_CALIBRATION_V1_IMAGE_BYTES);
    write_le32(header + 20, THERMAL_CALIBRATION_V1_GAIN_BYTES);
    write_le32(header + 24, THERMAL_CALIBRATION_V1_BADPOINT_BYTES);
    write_le_float(header + 28, flash_header->mean_diff);
    write_le_float(header + 32, flash_header->t_ref_low);
    if (calibration_ntc_model_fields_valid(
            flash_header->thermal_ntc_tag,
            flash_header->thermal_ntc_reference_adu,
            flash_header->thermal_ntc_k_q8,
            flash_header->thermal_ntc_limit_adu)) {
        write_le16(header + THERMAL_CALIBRATION_V1_NTC_MODEL_OFFSET,
                   flash_header->thermal_ntc_tag);
        write_le16(header + THERMAL_CALIBRATION_V1_NTC_MODEL_OFFSET + 2u,
                   flash_header->thermal_ntc_reference_adu);
        write_le16(header + THERMAL_CALIBRATION_V1_NTC_MODEL_OFFSET + 4u,
                   (uint16_t)flash_header->thermal_ntc_k_q8);
        write_le16(header + THERMAL_CALIBRATION_V1_NTC_MODEL_OFFSET + 6u,
                   flash_header->thermal_ntc_limit_adu);
    }
    write_le32(header + 36, manifest.image_crc32);
    write_le32(header + 40, manifest.gain_crc32);
    write_le32(header + 44, manifest.badpoint_crc32);
    write_le32(header + 48, manifest.payload_crc32);
    write_le32(header + 60, thermal_flash_crc32(header, 60u));
    return true;
}

bool thermal_calibration_v1_data(enum thermal_calibration_v1_data_kind kind,
                                 const uint8_t** data, uint32_t* bytes)
{
    const struct calibration_slot_layout* slot = calibration_active_layout();
    if (!data || !bytes || !slot
        || !calibration_flash_header_matches_slot(flash_header, slot)) {
        return false;
    }
    switch (kind) {
    case THERMAL_CALIBRATION_V1_DATA_IMAGE:
        *data = calibration_xip(slot->imgbg_offset);
        *bytes = THERMAL_CALIBRATION_V1_IMAGE_BYTES;
        return true;
    case THERMAL_CALIBRATION_V1_DATA_GAIN:
        *data = calibration_xip(slot->gain_offset);
        *bytes = THERMAL_CALIBRATION_V1_GAIN_BYTES;
        return true;
    case THERMAL_CALIBRATION_V1_DATA_BADPOINTS:
        *data = calibration_xip(slot->badpoints_offset);
        *bytes = THERMAL_CALIBRATION_V1_BADPOINT_BYTES;
        return true;
    }
    return false;
}

uint32_t thermal_calibration_v1_id(void)
{
    return calibration.active_calibration_id;
}

bool thermal_calibration_v1_ntc_model(
    struct thermal_calibration_v1_ntc_model* model)
{
    if (!model || !calibration.boot_has_calibration || !flash_header
        || flash_header->thermal_ntc_tag
               != THERMAL_CALIBRATION_V1_NTC_MODEL_TAG
        || !calibration_ntc_model_fields_valid(
            flash_header->thermal_ntc_tag,
            flash_header->thermal_ntc_reference_adu,
            flash_header->thermal_ntc_k_q8,
            flash_header->thermal_ntc_limit_adu)) {
        return false;
    }
    model->tag = flash_header->thermal_ntc_tag;
    model->reference_adu = flash_header->thermal_ntc_reference_adu;
    model->k_q8 = flash_header->thermal_ntc_k_q8;
    model->limit_adu = flash_header->thermal_ntc_limit_adu;
    return true;
}

bool thermal_calibration_v1_startup_model(
    struct thermal_calibration_v1_startup_model* model)
{
    if (!model || !calibration.boot_has_calibration || !flash_header
        || flash_header->thermal_ntc_tag
               != THERMAL_CALIBRATION_V1_STARTUP_MODEL_TAG
        || !calibration_ntc_model_fields_valid(
            flash_header->thermal_ntc_tag,
            flash_header->thermal_ntc_reference_adu,
            flash_header->thermal_ntc_k_q8,
            flash_header->thermal_ntc_limit_adu)) {
        return false;
    }
    model->tag = flash_header->thermal_ntc_tag;
    model->cold_ntc_threshold_adu =
        flash_header->thermal_ntc_reference_adu;
    model->amplitude_adu = flash_header->thermal_ntc_k_q8;
    model->tau_ds = flash_header->thermal_ntc_limit_adu;
    return true;
}

bool thermal_calibration_v1_row_ref_model(
    struct thermal_calibration_v1_row_ref_model* model)
{
    if (!model || !calibration.boot_has_calibration || !flash_header
        || flash_header->thermal_ntc_tag
               != THERMAL_CALIBRATION_V1_ROW_REF_MODEL_TAG
        || !calibration_ntc_model_fields_valid(
            flash_header->thermal_ntc_tag,
            flash_header->thermal_ntc_reference_adu,
            flash_header->thermal_ntc_k_q8,
            flash_header->thermal_ntc_limit_adu)) {
        return false;
    }
    model->tag = flash_header->thermal_ntc_tag;
    model->reference_adu = flash_header->thermal_ntc_reference_adu;
    model->k_q8 = flash_header->thermal_ntc_k_q8;
    model->limit_adu = flash_header->thermal_ntc_limit_adu;
    return true;
}
