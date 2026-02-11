#include <dutx.h>
#include <hardware/clocks.h>
#include <hardware/dma.h>
#include <pico/stdio/driver.h>
#include <pico/time.h>
#include <sdk.h>

static PIO dutx_pio;
static uint dutx_sm;
static uint dutx_ofs;
static int dutx_dma_chan;
static dma_channel_config dutx_dma_cfg;

static void dutx_out_chars(const char* buf, int len)
{
#if THDEBUG == 1
    dma_channel_wait_for_finish_blocking(dutx_dma_chan);
    dma_channel_configure(
        dutx_dma_chan, &dutx_dma_cfg, &dutx_pio->txf[dutx_sm], buf, len, true);
    dma_channel_wait_for_finish_blocking(dutx_dma_chan);
#endif
}

static stdio_driver_t dutx_stdio = {
    .out_chars = dutx_out_chars,
};

void dutx_init()
{
    dutx_pio = pio0;
    dutx_sm = 0;
    dutx_ofs = pio_add_program(dutx_pio, &dutx_program);

    int pin_tx = IO_DUTX;
    pio_sm_set_pins_with_mask64(
        dutx_pio, dutx_sm, 1ull << pin_tx, 1ull << pin_tx);
    pio_sm_set_pindirs_with_mask64(
        dutx_pio, dutx_sm, 1ull << pin_tx, 1ull << pin_tx);
    pio_gpio_init(dutx_pio, pin_tx);
    pio_sm_config c = dutx_program_get_default_config(dutx_ofs);
    sm_config_set_out_shift(&c, true, false, 32);
    sm_config_set_out_pins(&c, pin_tx, 1);
    sm_config_set_sideset_pins(&c, pin_tx);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    float div = (float)clock_get_hz(clk_sys) / (8 * 921600);
    sm_config_set_clkdiv(&c, div);
    pio_sm_init(dutx_pio, dutx_sm, dutx_ofs, &c);
    pio_sm_set_enabled(dutx_pio, dutx_sm, true);
    stdio_set_driver_enabled(&dutx_stdio, true);
    tight_loop_contents();
    sleep_ms(10);
    dutx_dma_chan = dma_claim_unused_channel(true);
    dutx_dma_cfg = dma_channel_get_default_config(dutx_dma_chan);
    channel_config_set_transfer_data_size(&dutx_dma_cfg, DMA_SIZE_8);
    channel_config_set_read_increment(&dutx_dma_cfg, true);
    channel_config_set_write_increment(&dutx_dma_cfg, false);
    channel_config_set_dreq(&dutx_dma_cfg,
                            pio_get_dreq(dutx_pio, dutx_sm, true));
    tight_loop_contents();
    sleep_ms(10);
}
