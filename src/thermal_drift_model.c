#include "thermal_drift_model.h"

#include <string.h>

#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"

#include "sdk.h"
#include "thermal_calibration_v1.h"
#include "thermal_ota.h"

/* The two existing calibration payload slots end at 0x4ac00. Reserve two
 * following sectors for a tiny journaled residual record; no V1 image/gain,
 * OTA or settings address moves. */
#define THERMAL_DRIFT_MODEL_SLOT_PRIMARY   0x04b000u
#define THERMAL_DRIFT_MODEL_SLOT_SECONDARY 0x04c000u

static const char thermal_drift_packet_magic[8] = {
    'T', '1', '6', '0', 'D', 'M', '2', '\0',
};
static const char thermal_drift_record_magic[8] = {
    'T', '1', '6', '0', 'D', 'R', '2', '\0',
};

struct __attribute__((packed)) thermal_drift_model_record {
    char magic[8];
    uint32_t generation;
    struct thermal_drift_model_packet packet;
    uint32_t crc32;
};

static_assert(sizeof(struct thermal_drift_model_record) <= FLASH_PAGE_SIZE,
              "thermal drift model record must fit one flash page");
static_assert(THERMAL_DRIFT_MODEL_SLOT_PRIMARY
                  >= 0x038000u + 160u * 120u * sizeof(float),
              "thermal drift record overlaps secondary gain table");
static_assert(THERMAL_DRIFT_MODEL_SLOT_SECONDARY + FLASH_SECTOR_SIZE
                  <= FLASH_CALIBRATION_DATA_SIZE,
              "thermal drift record overlaps OTA/settings sectors");

volatile uint8_t g_thermal_drift_model_valid = 0;
volatile uint8_t g_thermal_drift_model_active = 0;
volatile uint8_t g_thermal_drift_model_mean_active = 0;
volatile uint8_t g_thermal_drift_model_per_device = 0;
volatile uint8_t g_thermal_drift_model_ntc_clamped = 0;
volatile uint16_t g_thermal_drift_model_base_ntc_adu = 0;
volatile uint16_t g_thermal_drift_model_ntc_min_adu = 0;
volatile uint16_t g_thermal_drift_model_ntc_max_adu = 0;
volatile int16_t g_thermal_drift_model_mean_k_q8 = THERMAL_DRIFT_MEAN_K_Q8;
volatile int16_t g_thermal_drift_model_mean_anchor_comp_adu = 0;
volatile int16_t g_thermal_drift_model_device_anchor_residual_adu = 0;
volatile int16_t g_thermal_drift_model_anchor_comp_adu = 0;
volatile int16_t g_thermal_drift_model_gain_delta_q16 = 0;
volatile int32_t g_thermal_drift_model_gain_scale_q16 = 65536;

static struct {
    bool per_device;
    uint32_t generation;
    struct thermal_drift_model_packet packet;
} thermal_drift_model;

static bool ntc_adu_valid(uint16_t value)
{
    return value > 8u && value < 4088u;
}

static int32_t i32_abs(int32_t value)
{
    return value < 0 ? -value : value;
}

static int32_t clamp_i32(int32_t value, int32_t minimum, int32_t maximum)
{
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

static int32_t round_divide_i64(int64_t numerator, int32_t denominator)
{
    if (denominator < 0) {
        numerator = -numerator;
        denominator = -denominator;
    }
    return numerator >= 0
        ? (int32_t)((numerator + denominator / 2) / denominator)
        : -(int32_t)(((-numerator) + denominator / 2) / denominator);
}

static int32_t mean_anchor_comp_for(uint16_t base_ntc_adu,
                                    uint16_t current_ntc_adu,
                                    bool* clamped)
{
    const int32_t delta = (int32_t)current_ntc_adu
                          - (int32_t)base_ntc_adu;
    int32_t comp = round_divide_i64(
        (int64_t)delta * THERMAL_DRIFT_MEAN_K_Q8, 256);
    const int32_t bounded = clamp_i32(
        comp, -THERMAL_DRIFT_MEAN_LIMIT_ADU,
        THERMAL_DRIFT_MEAN_LIMIT_ADU);
    if (clamped && bounded != comp) *clamped = true;
    return bounded;
}

static int32_t residual_for(int16_t learned_value,
                            uint16_t base_ntc_adu,
                            uint16_t learned_ntc_adu,
                            uint16_t current_ntc_adu,
                            bool* clamped)
{
    int32_t span = (int32_t)learned_ntc_adu - (int32_t)base_ntc_adu;
    int32_t position = (int32_t)current_ntc_adu - (int32_t)base_ntc_adu;
    if (span > 0) {
        if (position < 0) {
            position = 0;
            if (clamped) *clamped = true;
        } else if (position > span) {
            position = span;
            if (clamped) *clamped = true;
        }
    } else {
        if (position > 0) {
            position = 0;
            if (clamped) *clamped = true;
        } else if (position < span) {
            position = span;
            if (clamped) *clamped = true;
        }
    }
    return round_divide_i64((int64_t)learned_value * position, span);
}

static const uint8_t* model_xip(uint32_t offset)
{
    return (const uint8_t*)(XIP_BASE + FLASH_TARGET_OFFSET + offset);
}

static uint32_t packet_crc32(const struct thermal_drift_model_packet* packet)
{
    return thermal_flash_crc32(
        (const uint8_t*)packet,
        offsetof(struct thermal_drift_model_packet, crc32));
}

static bool packet_reserved_zero(
    const struct thermal_drift_model_packet* packet)
{
    for (size_t i = 0; i < sizeof(packet->reserved); ++i) {
        if (packet->reserved[i] != 0u) return false;
    }
    return true;
}

static bool packet_valid(const struct thermal_drift_model_packet* packet,
                         uint32_t expected_calibration_id)
{
    uint16_t stored_base_ntc = 0;
    if (!packet
        || memcmp(packet->magic, thermal_drift_packet_magic,
                  sizeof(packet->magic)) != 0
        || packet->version != THERMAL_DRIFT_MODEL_VERSION
        || packet->bytes != THERMAL_DRIFT_MODEL_BYTES
        || packet->calibration_id == 0u
        || (expected_calibration_id != 0u
            && packet->calibration_id != expected_calibration_id)
        || (packet->flags & ~THERMAL_DRIFT_MODEL_FLAGS_MASK) != 0u
        || packet->flags == (THERMAL_DRIFT_MODEL_FLAG_GAIN_RESIDUAL
                             | THERMAL_DRIFT_MODEL_FLAG_CLEAR)
        || packet->flags == 0u
        || packet->mean_k_q8 != THERMAL_DRIFT_MEAN_K_Q8
        || packet->mean_limit_adu != THERMAL_DRIFT_MEAN_LIMIT_ADU
        || packet->total_anchor_limit_adu
               != THERMAL_DRIFT_MODEL_ANCHOR_LIMIT_ADU
        || !packet_reserved_zero(packet)
        || packet_crc32(packet) != packet->crc32
        || !ntc_adu_valid(packet->base_ntc_adu)) {
        return false;
    }
    if (expected_calibration_id != 0u
        && thermal_calibration_v1_base_ntc_adu(&stored_base_ntc)
        && packet->base_ntc_adu != stored_base_ntc) {
        return false;
    }
    if (packet->flags & THERMAL_DRIFT_MODEL_FLAG_CLEAR) {
        return packet->learned_ntc_adu == packet->base_ntc_adu
               && packet->anchor_residual_adu == 0
               && packet->gain_residual_q16 == 0;
    }
    if (!(packet->flags & THERMAL_DRIFT_MODEL_FLAG_GAIN_RESIDUAL)
        || !ntc_adu_valid(packet->learned_ntc_adu)
        || i32_abs((int32_t)packet->learned_ntc_adu
                   - (int32_t)packet->base_ntc_adu)
               < (int32_t)THERMAL_DRIFT_MODEL_MIN_NTC_SPAN_ADU
        || i32_abs(packet->anchor_residual_adu)
               > THERMAL_DRIFT_MODEL_ANCHOR_LIMIT_ADU
        || i32_abs(packet->gain_residual_q16)
               > THERMAL_DRIFT_MODEL_GAIN_Q16_LIMIT) {
        return false;
    }
    const int32_t learned_mean = mean_anchor_comp_for(
        packet->base_ntc_adu, packet->learned_ntc_adu, NULL);
    return i32_abs(learned_mean + packet->anchor_residual_adu)
           <= THERMAL_DRIFT_MODEL_ANCHOR_LIMIT_ADU;
}

static bool record_valid(const struct thermal_drift_model_record* record,
                         uint32_t expected_calibration_id)
{
    return record
           && memcmp(record->magic, thermal_drift_record_magic,
                     sizeof(record->magic)) == 0
           && packet_valid(&record->packet, expected_calibration_id)
           && thermal_flash_crc32(
                  (const uint8_t*)record,
                  offsetof(struct thermal_drift_model_record, crc32))
                  == record->crc32;
}

static bool generation_newer(uint32_t candidate, uint32_t current)
{
    return (int32_t)(candidate - current) > 0;
}

static bool per_device_packet_matches_active_calibration(void)
{
#if !THERMAL_DRIFT_RUNTIME_ENABLE
    return false;
#else
    return thermal_drift_model.per_device
           && thermal_drift_model.packet.calibration_id != 0u
           && thermal_drift_model.packet.calibration_id
                  == thermal_calibration_v1_id();
#endif
}

static void clear_runtime_state(void)
{
    const bool per_device = per_device_packet_matches_active_calibration();
    g_thermal_drift_model_valid = per_device ? 1u : 0u;
    g_thermal_drift_model_active = 0;
    g_thermal_drift_model_mean_active = 0;
    g_thermal_drift_model_per_device = per_device ? 1u : 0u;
    g_thermal_drift_model_ntc_clamped = 0;
    g_thermal_drift_model_mean_k_q8 = THERMAL_DRIFT_MEAN_K_Q8;
    g_thermal_drift_model_mean_anchor_comp_adu = 0;
    g_thermal_drift_model_device_anchor_residual_adu = 0;
    g_thermal_drift_model_anchor_comp_adu = 0;
    g_thermal_drift_model_gain_delta_q16 = 0;
    g_thermal_drift_model_gain_scale_q16 = 65536;
}

void thermal_drift_model_boot_load(void)
{
    memset(&thermal_drift_model, 0, sizeof(thermal_drift_model));
    g_thermal_drift_model_valid = 0;
    g_thermal_drift_model_base_ntc_adu = 0;
    g_thermal_drift_model_ntc_min_adu = 0;
    g_thermal_drift_model_ntc_max_adu = 0;

    const uint32_t calibration_id = thermal_calibration_v1_id();
    uint16_t stored_base_ntc = 0;
    if (thermal_calibration_v1_base_ntc_adu(&stored_base_ntc)
        && ntc_adu_valid(stored_base_ntc)) {
        g_thermal_drift_model_base_ntc_adu = stored_base_ntc;
    }
    if (calibration_id != 0u && g_thermal_drift_model_base_ntc_adu != 0u) {
        const struct thermal_drift_model_record* primary =
            (const struct thermal_drift_model_record*)model_xip(
                THERMAL_DRIFT_MODEL_SLOT_PRIMARY);
        const struct thermal_drift_model_record* secondary =
            (const struct thermal_drift_model_record*)model_xip(
                THERMAL_DRIFT_MODEL_SLOT_SECONDARY);
        const bool primary_valid = record_valid(primary, calibration_id);
        const bool secondary_valid = record_valid(secondary, calibration_id);
        const struct thermal_drift_model_record* selected = NULL;
        if (primary_valid && secondary_valid) {
            selected = generation_newer(secondary->generation,
                                        primary->generation)
                ? secondary : primary;
        } else if (primary_valid) {
            selected = primary;
        } else if (secondary_valid) {
            selected = secondary;
        }
        if (selected) {
            thermal_drift_model.packet = selected->packet;
            thermal_drift_model.generation = selected->generation;
            thermal_drift_model.per_device =
                (selected->packet.flags & THERMAL_DRIFT_MODEL_FLAG_CLEAR) == 0u;
            if (thermal_drift_model.per_device) {
                g_thermal_drift_model_valid = 1;
                g_thermal_drift_model_ntc_min_adu =
                    selected->packet.base_ntc_adu
                        < selected->packet.learned_ntc_adu
                    ? selected->packet.base_ntc_adu
                    : selected->packet.learned_ntc_adu;
                g_thermal_drift_model_ntc_max_adu =
                    selected->packet.base_ntc_adu
                        > selected->packet.learned_ntc_adu
                    ? selected->packet.base_ntc_adu
                    : selected->packet.learned_ntc_adu;
            }
        }
    }
    clear_runtime_state();
}

struct thermal_drift_model_runtime thermal_drift_model_evaluate(
    uint16_t current_ntc_adu)
{
    struct thermal_drift_model_runtime result = {};
    result.gain_scale_q16 = 65536;
    clear_runtime_state();

#if !THERMAL_DRIFT_RUNTIME_ENABLE
    (void)current_ntc_adu;
    return result;
#endif

    const uint32_t calibration_id = thermal_calibration_v1_id();
    const uint16_t base_ntc_adu = g_thermal_drift_model_base_ntc_adu;
    if (calibration_id == 0u || !ntc_adu_valid(base_ntc_adu)
        || !ntc_adu_valid(current_ntc_adu)) {
        return result;
    }

    bool clamped = false;
    const int32_t mean_anchor = mean_anchor_comp_for(
        base_ntc_adu, current_ntc_adu, &clamped);
    int32_t device_anchor = 0;
    int32_t gain_delta_q16 = 0;
    const bool per_device = per_device_packet_matches_active_calibration();
    if (per_device) {
        device_anchor = residual_for(
            thermal_drift_model.packet.anchor_residual_adu,
            base_ntc_adu, thermal_drift_model.packet.learned_ntc_adu,
            current_ntc_adu, &clamped);
        gain_delta_q16 = residual_for(
            thermal_drift_model.packet.gain_residual_q16,
            base_ntc_adu, thermal_drift_model.packet.learned_ntc_adu,
            current_ntc_adu, NULL);
    }

    const int32_t anchor_comp = clamp_i32(
        mean_anchor + device_anchor,
        -THERMAL_DRIFT_MODEL_ANCHOR_LIMIT_ADU,
        THERMAL_DRIFT_MODEL_ANCHOR_LIMIT_ADU);
    if (anchor_comp != mean_anchor + device_anchor) clamped = true;
    const int32_t gain_scale_q16 = clamp_i32(
        65536 + gain_delta_q16,
        THERMAL_DRIFT_MODEL_GAIN_SCALE_MIN_Q16,
        THERMAL_DRIFT_MODEL_GAIN_SCALE_MAX_Q16);

    result.active = true;
    result.mean_active = true;
    result.per_device = per_device;
    result.ntc_clamped = clamped;
    result.mean_anchor_comp_adu = (int16_t)mean_anchor;
    result.device_anchor_residual_adu = (int16_t)device_anchor;
    result.anchor_comp_adu = (int16_t)anchor_comp;
    result.gain_delta_q16 = (int16_t)gain_delta_q16;
    result.gain_scale_q16 = gain_scale_q16;

    g_thermal_drift_model_active = 1;
    g_thermal_drift_model_mean_active = 1;
    g_thermal_drift_model_per_device = per_device ? 1u : 0u;
    g_thermal_drift_model_ntc_clamped = clamped ? 1u : 0u;
    g_thermal_drift_model_mean_anchor_comp_adu = (int16_t)mean_anchor;
    g_thermal_drift_model_device_anchor_residual_adu =
        (int16_t)device_anchor;
    g_thermal_drift_model_anchor_comp_adu = (int16_t)anchor_comp;
    g_thermal_drift_model_gain_delta_q16 = (int16_t)gain_delta_q16;
    g_thermal_drift_model_gain_scale_q16 = gain_scale_q16;
    return result;
}

bool thermal_drift_model_packet(uint8_t out[THERMAL_DRIFT_MODEL_BYTES])
{
    if (!out || !per_device_packet_matches_active_calibration()) return false;
    memcpy(out, &thermal_drift_model.packet, THERMAL_DRIFT_MODEL_BYTES);
    return true;
}

bool thermal_drift_model_manifest(struct thermal_drift_model_manifest* out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    const bool mean_available = THERMAL_DRIFT_RUNTIME_ENABLE
        && thermal_calibration_v1_id() != 0u
        && ntc_adu_valid(g_thermal_drift_model_base_ntc_adu);
    const bool per_device = per_device_packet_matches_active_calibration();
    out->valid = per_device;
    out->per_device = per_device;
    out->mean_fallback_available = mean_available;
    out->generation = thermal_drift_model.generation;
    out->calibration_id = thermal_calibration_v1_id();
    out->base_ntc_adu = g_thermal_drift_model_base_ntc_adu;
    out->mean_k_q8 = THERMAL_DRIFT_MEAN_K_Q8;
    if (per_device) {
        out->packet_crc32 = thermal_drift_model.packet.crc32;
        out->learned_ntc_adu = thermal_drift_model.packet.learned_ntc_adu;
    }
    return true;
}

static enum thermal_drift_model_result thermal_drift_model_write_packet(
    const struct thermal_drift_model_packet* packet,
    uint32_t calibration_id)
{
    const struct thermal_drift_model_record* primary =
        (const struct thermal_drift_model_record*)model_xip(
            THERMAL_DRIFT_MODEL_SLOT_PRIMARY);
    const struct thermal_drift_model_record* secondary =
        (const struct thermal_drift_model_record*)model_xip(
            THERMAL_DRIFT_MODEL_SLOT_SECONDARY);
    const bool primary_valid = record_valid(primary, 0u);
    const bool secondary_valid = record_valid(secondary, 0u);
    const struct thermal_drift_model_record* newest = NULL;
    uint32_t target_offset = THERMAL_DRIFT_MODEL_SLOT_PRIMARY;
    if (primary_valid && secondary_valid) {
        newest = generation_newer(secondary->generation, primary->generation)
            ? secondary : primary;
        target_offset = newest == primary
            ? THERMAL_DRIFT_MODEL_SLOT_SECONDARY
            : THERMAL_DRIFT_MODEL_SLOT_PRIMARY;
    } else if (primary_valid) {
        newest = primary;
        target_offset = THERMAL_DRIFT_MODEL_SLOT_SECONDARY;
    } else if (secondary_valid) {
        newest = secondary;
        target_offset = THERMAL_DRIFT_MODEL_SLOT_PRIMARY;
    }

    struct thermal_drift_model_record record = {};
    memcpy(record.magic, thermal_drift_record_magic, sizeof(record.magic));
    record.generation = newest ? newest->generation + 1u : 1u;
    memcpy(&record.packet, packet, sizeof(record.packet));
    record.crc32 = thermal_flash_crc32(
        (const uint8_t*)&record,
        offsetof(struct thermal_drift_model_record, crc32));

    uint8_t page[FLASH_PAGE_SIZE] __attribute__((aligned(4)));
    memset(page, 0xff, sizeof(page));
    memcpy(page, &record, sizeof(record));
    bool verified = false;
    for (unsigned attempt = 0; attempt < 2u; ++attempt) {
        if (!thermal_ota_model_flash_erase(target_offset)
            || !thermal_ota_model_flash_program(target_offset, page)) {
            return THERMAL_DRIFT_MODEL_FLASH_ERROR;
        }
        flash_flush_cache();
        const struct thermal_drift_model_record* verify =
            (const struct thermal_drift_model_record*)model_xip(target_offset);
        if (record_valid(verify, calibration_id)
            && verify->generation == record.generation
            && memcmp(verify, &record, sizeof(record)) == 0) {
            verified = true;
            break;
        }
    }
    if (!verified) {
        return THERMAL_DRIFT_MODEL_VERIFY_ERROR;
    }
    thermal_drift_model_boot_load();
    return THERMAL_DRIFT_MODEL_OK;
}

enum thermal_drift_model_result thermal_drift_model_write(
    const uint8_t* data, size_t bytes)
{
    if (!data || bytes != THERMAL_DRIFT_MODEL_BYTES) {
        return THERMAL_DRIFT_MODEL_BAD_SIZE;
    }
    const uint32_t calibration_id = thermal_calibration_v1_id();
    const struct thermal_drift_model_packet* packet =
        (const struct thermal_drift_model_packet*)data;
    if (calibration_id == 0u || !packet_valid(packet, calibration_id)) {
        return THERMAL_DRIFT_MODEL_BAD_PACKET;
    }
    if (thermal_ota_active() || thermal_ota_install_pending()
        || thermal_calibration_v1_active()) {
        return THERMAL_DRIFT_MODEL_BUSY;
    }
    return thermal_drift_model_write_packet(packet, calibration_id);
}

enum thermal_drift_model_result thermal_drift_model_clear(void)
{
    const uint32_t calibration_id = thermal_calibration_v1_id();
    uint16_t base_ntc_adu = 0;
    if (calibration_id == 0u
        || !thermal_calibration_v1_base_ntc_adu(&base_ntc_adu)
        || !ntc_adu_valid(base_ntc_adu)) {
        return THERMAL_DRIFT_MODEL_BAD_PACKET;
    }
    if (thermal_ota_active() || thermal_ota_install_pending()
        || thermal_calibration_v1_active()) {
        return THERMAL_DRIFT_MODEL_BUSY;
    }
    struct thermal_drift_model_packet packet = {};
    memcpy(packet.magic, thermal_drift_packet_magic, sizeof(packet.magic));
    packet.version = THERMAL_DRIFT_MODEL_VERSION;
    packet.bytes = THERMAL_DRIFT_MODEL_BYTES;
    packet.calibration_id = calibration_id;
    packet.base_ntc_adu = base_ntc_adu;
    packet.learned_ntc_adu = base_ntc_adu;
    packet.flags = THERMAL_DRIFT_MODEL_FLAG_CLEAR;
    packet.mean_k_q8 = THERMAL_DRIFT_MEAN_K_Q8;
    packet.mean_limit_adu = THERMAL_DRIFT_MEAN_LIMIT_ADU;
    packet.total_anchor_limit_adu = THERMAL_DRIFT_MODEL_ANCHOR_LIMIT_ADU;
    packet.crc32 = packet_crc32(&packet);
    if (!packet_valid(&packet, calibration_id)) {
        return THERMAL_DRIFT_MODEL_BAD_PACKET;
    }
    return thermal_drift_model_write_packet(&packet, calibration_id);
}

const char* thermal_drift_model_result_string(
    enum thermal_drift_model_result result)
{
    static const char* const names[] = {
        "ok", "busy", "invalid model size", "invalid model packet",
        "flash write failed", "model readback verification failed",
    };
    return (unsigned)result < sizeof(names) / sizeof(names[0])
        ? names[result] : "unknown";
}
