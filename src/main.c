#include <bsp/board_api.h>
#include <hardware/adc.h>
#include <hardware/clocks.h>
#include <hardware/dma.h>
#include <hardware/flash.h>
#include <hardware/gpio.h>
#include <hardware/pio.h>
#include <hardware/regs/addressmap.h>
#include <hardware/regs/clocks.h>
#include <hardware/spi.h>
#include <hardware/structs/clocks.h>
#include <hardware/structs/uart.h>
#include <hardware/sync.h>
#include <hardware/timer.h>
#include <hardware/uart.h>
#include <hardware/vreg.h>
#include <init.h>
#include <memory.h>
#include <pico/flash.h>
#include <pico/multicore.h>
#include <pico/platform/common.h>
#include <pico/stdlib.h>
#include <pico/time.h>
#include <pico/types.h>
#include <sdk.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/cdefs.h>
#include <sys/types.h>
#include <utils.h>

#include "usb_descriptors.h"
#if MAIN_FUNC_MODE == MAIN_FUNC_MODE_160M_UVC
#    include "device/usbd.h"
#    include "tusb.h"
#    include "usb_descriptors.h"
#endif

#if MAIN_FUNC_MODE == MAIN_FUNC_MODE_160M_CDC
#    include <tusb.h>

#    include "bsp/board_api.h"
#    include "tusb_config.h"
#endif

#if CFG_TUSB_OSAL != OPT_OSAL_PICO
#    error "TinyUSB OSAL is NOT pico"
#endif

int spi_init_25mhz();
int ovclk_240mhz();
int sclk_io();
int spi_init_40MHz();
int spi_init_40MHz2();
int spi_init_40MHz3();
int spi_init_40MHz4();
int uvc_init();
int cdc_func();
int spi_init_40MHz2_noinit();
int baud_sel();
int adc_func();
int shutter_func();
int flash_func();

int main()
{
#if MAIN_FUNC_MODE == MAIN_FUNC_MODE_SPI_INIT_25MHZ
    return spi_init_25mhz();
#elif MAIN_FUNC_MODE == MAIN_FUNC_MODE_SCLK_IO
    return sclk_io();
#elif MAIN_FUNC_MODE == MAIN_FUNC_MODE_240MHZ
    return ovclk_240mhz();
#elif MAIN_FUNC_MODE == MAIN_FUNC_MODE_SPI_INIT_40MHZ
    return spi_init_40MHz2();
#elif MAIN_FUNC_MODE == MAIN_FUNC_MODE_160M_UVC
    return uvc_init();
#elif MAIN_FUNC_MODE == MAIN_FUNC_MODE_160M_CDC
    return cdc_func();
#elif MAIN_FUNC_MODE == MAIN_FUNC_MODE_SPI_INIT_40MHZ_NOINIT
    return spi_init_40MHz2_noinit();
#elif MAIN_FUNC_MODE == MAIN_FUNC_MODE_SPI_INIT_40MHZ_3
    return spi_init_40MHz3();
#elif MAIN_FUNC_MODE == MAIN_FUNC_MODE_SPI_INIT_40MHZ_4
    return spi_init_40MHz4();
#elif MAIN_FUNC_MODE == MAIN_FUNC_MODE_BAUD_SEL
    return baud_sel();
#elif MAIN_FUNC_MODE == MAIN_FUNC_MODE_ADC
    return adc_func();
#elif MAIN_FUNC_MODE == MAIN_FUNC_MODE_SHUTTER
    return shutter_func();
#elif MAIN_FUNC_MODE == MAIN_FUNC_MODE_FLASH
    return flash_func();
#endif
}

int spi_init_25mhz()
{
    extern void dutx_init();
    dutx_init();

    extern uint spi1clk;
    spi1clk = 25000000;
    init();

    while (true) {
        gpio_put(IO_LED, !gpio_get(IO_LED));
        println("running...");
        sleep_ms(1000);
    }

    errh(nullptr);
    return 0;
}

int ovclk_240mhz()
{
    gpio_init(IO_LED);
    gpio_set_dir(IO_LED, true);
    for (int i = 0; i < 3 * 2; ++i) {
        gpio_put(IO_LED, !gpio_get(IO_LED));
        sleep_ms(500);
    }
    gpio_put(IO_LED, 0);
    gpio_deinit(IO_LED);

    vreg_set_voltage(VREG_VOLTAGE_1_25);
    sleep_ms(10);
    set_sys_clock_khz(240000, true);
    sleep_ms(10);
    clock_configure(clk_peri,
                    0,
                    CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                    240 * MHZ,
                    240 * MHZ);

    gpio_init(IO_LED);
    gpio_set_dir(IO_LED, true);

    extern void dutx_init();
    dutx_init();

    while (true) {
        gpio_put(IO_LED, !gpio_get(IO_LED));
        tight_loop_contents();
        sleep_ms(1000);
        println("sys = %llu peri = %llu",
                (uint64_t)clock_get_hz(clk_sys),
                (uint64_t)clock_get_hz(clk_peri));
    }

    return 0;
}

int spi_init_40MHz()
{
    vreg_set_voltage(VREG_VOLTAGE_1_25);
    sleep_ms(10);
    set_sys_clock_khz(240000, true);
    sleep_ms(10);
    clock_configure(clk_peri,
                    0,
                    CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                    240 * MHZ,
                    240 * MHZ);
    extern void dutx_init();
    dutx_init();

    extern uint spi1clk;
    spi1clk = 40000000;
    init();

    while (true) {
        gpio_put(IO_LED, !gpio_get(IO_LED));
        println("running...");
        sleep_ms(1000);
        println("sys = %llu peri = %llu",
                (uint64_t)clock_get_hz(clk_sys),
                (uint64_t)clock_get_hz(clk_peri));
    }

    errh(nullptr);
    return 0;
}

enum {
    BL_NOT_MOUNTED = 250,
    BL_MOUNTED = 1000,
    BL_SUSPENDED = 2500,
};
static uint32_t bl_inv_ms = BL_NOT_MOUNTED;
static uint32_t frame_num = 0;
static uint32_t tx_busy = 0;
static uint32_t inv_ms = 1000 / FRAME_RATE;

static inline __attribute__((always_inline)) uint32_t now()
{
    return to_ms_since_boot(get_absolute_time());
}

void tud_mount_cb() { bl_inv_ms = BL_MOUNTED; }
void tud_umount_cb() { bl_inv_ms = BL_NOT_MOUNTED; }
void tud_resume_cb() { bl_inv_ms = BL_MOUNTED; }
void tud_suspend_cb(bool remote_wakeup_en)
{
    (void)remote_wakeup_en;
    bl_inv_ms = BL_SUSPENDED;
}
void led_blinking_task(void)
{
    static uint32_t start_ms = 0;

    if (now() - start_ms < bl_inv_ms)
        return;
    start_ms += bl_inv_ms;

    gpio_put(IO_LED, !gpio_get(IO_LED));
}

static void fill_random_yuy2(uint8_t* buf, size_t len)
{
    for (size_t i = 0; i < len; i += 4) {
        uint8_t y0 = rand() & 0xff;
        uint8_t y1 = rand() & 0xff;
        uint8_t u = 128 + (rand() % 32 - 16);
        uint8_t v = 128 + (rand() % 32 - 16);

        buf[i + 0] = y0;
        buf[i + 1] = u;
        buf[i + 2] = y1;
        buf[i + 3] = v;
    }
}

void video_task()
{
    static uint8_t frame_buffer[FRAME_SIZE];
    static uint32_t start_ms = 0;
    static uint32_t already_sent = 0;

    if (!tud_video_n_streaming(0, 0)) {
        already_sent = 0;
        frame_num = 0;
        return;
    }

    if (!already_sent) {
        already_sent = 1;
        tx_busy = 1;
        start_ms = now();
        fill_random_yuy2(frame_buffer, FRAME_SIZE);
        tud_video_n_frame_xfer(0, 0, (void*)frame_buffer, FRAME_SIZE);
    }

    uint32_t cur = now();
    if (cur - start_ms < inv_ms || tx_busy) {
        return;
    }
    start_ms += inv_ms;
    tx_busy = 1;
    fill_random_yuy2(frame_buffer, FRAME_SIZE);
    tud_video_n_frame_xfer(0, 0, (void*)frame_buffer, FRAME_SIZE);
}

void tud_video_frame_xfer_complete_cb(uint_fast8_t ctl_idx,
                                      uint_fast8_t stm_idx)
{
    (void)ctl_idx;
    (void)stm_idx;
    tx_busy = 0;
    ++frame_num;
}

int tud_video_commit_cb(uint_fast8_t ctl_idx,
                        uint_fast8_t stm_idx,
                        video_probe_and_commit_control_t const* parameters)
{
    (void)ctl_idx;
    (void)stm_idx;
    inv_ms = parameters->dwFrameInterval / 10000;
    return VIDEO_ERROR_NONE;
}

int uvc_init()
{
    set_sys_clock_khz(160000, true);
    sleep_ms(10);
    clock_configure(clk_peri,
                    0,
                    CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                    160 * MHZ,
                    160 * MHZ);
    extern void dutx_init();
    dutx_init();

    gpio_init(IO_LED);
    gpio_set_dir(IO_LED, true);
    gpio_put(IO_LED, 1);

    println("==========start==========");
#if MAIN_FUNC_MODE == MAIN_FUNC_MODE_160M_UVC
    tusb_init();
    while (true) {
        tud_task();
        led_blinking_task();
        video_task();
    }
#endif

    errh(nullptr);
    return 0;
}

int cdc_func()
{
    set_sys_clock_khz(160000, true);
    sleep_ms(10);
    clock_configure(clk_peri,
                    0,
                    CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                    160 * MHZ,
                    160 * MHZ);
    extern void dutx_init();
    dutx_init();

#if MAIN_FUNC_MODE == MAIN_FUNC_MODE_160M_CDC
    println("==========start==========");
    board_init();
    tusb_init();

    if (board_init_after_tusb) {
        board_init_after_tusb();
    }

    while (true) {
        tud_task();
        if (tud_cdc_n_connected(0)) {
            // print on CDC 0 some debug message
            printf("Connected to CDC 0\n");
            sleep_ms(5000);  // wait for 5 seconds
        }
    }
#endif

    errh(nullptr);
    return 0;
}

int spi_init_40MHz2()
{
    set_sys_clock_khz(160000, true);
    sleep_ms(10);
    clock_configure(clk_peri,
                    0,
                    CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                    160 * MHZ,
                    160 * MHZ);
    extern void dutx_init();
    dutx_init();

    println("MODE: %s", __PRETTY_FUNCTION__);
    println("==========start==========");
    println(
        "RxBuf: %u, RxRing: %u", sizeof(struct RxBuf), sizeof(struct RxRing));

    extern uint spi1clk;
    spi1clk = 45000000;
    uint32_t fhead = 0;
    uint32_t ftail = 0;
    uint32_t unknpkt = 0;
    uint32_t ecda = 0;
    uint32_t c7da = 0;
    uint32_t c7f1 = 0;
    int cnt = 0;
    uint64_t chut = 0;
    // uint64_t chlt = 0;
    // uint8_t* odata = nullptr;
    // uint32_t odatasiz;
    uint64_t lt = time_us_64();

    init();
    while (true) {
        rx_poll();
        if (rxring.ridx != rxring.widx) {
            struct RxBuf* b = &rxring.buf[rxring.ridx];

#if RXPKTDBG == 1
            uint64_t rt = time_us_64();
            uint64_t dt = rt - lt;
            lt = rt;
            println(
                "%04d <%06llu> buf [%04lld] %02x %02x %02x %02x - %02x %02x "
                "%02x "
                "%02x",
                cnt,
                dt,
                (uint64_t)rxring.ridx,
                b->data[2],
                b->data[3],
                b->data[4],
                b->data[5],
                b->data[BUFSIZE - 6],
                b->data[BUFSIZE - 5],
                b->data[BUFSIZE - 4],
                b->data[BUFSIZE - 3]);

            if (make_line0((const uint16_t*)b->data, &odata, &odatasiz) >= 0) {
                for (uint32_t i = 0; i < odatasiz; ++i) {
                    print("%02x ", odata[i]);
                }
                println("");
            }
#endif

            // chlt = time_us_64();
            if (b->data[2] == 0xff && b->data[3] == 0x00
                && b->data[BUFSIZE - 6] == 0xff
                && b->data[BUFSIZE - 5] == 0x00) {
                if (b->data[5] == 0xec && b->data[BUFSIZE - 3] == 0xda) {
                    ecda++;
                    if (fhead == 0) {
                        fhead = cnt;
                    }
                } else if (b->data[5] == 0xc7 && b->data[BUFSIZE - 3] == 0xda) {
                    c7da++;
                } else if (b->data[5] == 0xc7 && b->data[BUFSIZE - 3] == 0xf1) {
                    c7f1++;
                    if (ftail == 0) {
                        ftail = cnt;
                    }
                } else {
                    unknpkt++;
                }
            } else {
                unknpkt++;
            }
            // if (make_line((const uint16_t*)b->data, &odata, &odatasiz) >= 0)
            // {
            //     uart_write_blocking(OUART, odata, odatasiz);
            //     if (chut == 0) {
            //         chut = time_us_64() - chlt;
            //     }
            // }

            rxring.ridx = (rxring.ridx + 1) % BUFCOUNT;
            cnt++;
            if (cnt >= 1024) {
                break;
            }

#if RXPKTDBG == 1
            dma_channel_configure(
                sdch, &clkdcfg, &spi_get_hw(spi1)->dr, &Z, 0xffffffff, true);
#endif
        }
    }
    dma_channel_abort(sdch);

    uint64_t eut = time_us_64() - lt;
    println(
        "<%llu us> head: %llu body: %llu tail: %llu unk: %llu fhead: %llu "
        "ftail: %llu",
        (uint64_t)eut,
        (uint64_t)ecda,
        (uint64_t)c7da,
        (uint64_t)c7f1,
        (uint64_t)unknpkt,
        (uint64_t)fhead,
        (uint64_t)ftail);
    println("chut: %llu", chut);
    println("spi clk -- %llu", (uint64_t)spi1clk);

    println("==========finish==========");

    // const char* string = "running...\r\n";
    // while (true) {
    //     uart_write_blocking(OUART, (const uint8_t*)string, strlen(string));
    //     sleep_ms(1000);
    // }

    // for (int i = 0; i < BUFCOUNT; ++i) {
    //     println("==== buf [%d] %llu ====", i, (uint64_t)rxring.buf[i].len);
    //     dump_buf_8bytes(rxring.buf[i].data, BUFSIZE + BUFOF);
    //     println("");
    // }

    errh(nullptr);
    return 0;
}

int spi_init_40MHz2_noinit()
{
    set_sys_clock_khz(160000, true);
    sleep_ms(10);
    clock_configure(clk_peri,
                    0,
                    CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                    160 * MHZ,
                    160 * MHZ);
    extern void dutx_init();
    dutx_init();

    println("MODE: %s", __PRETTY_FUNCTION__);
    println("==========start==========");
    println(
        "RxBuf: %u, RxRing: %u", sizeof(struct RxBuf), sizeof(struct RxRing));

    extern uint spi1clk;
    spi1clk = 40000000;
    uint32_t fhead = 0;
    uint32_t ftail = 0;
    uint32_t unknpkt = 0;
    uint32_t ecda = 0;
    uint32_t c7da = 0;
    uint32_t c7f1 = 0;
    int cnt = 0;
    uint64_t lt = time_us_64();

    extern void wspi_init0_noinit();
    wspi_init0_noinit();

    while (true) {
        rx_poll();
        if (rxring.ridx != rxring.widx) {
            struct RxBuf* b = &rxring.buf[rxring.ridx];

            if (b->data[2] == 0xff && b->data[3] == 0x00
                && b->data[BUFSIZE - 6] == 0xff
                && b->data[BUFSIZE - 5] == 0x00) {
                if (b->data[5] == 0xec && b->data[BUFSIZE - 3] == 0xda) {
                    ecda++;
                    if (fhead == 0) {
                        fhead = cnt;
                    }
                } else if (b->data[5] == 0xc7 && b->data[BUFSIZE - 3] == 0xda) {
                    c7da++;
                } else if (b->data[5] == 0xc7 && b->data[BUFSIZE - 3] == 0xf1) {
                    c7f1++;
                    if (ftail == 0) {
                        ftail = cnt;
                    }
                } else {
                    unknpkt++;
                }
            } else {
                unknpkt++;
            }

            println("%llu/%llu", (uint64_t)cnt, (uint64_t)BUFCOUNT - 1);

            rxring.ridx = (rxring.ridx + 1) % BUFCOUNT;
            cnt++;
            if (cnt >= BUFCOUNT - 1) {
                break;
            }
        }
    }
    // dma_channel_abort(sdch);

    uint64_t eut = time_us_64() - lt;
    println(
        "<%llu us> head: %llu body: %llu tail: %llu unk: %llu fhead: %llu "
        "ftail: %llu",
        (uint64_t)eut,
        (uint64_t)ecda,
        (uint64_t)c7da,
        (uint64_t)c7f1,
        (uint64_t)unknpkt,
        (uint64_t)fhead,
        (uint64_t)ftail);
    println("spi clk -- %llu", (uint64_t)spi1clk);

    println("==========finish==========");

    for (int i = 0; i < BUFCOUNT; ++i) {
        println("==== buf [%d] %llu ====", i, (uint64_t)rxring.buf[i].len);
        dump_buf_8bytes(rxring.buf[i].data, BUFSIZE + BUFOF);
        println("");
    }

    errh(nullptr);
    return 0;
}

int spi_init_40MHz3()
{
    set_sys_clock_khz(160000, true);
    sleep_ms(10);
    clock_configure(clk_peri,
                    0,
                    CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                    160 * MHZ,
                    160 * MHZ);
    extern void dutx_init();
    dutx_init();

    println("MODE: %s", __PRETTY_FUNCTION__);
    println("==========start==========");

    extern uint spi1clk;
    spi1clk = 45000000;
    uint32_t unknpkt = 0;
    uint32_t ecda = 0;
    uint32_t c7da = 0;
    uint32_t c7f1 = 0;
    int cnt = 0;
    uint32_t head[32];
    uint32_t tail[32];
    uint32_t unkp[32];
    uint64_t lt = time_us_64();

    extern void wspi_reinit1();
    extern void wspi_init1();
    wspi_init1();
    while (true) {
        rx_poll();
        if (rxring.ridx != rxring.widx) {
            struct RxBuf* b = &rxring.buf[rxring.ridx];

            if (b->data[2] == 0xff && b->data[3] == 0x00
                && b->data[BUFSIZE - 6] == 0xff
                && b->data[BUFSIZE - 5] == 0x00) {
                if (b->data[5] == 0xec && b->data[BUFSIZE - 3] == 0xda) {
                    head[ecda++] = cnt;
                } else if (b->data[5] == 0xc7 && b->data[BUFSIZE - 3] == 0xda) {
                    c7da++;
                } else if (b->data[5] == 0xc7 && b->data[BUFSIZE - 3] == 0xf1) {
                    tail[c7f1++] = cnt;
                    wspi_reinit1();
                } else {
                    unkp[unknpkt++] = cnt;
                }
            } else {
                unkp[unknpkt++] = cnt;
            }

            rxring.ridx = (rxring.ridx + 1) % BUFCOUNT;
            cnt++;
            if (cnt >= BUFCOUNT - 1) {
                break;
            }
        }
    }
    // dma_channel_abort(sdch);

    uint64_t eut = time_us_64() - lt;
    println("<%llu us> head: %llu body: %llu tail: %llu unk: %llu",
            (uint64_t)eut,
            (uint64_t)ecda,
            (uint64_t)c7da,
            (uint64_t)c7f1,
            (uint64_t)unknpkt);

    print("head<%llu>: ", (uint64_t)ecda);
    for (uint32_t i = 0; i < ecda; ++i) {
        print("%llu ", (uint64_t)head[i]);
    }
    println("");

    print("tail<%llu>: ", (uint64_t)c7f1);
    for (uint32_t i = 0; i < c7f1; ++i) {
        print("%llu ", (uint64_t)tail[i]);
    }
    println("");

    print("unk<%llu>: ", (uint64_t)unknpkt);
    for (uint32_t i = 0; i < unknpkt; ++i) {
        print("%llu ", (uint64_t)unkp[i]);
    }
    println("");

    println("==========finish==========");

    // const char* string = "running...\r\n";
    // while (true) {
    //     uart_write_blocking(OUART, (const uint8_t*)string, strlen(string));
    //     sleep_ms(1000);
    // }

    for (int i = 0; i < BUFCOUNT; ++i) {
        println("==== buf [%d] %llu ====", i, (uint64_t)rxring.buf[i].len);
        dump_buf_8bytes(rxring.buf[i].data, BUFSIZE + BUFOF);
        println("");
    }

    errh(nullptr);
    return 0;
}

enum rawbuf_state {
    RAWBUF_FREE = 0,
    RAWBUF_WRITING,
    RAWBUF_READY,
    RAWBUF_READING,
};

#define RAWBUFADCCNT     1
#define RAWBUFCNT        3
#define RAWBUFSIZE       (SENSOR_W * SENSOR_H * 2)
#define RAWBUFSIZEOFS    (SENSOR_H * 4)
#define RAWBUFSIZEOFSADC (2 * RAWBUFADCCNT)
#define RAWBUFSIZETOTAL  (RAWBUFSIZE + RAWBUFSIZEOFS + RAWBUFSIZEOFSADC)
static uint8_t rawbuf[RAWBUFCNT][RAWBUFSIZETOTAL];
static volatile uint8_t rawbuf_state[RAWBUFCNT];
static volatile int rawbuf_last_ready;

static void rawbuf_init(void)
{
    for (int i = 0; i < RAWBUFCNT; i++) {
        rawbuf_state[i] = RAWBUF_FREE;
    }
    rawbuf_last_ready = -1;
}

static int rawbuf_find_free(void)
{
    for (int i = 0; i < RAWBUFCNT; i++) {
        if (rawbuf_state[i] == RAWBUF_FREE)
            return i;
    }
    return -1;
}

static inline void rawbuf_copy_line(int buf_idx,
                                    int line,
                                    const uint8_t* src,
                                    size_t len)
{
    if (line >= SENSOR_H)
        return;

#if RAW16_MODE == 1
    const size_t line_stride = SENSOR_W * 2 + 4;
    uint8_t* dst = &rawbuf[buf_idx][line * line_stride];
    memcpy(dst, src, SENSOR_W * 2);
    memcpy(dst + SENSOR_W * 2, src + SENSOR_W * 2 + 4, 4);
#else
    uint8_t* dst = &rawbuf[buf_idx][line * SENSOR_W * 2];
    memcpy(dst, src, len);
#endif
}

static int rawbuf_pick_ready(void)
{
    int idx = rawbuf_last_ready;

    if (idx >= 0 && rawbuf_state[idx] == RAWBUF_READY)
        return idx;

    for (int i = 0; i < RAWBUFCNT; i++) {
        if (rawbuf_state[i] == RAWBUF_READY)
            return i;
    }

    return -1;
}

#define HIST_BUCKETS 64
#define HIST_SHIFT   8 /* 14bit >> 8 = 6bit bucket */

static inline uint16_t be16_to_u14(const uint8_t* p)
{
    uint16_t v;
    v = ((uint16_t)p[0] << 8) | p[1];
    return v & 0x3fff;
}

static void calc_percentile_u14_be(const uint8_t* raw,
                                   size_t px_cnt,
                                   uint16_t* low,
                                   uint16_t* high)
{
    uint32_t hist[64] = {0};
    uint32_t i, acc;
    uint32_t p1_cnt = px_cnt / 100;
    uint32_t p99_cnt = px_cnt - p1_cnt;

    for (i = 0; i < px_cnt; i++) {
        uint16_t v = be16_to_u14(&raw[i * 2]);
        hist[v >> 8]++;
    }

    acc = 0;
    for (i = 0; i < 64; i++) {
        acc += hist[i];
        if (acc >= p1_cnt) {
            *low = i << 8;
            break;
        }
    }

    acc = 0;
    for (i = 0; i < 64; i++) {
        acc += hist[i];
        if (acc >= p99_cnt) {
            *high = (i << 8) | 0xff;
            break;
        }
    }

    if (*high <= *low)
        *high = *low + 1;
}

static inline uint8_t norm_u14_to_u8(uint16_t v, uint16_t low, uint16_t high)
{
    uint32_t t;

    if (v <= low)
        return 0;

    if (v >= high)
        return 254;

    t = (uint32_t)(v - low) * 254;
    t /= (high - low);

    if (t > 254)
        t = 254;

    return (uint8_t)t;
}

enum tx_buf_state {
    TXBUF_FREE = 0,
    TXBUF_FILLING,
    TXBUF_READY,
    TXBUF_SENDING,
};

static enum UartOutMode ouomode = UART_OUT_MODE_DEF;
#define PX_CNT (SENSOR_W * SENSOR_H)
#if RAW16_MODE
#    define TX_PAYLOAD (RAWBUFSIZETOTAL + 2)
#else
#    define TX_PAYLOAD (PX_CNT + 1) /* 包头 + 归一化像素 */
#endif
#define TXBUF_CNT 3
static int uart_tx_dma_chan = -1;
static uint8_t norm_buf[TXBUF_CNT][TX_PAYLOAD];
int tx_buf_idx;
static volatile int dma_busy;
static int buf_idx;
static volatile enum tx_buf_state tx_state[TXBUF_CNT];
static volatile int newest_ready = -1;
static volatile int sending = -1;

void normalize_frame_be(uint8_t* raw, uint8_t* out)
{
    uint16_t low, high;
    size_t i;

    calc_percentile_u14_be(raw, PX_CNT, &low, &high);

    out[0] = 0xff; /* 包头 */

    for (i = 0; i < PX_CNT; i++) {
        uint16_t v = be16_to_u14(&raw[i * 2]);
        out[i + 1] = norm_u14_to_u8(v, low, high);
    }
}

void calc_percentile_u14(const uint16_t* raw14,
                         size_t px_cnt,
                         uint16_t* low,
                         uint16_t* high)
{
    uint32_t hist[HIST_BUCKETS] = {0};
    size_t i;
    uint32_t acc;
    uint32_t p1_cnt = px_cnt / 100;
    uint32_t p99_cnt = px_cnt - p1_cnt;

    for (i = 0; i < px_cnt; i++)
        hist[raw14[i] >> HIST_SHIFT]++;

    acc = 0;
    for (i = 0; i < HIST_BUCKETS; i++) {
        acc += hist[i];
        if (acc >= p1_cnt) {
            *low = i << HIST_SHIFT;
            break;
        }
    }

    acc = 0;
    for (i = 0; i < HIST_BUCKETS; i++) {
        acc += hist[i];
        if (acc >= p99_cnt) {
            *high = (i << HIST_SHIFT) | 0xff;
            break;
        }
    }

    if (*high <= *low)
        *high = *low + 1;
}

static inline uint16_t gray14_to_bin(uint16_t g)
{
    g &= 0x3fff;
    g ^= g >> 1;
    g ^= g >> 2;
    g ^= g >> 4;
    g ^= g >> 8;
    return g & 0x3fff;
}

static inline uint16_t clip_u16(int32_t v)
{
    if (v < 0) {
        return 0;
    }
    if (v > 65535) {
        return 65535;
    }
    return (uint16_t)v;
}

#if 0
static void raw16_decode_to_normbuf(const uint8_t* raw, uint8_t* out, size_t)
{
    uint8_t* dst = &out[2];
    const size_t line_stride = SENSOR_W * 2 + 4;

    for (size_t y = 0; y < SENSOR_H; y++) {
        const uint8_t* line = raw + y * line_stride;

        for (size_t x = 0; x < SENSOR_W; x++) {
            uint16_t r = be16_to_u14(&line[x * 2]);
            r = gray14_to_bin(r);

            size_t idx = (y * (SENSOR_W + 2) + x) * 2;
            dst[idx] = (uint8_t)(r >> 8);
            dst[idx + 1] = (uint8_t)(r & 0xff);
        }

        memcpy(
            &dst[(y * (SENSOR_W + 2) + SENSOR_W) * 2], &line[SENSOR_W * 2], 4);
    }

    const uint8_t* adc = raw + SENSOR_H * line_stride;
    memcpy(&dst[SENSOR_H * (SENSOR_W + 2) * 2], adc, 2);
}
#else
static void raw16_decode_to_normbuf(const uint8_t* raw,
                                    uint8_t* out,
                                    size_t size)
{
    uint8_t* dst = &out[2];
    const size_t line_stride = SENSOR_W * 2 + 4;
    const size_t px_per_line = SENSOR_W + 2;

    for (size_t y = 0; y < SENSOR_H; y++) {
        const uint8_t* line = raw + y * line_stride;

        for (size_t x = 0; x < px_per_line; x++) {
            const uint8_t* src = &line[x * 2];

            uint16_t r = be16_to_u14(src);
            r = gray14_to_bin(r);

            size_t idx = (y * px_per_line + x) * 2;
            dst[idx] = (uint8_t)(r >> 8);
            dst[idx + 1] = (uint8_t)(r & 0xff);
        }
    }

    const uint8_t* adc = raw + SENSOR_H * line_stride;
    memcpy(&dst[SENSOR_H * px_per_line * 2], adc, 2);
}
#endif

// static void raw16_decode_to_normbuf(const uint8_t* raw,
//                                     uint8_t* out,
//                                     size_t px_cnt)
// {
//     uint8_t* dst = &out[2];
//
//     for (size_t i = 0; i < px_cnt; i++) {
//         uint16_t v;
//
//         if (i < PX_CNT - RAWBUFADCCNT) {
//             uint16_t r = be16_to_u14(&raw[i * 2]);
//             r = gray14_to_bin(r);
//             v = r;
//         } else {
//             v = ((uint16_t)raw[i * 2] << 8) | raw[i * 2 + 1];
//         }
//
//         dst[i * 2] = (uint8_t)(v >> 8);
//         dst[i * 2 + 1] = (uint8_t)(v & 0xff);
//     }
// }
//
void binary_calib_frame(const uint8_t* raw,
                        const uint16_t* calib,
                        uint8_t* out,
                        size_t px_cnt)
{
    uint16_t low, high;
    static uint16_t raw14[PX_CNT];
    for (size_t i = 0; i < px_cnt; i++) {
        uint16_t r = be16_to_u14(&raw[i * 2]);
        r = gray14_to_bin(r);
        int32_t diff = (int32_t)r - (int32_t)calib[i];
        raw14[i] = clip_u16(diff);
    }
    calc_percentile_u14(raw14, PX_CNT, &low, &high);

    out[0] = 0xff;
    for (int i = 0; i < PX_CNT; i++) {
        out[i + 1] = norm_u14_to_u8(raw14[i], low, high);
    }
}

void normalize_frame_corrected_be(const uint8_t* raw,
                                  const uint16_t* calib,
                                  uint8_t* out)
{
    uint16_t low, high;
    uint16_t raw14[PX_CNT];
    size_t i;

    for (i = 0; i < PX_CNT; i++) {
        uint16_t r = be16_to_u14(&raw[i * 2]);
        r = gray14_to_bin(r);
        int32_t diff = (int32_t)r - (int32_t)calib[i];
        if (diff < 0) {
            diff = 0;
        }
        raw14[i] = (uint16_t)diff;
    }

    calc_percentile_u14(raw14, PX_CNT, &low, &high);

    out[0] = 0xff;
    for (i = 0; i < PX_CNT; i++) {
        out[i + 1] = norm_u14_to_u8(raw14[i], low, high);
    }
}

static inline int txbuf_pick_free(void)
{
    for (int i = 0; i < TXBUF_CNT; i++) {
        if (tx_state[i] == TXBUF_FREE)
            return i;
    }

    for (int i = 0; i < TXBUF_CNT; i++) {
        if (tx_state[i] == TXBUF_READY)
            return i;
    }

    return -1;
}

static inline void uart_tx_dma_start(const uint8_t* buf, size_t len)
{
    dma_busy = 1;

    dma_channel_set_read_addr(uart_tx_dma_chan, buf, false);
    dma_channel_set_trans_count(uart_tx_dma_chan, len, true);
}

static inline void try_start_dma(void)
{
    if (dma_busy)
        return;

    int b = newest_ready;
    if (b < 0)
        return;

    if (tx_state[b] != TXBUF_READY)
        return;

    tx_state[b] = TXBUF_SENDING;
    newest_ready = -1;
    sending = b;

    __dmb();
    dma_busy = 1;

    uart_tx_dma_start(norm_buf[b], TX_PAYLOAD);
}

void __isr dma_irq1_handler(void)
{
    dma_hw->ints1 = 1u << uart_tx_dma_chan;

    int b = sending;
    tx_state[b] = TXBUF_FREE;
    __dmb();
    sending = -1;
    dma_busy = 0;
    __sev();
}

static void fill_txbuf(int tx_idx, int raw_idx)
{
#if RAW16_MODE
    /* RAW16 模式：复制原始数据 + 2字节包头 */
    norm_buf[tx_idx][0] = 0xFF;
    norm_buf[tx_idx][1] = 0xFF;
    // memcpy(&norm_buf[tx_idx][2], rawbuf[raw_idx], RAWBUFSIZETOTAL);
    raw16_decode_to_normbuf(rawbuf[raw_idx], norm_buf[tx_idx], PX_CNT);
#else
    extern const uint16_t calib[];
    /* RAW14 模式：归一化处理 */
    // normalize_frame_corrected_be(rawbuf[raw_idx], rawdata, norm_buf[tx_idx]);
    binary_calib_frame(rawbuf[raw_idx], calib, norm_buf[tx_idx], PX_CNT);
#endif
}

static uint32_t curr_uartb;
uint32_t baud_get2(uint8_t cmd)
{
    uint32_t baud = 0;
    ouomode = UOM_CALI_U8;
    switch (cmd) {
    case 0x11:
        baud = UBM11;
        break;
    case 0x22:
        baud = UBM22;
        break;
    case 0x33:
        baud = UBM33;
        break;
    case 0x44:
        baud = UBM44;
        break;
    case 0x55:
        baud = UBM55;
        break;
    case 0x66:
        baud = UBM66;
        break;
    case 0x77:
        baud = UBM77;
        break;
    case 0x88:
        baud = UBM88;
        break;
    case 0xAA:
        baud = UBMAA;
        ouomode = UOM_RAW_U16;
        break;
    default:
        break;
    };
    return baud;
}

void ouart_init(const uint8_t cmd)
{
    uint32_t baud = baud_get2(cmd);
    uart_deinit(OUART);
    gpio_set_function(IO_OTX, GPIO_FUNC_UART);
    gpio_set_function(IO_ORX, GPIO_FUNC_UART);
    uart_init(OUART, baud);
    uart_set_hw_flow(OUART, false, false);
    uart_set_format(OUART, 8, 1, UART_PARITY_NONE);

    dma_channel_config c;
    uart_tx_dma_chan = dma_claim_unused_channel(true);
    c = dma_channel_get_default_config(uart_tx_dma_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, uart_get_dreq(OUART, true));
    irq_set_exclusive_handler(DMA_IRQ_1, dma_irq1_handler);
    irq_set_enabled(DMA_IRQ_1, true);
    dma_channel_set_irq1_enabled(uart_tx_dma_chan, true);

    dma_channel_configure(
        uart_tx_dma_chan, &c, &uart_get_hw(OUART)->dr, nullptr, 0, false);

    curr_uartb = baud;
}

void uart_poll2(uint32_t (*sel)(uint8_t))
{
    if (!uart_is_readable(OUART)) {
        return;
    }

    uint8_t cmd = uart_getc(OUART);
    uint32_t baud = sel(cmd);

    if (baud == 0 || baud == curr_uartb) {
        return;
    }

    while (dma_busy) {
        tight_loop_contents();
    }

    dma_channel_abort(uart_tx_dma_chan);
    while (dma_channel_is_busy(uart_tx_dma_chan))
        tight_loop_contents();

    uart_deinit(OUART);

    gpio_set_function(IO_OTX, GPIO_FUNC_UART);
    gpio_set_function(IO_ORX, GPIO_FUNC_UART);

    uart_init(OUART, baud);
    uart_set_hw_flow(OUART, false, false);
    uart_set_format(OUART, 8, 1, UART_PARITY_NONE);

    curr_uartb = baud;
}

void core1_rawbuf_consumer(void)
{
    int idx;
    for (int i = 0; i < TXBUF_CNT; i++) {
        tx_state[i] = TXBUF_FREE;
    }

    newest_ready = -1;
    sending = -1;
    dma_busy = 0;

    println("Core1 consumer start");
    flash_safe_execute_core_init();

    ouart_init(UART_BAUD_MODE_DEF);
    dma_busy = 0;
    buf_idx = 0;

    extern const uint8_t rawdata[];

    while (true) {
        uart_poll2(baud_get2);
        bool did_work = false;

        idx = rawbuf_pick_ready();
        if (idx >= 0) {
            __dmb();
            rawbuf_state[idx] = RAWBUF_READING;

            int b = txbuf_pick_free();
            if (b >= 0) {
                tx_state[b] = TXBUF_FILLING;

                fill_txbuf(b, idx);

                tx_state[b] = TXBUF_READY;
                newest_ready = b;

                __sev();
            }

            __dmb();
            rawbuf_state[idx] = RAWBUF_FREE;
            did_work = true;
        }

        try_start_dma();

        if (!did_work)
            __wfe();
    }
}

static inline void uart_tx_dma_wait_done(void)
{
    dma_channel_wait_for_finish_blocking(uart_tx_dma_chan);
    dma_busy = 0;
}

#if RAW_OUT_MODE == 1
void core1_rawbuf_consumer2(void)
{
    // uint32_t cnt = 0;
    int idx;

    println("Core1 consumer start");
    uart_deinit(OUART);
    gpio_set_function(IO_OTX, GPIO_FUNC_UART);
    gpio_set_function(IO_ORX, GPIO_FUNC_UART);
    uart_init(OUART, 5000000);
    uart_set_hw_flow(OUART, false, false);
    uart_set_format(OUART, 8, 1, UART_PARITY_NONE);

    dma_channel_config c;

    uart_tx_dma_chan = dma_claim_unused_channel(true);

    c = dma_channel_get_default_config(uart_tx_dma_chan);

    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, uart_get_dreq(OUART, true));

    dma_channel_configure(
        uart_tx_dma_chan, &c, &uart_get_hw(OUART)->dr, nullptr, 0, false);

    dma_busy = 0;
    buf_idx = 0;

    while (true) {
        while ((idx = rawbuf_pick_ready()) < 0) {
            __wfe();
        }

        // cnt++;
        // println("%llu %llu", (uint64_t)cnt, (uint64_t)idx);
        __dmb();
        rawbuf_state[idx] = RAWBUF_READING;

        // println("========");
        // dump_buf_8bytes(rawbuf[idx], RAWBUFSIZE);

        if (dma_busy) {
            uart_tx_dma_wait_done();
        }
        normalize_frame_be(rawbuf[idx], norm_buf[buf_idx]);
        uart_tx_dma_start(norm_buf[tx_buf_idx], TX_PAYLOAD);

        __dmb();
        rawbuf_state[idx] = RAWBUF_FREE;
        __sev();
        buf_idx ^= 1;
    }
}
#endif

#define SHUTTER_ENABLE 1

void shutter_on()
{
#if SHUTTER_ENABLE == 1
    gpio_put(IO_SVMS, 1);
    gpio_put(IO_SVM1, 1);
    gpio_put(IO_SVM2, 0);
#endif
    sleep_ms(200);
#if SHUTTER_ENABLE == 1
    gpio_put(IO_SVMS, 0);
    gpio_put(IO_SVM1, 0);
    gpio_put(IO_SVM2, 0);
#endif
}

void shutter_off()
{
#if SHUTTER_ENABLE == 1
    gpio_put(IO_SVMS, 1);
    gpio_put(IO_SVM1, 0);
    gpio_put(IO_SVM2, 1);
#endif
    sleep_ms(200);
#if SHUTTER_ENABLE == 1
    gpio_put(IO_SVMS, 0);
    gpio_put(IO_SVM1, 0);
    gpio_put(IO_SVM2, 0);
#endif
}

uint16_t adc_read_median()
{
#define ADC_SAMPLES 5
    uint16_t v[ADC_SAMPLES];

    for (int i = 0; i < ADC_SAMPLES; i++) {
        v[i] = adc_read();
    }

    for (int i = 0; i < ADC_SAMPLES - 1; i++) {
        for (int j = i + 1; j < ADC_SAMPLES; j++) {
            if (v[j] < v[i]) {
                uint16_t t = v[i];
                v[i] = v[j];
                v[j] = t;
            }
        }
    }

    return v[ADC_SAMPLES / 2];
#if defined(ADC_SAMPLES)
#    undef ADC_SAMPLES
#endif
}

uint16_t adc_read_trimmed_mean()
{
#define ADC_SAMPLES 8
    uint16_t v[ADC_SAMPLES];
    uint32_t sum = 0;

    for (int i = 0; i < ADC_SAMPLES; i++) {
        v[i] = adc_read();
        sleep_us(5);
    }

    for (int i = 0; i < ADC_SAMPLES - 1; i++) {
        for (int j = i + 1; j < ADC_SAMPLES; j++) {
            if (v[j] < v[i]) {
                uint16_t t = v[i];
                v[i] = v[j];
                v[j] = t;
            }
        }
    }

    for (int i = 1; i < ADC_SAMPLES - 1; i++) {
        sum += v[i];
    }

    return (uint16_t)(sum / (ADC_SAMPLES - 2));

#if defined(ADC_SAMPLES)
#    undef ADC_SAMPLES
#endif
}

uint16_t adc_read_trimmed_mean_from_buf(const uint16_t* data,
                                        const uint32_t size)
{
    uint32_t sum = 0;
    uint16_t min = 0xffff;
    uint16_t max = 0x0000;

    if (!data || size < 3) {
        return 0;
    }

    for (uint32_t i = 0; i < size; i++) {
        uint16_t v = data[i];

        sum += v;

        if (v < min)
            min = v;
        if (v > max)
            max = v;
    }

    sum -= min;
    sum -= max;

    return (uint16_t)(sum / (size - 2));
}

uint16_t raw16_line_vtemp_avg(const uint8_t* line)
{
    const uint8_t* p;
    uint16_t v0, v1;

    p = line + SENSOR_W * 2 + 4;

    v0 = gray14_to_bin(be16_to_u14(&p[0]));
    v1 = gray14_to_bin(be16_to_u14(&p[2]));

    return (uint16_t)(((uint32_t)v0 + v1) >> 1);
}

uint16_t raw16_vtemp_avg(const uint8_t* line)
{
    uint16_t v0, v1;

    v0 = gray14_to_bin(be16_to_u14(&line[0]));
    v1 = gray14_to_bin(be16_to_u14(&line[2]));

    return (uint16_t)(((uint32_t)v0 + v1) >> 1);
}

int spi_init_40MHz4()
{
    set_sys_clock_khz(160000, true);
    sleep_ms(10);
    clock_configure(clk_peri,
                    0,
                    CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                    160 * MHZ,
                    160 * MHZ);
    clock_configure(clk_adc,
                    0,
                    CLOCKS_CLK_ADC_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
                    48 * MHZ,
                    48 * MHZ);
    extern void dutx_init();
    dutx_init();

    println("MODE: %s", __PRETTY_FUNCTION__);
    println("==========start==========");
    gpio_init(IO_LED);
    gpio_set_dir(IO_LED, true);
    gpio_put(IO_LED, 0);

#if RAW16_MODE == 1
#    define ADC_SAMPLES 1800
#else
#    define ADC_SAMPLES 1
#endif
    const uint32_t adcsamp = ADC_SAMPLES;
    uint16_t adcbuf[ADC_SAMPLES];
#undef ADC_SAMPLES
    memset(adcbuf, 0x00, adcsamp * sizeof(uint16_t));

    gpio_deinit(IO_NTC);
    adc_init();
    // adc_set_clkdiv(480.0f);
    adc_gpio_init(IO_NTC);
    adc_select_input(0);
    adc_fifo_setup(true, true, 1, false, false);
    int adcdch = dma_claim_unused_channel(true);
    dma_channel_config adcdcfg = dma_channel_get_default_config(adcdch);
    channel_config_set_transfer_data_size(&adcdcfg, DMA_SIZE_16);
    channel_config_set_read_increment(&adcdcfg, false);
    channel_config_set_write_increment(&adcdcfg, true);
    channel_config_set_dreq(&adcdcfg, DREQ_ADC);
    dma_channel_configure(
        adcdch, &adcdcfg, adcbuf, &adc_hw->fifo, adcsamp, false);

    gpio_init(IO_SVMS);
    gpio_set_dir(IO_SVMS, true);
    gpio_put(IO_SVMS, 0);
    gpio_init(IO_SVM1);
    gpio_set_dir(IO_SVM1, true);
    gpio_put(IO_SVM1, 0);
    gpio_init(IO_SVM2);
    gpio_set_dir(IO_SVM2, true);
    gpio_put(IO_SVM2, 0);

    gpio_put(IO_SVMS, 1);
    gpio_put(IO_SVM1, 0);
    gpio_put(IO_SVM2, 1);
    sleep_ms(250);
    gpio_put(IO_SVMS, 0);
    gpio_put(IO_SVM1, 0);
    gpio_put(IO_SVM2, 0);
#if RAW16_MODE
    bool shutter_is_on = false;
    static uint8_t vtemps[SENSOR_H][4] = {0};
#endif

    extern uint spi1clk;
    spi1clk = 45000000;
    int32_t lidx = -1;
    int cur_buf = -1;
    int next_buf = -1;
#define TEST_MAX_CNT_MAX 0xFFFFFFFF
#define TEST_MAX_CNT     TEST_MAX_CNT_MAX
#if TEST_MAX_CNT != TEST_MAX_CNT_MAX
#    define POS_NUM 10
    uint64_t cnt = 0;
    uint64_t headcnt = 0;
    uint64_t tailcnt = 0;
    uint64_t bodycnt = 0;
    uint64_t headpos[POS_NUM];
    uint64_t tailpos[POS_NUM];
    uint64_t bodycnts[POS_NUM];
    memset(headpos, 0x00, POS_NUM * sizeof(uint64_t));
    memset(tailpos, 0x00, POS_NUM * sizeof(uint64_t));
    memset(bodycnts, 0x00, POS_NUM * sizeof(uint64_t));
#endif

    rawbuf_init();
    next_buf = rawbuf_find_free();
    if (next_buf >= 0) {
        rawbuf_state[next_buf] = RAWBUF_WRITING;
    }

    multicore_launch_core1(core1_rawbuf_consumer);
    sleep_ms(5);

    extern void wspi_reinit1();
    extern void wspi_init1();
    extern void wspi_stop1();
    wspi_init1();
    while (true) {
        rx_poll();
        if (rxring.ridx != rxring.widx) {
            struct RxBuf* b = &rxring.buf[rxring.ridx];

            if (b->data[2] == 0xff && b->data[3] == 0x00
                && b->data[BUFSIZE - 6] == 0xff
                && b->data[BUFSIZE - 5] == 0x00) {
                if (b->data[5] == 0xec && b->data[BUFSIZE - 3] == 0xda) {
                    lidx = 0;
                    if (cur_buf < 0 && next_buf >= 0) {
                        cur_buf = next_buf;
                        next_buf = -1;
                    }

                    if (cur_buf < 0) {
                        lidx = -1;
                        goto next_rx;
                    }

                    adc_run(true);
                    dma_channel_start(adcdch);

                    rawbuf_copy_line(cur_buf, lidx, &b->data[14], SENSOR_W * 2);
#if RAW16_MODE == 1
                    // vtemps[lidx] = raw16_line_vtemp_avg(&b->data[14]);
                    memcpy(vtemps[lidx], &b->data[14 + SENSOR_W * 2 + 4], 4);
#endif
                    lidx++;
#if TEST_MAX_CNT != TEST_MAX_CNT_MAX
                    headpos[headcnt++] = cnt;
                    if (headcnt != 0) {
                        bodycnts[headcnt - 1]++;
                    }
#endif
                } else if (b->data[5] == 0xc7 && b->data[BUFSIZE - 3] == 0xda) {
                    if (lidx < 0 || cur_buf < 0) {
                        goto next_rx;
                    }

                    rawbuf_copy_line(cur_buf, lidx, &b->data[14], SENSOR_W * 2);
#if RAW16_MODE == 1
                    // vtemps[lidx] = raw16_line_vtemp_avg(&b->data[14]);
                    memcpy(vtemps[lidx], &b->data[14 + SENSOR_W * 2 + 4], 4);
#endif

                    lidx++;
#if TEST_MAX_CNT != TEST_MAX_CNT_MAX
                    bodycnt++;
#endif
                } else if (b->data[5] == 0xc7 && b->data[BUFSIZE - 3] == 0xf1) {
                    // rawbuf_copy_line(cur_buf, lidx, &b->data[14], SENSOR_W *
                    // 2);
#if TEST_MAX_CNT != TEST_MAX_CNT_MAX
                    tailpos[tailcnt++] = cnt;
#endif
                    wspi_stop1();
#if RAW_OUT_MODE == 1
                    dump_buf_8bytes(rawbuf[cur_buf], RAWBUFSIZE);
#endif
                    dma_channel_wait_for_finish_blocking(adcdch);
                    adc_run(false);
                    dma_channel_configure(adcdch,
                                          &adcdcfg,
                                          adcbuf,
                                          &adc_hw->fifo,
                                          adcsamp,
                                          false);
#if RAW16_MODE == 1
                    // uint16_t ntc = adc_read();
                    // uint16_t ntc = adc_read_median();
                    // uint16_t ntc = adc_read_trimmed_mean();
                    if (shutter_is_on) {
                        uint16_t ntc =
                            adc_read_trimmed_mean_from_buf(adcbuf, adcsamp);
                        rawbuf[cur_buf][RAWBUFSIZETOTAL - 2] =
                            (uint8_t)((ntc >> 8) & 0xFF);
                        rawbuf[cur_buf][RAWBUFSIZETOTAL - 1] =
                            (uint8_t)(ntc & 0xFF);
                        shutter_off();
                        shutter_is_on = false;
                    } else {
                        rawbuf[cur_buf][RAWBUFSIZETOTAL - 2] =
                            rawbuf[cur_buf][RAWBUFSIZETOTAL - 1] = 0x00;
                        shutter_on();
                        shutter_is_on = true;
                    }
                    // gpio_put(IO_LED, !gpio_get(IO_LED));

                    {
                        uint16_t vtemp1 = raw16_vtemp_avg(vtemps[0]);
                        // uint16_t vtemp2 = raw16_vtemp_avg(vtemps[1]);
                        // uint32_t vtemp_total = vtemp1;
                        // vtemp_total += vtemp2;
                        // uint32_t vtemp_avg = vtemp_total / 2;
                        // float vtemp = 25.0f + (float)(vtemp_avg - 8192)
                        // / 70.0f;
                        float vtemp = 25.0f + (float)(vtemp1 - 8192) / 70.0f;
                        println(" %0.4f", vtemp);
                    }

                    // uint64_t vtemp_total = 0;
                    // bool vtemps_chk = true;
                    // for (uint32_t i = 0; i < SENSOR_H / 2; ++i) {
                    //     uint16_t vtemp_u16 = raw16_vtemp_avg(vtemps[i]);
                    //     if (vtemp_u16 == 0) {
                    //         vtemps_chk = false;
                    //         break;
                    //     }
                    //     vtemp_total += vtemp_u16;
                    // }
                    // if (vtemps_chk) {
                    //     uint64_t vtemp_avg = vtemp_total / (SENSOR_H / 2);
                    //     float vtemp = 25.0f + (float)(vtemp_avg - 8192)
                    //     / 70.0f; println("%0.4f", vtemp);
                    // } else {
                    //     println("chkf");
                    // }
                    // memset(vtemps, 0x00, SENSOR_H * 4);

                    // bool vtemps_chk = true;
                    // uint64_t vtemp_total = 0;
                    // for (uint32_t i = 0; i < SENSOR_H; ++i) {
                    //     if (vtemps[i] == 0) {
                    //         vtemps_chk = false;
                    //         break;
                    //     }
                    //     vtemps_chk += vtemps[i];
                    // }
                    // if (vtemps_chk) {
                    //     uint64_t vtemp_avg = vtemp_total / SENSOR_H;
                    //     float vtemp = 25.0f + (float)(vtemp_avg - 8192)
                    //     / 70.0f; println("%0.4f", vtemp);
                    // } else {
                    //     println("chkf");
                    // }
                    // memset(vtemps, 0x00, SENSOR_H * sizeof(uint16_t));
#endif
                    if (cur_buf >= 0) {
                        __dmb();
                        rawbuf_state[cur_buf] = RAWBUF_READY;
                        rawbuf_last_ready = cur_buf;
                        __sev();
                    }

                    cur_buf = -1;
                    lidx = -1;

                    while (true) {
                        next_buf = rawbuf_find_free();
                        if (next_buf >= 0) {
                            rawbuf_state[next_buf] = RAWBUF_WRITING;
                            memset(rawbuf[next_buf], 0x00, RAWBUFSIZETOTAL);
                            __dmb();
                            break;
                        }
                        __wfe();
                    }

                    wspi_reinit1();
                } else {
                    continue;
                }
            } else {
                continue;
            }
        next_rx:
            rxring.ridx = (rxring.ridx + 1) % BUFCOUNT;
#if TEST_MAX_CNT != TEST_MAX_CNT_MAX
            cnt++;
            if (cnt >= TEST_MAX_CNT) {
                break;
            }
#endif
        }
    }

#if TEST_MAX_CNT != TEST_MAX_CNT_MAX
    println("total: %llu head: %llu tail: %llu body: %llu",
            cnt,
            headcnt,
            tailcnt,
            bodycnt);
    print("head pos: ");
    for (int i = 0; i < POS_NUM; ++i) {
        print("%llu ", headpos[i]);
    }
    println("");
    print("tail pos: ");
    for (int i = 0; i < POS_NUM; ++i) {
        print("%llu ", tailpos[i]);
    }
    println("");
    print("body cnt: ");
    for (int i = 0; i < POS_NUM; ++i) {
        print("%llu ", bodycnts[i]);
    }
    println("");
#endif

    errh(nullptr);
    return 0;
}

int sclk_io()
{
    gpio_init(IO_SCLK);
    gpio_set_dir(IO_SCLK, true);
    gpio_init(IO_LED);
    gpio_set_dir(IO_LED, true);

    gpio_put(IO_SCLK, 1);
    gpio_put(IO_LED, 1);

    while (true) {
        gpio_put(IO_SCLK, !gpio_get(IO_SCLK));
        gpio_put(IO_LED, !gpio_get(IO_LED));
        sleep_ms(500);
        tight_loop_contents();
    }

    return 0;
}

void uart_swb(uint32_t baud)
{
    if (baud == curr_uartb || baud == 0) {
        return;
    }

    uart_deinit(OUART);
    gpio_set_function(IO_OTX, GPIO_FUNC_UART);
    gpio_set_function(IO_ORX, GPIO_FUNC_UART);
    uart_init(OUART, baud);
    uart_set_hw_flow(OUART, false, false);
    uart_set_format(OUART, 8, 1, UART_PARITY_NONE);

    curr_uartb = baud;
}

void uart_poll(uint32_t (*sel)(uint8_t))
{
    if (!uart_is_readable(OUART)) {
        return;
    }

    uint8_t cmd = uart_getc(OUART);
    uint32_t baud = sel(cmd);
    uart_swb(baud);
}

enum BUartMode : uint32_t {
    BCMD11 = 9600,
    BCMD22 = 115200,
    BCMD33 = 921600,
};

uint32_t baud_get(uint8_t cmd)
{
    uint32_t baud = 0;
    switch (cmd) {
    case 0x11:
        baud = BCMD11;
        break;
    case 0x22:
        baud = BCMD22;
        break;
    case 0x33:
        baud = BCMD33;
        break;
    default:
        break;
    };
    return baud;
}

int baud_sel()
{
    set_sys_clock_khz(160000, true);
    sleep_ms(10);
    clock_configure(clk_peri,
                    0,
                    CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                    160 * MHZ,
                    160 * MHZ);
    extern void dutx_init();
    dutx_init();

    println("MODE: %s", __PRETTY_FUNCTION__);
    println("==========start==========");
    gpio_init(IO_LED);
    gpio_set_dir(IO_LED, true);

    curr_uartb = BCMD11;
    uart_deinit(OUART);
    gpio_set_function(IO_OTX, GPIO_FUNC_UART);
    gpio_set_function(IO_ORX, GPIO_FUNC_UART);
    uart_init(OUART, curr_uartb);
    uart_set_hw_flow(OUART, false, false);
    uart_set_format(OUART, 8, 1, UART_PARITY_NONE);

    const char* string = "running...\r\n";
    while (true) {
        uart_poll(baud_get);
        uart_write_blocking(OUART, (const uint8_t*)string, strlen(string));
        sleep_ms(1000);
    }

    errh(nullptr);
    return 0;
}

int adc_func()
{
    set_sys_clock_khz(160000, true);
    sleep_ms(10);
    clock_configure(clk_peri,
                    0,
                    CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                    160 * MHZ,
                    160 * MHZ);
    extern void dutx_init();
    dutx_init();

    println("MODE: %s", __PRETTY_FUNCTION__);
    println("==========start==========");
    gpio_init(IO_LED);
    gpio_set_dir(IO_LED, true);
    adc_init();
    adc_gpio_init(IO_NTC);
    adc_select_input(0);

    while (true) {
        uint16_t value = adc_read();
        println("%llu", (uint64_t)value);
        gpio_put(IO_LED, !gpio_get(IO_LED));
        sleep_ms(1000);
    }

    errh(nullptr);
    return 0;
}

int shutter_func()
{
    set_sys_clock_khz(160000, true);
    sleep_ms(10);
    clock_configure(clk_peri,
                    0,
                    CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                    160 * MHZ,
                    160 * MHZ);
    extern void dutx_init();
    dutx_init();

    println("MODE: %s", __PRETTY_FUNCTION__);
    println("==========start==========");
    gpio_init(IO_LED);
    gpio_set_dir(IO_LED, true);
    gpio_init(IO_SVMS);
    gpio_set_dir(IO_SVMS, true);
    gpio_put(IO_SVMS, 0);
    gpio_init(IO_SVM1);
    gpio_set_dir(IO_SVM1, true);
    gpio_put(IO_SVM1, 0);
    gpio_init(IO_SVM2);
    gpio_set_dir(IO_SVM2, true);
    gpio_put(IO_SVM2, 0);

    gpio_put(IO_LED, 0);
    gpio_put(IO_SVMS, 1);
    gpio_put(IO_SVM1, 1);
    gpio_put(IO_SVM2, 0);
    sleep_ms(5000);
    gpio_put(IO_SVMS, 0);
    gpio_put(IO_SVM1, 0);
    gpio_put(IO_SVM2, 0);
    gpio_put(IO_LED, 1);

    while (true) {
        sleep_ms(1000);
    }

    errh(nullptr);
    return 0;
}

void __not_in_flash_func(call_flash_range_erase)(void* param)
{
    uint32_t offset = (uint32_t)param;
    flash_range_erase(offset, FLASH_SECTOR_SIZE);
}

void __not_in_flash_func(call_flash_range_program)(void* param)
{
    uintptr_t* p = (uintptr_t*)param;
    uint32_t offset = p[0];
    const uint8_t* data = (const uint8_t*)p[1];

    flash_range_program(offset, data, FLASH_PAGE_SIZE);
}

int flash_func()
{
#if MAIN_FUNC_MODE == MAIN_FUNC_MODE_FLASH
    set_sys_clock_khz(160000, true);
    sleep_ms(10);
    clock_configure(clk_peri,
                    0,
                    CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                    160 * MHZ,
                    160 * MHZ);
    extern void dutx_init();
    dutx_init();

    println("MODE: %s", __PRETTY_FUNCTION__);
    println("==========start==========");
    gpio_init(IO_LED);
    gpio_set_dir(IO_LED, true);
    flash_safe_execute_core_init();

    multicore_launch_core1(core1_rawbuf_consumer);

    println("read");
    dump_buf_8bytes(flash_target_contents, FLASH_PAGE_SIZE);
    println("done");

    uint8_t random_data[FLASH_PAGE_SIZE];
    for (uint32_t i = 0; i < FLASH_PAGE_SIZE; ++i) {
        random_data[i] = rand() >> 16;
    }
    println("Gen data:");
    dump_buf_8bytes(random_data, FLASH_PAGE_SIZE);

    println("erase");
    int rc = flash_safe_execute(
        call_flash_range_erase, (void*)FLASH_TARGET_OFFSET, FLASH_SECTOR_SIZE);
    if (rc != PICO_OK) {
        eprintln("rc = %d", rc);
        errh("erase failed");
    } else {
        println("erase done");
    }

    println("read back");
    dump_buf_8bytes(flash_target_contents, FLASH_PAGE_SIZE);
    println("done");

    println("program");
    uintptr_t params[] = {FLASH_TARGET_OFFSET, (uintptr_t)random_data};
    rc =
        flash_safe_execute(call_flash_range_program, params, FLASH_SECTOR_SIZE);
    if (rc != PICO_OK) {
        eprintln("rc = %d", rc);
        errh("program failed");
    } else {
        println("program done");
    }

    dutx_init();
    println("read back");
    dump_buf_8bytes(flash_target_contents, FLASH_PAGE_SIZE);
    println("done");

    bool mismatch = false;
    for (uint32_t i = 0; i < FLASH_PAGE_SIZE; ++i) {
        if (random_data[i] != flash_target_contents[i]) {
            mismatch = true;
            break;
        }
    }
    if (mismatch) {
        eprintln("program failed");
    } else {
        println("program succ");
    }

    while (true) {
        sleep_ms(1000);
        gpio_put(IO_LED, !gpio_get(IO_LED));
    }

#endif
    errh(nullptr);
    return 0;
}
