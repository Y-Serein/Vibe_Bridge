#ifndef THERMAL_OTA_H
#define THERMAL_OTA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Core0 polls this word from its SRAM-resident capture loop. Core1 enters an
 * SRAM wait loop before publishing a request, so neither core can fetch XIP
 * while Core0 owns the flash. This deliberately avoids the multicore FIFO,
 * which is already part of the sensor transport protocol. */
#define THERMAL_OTA_CORE0_FLASH_REQUEST 1u
extern volatile uint32_t g_thermal_ota_core0_flash_state;
void thermal_ota_init(void);
void thermal_ota_core0_flash_service(void);
int thermal_ota_boot_probe(void);

enum thermal_ota_result {
    THERMAL_OTA_OK = 0,
    THERMAL_OTA_BUSY,
    THERMAL_OTA_BAD_SIZE,
    THERMAL_OTA_FLASH_ERROR,
    THERMAL_OTA_INCOMPLETE,
    THERMAL_OTA_VERIFY_ERROR,
    THERMAL_OTA_BAD_IMAGE,
};

enum thermal_ota_result thermal_ota_begin(uint32_t image_bytes);
enum thermal_ota_result thermal_ota_write(const uint8_t* data, size_t bytes);
enum thermal_ota_result thermal_ota_finish(uint32_t* crc32_out);
void thermal_ota_abort(void);

bool thermal_ota_active(void);
bool thermal_ota_install_pending(void);
uint32_t thermal_ota_expected_bytes(void);
uint32_t thermal_ota_received_bytes(void);
uint32_t thermal_ota_max_image_bytes(void);
const char* thermal_ota_result_string(enum thermal_ota_result result);
const char* thermal_ota_build_id(void);
uint32_t thermal_ota_calibration_crc32(void);
bool thermal_ota_last_watchdog_reset(void);
uint32_t thermal_ota_diag_stage(void);
uint32_t thermal_ota_diag_offset(void);
int32_t thermal_ota_diag_result(void);

/* Internal persistent-data helpers. Offsets are relative to the calibration
 * window and are intentionally restricted to that window. They use the same
 * Core0 SRAM flash rendezvous as OTA. */
bool thermal_ota_calibration_flash_erase(uint32_t relative_offset);
bool thermal_ota_calibration_flash_program(uint32_t relative_offset,
                                           const uint8_t* page);
/* Small post-calibration model records use the same Core0 SRAM flash
 * rendezvous as V1 calibration, but are allowed only when neither OTA nor a
 * V1 transaction is active. */
bool thermal_ota_model_flash_erase(uint32_t relative_offset);
bool thermal_ota_model_flash_program(uint32_t relative_offset,
                                     const uint8_t* page);
uint32_t thermal_flash_crc32_begin(void);
uint32_t thermal_flash_crc32_update(uint32_t crc, const uint8_t* data,
                                    size_t bytes);
uint32_t thermal_flash_crc32_finish(uint32_t crc);
uint32_t thermal_flash_crc32(const uint8_t* data, size_t bytes);

/* One flash page in the final dedicated 4 KiB sector. Intended for small
 * product settings, never sensor calibration data. */
bool thermal_settings_read(void* data, size_t bytes);
/* Read the former 0x0ff000 settings location during the one-time layout
 * migration. New settings must never be written there. */
bool thermal_settings_read_legacy(void* data, size_t bytes);
bool thermal_settings_write(const void* data, size_t bytes);
bool thermal_settings_write_during_calibration(const void* data,
                                               size_t bytes);

void thermal_ota_schedule_install(uint32_t delay_ms);
void thermal_ota_task(void);

#endif
