#include <hardware/gpio.h>
#include <pico/time.h>
#include <sdk.h>
#include <stdint.h>

void led_init()
{
    gpio_init(IO_LED);
    gpio_set_dir(IO_LED, true);
    gpio_put(IO_LED, LED_STATE_DEF);

#if LED_BOTH == 1
    gpio_init(IO_PLED);
    gpio_set_dir(IO_PLED, true);
    gpio_put(IO_PLED, LED_STATE_DEF);
#endif
}

void led_set(const bool state)
{
    // const bool val = (state == 0) ? false : true;

    gpio_put(IO_LED, state);

#if LED_BOTH == 1
    gpio_put(IO_PLED, state);
#endif
}

uint8_t led_get() { return gpio_get(IO_LED); }

void led_on() { led_set(LED_ON_VAL); }

void led_off() { led_set(LED_OFF_VAL); }

void led_toggle() { led_set(!led_get()); }

void led_deinit()
{
    gpio_deinit(IO_LED);

#if LED_BOTH == 1
    gpio_deinit(IO_PLED);
#endif
}

void led_on_event_blocking(const uint8_t cnt, const uint32_t inv)
{
    for (uint16_t i = 0; i < cnt * 2; ++i) {
        led_toggle();
        sleep_ms(inv);
    }
}

void led_on_failed(const uint8_t cnt)
{
    led_on_event_blocking(cnt, LED_FAILED_INV);
}

void led_on_succ(const uint8_t cnt)
{
    led_on_event_blocking(cnt, LED_SUCC_INV);
}
