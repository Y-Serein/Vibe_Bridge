#include <hardware/gpio.h>
#include <pico/time.h>
#include <sdk.h>
#include <stdio.h>

const uint8_t* flash_target_contents =
    (const uint8_t*)(XIP_BASE + FLASH_TARGET_OFFSET);

void errh(const char* msg)
{
    if (msg != nullptr) {
        eprintln("Err(%s)", msg);
    }

    gpio_deinit(IO_LED);
    gpio_init(IO_LED);
    gpio_set_dir(IO_LED, true);
    while (true) {
        gpio_put(IO_LED, !gpio_get(IO_LED));
        sleep_ms(500);
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
