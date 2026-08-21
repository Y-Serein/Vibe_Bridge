#include "thermal_ota.h"

#include <string.h>

#include "boot/picoboot_constants.h"
#include "hardware/flash.h"
#include "hardware/regs/watchdog.h"
#include "hardware/structs/watchdog.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "pico/platform/common.h"
#include "pico/stdlib.h"

#include "sdk.h"

/* Flash layout (ZD25WQ16C, 16 Mbit / 2 MiB):
 *   0x000000..0x0bffff  active application slot       (768 KiB)
 *   0x0c0000..0x17ffff  staged raw .bin image slot    (768 KiB)
 *   0x180000..0x1fdfff  sensor calibration data       (504 KiB)
 *   0x1fe000..0x1fefff  OTA transaction header          (4 KiB)
 *   0x1ff000..0x1fffff  persistent product settings     (4 KiB)
 *
 * Older firmware incorrectly described this part as 4 MiB and accessed the
 * calibration window at 0x380000. On the 2 MiB device that address wrapped
 * to the same physical 0x180000 location. OTA must never depend on wrapping:
 * an erase at the former staging address 0x200000 aliases address zero and
 * can destroy the executing image.
 */
#define OTA_ACTIVE_LIMIT       (FLASH_TARGET_OFFSET / 2u)
#define OTA_STAGE_IMAGE_OFS    OTA_ACTIVE_LIMIT
#define OTA_STAGE_END_OFS      FLASH_TARGET_OFFSET
#define OTA_STAGE_HEADER_OFS   FLASH_OTA_HEADER_OFFSET
#define PRODUCT_SETTINGS_OFS   FLASH_PRODUCT_SETTINGS_OFFSET
#define LEGACY_SETTINGS_OFS    ((1u * 1024u * 1024u) - FLASH_SECTOR_SIZE)
#define OTA_MAX_IMAGE_BYTES    (OTA_STAGE_END_OFS - OTA_STAGE_IMAGE_OFS)
#define OTA_MIN_IMAGE_BYTES    4096u
#define OTA_HEADER_MAGIC       "T160OTA1"
#define OTA_WATCHDOG_MS        8000u
#define OTA_WATCHDOG_LOAD_US   (OTA_WATCHDOG_MS * 1000u)
#define OTA_DIAG_MAGIC         0x544f5441u

enum ota_diag_stage {
    OTA_DIAG_IDLE = 0,
    OTA_DIAG_BEGIN = 1,
    OTA_DIAG_CORE1_WAIT = 2,
    OTA_DIAG_CORE0_ERASE = 3,
    OTA_DIAG_CORE0_PROGRAM = 4,
    OTA_DIAG_FLASH_DONE = 5,
    OTA_DIAG_VERIFY = 6,
    OTA_DIAG_INSTALL_REQUEST = 7,
    OTA_DIAG_INSTALL_ERASE = 8,
    OTA_DIAG_INSTALL_PROGRAM = 9,
    OTA_DIAG_INSTALL_DONE = 10,
    OTA_DIAG_BAD_HEADER = 11,
    OTA_DIAG_BAD_STAGED_CRC = 12,
};

static_assert(OTA_STAGE_HEADER_OFS % FLASH_SECTOR_SIZE == 0,
              "OTA header must be sector aligned");
static_assert(OTA_STAGE_IMAGE_OFS % FLASH_SECTOR_SIZE == 0,
              "OTA image must be sector aligned");
static_assert(FLASH_TARGET_OFFSET == 1536u * 1024u,
              "the two firmware slots require a 1.5 MiB prefix");
static_assert(OTA_STAGE_IMAGE_OFS == OTA_ACTIVE_LIMIT,
              "active and staging slots must be adjacent");
static_assert(OTA_MAX_IMAGE_BYTES == OTA_ACTIVE_LIMIT,
              "active and staging slots must have equal capacity");
static_assert(OTA_STAGE_END_OFS == FLASH_TARGET_OFFSET,
              "OTA staging must end where calibration begins");
static_assert(FLASH_TARGET_OFFSET + FLASH_CALIBRATION_DATA_SIZE
                  == OTA_STAGE_HEADER_OFS,
              "OTA header must follow calibration data");
static_assert(OTA_STAGE_HEADER_OFS + FLASH_SECTOR_SIZE
                  == PRODUCT_SETTINGS_OFS,
              "settings must immediately follow the OTA header");
static_assert(PRODUCT_SETTINGS_OFS + FLASH_SECTOR_SIZE == FLASH_SIZE,
              "settings must occupy the final Flash sector");
static_assert(FLASH_OFS_GAIN + SENSOR_W * SENSOR_H * sizeof(float)
                  <= FLASH_CALIBRATION_DATA_SIZE,
              "calibration tables overlap reserved tail sectors");
static_assert(FLASH_OFS_IMGBG + SENSOR_W * SENSOR_H * sizeof(uint16_t)
                  <= FLASH_CALIBRATION_DATA_SIZE,
              "background table overlaps reserved tail sectors");
static_assert(FLASH_OFS_BADPTS + MAX_BADPT * sizeof(struct BadPt)
                  <= FLASH_CALIBRATION_DATA_SIZE,
              "bad-pixel table overlaps reserved tail sectors");
static_assert(FLASH_SECTOR_SIZE == 4096u,
              "layout constants assume 4 KiB erase sectors");
static_assert(FLASH_SIZE == PICO_FLASH_SIZE_BYTES,
              "SDK and linker flash sizes must match");

struct __attribute__((packed)) ota_stage_header {
    char magic[8];
    uint32_t image_bytes;
    uint32_t image_crc32;
    uint32_t image_offset;
    uint32_t reserved[3];
};

struct ota_flash_op {
    uint32_t offset;
    const uint8_t* data;
    uint8_t kind;
};

enum ota_flash_kind {
    OTA_FLASH_ERASE = 1,
    OTA_FLASH_PROGRAM = 2,
    OTA_FLASH_INSTALL = 3,
};

enum ota_core0_flash_state {
    OTA_CORE0_FLASH_IDLE = 0,
    OTA_CORE0_FLASH_REQUEST = THERMAL_OTA_CORE0_FLASH_REQUEST,
    OTA_CORE0_FLASH_BUSY = 2,
    OTA_CORE0_FLASH_DONE = 3,
};

static struct {
    bool active;
    bool install_pending;
    bool watchdog_armed;
    uint32_t install_at_ms;
    uint32_t expected;
    uint32_t received;
    uint32_t programmed;
    uint32_t crc;
    uint16_t page_used;
    uint8_t page[FLASH_PAGE_SIZE] __attribute__((aligned(4)));
} ota;

volatile uint32_t g_thermal_ota_core0_flash_state = OTA_CORE0_FLASH_IDLE;
static struct ota_flash_op core0_flash_op;
static volatile int core0_flash_result;

void thermal_ota_init(void)
{
    /* The RP2350 ROM otherwise defaults FLASH_DEVINFO CS0 to 16 MiB when OTP
     * has not provisioned it. Its checked flash API consults this boot-RAM
     * value, not PICO_FLASH_SIZE_BYTES, so publish the real hardware size. */
    flash_devinfo_set_cs_size(0, FLASH_DEVINFO_SIZE_2M);
}

static int thermal_ota_install_now(void)
    __attribute__((noinline,
                   section(".time_critical.thermal_ota_install"),
                   optimize("no-tree-loop-distribute-patterns")));

static void ota_reboot_now(uint32_t reboot_type, uint32_t update_base)
    __attribute__((noreturn, noinline,
                   section(".time_critical.thermal_ota_reboot")));

/* RP2350 requires the ROM reboot2 protocol after programming Flash. A raw
 * watchdog trigger leaves the ROM without the scratch-register boot request
 * that picotool/UF2 use, and can fall through to BOOTSEL even when the new
 * image is valid. This helper itself must remain in SRAM because the active
 * XIP image may already have been replaced. */
static void ota_reboot_now(uint32_t reboot_type, uint32_t update_base)
{
    rom_reboot_fn reboot_fn =
        (rom_reboot_fn)rom_func_lookup_inline(ROM_FUNC_REBOOT);
    const int rc = reboot_fn(reboot_type | REBOOT2_FLAG_NO_RETURN_ON_SUCCESS,
                             10, update_base, 0);

    /* NO_RETURN_ON_SUCCESS means reaching here is a ROM error. Preserve it
     * for post-mortem diagnostics and force a safe reset as a last resort. */
    watchdog_hw->scratch[3] = (uint32_t)rc;
    watchdog_hw->ctrl = WATCHDOG_CTRL_TRIGGER_BITS;
    while (true) __wfi();
}

static inline __attribute__((always_inline))
void ota_diag_set(uint32_t stage, uint32_t offset)
{
    watchdog_hw->scratch[0] = OTA_DIAG_MAGIC;
    watchdog_hw->scratch[1] = stage;
    watchdog_hw->scratch[2] = offset;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t len)
{
    static const uint32_t table[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
        0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
        0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
        0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
    };
    while (len--) {
        crc ^= *data++;
        crc = (crc >> 4) ^ table[crc & 0x0fu];
        crc = (crc >> 4) ^ table[crc & 0x0fu];
    }
    return crc;
}

static int __attribute__((noinline,
                          section(".time_critical.thermal_ota_rom_flash")))
ota_rom_flash_op(uint8_t kind, uint32_t offset, const uint8_t* data)
{
    rom_flash_op_fn fn =
        (rom_flash_op_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_OP);
    const cflash_flags_t flags = {
        .flags = (CFLASH_ASPACE_VALUE_STORAGE << CFLASH_ASPACE_LSB)
                 | (CFLASH_SECLEVEL_VALUE_SECURE << CFLASH_SECLEVEL_LSB)
                 | ((kind == OTA_FLASH_ERASE ? CFLASH_OP_VALUE_ERASE
                                              : CFLASH_OP_VALUE_PROGRAM)
                    << CFLASH_OP_LSB),
    };
    const uint32_t bytes = kind == OTA_FLASH_ERASE
                               ? FLASH_SECTOR_SIZE
                               : FLASH_PAGE_SIZE;
    return fn(flags, XIP_BASE + offset, bytes, (uint8_t*)data);
}

int thermal_ota_boot_probe(void)
{
    if (!bootrom_try_acquire_lock(BOOTROM_LOCK_FLASH_OP)) {
        return BOOTROM_ERROR_LOCK_REQUIRED;
    }
    const uint32_t irq_state = save_and_disable_interrupts();
    const int rc = ota_rom_flash_op(OTA_FLASH_ERASE,
                                    OTA_STAGE_HEADER_OFS, NULL);
    restore_interrupts(irq_state);
    bootrom_release_lock(BOOTROM_LOCK_FLASH_OP);
    return rc;
}

void __attribute__((noinline, section(".time_critical.thermal_ota_core0")))
thermal_ota_core0_flash_service(void)
{
    if (g_thermal_ota_core0_flash_state != OTA_CORE0_FLASH_REQUEST) return;

    const uint32_t irq_state = save_and_disable_interrupts();
    const uint8_t kind = core0_flash_op.kind;
    const uint32_t offset = core0_flash_op.offset;
    const uint8_t* data = core0_flash_op.data;
    g_thermal_ota_core0_flash_state = OTA_CORE0_FLASH_BUSY;
    __dmb();

    if (kind == OTA_FLASH_INSTALL) {
        core0_flash_result = thermal_ota_install_now();
        watchdog_hw->scratch[3] = (uint32_t)core0_flash_result;
        g_thermal_ota_core0_flash_state = OTA_CORE0_FLASH_DONE;
        __dmb();
        __sev();

        /* The active XIP image has been replaced. Core1 owns the Boot ROM
         * Flash lock and will release it before rebooting; Core0 must execute
         * nothing outside this SRAM section while that happens. */
        while (true) __wfe();
    }
    if (kind == OTA_FLASH_ERASE) {
        ota_diag_set(OTA_DIAG_CORE0_ERASE, offset);
        core0_flash_result = ota_rom_flash_op(kind, offset, NULL);
    } else if (kind == OTA_FLASH_PROGRAM) {
        ota_diag_set(OTA_DIAG_CORE0_PROGRAM, offset);
        core0_flash_result = ota_rom_flash_op(kind, offset, data);
    } else {
        core0_flash_result = BOOTROM_ERROR_INVALID_ARG;
    }

    watchdog_hw->scratch[3] = (uint32_t)core0_flash_result;
    ota_diag_set(OTA_DIAG_FLASH_DONE, offset);
    g_thermal_ota_core0_flash_state = OTA_CORE0_FLASH_DONE;
    __dmb();
    __sev();
    restore_interrupts(irq_state);
}

static bool __attribute__((noinline,
                           section(".time_critical.thermal_ota_core1_wait")))
ota_wait_for_core0_flash(void)
{
    const uint32_t irq_state = save_and_disable_interrupts();
    ota_diag_set(OTA_DIAG_CORE1_WAIT, core0_flash_op.offset);
    __dmb();
    g_thermal_ota_core0_flash_state = OTA_CORE0_FLASH_REQUEST;
    __dmb();
    __sev();
    while (g_thermal_ota_core0_flash_state != OTA_CORE0_FLASH_DONE) __wfe();
    g_thermal_ota_core0_flash_state = OTA_CORE0_FLASH_IDLE;
    __dmb();
    restore_interrupts(irq_state);
    return core0_flash_result == BOOTROM_OK;
}

static bool ota_submit_core0_flash(uint8_t kind, uint32_t offset,
                                   const uint8_t* data)
{
    if (g_thermal_ota_core0_flash_state != OTA_CORE0_FLASH_IDLE) return false;
    if (!bootrom_try_acquire_lock(BOOTROM_LOCK_FLASH_OP)) return false;
    core0_flash_op.offset = offset;
    core0_flash_op.data = data;
    core0_flash_op.kind = kind;
    watchdog_update();
    const bool ok = ota_wait_for_core0_flash();
    bootrom_release_lock(BOOTROM_LOCK_FLASH_OP);
    watchdog_update();
    return ok;
}

static bool ota_flash_erase(uint32_t offset)
{
    return ota_submit_core0_flash(OTA_FLASH_ERASE, offset, NULL);
}

static bool ota_flash_program(uint32_t offset, const uint8_t* page)
{
    return ota_submit_core0_flash(OTA_FLASH_PROGRAM, offset, page);
}

bool thermal_settings_read(void* data, size_t bytes)
{
    if (!data || bytes > FLASH_PAGE_SIZE) return false;
    memcpy(data, (const void*)(XIP_BASE + PRODUCT_SETTINGS_OFS), bytes);
    return true;
}

bool thermal_settings_read_legacy(void* data, size_t bytes)
{
    if (!data || bytes > FLASH_PAGE_SIZE) return false;
    memcpy(data, (const void*)(XIP_BASE + LEGACY_SETTINGS_OFS), bytes);
    return true;
}

bool thermal_settings_write(const void* data, size_t bytes)
{
    if (!data || !bytes || bytes > FLASH_PAGE_SIZE
        || ota.active || ota.install_pending) {
        return false;
    }
    uint8_t page[FLASH_PAGE_SIZE] __attribute__((aligned(4)));
    memset(page, 0xff, sizeof(page));
    memcpy(page, data, bytes);
    if (!ota_flash_erase(PRODUCT_SETTINGS_OFS)) return false;
    return ota_flash_program(PRODUCT_SETTINGS_OFS, page);
}

static void ota_watchdog_disarm(void)
{
    if (!ota.watchdog_armed) return;
    watchdog_disable();
    ota.watchdog_armed = false;
}

static bool ota_flush_page(void)
{
    if (!ota.page_used) return true;
    if (ota.programmed % FLASH_SECTOR_SIZE == 0
        && !ota_flash_erase(OTA_STAGE_IMAGE_OFS + ota.programmed)) {
        return false;
    }
    for (uint16_t i = ota.page_used; i < FLASH_PAGE_SIZE; i++) {
        ota.page[i] = 0xff;
    }
    if (!ota_flash_program(OTA_STAGE_IMAGE_OFS + ota.programmed, ota.page)) {
        return false;
    }
    const uint8_t* verify =
        (const uint8_t*)(XIP_BASE + OTA_STAGE_IMAGE_OFS + ota.programmed);
    for (uint16_t i = 0; i < FLASH_PAGE_SIZE; i++) {
        if (verify[i] != ota.page[i]) return false;
    }
    ota.programmed += FLASH_PAGE_SIZE;
    ota.page_used = 0;
    return true;
}

enum thermal_ota_result thermal_ota_begin(uint32_t image_bytes)
{
    if (ota.active || ota.install_pending) return THERMAL_OTA_BUSY;
    if (image_bytes < OTA_MIN_IMAGE_BYTES
        || image_bytes > OTA_MAX_IMAGE_BYTES
        || image_bytes > OTA_ACTIVE_LIMIT) {
        return THERMAL_OTA_BAD_SIZE;
    }
    memset(&ota, 0, sizeof(ota));
    ota.active = true;
    ota.expected = image_bytes;
    ota.crc = UINT32_MAX;
    ota_diag_set(OTA_DIAG_BEGIN, 0);
    watchdog_enable(OTA_WATCHDOG_MS, true);
    ota.watchdog_armed = true;
    /* An erased header means a disconnected/partial upload can never be
     * mistaken for an installable image. */
    if (!ota_flash_erase(OTA_STAGE_HEADER_OFS)) {
        ota.active = false;
        ota_watchdog_disarm();
        return THERMAL_OTA_FLASH_ERROR;
    }
    return THERMAL_OTA_OK;
}

enum thermal_ota_result thermal_ota_write(const uint8_t* data, size_t bytes)
{
    if (!ota.active || (!data && bytes)) return THERMAL_OTA_BUSY;
    if (bytes > ota.expected - ota.received) return THERMAL_OTA_BAD_SIZE;
    ota.crc = crc32_update(ota.crc, data, bytes);
    ota.received += (uint32_t)bytes;

    while (bytes) {
        size_t room = FLASH_PAGE_SIZE - ota.page_used;
        size_t take = bytes < room ? bytes : room;
        memcpy(&ota.page[ota.page_used], data, take);
        ota.page_used += (uint16_t)take;
        data += take;
        bytes -= take;
        if (ota.page_used == FLASH_PAGE_SIZE && !ota_flush_page()) {
            ota.active = false;
            ota_watchdog_disarm();
            return THERMAL_OTA_FLASH_ERROR;
        }
    }
    return THERMAL_OTA_OK;
}

static bool ota_image_header_valid(const uint8_t* image)
{
    uint32_t initial_sp;
    uint32_t reset_vector;
    memcpy(&initial_sp, image, sizeof(initial_sp));
    memcpy(&reset_vector, image + sizeof(initial_sp), sizeof(reset_vector));
    return initial_sp >= SRAM_BASE && initial_sp <= SRAM_END
           && reset_vector >= XIP_BASE
           && reset_vector < XIP_BASE + OTA_ACTIVE_LIMIT
           && (reset_vector & 1u);
}

enum thermal_ota_result thermal_ota_finish(uint32_t* crc32_out)
{
    if (!ota.active || ota.received != ota.expected) {
        ota.active = false;
        ota_watchdog_disarm();
        return THERMAL_OTA_INCOMPLETE;
    }
    if (!ota_flush_page()) {
        ota.active = false;
        ota_watchdog_disarm();
        return THERMAL_OTA_FLASH_ERROR;
    }

    ota_diag_set(OTA_DIAG_VERIFY, ota.expected);
    watchdog_update();
    const uint8_t* image =
        (const uint8_t*)(XIP_BASE + OTA_STAGE_IMAGE_OFS);
    const uint32_t crc = ~ota.crc;
    const uint32_t readback_crc =
        ~crc32_update(UINT32_MAX, image, ota.expected);
    if (crc != readback_crc) {
        ota.active = false;
        ota_watchdog_disarm();
        return THERMAL_OTA_VERIFY_ERROR;
    }
    if (!ota_image_header_valid(image)) {
        ota.active = false;
        ota_watchdog_disarm();
        return THERMAL_OTA_BAD_IMAGE;
    }

    struct ota_stage_header header = {
        .magic = OTA_HEADER_MAGIC,
        .image_bytes = ota.expected,
        .image_crc32 = crc,
        .image_offset = OTA_STAGE_IMAGE_OFS,
    };
    uint8_t header_page[FLASH_PAGE_SIZE] __attribute__((aligned(4)));
    memset(header_page, 0xff, sizeof(header_page));
    memcpy(header_page, &header, sizeof(header));
    if (!ota_flash_program(OTA_STAGE_HEADER_OFS, header_page)) {
        ota.active = false;
        ota_watchdog_disarm();
        return THERMAL_OTA_FLASH_ERROR;
    }
    ota.active = false;
    if (crc32_out) *crc32_out = crc;
    return THERMAL_OTA_OK;
}

void thermal_ota_abort(void)
{
    ota_watchdog_disarm();
    ota.active = false;
    ota.expected = 0;
    ota.received = 0;
    ota.programmed = 0;
    ota.page_used = 0;
}

bool thermal_ota_active(void) { return ota.active; }
bool thermal_ota_install_pending(void) { return ota.install_pending; }
uint32_t thermal_ota_expected_bytes(void) { return ota.expected; }
uint32_t thermal_ota_received_bytes(void) { return ota.received; }
uint32_t thermal_ota_max_image_bytes(void) { return OTA_MAX_IMAGE_BYTES; }

const char* thermal_ota_result_string(enum thermal_ota_result result)
{
    static const char* const names[] = {
        "ok", "busy", "bad size", "flash error", "incomplete",
        "verify error", "invalid RP2350 image",
    };
    return (unsigned)result < sizeof(names) / sizeof(names[0])
               ? names[result]
               : "unknown";
}

const char* thermal_ota_build_id(void)
{
    return __DATE__ " " __TIME__;
}

uint32_t thermal_ota_calibration_crc32(void)
{
    const uint8_t* calibration =
        (const uint8_t*)(XIP_BASE + FLASH_TARGET_OFFSET);
    return ~crc32_update(UINT32_MAX, calibration,
                         FLASH_CALIBRATION_DATA_SIZE);
}

bool thermal_ota_last_watchdog_reset(void)
{
    return watchdog_enable_caused_reboot();
}

uint32_t thermal_ota_diag_stage(void)
{
    return watchdog_hw->scratch[0] == OTA_DIAG_MAGIC
               ? watchdog_hw->scratch[1]
               : OTA_DIAG_IDLE;
}

uint32_t thermal_ota_diag_offset(void)
{
    return watchdog_hw->scratch[0] == OTA_DIAG_MAGIC
               ? watchdog_hw->scratch[2]
               : 0;
}

int32_t thermal_ota_diag_result(void)
{
    return watchdog_hw->scratch[0] == OTA_DIAG_MAGIC
               ? (int32_t)watchdog_hw->scratch[3]
               : 0;
}

/* From this point onward the active image is about to be destroyed. Core1
 * waits in SRAM while this Core0 function and all flash primitives execute
 * from SRAM. It returns only to the SRAM Core0 service, which signals Core1
 * and waits there for reset. */
static int thermal_ota_install_now(void)
{
    struct ota_stage_header header;
    const uint8_t* stored_header =
        (const uint8_t*)(XIP_BASE + OTA_STAGE_HEADER_OFS);
    uint8_t* header_dst = (uint8_t*)&header;
    for (size_t i = 0; i < sizeof(header); i++) {
        header_dst[i] = stored_header[i];
    }
    if (memcmp(header.magic, OTA_HEADER_MAGIC, sizeof(header.magic)) != 0
        || header.image_offset != OTA_STAGE_IMAGE_OFS
        || header.image_bytes < OTA_MIN_IMAGE_BYTES
        || header.image_bytes > OTA_MAX_IMAGE_BYTES) {
        ota_diag_set(OTA_DIAG_BAD_HEADER, header.image_bytes);
        return BOOTROM_ERROR_INVALID_DATA;
    }

    /* Re-read the complete staged image immediately before touching the
     * active slot. A corrupted header or staging bit error must leave the
     * currently bootable firmware intact. */
    const uint8_t* staged_image =
        (const uint8_t*)(XIP_BASE + OTA_STAGE_IMAGE_OFS);
    const uint32_t staged_crc =
        ~crc32_update(UINT32_MAX, staged_image, header.image_bytes);
    if (staged_crc != header.image_crc32) {
        watchdog_hw->scratch[3] = staged_crc;
        ota_diag_set(OTA_DIAG_BAD_STAGED_CRC, header.image_bytes);
        return BOOTROM_ERROR_MODIFIED_DATA;
    }

    (void)save_and_disable_interrupts();

    static uint8_t install_page[FLASH_PAGE_SIZE]
        __attribute__((aligned(4)));
    const uint32_t install_bytes =
        (header.image_bytes + FLASH_SECTOR_SIZE - 1u)
        & ~(FLASH_SECTOR_SIZE - 1u);

    for (uint32_t sector = 0; sector < install_bytes;
         sector += FLASH_SECTOR_SIZE) {
        ota_diag_set(OTA_DIAG_INSTALL_ERASE, sector);
        watchdog_hw->load = OTA_WATCHDOG_LOAD_US;
        int install_rc = ota_rom_flash_op(OTA_FLASH_ERASE, sector, NULL);
        if (install_rc != BOOTROM_OK) {
            watchdog_hw->scratch[3] = (uint32_t)install_rc;
            return install_rc;
        }
        for (uint32_t page = 0; page < FLASH_SECTOR_SIZE;
             page += FLASH_PAGE_SIZE) {
            const uint8_t* src =
                (const uint8_t*)(XIP_BASE + OTA_STAGE_IMAGE_OFS
                                 + sector + page);
            for (uint32_t i = 0; i < FLASH_PAGE_SIZE; i++) {
                install_page[i] = src[i];
            }
            ota_diag_set(OTA_DIAG_INSTALL_PROGRAM, sector + page);
            watchdog_hw->load = OTA_WATCHDOG_LOAD_US;
            install_rc = ota_rom_flash_op(OTA_FLASH_PROGRAM,
                                          sector + page, install_page);
            if (install_rc != BOOTROM_OK) {
                watchdog_hw->scratch[3] = (uint32_t)install_rc;
                return install_rc;
            }
        }
    }

    ota_diag_set(OTA_DIAG_INSTALL_DONE, header.image_bytes);
    watchdog_hw->scratch[3] = BOOTROM_OK;
    return BOOTROM_OK;
}

static void __attribute__((noreturn, noinline,
                           section(".time_critical.thermal_ota_install_wait")))
ota_request_core0_install(void)
{
    (void)save_and_disable_interrupts();
    core0_flash_op.offset = 0;
    core0_flash_op.data = NULL;
    core0_flash_op.kind = OTA_FLASH_INSTALL;
    ota_diag_set(OTA_DIAG_INSTALL_REQUEST, 0);
    __dmb();
    g_thermal_ota_core0_flash_state = OTA_CORE0_FLASH_REQUEST;
    __dmb();
    __sev();
    while (g_thermal_ota_core0_flash_state != OTA_CORE0_FLASH_DONE) __wfe();

    /* Core1 acquired this lock in thermal_ota_task(), so Core1 must release
     * it. Calling ROM reboot on Core0 while Core1 retained the lock deadlocked
     * the previous implementation with USB still enumerated but no HTTP. */
    const int install_rc = core0_flash_result;
    bootrom_release_lock(BOOTROM_LOCK_FLASH_OP);
    __dmb();
    watchdog_hw->scratch[3] = (uint32_t)install_rc;
    ota_reboot_now(REBOOT2_FLAG_REBOOT_TYPE_FLASH_UPDATE, XIP_BASE);
}

void thermal_ota_schedule_install(uint32_t delay_ms)
{
    ota.install_pending = true;
    ota.install_at_ms = to_ms_since_boot(get_absolute_time()) + delay_ms;
}

void thermal_ota_task(void)
{
    if (ota.watchdog_armed) watchdog_update();
    if (ota.install_pending
        && (int32_t)(to_ms_since_boot(get_absolute_time())
                     - ota.install_at_ms) >= 0) {
        if (!bootrom_try_acquire_lock(BOOTROM_LOCK_FLASH_OP)) return;
        ota_request_core0_install();
    }
}
