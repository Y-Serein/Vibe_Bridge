#include <hardware/flash.h>
#include <hardware/gpio.h>
#include <hardware/sync.h>
#include <hardware/watchdog.h>
#include <pico/time.h>
#include <pico/unique_id.h>
#include <sdk.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "led.h"

const uint8_t* flash_target_contents =
    (const uint8_t*)(XIP_BASE + FLASH_TARGET_OFFSET);

const struct FlashDataInfo* flash_header =
    (const struct FlashDataInfo*)(XIP_BASE + FLASH_TARGET_OFFSET
                                  + FLASH_OFS_INFO);

const uint16_t* flash_imgbg =
    (const uint16_t*)(XIP_BASE + FLASH_TARGET_OFFSET + FLASH_OFS_IMGBG);

const float* flash_gain =
    (const float*)(XIP_BASE + FLASH_TARGET_OFFSET + FLASH_OFS_GAIN);

const struct BadPt* flash_badpt =
    (const struct BadPt*)(XIP_BASE + FLASH_TARGET_OFFSET + FLASH_OFS_BADPTS);

enum DataOutMode dataomode = DATA_OUT_MODE_DEF;

struct Button buttons[BUTTON_NUM_MAX];

#if MAIN_FUNC_MODE == MAIN_FUNC_MODE_BOARD_ID
uint8_t uid[8];
#endif

char uid[UID_LEN];

void errh(const char* msg)
{
    if (msg != nullptr) {
        eprintln("Err(%s)", msg);
    }

    led_deinit();
    led_init();

    led_on_failed(UINT8_MAX);
    watchdog_reboot(0, 0, 0);
    while (true) {
        tight_loop_contents();
    }
}

void dump_buf_8bytes(const uint8_t* buf, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        if ((i & 0x7) == 0) {
            if (i != 0)
                println("");
            print("[%04zu]: ", i);
        }

        print("%02x ", buf[i]);
    }

    println("");
}

void gpio_irq_handler(uint gpio, uint32_t events)
{
    for (int i = 0; i < BUTTON_NUM_MAX; i++) {
        struct Button* btn = &buttons[i];
        if (btn->pin != gpio || btn->event) {
            continue;
        }

        uint32_t now = to_ms_since_boot(get_absolute_time());
        if ((now - btn->last_tick) < DEBOUNCE) {
            return;
        }
        btn->last_tick = now;

        bool level = gpio_get(btn->pin);
        if (!level && btn->state) {
            btn->state = 0;
            btn->event = true;
        } else {
            btn->state = level;
        }
    }
}

void button_init()
{
    buttons[0].pin = 0;
    buttons[0].state = 1;
    buttons[0].last_tick = 0;
    buttons[0].event = false;

    for (int i = 0; i < BUTTON_NUM_MAX; ++i) {
        struct Button* btn = &buttons[i];
        gpio_init(btn->pin);
        gpio_set_dir(btn->pin, GPIO_IN);
        gpio_pull_up(btn->pin);
        gpio_set_irq_enabled_with_callback(
            btn->pin, GPIO_IRQ_EDGE_FALL, true, gpio_irq_handler);
    }
}

void button_scan(struct GpioBtn* btn)
{
    uint8_t level = gpio_get(btn->gpio);
#if LED_BTN_SYNC_MODE == 1
    if (btn->gpio == IO_BTN) {
        gpio_put(IO_LED, level);
        gpio_put(IO_PLED, level);
    }
#endif
    uint32_t now = to_ms_since_boot(get_absolute_time());

    if (level != btn->last_level) {
        btn->last_level = level;
        btn->last_change_ms = now;
        return;
    }

    if ((now - btn->last_change_ms) < DEBOUNCE) {
        return;
    }

    if (level != btn->stable_level) {
        btn->stable_level = level;

        if (!level) {
            btn->press_ms = now;
            btn->event |= GB_EV_PRESS;
        } else {
            btn->event |= GB_EV_RELEASE;
        }
    }

    if (!btn->stable_level && !(btn->event & GB_EV_LONG)
        && (now - btn->press_ms >= GB_LONG_PRESS)) {
        btn->event |= GB_EV_LONG;
    }
}

#if MAIN_FUNC_MODE == MAIN_FUNC_MODE_BOARD_ID
void flash_read_unique_id()
{
    const uint8_t tx[5] = {0x4B, 0x00, 0x00, 0x00, 0x00};
    memset(uid, 0x00, sizeof(uid));

    uint32_t irq_state = save_and_disable_interrupts();
    flash_do_cmd(tx, uid, sizeof(tx));
    restore_interrupts(irq_state);
}
#endif
