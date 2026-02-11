#include <hardware/clocks.h>
#include <hardware/dma.h>
#include <hardware/gpio.h>
#include <hardware/irq.h>
#include <hardware/pio.h>
#include <hardware/regs/intctrl.h>
#include <hardware/spi.h>
#include <hardware/structs/clocks.h>
#include <hardware/structs/dma.h>
#include <hardware/structs/io_bank0.h>
#include <hardware/uart.h>
#include <init.h>
#include <pico/assert.h>
#include <pico/platform/common.h>
#include <pico/stdlib.h>
#include <pico/time.h>
#include <pio0_io.h>
#include <piospi0.h>
#include <rei.h>
#include <sdk.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

static inline void clk_fs_set(bool fs, bool clk)
{
    uint32_t v = 0;
    if (fs)
        v |= (1u << 13);
    if (clk)
        v |= (1u << 11);

    gpio_put_masked((1u << 11) | (1u << 13), v);
    tight_loop_contents();
}

static inline void reg_send_bit(bool bit)
{
    gpio_put(IO_OSDA, bit);
    tight_loop_contents();

    for (int i = 0; i < 32; i++) {
        clk_fs_set(true, false);
        sleep_us(100);
        clk_fs_set(true, true);
    }
}

static void reg_send_320bits_rst()
{
    // 4302727C252E02154018191C82060413917F8200F800000FFFFFFFE6AFEB702C851649000000002F
    static const uint32_t reg_data[10] = {
        0x4302727C,
        0x252E0215,
        0x4018191C,
        0x82060413,
        0x917F8200,
        0xF800000F,
        0xFFFFFFE6,
        0xAFEB702C,
        0x85164900,
        0x0000002F,
    };
    clk_fs_set(true, true);
    for (int i = 0; i < 50; i++) {
        clk_fs_set(true, false);
        sleep_us(100);
        clk_fs_set(true, true);
    }

    for (size_t w = 0; w < 10; w++) {
        uint32_t v = reg_data[w];
        for (int b = 31; b >= 0; b--) {
            reg_send_bit((v >> b) & 1u);
        }
    }

    clk_fs_set(false, false);
    gpio_put(IO_OSDA, 0);
    tight_loop_contents();
}

static void reg_send_320bits_rst2()
{
    // 4302727C252E02154018191882060413917F8200F800000FFFFFFFE6AFEB702C851649000000002F
    static const uint32_t reg_data[10] = {
        0x4302727C,
        0x252E0215,
        0x40181918,
        0x82060413,
        0x917F8200,
        0xF800000F,
        0xFFFFFFE6,
        0xAFEB702C,
        0x85164900,
        0x0000002F,
    };
    clk_fs_set(true, true);
    for (int i = 0; i < 50; i++) {
        clk_fs_set(true, false);
        sleep_us(100);
        clk_fs_set(true, true);
    }

    for (size_t w = 0; w < 10; w++) {
        uint32_t v = reg_data[w];
        for (int b = 31; b >= 0; b--) {
            reg_send_bit((v >> b) & 1u);
        }
    }

    clk_fs_set(false, false);
    gpio_put(IO_OSDA, 0);
    tight_loop_contents();
}

static void reg_send_320bits_init()
{
    // 4302727C252E02154018191C8206041F917F8200F800000FFFFFFFE6AFEB702C851649000000002F
    static const uint32_t reg_data[10] = {
        0x4302727C,
        0x252E0215,
        0x4018191C,
        0x8206041F,
        0x917F8200,
        0xF800000F,
        0xFFFFFFE6,
        0xAFEB702C,
        0x85164900,
        0x0000002F,
    };
    clk_fs_set(true, true);
    for (int i = 0; i < 50; i++) {
        clk_fs_set(true, false);
        sleep_us(100);
        clk_fs_set(true, true);
    }

    for (size_t w = 0; w < 10; w++) {
        uint32_t v = reg_data[w];
        for (int b = 31; b >= 0; b--) {
            reg_send_bit((v >> b) & 1u);
        }
    }

    clk_fs_set(false, false);
    gpio_put(IO_OSDA, 0);
    tight_loop_contents();
}

volatile uint32_t u32bits = 0;
volatile int8_t u32bits_idx = 31;
uint64_t sof = 0;
uint64_t sol = 0;
uint64_t eof = 0;
uint64_t eol = 0;
uint64_t irq = 0;
uint64_t spifreq = 0;
PIO upio0;
uint usm0;
uint uofs0;

static void gpio_cb(uint n, uint32_t e)
{
    (void)n;
    irq++;
    if (!(e & GPIO_IRQ_EDGE_FALL)) {
        return;
    }

    bool val = gpio_get(IO_ISDA);

    u32bits &= ~(1UL << u32bits_idx);
    u32bits |= ((uint32_t)val << u32bits_idx);

    if (u32bits_idx == 0) {
        switch (u32bits) {
        case 0xFF0000EC:
            sof += 1;
            break;
        case 0xFF0000DA:
            eol += 1;
            break;
        case 0xFF0000C7:
            sol += 1;
            break;
        case 0xFF0000F1:
            eof += 1;
            break;
        default:
            break;
        }

        u32bits_idx = 31;
        u32bits = 0;
    } else {
        u32bits_idx--;
    }
}

static inline __attribute__((always_inline)) void pre_init()
{
    stdio_init_all();
    gpio_init(IO_LED);
    gpio_set_dir(IO_LED, 1);
    gpio_put(IO_LED, 1);
    tight_loop_contents();

    // uart init
    uart_deinit(uart0);
    uart_deinit(uart1);
    gpio_set_function(IO_UTX, 2);
    gpio_set_function(IO_URX, 2);
    uart_init(uart0, 3000000);
    uart_set_hw_flow(uart0, false, false);
    uart_set_format(uart0, 8, 1, UART_PARITY_NONE);
    tight_loop_contents();
}

void init_wspi()
{
    extern const uint8_t reg_rstdata[];
    extern const uint8_t reg_initdata[];

    pre_init();
    // CTL
    gpio_init(IO_EN);
    gpio_set_dir(IO_EN, 1);
    gpio_put(IO_EN, 0);
    // RSTB
    gpio_init(IO_RSTB);
    gpio_set_dir(IO_RSTB, 1);
    gpio_put(IO_RSTB, 1);

    // ISDA
    gpio_init(IO_ISDA);
    gpio_set_dir(IO_ISDA, 0);
    gpio_init(IO_ICLK);
    // ICLK
    gpio_set_dir(IO_ICLK, 0);
    gpio_pull_up(IO_ICLK);
    gpio_set_irq_enabled_with_callback(
        IO_ICLK, GPIO_IRQ_EDGE_FALL, true, &gpio_cb);

    // OSDA & OCLK SPI
    gpio_set_function(IO_OSDA, 1);
    gpio_set_function(IO_OCLK, 1);
    spifreq = (uint64_t)spi_init(spi1, 25000000);
    // OFS
    gpio_init(IO_OFS);
    gpio_set_dir(IO_OFS, 1);
    gpio_put(IO_OFS, 0);

    // IO value init
    sleep_ms(100);
    {
        uint mask = 0;
        uint value = 0;
        // CTL = 0
        mask |= (1u << IO_EN);
        // RSTB = 1
        mask |= (1u << IO_RSTB);
        value |= (1u << IO_RSTB);
        // OFS = 0
        mask |= (1u << IO_OFS);
        gpio_put_masked(mask, value);
        tight_loop_contents();
        sleep_ms(10);
        // RSTB = 0
        gpio_put(IO_RSTB, 0);
        tight_loop_contents();
        sleep_ms(10);
        // RSTB = 1
        gpio_put(IO_RSTB, 1);
        tight_loop_contents();
        sleep_ms(60);
    }

    gpio_put(IO_OFS, 1);
    tight_loop_contents();
    spi_write_blocking(spi1, reg_rstdata, REG_DATE_SPI_LEN);
    gpio_put(IO_OFS, 0);
    tight_loop_contents();

    sleep_ms(60);
    gpio_put(IO_OFS, 1);
    tight_loop_contents();
    spi_write_blocking(spi1, reg_initdata, REG_DATE_SPI_LEN);
    gpio_put(IO_OFS, 0);
    tight_loop_contents();

    gpio_put(IO_OFS, 1);

    while (1) {
        spi_write_blocking(spi1, reg_initdata, REG_DATE_SPI_LEN);
        tight_loop_contents();
    }
}

void init_wio(int rst)
{
    pre_init();
    // CTL
    gpio_init(IO_EN);
    gpio_set_dir(IO_EN, 1);
    gpio_put(IO_EN, 0);
    // RSTB
    gpio_init(IO_RSTB);
    gpio_set_dir(IO_RSTB, 1);
    gpio_put(IO_RSTB, 1);

    // ISDA
    gpio_init(IO_ISDA);
    gpio_set_dir(IO_ISDA, 0);
    gpio_init(IO_ICLK);
    // ICLK
    gpio_set_dir(IO_ICLK, 0);
    gpio_pull_up(IO_ICLK);
    gpio_set_irq_enabled_with_callback(
        IO_ICLK, GPIO_IRQ_EDGE_FALL, true, &gpio_cb);

    // OSDA
    gpio_init(IO_OSDA);
    gpio_set_dir(IO_OSDA, 1);
    gpio_put(IO_OSDA, 0);
    // OCLK
    gpio_init(IO_OCLK);
    gpio_set_dir(IO_OCLK, 1);
    gpio_put(IO_OCLK, 0);
    // OFS
    gpio_init(IO_OFS);
    gpio_set_dir(IO_OFS, 1);
    gpio_put(IO_OFS, 0);

    // IO value init
    sleep_ms(100);
    {
        uint mask = 0;
        uint value = 0;
        // CTL = 0
        mask |= (1u << IO_EN);
        // RSTB = 1
        mask |= (1u << IO_RSTB);
        value |= (1u << IO_RSTB);
        // OFS = 0
        mask |= (1u << IO_OFS);
        // OSDA = 0
        mask |= (1u << IO_OSDA);
        // OCLK = 0
        mask |= (1u << IO_OCLK);
        gpio_put_masked(mask, value);
        tight_loop_contents();
        sleep_ms(10);
        // RSTB = 0
        gpio_put(IO_RSTB, 0);
        tight_loop_contents();
        sleep_ms(10);
        // RSTB = 1
        gpio_put(IO_RSTB, 1);
        tight_loop_contents();
        sleep_ms(60);
    }

    if (rst != 2) {
        reg_send_320bits_rst();
    } else {
        reg_send_320bits_rst2();
    }

    sleep_ms(60);
    reg_send_320bits_init();

    gpio_put(IO_OFS, 1);
}

void init_wpio()
{
    stdio_init_all();
    gpio_init(IO_LED);
    gpio_set_dir(IO_LED, 1);
    gpio_put(IO_LED, 1);
    tight_loop_contents();

    uart_deinit(uart0);
    uart_deinit(uart1);

    uint pin = IO_OCLK;
    hard_assert(pio_claim_free_sm_and_add_program_for_gpio_range(
        &pio0io_program, &upio0, &usm0, &uofs0, pin, 1, true));
    pio_sm_set_pins_with_mask64(upio0, usm0, 1ull << pin, 1ull << pin);
    pio_sm_set_pindirs_with_mask64(upio0, usm0, 1ull << pin, 1ull << pin);
    gpio_disable_pulls(pin);
    gpio_set_drive_strength(pin, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_slew_rate(pin, GPIO_SLEW_RATE_FAST);
    pio_sm_config c = pio0io_program_get_default_config(uofs0);
    sm_config_set_set_pins(&c, pin, 1);
    sm_config_set_out_pins(&c, pin, 1);
    sm_config_set_clkdiv(&c, 1.0f);
    pio_gpio_init(upio0, pin);
    pio_sm_init(upio0, usm0, uofs0, &c);
    pio_sm_set_enabled(upio0, usm0, true);
}

static inline __attribute__((always_inline)) void piospi0_program_init(
    uint data_pin, uint clk_pin, uint fs_pin, float clk_div)
{
    // gpio_init(data_pin);
    // gpio_set_dir(data_pin, true);
    // gpio_put(data_pin, 0);
    // gpio_init(clk_pin);
    // gpio_set_dir(clk_pin, true);
    // gpio_put(clk_pin, 0);
    // gpio_init(fs_pin);
    // gpio_set_dir(fs_pin, true);
    // gpio_put(fs_pin, 0);
    // tight_loop_contents();

    upio0 = pio0;
    usm0 = 0;
    uofs0 = pio_add_program(upio0, &piospi0_program);

    pio_gpio_init(upio0, data_pin);
    pio_gpio_init(upio0, clk_pin);
    pio_gpio_init(upio0, fs_pin);
    gpio_disable_pulls(data_pin);
    gpio_disable_pulls(clk_pin);
    gpio_disable_pulls(fs_pin);
    gpio_set_drive_strength(data_pin, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_drive_strength(clk_pin, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_drive_strength(fs_pin, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_slew_rate(data_pin, GPIO_SLEW_RATE_FAST);
    gpio_set_slew_rate(clk_pin, GPIO_SLEW_RATE_FAST);
    gpio_set_slew_rate(fs_pin, GPIO_SLEW_RATE_FAST);
    pio_sm_set_consecutive_pindirs(upio0, usm0, data_pin, 1, true);
    pio_sm_set_consecutive_pindirs(upio0, usm0, clk_pin, 1, true);
    pio_sm_set_consecutive_pindirs(upio0, usm0, fs_pin, 1, true);
    pio_sm_config c = piospi0_program_get_default_config(uofs0);
    sm_config_set_sideset_pins(&c, clk_pin);
    sm_config_set_out_pins(&c, data_pin, 1);
    sm_config_set_set_pins(&c, fs_pin, 1);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    sm_config_set_clkdiv(&c, clk_div);
    sm_config_set_out_shift(&c, false, false, 32);
    pio_sm_init(upio0, usm0, uofs0, &c);
    pio_sm_set_enabled(upio0, usm0, true);
}

static inline __attribute__((always_inline)) void piospi0_wait_idle(PIO pio,
                                                                    uint sm)
{
    uint32_t sm_stall_mask = 1u << (sm + PIO_FDEBUG_TXSTALL_LSB);
    pio->fdebug = sm_stall_mask;
    while (!(pio->fdebug & sm_stall_mask))
        ;
}

static inline __attribute__((always_inline)) void piospi0_put_u32(PIO pio,
                                                                  uint sm,
                                                                  uint32_t v)
{
    while (pio_sm_is_tx_fifo_full(pio, sm))
        ;
    pio->txf[sm] = v;
}

static inline __attribute__((always_inline)) void piospi0_send_u32(
    const uint32_t* data, uint32_t word_count)
{
    if (data == nullptr || word_count == 0) {
        return;
    }

    pio_sm_set_enabled(upio0, usm0, false);
    pio_sm_restart(upio0, usm0);
    pio_sm_clear_fifos(upio0, usm0);
    piospi0_put_u32(upio0, usm0, 49);
    piospi0_put_u32(upio0, usm0, word_count * 32 - 1);

    uint32_t i = 0;
    for (; i < 6 && i < word_count; i++) {
        piospi0_put_u32(upio0, usm0, data[i]);
    }

    pio_sm_set_enabled(upio0, usm0, true);

    for (; i < word_count; i++) {
        piospi0_put_u32(upio0, usm0, data[i]);
    }
}

void init_wpiospi()
{
#if 0
    extern const uint32_t reg_rstdata32[];
    extern const uint32_t reg_initdata32[];
#endif

    pre_init();
    // CTL
    gpio_init(IO_EN);
    gpio_set_dir(IO_EN, 1);
    gpio_put(IO_EN, 0);
    // RSTB
    gpio_init(IO_RSTB);
    gpio_set_dir(IO_RSTB, 1);
    gpio_put(IO_RSTB, 1);

    // ISDA
    gpio_init(IO_ISDA);
    gpio_set_dir(IO_ISDA, 0);
    // ICLK
    gpio_init(IO_ICLK);
    gpio_set_dir(IO_ICLK, 0);
    gpio_pull_up(IO_ICLK);
    gpio_set_irq_enabled_with_callback(
        IO_ICLK, GPIO_IRQ_EDGE_FALL, true, &gpio_cb);

    piospi0_program_init(IO_OSDA, IO_OCLK, IO_OFS, 1.5f);

    // IO value init
    sleep_ms(100);
    {
        uint mask = 0;
        uint value = 0;
        // CTL = 1
        mask |= (1u << IO_EN);
        value |= (1u << IO_EN);
        // RSTB = 1
        mask |= (1u << IO_RSTB);
        value |= (1u << IO_RSTB);
        gpio_put_masked(mask, value);
        tight_loop_contents();
        sleep_ms(10);
        // RSTB = 0
        gpio_put(IO_RSTB, 0);
        tight_loop_contents();
        sleep_ms(10);
        // RSTB = 1
        gpio_put(IO_RSTB, 1);
        tight_loop_contents();

        // reset delay
        sleep_ms(60);
    }

    // rst REG
    piospi0_wait_idle(upio0, usm0);
#if 0
    piospi0_send_u32(reg_rstdata32, REG_DATE_SPI_LEN32);
#endif
    piospi0_wait_idle(upio0, usm0);

    // reset delay
    pio_remove_program(upio0, &piospi0_program, uofs0);
    sleep_ms(60);
    piospi0_program_init(IO_OSDA, IO_OCLK, IO_OFS, 1.5f);
    // init REG
    piospi0_wait_idle(upio0, usm0);
#if 0
    piospi0_send_u32(reg_initdata32, REG_DATE_SPI_LEN32);
#endif
    piospi0_wait_idle(upio0, usm0);

    // test
    // while (true) {
    //     piospi0_send_u32(reg_initdata32, REG_DATE_SPI_LEN32);
    //     piospi0_wait_idle(upio0, usm0);
    // }

    // release OFS
    pio_remove_program(upio0, &piospi0_program, uofs0);

    // OFS
    gpio_init(IO_OFS);
    gpio_set_dir(IO_OFS, 1);
    // OFS = 0
    gpio_put(IO_OFS, 0);
}

void th160_init()
{
#define FPS          12
#define W            160
#define H            120
#define BSIZE        ((W * H) + 1)
#define BUF_AT(y, x) buffer[1 + (y)*W + (x)]

#define FPS_PRINT_INTERVAL_MS 2000

    pre_init();
    uint8_t buffer[BSIZE];
    buffer[0] = 0xFF;

    for (uint8_t y = 0; y < H / 2; ++y) {
        for (uint8_t x = 0; x < W; ++x) {
            BUF_AT(y, x) = x;
        }
    }
    for (uint8_t y = H / 2; y < H; ++y) {
        for (uint8_t x = 0; x < W; ++x) {
            BUF_AT(y, x) = W - x;
        }
    }

    const uint32_t PERIOD = 1000 / FPS;
    uint64_t frame_cnt = 0;
    uint64_t uart_total_us = 0;
    absolute_time_t next_time = make_timeout_time_ms(PERIOD);
    absolute_time_t fps_t0 = get_absolute_time();

    while (true) {
        absolute_time_t t0 = get_absolute_time();
        uart_write_blocking(uart0, buffer, BSIZE);
        // (void)buffer;
        // const char* test_str = "Hello from rp2350\n";
        // uart_write_blocking(uart0, (const uint8_t*)test_str,
        // strlen(test_str));
        absolute_time_t t1 = get_absolute_time();
        uart_total_us += absolute_time_diff_us(t0, t1);

        frame_cnt++;
        absolute_time_t now = get_absolute_time();
        int64_t dt_us = absolute_time_diff_us(fps_t0, now);
        if (dt_us >= FPS_PRINT_INTERVAL_MS * 1000LL) {
            float fps = (frame_cnt * 1000000.0f) / (float)dt_us;
            float avg_uart_us =
                frame_cnt ? (float)uart_total_us / frame_cnt : 0;
            printf("FPS: %.2f, uart avg: %.2f us\n", fps, avg_uart_us);

            frame_cnt = 0;
            uart_total_us = 0;
            fps_t0 = now;
        }

        sleep_until(next_time);
        next_time = delayed_by_ms(next_time, PERIOD);
    }

    // uint8_t buffer[32];
    // int cnt = 0;
    //
    // while (true) {
    //     memset(buffer, 0x00, 32);
    //     int ret = snprintf((char*)buffer,
    //                        32,
    //                        "#%d %llu %llu\n",
    //                        cnt,
    //                        (uint64_t)clock_get_hz(clk_sys),
    //                        (uint64_t)clock_get_hz(clk_peri));
    //     size_t len = (ret >= 32) ? 31 : ret;
    //     buffer[len] = '\0';
    //     uart_write_blocking(uart0, buffer, len);
    //
    //     extern uint64_t irq;
    //     extern uint64_t sof;
    //     extern uint64_t sol;
    //     extern uint64_t eof;
    //     extern uint64_t eol;
    //
    //     printf("[%llu] sof:%llu sol:%llu eof:%llu eol:%llu",
    //            irq,
    //            sof,
    //            sol,
    //            eof,
    //            eol);
    //
    //     printf("[%llu]<%d> %s\n", time_us_64(), len, buffer);
    //     sleep_ms(1000);
    //     cnt++;
    // }

#ifdef FPS
#    undef FPS
#endif
#ifdef W
#    undef W
#endif
#ifdef H
#    undef H
#endif
#ifdef BSIZE
#    undef BSIZE
#endif
#ifdef BUF_AT
#    undef BUF_AT
#endif
#ifdef FPS_PRINT_INTERVAL_MS
#    undef FPS_PRINT_INTERVAL_MS
#endif
}

uint spi1clk;
struct RxRing rxring;
PIO rxpio = pio1;
uint rxsm;
uint rxofs;
bool rxfinish = false;
uint32_t dirqc = 0;
uint32_t pirqc = 0;
struct RxDma rxchan;
int sdch;
dma_channel_config clkdcfg;
const uint8_t Z = 0x00;
static volatile bool dma_done = false;

static void __isr dma_irq0_handler(void)
{
    uint32_t ch = rxchan.chan;

    if (dma_hw->ints0 & (1u << ch)) {
        dma_hw->ints0 = 1u << ch;
        dma_done = true;
    }
}

void rx_poll(void)
{
    if (!dma_done)
        return;

    dma_done = false;
    // pio_sm_set_enabled(rxpio, rxsm, false);
    // while (!pio_sm_is_rx_fifo_empty(rxpio, rxsm)) {
    //     (void)pio_sm_get(rxpio, rxsm);
    // }

#if RXPKTDBG == 1
    dma_channel_abort(sdch);
#endif

    rxring.buf[rxring.widx].len = BUFSIZE;
    uint32_t next = (rxring.widx + 1) % BUFCOUNT;
    if (next == rxring.ridx) {
        rxring.ridx = (rxring.ridx + 1) % BUFCOUNT;
    }
    rxring.widx = next;
    rxchan.buf_idx = rxring.widx;
    dma_channel_set_write_addr(
        rxchan.chan, rxring.buf[rxchan.buf_idx].data, false);
    dma_channel_set_trans_count(rxchan.chan, BUFSIZE, true);

    // pio_sm_set_enabled(rxpio, rxsm, true);
}

void wspi_init0()
{
    for (int i = 0; i < BUFCOUNT; ++i) {
        memset(rxring.buf[i].data, 0x00, BUFSIZE + BUFOF);
        rxring.buf[i].len = 0;
    }
    rxring.widx = 0;
    rxring.ridx = 0;
    dma_done = false;

    extern const uint8_t reg_initdata[];

    uart_deinit(OUART);
    gpio_set_function(IO_OTX, GPIO_FUNC_UART);
    gpio_set_function(IO_ORX, GPIO_FUNC_UART);
    uart_init(OUART, 5000000);
    uart_set_hw_flow(OUART, false, false);
    uart_set_format(OUART, 8, 1, UART_PARITY_NONE);

    // CTL = 0
    gpio_init(IO_SCTL);
    gpio_set_dir(IO_SCTL, true);
    gpio_put(IO_SCTL, 0);
    // RSTN = 0
    gpio_init(IO_SRST);
    gpio_set_dir(IO_SRST, true);
    gpio_put(IO_SRST, 0);
    // CS = 0
    gpio_init(IO_SCS);
    gpio_set_dir(IO_SCS, true);
    gpio_put(IO_SCS, 0);
    // SPI1
    spi1clk = spi_init(spi1, spi1clk);
    spi_set_format(spi1, 8, SPI_CPOL_1, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(IO_SSDA, GPIO_FUNC_SPI);  // MOSI
    gpio_set_function(IO_SCLK, GPIO_FUNC_SPI);  // SCLK
    gpio_disable_pulls(IO_SCLK);
    gpio_set_drive_strength(IO_SCLK, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_slew_rate(IO_SCLK, GPIO_SLEW_RATE_FAST);
    gpio_disable_pulls(IO_SSDA);
    gpio_set_drive_strength(IO_SSDA, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_slew_rate(IO_SSDA, GPIO_SLEW_RATE_FAST);
    static const uint8_t CLK_A[40 / 8] = {0x00, 0x00, 0x00, 0x00, 0x00};
    static const uint8_t CLK_B[32 / 8] = {0xff, 0xff, 0xff, 0xff};
    // RSTN = 1
    gpio_put(IO_SRST, 1);
    // CLK A
    spi_write_blocking(spi1, CLK_A, 40 / 8);
    // CS = 1
    gpio_put(IO_SCS, 1);
    // INIT
    spi_write_blocking(spi1, reg_initdata, 1286 - 32 / 8);
    // CS = 0
    gpio_put(IO_SCS, 0);
    for (int i = 0; i < 5; ++i) {
        spi_write_blocking(spi1, CLK_B, 32 / 8);
    }

    (void)spi_get_hw(spi1)->sr;
    while (!spi_is_writable(spi1)) {
        tight_loop_contents();
    }

    pio_gpio_init(rxpio, IO_PSDA);
    pio_gpio_init(rxpio, IO_PCLK);
    rxofs = pio_add_program(rxpio, &rei_program);
    rxsm = pio_claim_unused_sm(rxpio, true);
    pio_sm_config rxpioc = rei_program_get_default_config(rxofs);
    sm_config_set_in_pins(&rxpioc, IO_PSDA);
    sm_config_set_in_shift(&rxpioc, false, true, 8);
    pio_sm_set_consecutive_pindirs(rxpio, rxsm, IO_PSDA, 2, false);
    // pio_sm_set_consecutive_pindirs(rxpio, rxsm, IO_PCLK, 1, false);
    sm_config_set_clkdiv(&rxpioc, 1.0f);
    pio_sm_init(rxpio, rxsm, rxofs, &rxpioc);
    pio_sm_clear_fifos(rxpio, rxsm);
    pio_sm_set_enabled(rxpio, rxsm, true);
    pio_sm_put_blocking(rxpio, rxsm, BUFSIZE * 8);

    rxchan.chan = dma_claim_unused_channel(true);
    rxchan.buf_idx = rxring.widx;
    dma_channel_config dcfg0 = dma_channel_get_default_config(rxchan.chan);
    channel_config_set_transfer_data_size(&dcfg0, DMA_SIZE_8);
    channel_config_set_read_increment(&dcfg0, false);
    channel_config_set_write_increment(&dcfg0, true);
    channel_config_set_dreq(&dcfg0, pio_get_dreq(rxpio, rxsm, false));
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq0_handler);
    irq_set_enabled(DMA_IRQ_0, true);
    dma_channel_set_irq0_enabled(rxchan.chan, true);

    dma_channel_configure(rxchan.chan,
                          &dcfg0,
                          rxring.buf[rxchan.buf_idx].data,
                          &rxpio->rxf[rxsm], /* src */
                          BUFSIZE,
                          true);

    sdch = dma_claim_unused_channel(true);
    clkdcfg = dma_channel_get_default_config(sdch);
    channel_config_set_transfer_data_size(&clkdcfg, DMA_SIZE_8);
    channel_config_set_read_increment(&clkdcfg, false);
    channel_config_set_write_increment(&clkdcfg, false);
    channel_config_set_dreq(&clkdcfg, DREQ_SPI1_TX);
    dma_channel_configure(
        sdch, &clkdcfg, &spi_get_hw(spi1)->dr, &Z, 0xffffffff, true);
}

void wspi_init0_noinit()
{
    for (int i = 0; i < BUFCOUNT; ++i) {
        memset(rxring.buf[i].data, 0x00, BUFSIZE + BUFOF);
        rxring.buf[i].len = 0;
    }
    rxring.widx = 0;
    rxring.ridx = 0;
    dma_done = false;

    // extern const uint8_t reg_initdata[];
    gpio_init(IO_LED);
    gpio_set_dir(IO_LED, true);
    gpio_put(IO_LED, 0);

    // CTL = 0
    gpio_init(IO_SCTL);
    gpio_set_dir(IO_SCTL, true);
    gpio_put(IO_SCTL, 0);
    // RSTN = 0
    gpio_init(IO_SRST);
    gpio_set_dir(IO_SRST, true);
    gpio_put(IO_SRST, 0);
    // CS = 0
    gpio_init(IO_SCS);
    gpio_set_dir(IO_SCS, true);
    gpio_put(IO_SCS, 0);
    // SPI1
    spi1clk = spi_init(spi1, spi1clk);
    spi_set_format(spi1, 8, SPI_CPOL_1, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(IO_SSDA, GPIO_FUNC_SPI);  // MOSI
    gpio_set_function(IO_SCLK, GPIO_FUNC_SPI);  // SCLK
    gpio_disable_pulls(IO_SCLK);
    gpio_set_drive_strength(IO_SCLK, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_slew_rate(IO_SCLK, GPIO_SLEW_RATE_FAST);
    gpio_disable_pulls(IO_SSDA);
    gpio_set_drive_strength(IO_SSDA, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_slew_rate(IO_SSDA, GPIO_SLEW_RATE_FAST);
    // static const uint8_t CLK_A[40 / 8] = {0x00, 0x00, 0x00, 0x00, 0x00};
    // static const uint8_t CLK_B[32 / 8] = {0xff, 0xff, 0xff, 0xff};
    // // RSTN = 1
    // gpio_put(IO_SRST, 1);
    // // CLK A
    // spi_write_blocking(spi1, CLK_A, 40 / 8);
    // // CS = 1
    // gpio_put(IO_SCS, 1);
    // // INIT
    // spi_write_blocking(spi1, reg_initdata, 1286 - 32 / 8);
    // // CS = 0
    // gpio_put(IO_SCS, 0);
    // for (int i = 0; i < 5; ++i) {
    //     spi_write_blocking(spi1, CLK_B, 32 / 8);
    // }
    // (void)spi_get_hw(spi1)->sr;
    // while (!spi_is_writable(spi1)) {
    //     tight_loop_contents();
    // }

    println("spi hw ready, sleep 2s start");
    sleep_ms(2000);

    gpio_put(IO_LED, 1);
    println("sleep finish");
    println("PIO starting...");
    println("SPI CLK starting...");

    pio_gpio_init(rxpio, IO_PSDA);
    pio_gpio_init(rxpio, IO_PCLK);
    rxofs = pio_add_program(rxpio, &rei_program);
    rxsm = pio_claim_unused_sm(rxpio, true);
    pio_sm_config rxpioc = rei_program_get_default_config(rxofs);
    sm_config_set_in_pins(&rxpioc, IO_PSDA);
    sm_config_set_in_shift(&rxpioc, false, true, 8);
    pio_sm_set_consecutive_pindirs(rxpio, rxsm, IO_PSDA, 2, false);
    // pio_sm_set_consecutive_pindirs(rxpio, rxsm, IO_PCLK, 1, false);
    sm_config_set_clkdiv(&rxpioc, 1.0f);
    pio_sm_init(rxpio, rxsm, rxofs, &rxpioc);
    pio_sm_clear_fifos(rxpio, rxsm);
    pio_sm_set_enabled(rxpio, rxsm, true);
    pio_sm_put_blocking(rxpio, rxsm, BUFSIZE * 8);

    rxchan.chan = dma_claim_unused_channel(true);
    rxchan.buf_idx = rxring.widx;
    dma_channel_config dcfg0 = dma_channel_get_default_config(rxchan.chan);
    channel_config_set_transfer_data_size(&dcfg0, DMA_SIZE_8);
    channel_config_set_read_increment(&dcfg0, false);
    channel_config_set_write_increment(&dcfg0, true);
    channel_config_set_dreq(&dcfg0, pio_get_dreq(rxpio, rxsm, false));
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq0_handler);
    irq_set_enabled(DMA_IRQ_0, true);
    dma_channel_set_irq0_enabled(rxchan.chan, true);

    dma_channel_configure(rxchan.chan,
                          &dcfg0,
                          rxring.buf[rxchan.buf_idx].data,
                          &rxpio->rxf[rxsm], /* src */
                          BUFSIZE,
                          true);

    sdch = dma_claim_unused_channel(true);
    clkdcfg = dma_channel_get_default_config(sdch);
    channel_config_set_transfer_data_size(&clkdcfg, DMA_SIZE_8);
    channel_config_set_read_increment(&clkdcfg, false);
    channel_config_set_write_increment(&clkdcfg, false);
    channel_config_set_dreq(&clkdcfg, DREQ_SPI1_TX);
    dma_channel_configure(
        sdch, &clkdcfg, &spi_get_hw(spi1)->dr, &Z, 0xffffffff, true);
}

void wspi_stop1()
{
    dma_channel_abort(sdch);
    dma_channel_abort(rxchan.chan);

    pio_sm_set_enabled(rxpio, rxsm, false);
    pio_sm_clear_fifos(rxpio, rxsm);
    pio_sm_unclaim(rxpio, rxsm);
    rxsm = -1;
    pio_remove_program(rxpio, &rei_program, rxofs);
}

static const uint8_t SPI_CLK_A[40 / 8] = {0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t SPI_CLK_B[32 / 8] = {0xff, 0xff, 0xff, 0xff};
void wspi_reinit1()
{
    extern const uint8_t reg_initdata[];

    gpio_put(IO_SCTL, 0);
    // gpio_put(IO_SRST, 0);
    gpio_put(IO_SCS, 0);
    // gpio_put(IO_SRST, 1);
    spi_write_blocking(spi1, SPI_CLK_A, 40 / 8);
    gpio_put(IO_SCS, 1);
    spi_write_blocking(spi1, reg_initdata, 1286 - 32 / 8);
    gpio_put(IO_SCS, 0);
    for (int i = 0; i < 5; ++i) {
        spi_write_blocking(spi1, SPI_CLK_B, 32 / 8);
    }

    (void)spi_get_hw(spi1)->sr;
    while (!spi_is_writable(spi1)) {
        tight_loop_contents();
    }

    pio_gpio_init(rxpio, IO_PSDA);
    pio_gpio_init(rxpio, IO_PCLK);
    rxofs = pio_add_program(rxpio, &rei_program);
    rxsm = pio_claim_unused_sm(rxpio, true);
    pio_sm_config rxpioc = rei_program_get_default_config(rxofs);
    sm_config_set_in_pins(&rxpioc, IO_PSDA);
    sm_config_set_in_shift(&rxpioc, false, true, 8);
    pio_sm_set_consecutive_pindirs(rxpio, rxsm, IO_PSDA, 2, false);
    sm_config_set_clkdiv(&rxpioc, 1.0f);
    pio_sm_init(rxpio, rxsm, rxofs, &rxpioc);
    pio_sm_clear_fifos(rxpio, rxsm);
    pio_sm_set_enabled(rxpio, rxsm, true);
    pio_sm_put_blocking(rxpio, rxsm, BUFSIZE * 8);

    rxchan.buf_idx = rxring.widx;
    dma_channel_config dcfg0 = dma_channel_get_default_config(rxchan.chan);
    channel_config_set_transfer_data_size(&dcfg0, DMA_SIZE_8);
    channel_config_set_read_increment(&dcfg0, false);
    channel_config_set_write_increment(&dcfg0, true);
    channel_config_set_dreq(&dcfg0, pio_get_dreq(rxpio, rxsm, false));
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq0_handler);
    irq_set_enabled(DMA_IRQ_0, true);
    dma_channel_set_irq0_enabled(rxchan.chan, true);

    dma_channel_configure(rxchan.chan,
                          &dcfg0,
                          rxring.buf[rxchan.buf_idx].data,
                          &rxpio->rxf[rxsm], /* src */
                          BUFSIZE,
                          true);

    dma_channel_configure(
        sdch, &clkdcfg, &spi_get_hw(spi1)->dr, &Z, 0xffffffff, true);
}

void wspi_init1()
{
    for (int i = 0; i < BUFCOUNT; ++i) {
        memset(rxring.buf[i].data, 0x00, BUFSIZE + BUFOF);
        rxring.buf[i].len = 0;
    }
    rxring.widx = 0;
    rxring.ridx = 0;
    dma_done = false;

    extern const uint8_t reg_initdata[];

    // CTL = 0
    gpio_init(IO_SCTL);
    gpio_set_dir(IO_SCTL, true);
    gpio_put(IO_SCTL, 0);
    // RSTN = 0
    gpio_init(IO_SRST);
    gpio_set_dir(IO_SRST, true);
    gpio_put(IO_SRST, 0);
    // CS = 0
    gpio_init(IO_SCS);
    gpio_set_dir(IO_SCS, true);
    gpio_put(IO_SCS, 0);
    // SPI1
    spi1clk = spi_init(spi1, spi1clk);
    spi_set_format(spi1, 8, SPI_CPOL_1, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(IO_SSDA, GPIO_FUNC_SPI);  // MOSI
    gpio_set_function(IO_SCLK, GPIO_FUNC_SPI);  // SCLK
    gpio_disable_pulls(IO_SCLK);
    gpio_set_drive_strength(IO_SCLK, GPIO_DRIVE_STRENGTH_8MA);
    gpio_set_slew_rate(IO_SCLK, GPIO_SLEW_RATE_FAST);
    gpio_disable_pulls(IO_SSDA);
    gpio_set_drive_strength(IO_SSDA, GPIO_DRIVE_STRENGTH_8MA);
    gpio_set_slew_rate(IO_SSDA, GPIO_SLEW_RATE_FAST);
    // RSTN = 1
    gpio_put(IO_SRST, 1);
    // CLK A
    spi_write_blocking(spi1, SPI_CLK_A, 40 / 8);
    // CS = 1
    gpio_put(IO_SCS, 1);
    // INIT
    spi_write_blocking(spi1, reg_initdata, 1286 - 32 / 8);
    // CS = 0
    gpio_put(IO_SCS, 0);
    for (int i = 0; i < 5; ++i) {
        spi_write_blocking(spi1, SPI_CLK_B, 32 / 8);
    }

    (void)spi_get_hw(spi1)->sr;
    while (!spi_is_writable(spi1)) {
        tight_loop_contents();
    }

    pio_gpio_init(rxpio, IO_PSDA);
    pio_gpio_init(rxpio, IO_PCLK);
    rxofs = pio_add_program(rxpio, &rei_program);
    rxsm = pio_claim_unused_sm(rxpio, true);
    pio_sm_config rxpioc = rei_program_get_default_config(rxofs);
    sm_config_set_in_pins(&rxpioc, IO_PSDA);
    sm_config_set_in_shift(&rxpioc, false, true, 8);
    pio_sm_set_consecutive_pindirs(rxpio, rxsm, IO_PSDA, 2, false);
    sm_config_set_clkdiv(&rxpioc, 1.0f);
    pio_sm_init(rxpio, rxsm, rxofs, &rxpioc);
    pio_sm_clear_fifos(rxpio, rxsm);
    pio_sm_set_enabled(rxpio, rxsm, true);
    pio_sm_put_blocking(rxpio, rxsm, BUFSIZE * 8);

    rxchan.chan = dma_claim_unused_channel(true);
    rxchan.buf_idx = rxring.widx;
    dma_channel_config dcfg0 = dma_channel_get_default_config(rxchan.chan);
    channel_config_set_transfer_data_size(&dcfg0, DMA_SIZE_8);
    channel_config_set_read_increment(&dcfg0, false);
    channel_config_set_write_increment(&dcfg0, true);
    channel_config_set_dreq(&dcfg0, pio_get_dreq(rxpio, rxsm, false));
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq0_handler);
    irq_set_enabled(DMA_IRQ_0, true);
    dma_channel_set_irq0_enabled(rxchan.chan, true);

    dma_channel_configure(rxchan.chan,
                          &dcfg0,
                          rxring.buf[rxchan.buf_idx].data,
                          &rxpio->rxf[rxsm], /* src */
                          BUFSIZE,
                          true);

    sdch = dma_claim_unused_channel(true);
    clkdcfg = dma_channel_get_default_config(sdch);
    channel_config_set_transfer_data_size(&clkdcfg, DMA_SIZE_8);
    channel_config_set_read_increment(&clkdcfg, false);
    channel_config_set_write_increment(&clkdcfg, false);
    channel_config_set_dreq(&clkdcfg, DREQ_SPI1_TX);
    dma_channel_configure(
        sdch, &clkdcfg, &spi_get_hw(spi1)->dr, &Z, 0xffffffff, true);
}

void init()
{
    // set_sys_clock_pll(1056000000, 6, 1);
    // init_wspi();
    // init_wio(1);
    // init_wio(2);
    // init_wpio();
    // init_wpiospi();
    // th160_init();
    wspi_init0();
}
