#include <bsp/board_api.h>
#include <cmap/cmap.h>
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
#include <hardware/watchdog.h>
#include <init.h>
#include <led.h>
#include <math.h>
#include <memory.h>
#include <pico/flash.h>
#include <pico/multicore.h>
#include <pico/platform/common.h>
#include <pico/platform/sections.h>
#include <pico/stdlib.h>
#include <pico/time.h>
#include <pico/types.h>
#include <pico/unique_id.h>
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
int uvc_func();
int cdc_func();
int spi_init_40MHz2_noinit();
int baud_sel();
int adc_func();
int shutter_func();
int flash_func();
int gpio0_func();
int uvc_max_func();
int board_id_func();
int board_id_board_func();

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
    return uvc_func();
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
#elif MAIN_FUNC_MODE == MAIN_FUNC_MODE_GPIO0
    return gpio0_func();
#elif MAIN_FUNC_MODE == MAIN_FUNC_MODE_UVC_MAX
    return uvc_max_func();
#elif MAIN_FUNC_MODE == MAIN_FUNC_MODE_BOARD_ID
    return board_id_func();
#elif MAIN_FUNC_MODE == MAIN_FUNC_MODE_BOARD_ID_BOARD
    return board_id_board_func();
#endif
}

int spi_init_25mhz()
{
#if MAIN_FUNC_MODE == MAIN_FUNC_MODE_SPI_INIT_25MHZ
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
#endif

    errh(nullptr);
    return 0;
}

int ovclk_240mhz()
{
#if MAIN_FUNC_MODE == MAIN_FUNC_MODE_240MHZ
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
#endif
    errh(nullptr);
    return 0;
}

int spi_init_40MHz()
{
#if MAIN_FUNC_MODE == MAIN_FUNC_MODE_SPI_INIT_40MHZ
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
#endif

    errh(nullptr);
    return 0;
}

#if MAIN_FUNC_MODE == MAIN_FUNC_MODE_160M_UVC
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
#endif

int uvc_func()
{
#if MAIN_FUNC_MODE == MAIN_FUNC_MODE_160M_UVC
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
#if MAIN_FUNC_MODE == MAIN_FUNC_MODE_160M_CDC
    set_sys_clock_khz(160000, true);
    sleep_ms(10);
    clock_configure(clk_peri,
                    0,
                    CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                    160 * MHZ,
                    160 * MHZ);
    extern void dutx_init();
    dutx_init();

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
#if MAIN_FUNC_MODE == MAIN_FUNC_MODE_SPI_INIT_40MHZ_2
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

#    if RXPKTDBG == 1
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
#    endif

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

#    if RXPKTDBG == 1
            dma_channel_configure(
                sdch, &clkdcfg, &spi_get_hw(spi1)->dr, &Z, 0xffffffff, true);
#    endif
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
#endif

    errh(nullptr);
    return 0;
}

int spi_init_40MHz2_noinit()
{
#if MAIN_FUNC_MODE == MAIN_FUNC_MODE_SPI_INIT_40MHZ_NOINIT
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
#endif

    errh(nullptr);
    return 0;
}

int spi_init_40MHz3()
{
#if MAIN_FUNC_MODE == MAIN_FUNC_MODE_SPI_INIT_40MHZ_3
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
#endif

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
static enum UartOutMode ouomode = UART_OUT_MODE_DEF;

static inline __attribute__((always_inline)) void rawbuf_init(void)
{
    for (int i = 0; i < RAWBUFCNT; i++) {
        rawbuf_state[i] = RAWBUF_FREE;
    }
    rawbuf_last_ready = -1;
}

static inline __attribute__((always_inline)) int rawbuf_find_free(void)
{
    for (int i = 0; i < RAWBUFCNT; i++) {
        if (rawbuf_state[i] == RAWBUF_FREE) {
            return i;
        }
    }
    return -1;
}

static inline __attribute__((always_inline)) void rawbuf_copy_line(
    int buf_idx, int line, const uint8_t* src, size_t len)
{
    if (line >= SENSOR_H) {
        return;
    }

    if (ouomode == UOM_RAW_U16) {
        const size_t line_stride = SENSOR_W * 2 + 4;
        uint8_t* dst = &rawbuf[buf_idx][line * line_stride];
        memcpy(dst, src, SENSOR_W * 2);
        memcpy(dst + SENSOR_W * 2, src + SENSOR_W * 2 + 4, 4);
    } else if (ouomode == UOM_CALI_U8) {
        uint8_t* dst = &rawbuf[buf_idx][line * SENSOR_W * 2];
        memcpy(dst, src, len);
    }
}

static inline __attribute__((always_inline)) int rawbuf_pick_ready()
{
    int idx = rawbuf_last_ready;

    if (idx >= 0 && rawbuf_state[idx] == RAWBUF_READY) {
        return idx;
    }

    for (int i = 0; i < RAWBUFCNT; i++) {
        if (rawbuf_state[i] == RAWBUF_READY) {
            return i;
        }
    }

    return -1;
}

#define HIST_BUCKETS 64
#define HIST_SHIFT   8 /* 14bit >> 8 = 6bit bucket */

static inline __attribute__((always_inline)) uint16_t be16_to_u14(
    const uint8_t* p)
{
    uint16_t v;
    v = ((uint16_t)p[0] << 8) | p[1];
    return v & 0x3fff;
}

static inline __attribute__((always_inline)) void calc_percentile_u14_be(
    const uint8_t* raw, size_t px_cnt, uint16_t* low, uint16_t* high)
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

    if (*high <= *low) {
        *high = *low + 1;
    }
}

static inline __attribute__((always_inline)) uint8_t norm_u14_to_u8(
    uint16_t v, uint16_t low, uint16_t high)
{
    uint32_t t;

    if (v <= low) {
        return 0;
    }

    if (v >= high) {
        return 254;
    }

    t = (uint32_t)(v - low) * 254;
    t /= (high - low);

    if (t > 254) {
        t = 254;
    }

    return (uint8_t)t;
}

enum tx_buf_state {
    TXBUF_FREE = 0,
    TXBUF_FILLING,
    TXBUF_READY,
    TXBUF_SENDING,
};

#define PX_CNT     (SENSOR_W * SENSOR_H)
#define TX_PAYLOAD (RAWBUFSIZETOTAL + 2)
#define TXBUF_CNT  3
static int uart_tx_dma_chan = -1;
static uint8_t norm_buf[TXBUF_CNT][TX_PAYLOAD];
static int tx_buf_idx;
static volatile int dma_busy;
static int buf_idx;
static volatile enum tx_buf_state tx_state[TXBUF_CNT];
static volatile int newest_ready = -1;
static volatile int sending = -1;
#define CALIBUF_CNT       2
#define CALIBUFSIZE       RAWBUFSIZE
#define CALIBUF_IMG_L_IDX 0
#define CALIBUF_IMG_H_IDX 1
static uint8_t caliidx = CALIBUF_IMG_L_IDX;
static uint8_t calibuf[CALIBUF_CNT][CALIBUFSIZE];
static enum CaliMode cmode = CALIMODE_DEF;
static const uint16_t* calibg = nullptr;
static const float* caligain = nullptr;
static const struct BadPt* calibadpt = nullptr;

static inline __attribute__((always_inline)) void normalize_frame_be(
    uint8_t* raw, uint8_t* out)
{
    uint16_t low, high;
    size_t i;

    calc_percentile_u14_be(raw, PX_CNT, &low, &high);

    out[0] = 0xff;

    for (i = 0; i < PX_CNT; i++) {
        uint16_t v = be16_to_u14(&raw[i * 2]);
        out[i + 1] = norm_u14_to_u8(v, low, high);
    }
}

static inline __attribute__((always_inline)) void calc_percentile_u14(
    const uint16_t* raw14, size_t px_cnt, uint16_t* low, uint16_t* high)
{
    uint32_t hist[HIST_BUCKETS] = {0};
    size_t i;
    uint32_t acc;
    uint32_t p1_cnt = px_cnt / 100;
    uint32_t p99_cnt = px_cnt - p1_cnt;

    for (i = 0; i < px_cnt; i++) {
        hist[raw14[i] >> HIST_SHIFT]++;
    }

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

    if (*high <= *low) {
        *high = *low + 1;
    }
}

static inline __attribute__((always_inline)) uint16_t gray14_to_bin(uint16_t g)
{
    g &= 0x3fff;
    g ^= g >> 1;
    g ^= g >> 2;
    g ^= g >> 4;
    g ^= g >> 8;
    return g & 0x3fff;
}

static inline __attribute__((always_inline)) uint16_t clip_u16(int32_t v)
{
    if (v < 0) {
        return 0;
    }
    if (v > 65535) {
        return 65535;
    }
    return (uint16_t)v;
}

static inline __attribute__((always_inline)) void raw16_decode_to_normbuf(
    const uint8_t* raw, uint8_t* out, size_t size)
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

static inline __attribute__((always_inline)) int cali_ref_stream(
    const uint8_t* raw_h,
    const uint8_t* raw_l,
    float* gain_map,
    struct BadPt* badpts,
    const uint8_t badpts_size)
{
    const size_t line_stride = SENSOR_W * 2;
    const size_t px_per_line = SENSOR_W;
    const uint8_t* rawl_src = raw_l;
    const uint8_t* rawh_src = raw_h;

    float diff_sum = 0.0f;
    size_t diff_count = 0;

    for (uint8_t i = 0; i < badpts_size; ++i) {
        badpts[i].x = 0xFF;
        badpts[i].y = 0xFF;
    }

    for (size_t y = 0; y < SENSOR_H; ++y) {
        const uint8_t* line_h = rawh_src + y * line_stride;
        const uint8_t* line_l = rawl_src + y * line_stride;

        size_t curr_bad_n = 0;
        uint8_t curr_bad_x[SENSOR_W + 2];

        for (size_t x = 0; x < SENSOR_W; ++x) {
            const uint8_t* src_h = line_h + x * 2;
            const uint8_t* src_l = line_l + x * 2;

            uint16_t h = be16_to_u14(src_h);
            uint16_t l = be16_to_u14(src_l);

            int32_t diff = (int32_t)h - (int32_t)l;
            if (diff == 0) {
                diff = 1;
            }
            diff_sum += diff;
            diff_count++;

            if (gain_map != nullptr) {
                gain_map[y * px_per_line + x] = (float)diff;
            }

#if CALI_PASS_MODE != 1
            if (y == 0 || y == SENSOR_H - 1 || x == 0 || x == SENSOR_W - 1) {
                if (diff < 0) {
                    return -1;
                }
            }
#endif
        }
    }

#if CALI_PASS_MODE != 1
    if (diff_count == 0) {
        return -2;
    }
#else
    if (diff_count == 0) {
        diff_count = 1;
    }
#endif
    float avg_diff = diff_sum / (float)diff_count;

    size_t bad_idx = 0;
    if (gain_map != nullptr) {
        for (size_t y = 0; y < SENSOR_H; ++y) {
            for (size_t x = 0; x < px_per_line; ++x) {
                float diff = gain_map[y * px_per_line + x];
                gain_map[y * px_per_line + x] = avg_diff / diff;
                if (diff < avg_diff * 0.6) {
                    if (bad_idx >= badpts_size) {
#if CALI_PASS_MODE != 1
                        return -3;
#else
                        continue;
#endif
                    }
                    badpts[bad_idx].y = y;
                    badpts[bad_idx].x = x;
                    bad_idx++;
                }
            }
        }
    }

#if CALI_PASS_MODE != 1
    for (size_t i = 0; i < bad_idx - 1; ++i) {
        int dy = (int)badpts[i].y - (int)badpts[i + 1].y;
        int dx = (int)badpts[i].x - (int)badpts[i + 1].x;
        if (abs(dy) <= 1 && abs(dx) <= 1) {
            return -4;
        }
    }
#endif

    return 0;
}

static inline __attribute__((always_inline)) void be_u8_to_le_u16_inplace(
    uint8_t* buf, size_t len)
{
    if (!buf || len < 2) {
        return;
    }

    for (size_t i = 0; i + 1 < len; i += 2) {
        uint8_t tmp = buf[i];
        buf[i] = buf[i + 1];
        buf[i + 1] = tmp;
    }
}

void __not_in_flash_func(__flash_write2__)(void* param)
{
    const struct FlashOpInfo* opinfo = (const struct FlashOpInfo*)param;
    if (opinfo == nullptr) {
        return;
    }
    uint32_t ofs = FLASH_TARGET_OFFSET + opinfo->ofs;
    flash_range_erase(ofs, opinfo->size);
    flash_range_program(ofs, opinfo->data, opinfo->size);
}

static inline __attribute__((always_inline)) bool flash_write2(
    const struct FlashOpInfo* flashop)
{
    static uint8_t buffer[FLASH_SECTOR_SIZE];
    int rc = -1;
    const uint32_t fsector = flashop->size / FLASH_SECTOR_SIZE;
    const uint32_t lsector = flashop->size % FLASH_SECTOR_SIZE;
    const uint8_t* data = flash_target_contents + flashop->ofs;

    for (uint32_t i = 0; i < fsector; ++i) {
        memcpy(
            buffer, flashop->data + i * FLASH_SECTOR_SIZE, FLASH_SECTOR_SIZE);
        struct FlashOpInfo op;
        op.ofs = flashop->ofs + i * FLASH_SECTOR_SIZE;
        op.data = buffer;
        op.size = FLASH_SECTOR_SIZE;
        rc = flash_safe_execute(__flash_write2__, (void*)&op, 500);
        if (rc != PICO_OK) {
            eprintln("flash failed, rc = %d", rc);
            return false;
        }
        const uint8_t* flashdata = data + i * FLASH_SECTOR_SIZE;
        for (uint32_t j = 0; j < FLASH_SECTOR_SIZE; ++j) {
            if (flashdata[j] != buffer[j]) {
                eprintln("flash check failed in %llu %02x != %02x",
                         (uint64_t)j,
                         flashdata[j],
                         buffer[j]);
                return false;
            }
        }
    }

    if (lsector != 0) {
        memset(buffer, 0xFF, FLASH_SECTOR_SIZE);
        memcpy(buffer, flashop->data + fsector * FLASH_SECTOR_SIZE, lsector);
        struct FlashOpInfo op;
        op.ofs = flashop->ofs + fsector * FLASH_SECTOR_SIZE;
        op.data = buffer;
        op.size = FLASH_SECTOR_SIZE;
        rc = flash_safe_execute(__flash_write2__, (void*)&op, 500);
        if (rc != PICO_OK) {
            eprintln("flash failed, rc = %d", rc);
            return false;
        }
        const uint8_t* flashdata = data + fsector * FLASH_SECTOR_SIZE;
        for (uint32_t j = 0; j < FLASH_SECTOR_SIZE; ++j) {
            if (flashdata[j] != buffer[j]) {
                eprintln("flash check failed in %llu %02x != %02x",
                         (uint64_t)j,
                         flashdata[j],
                         buffer[j]);
                return false;
            }
        }
    }

    return true;
}

static inline __attribute__((always_inline)) bool float_equal(float a, float b)
{
    const float abs_eps = FLOAT_ABS_EPS;
    const float rel_eps = FLOAT_REL_EPS;

    float diff = fabsf(a - b);
    if (diff <= abs_eps) {
        return true;
    }

    return diff <= rel_eps * fmaxf(fabsf(a), fabsf(b));
}

static inline __attribute__((always_inline)) bool start_cali()
{
    multicore_fifo_push_blocking(CORE0_EV0);
    while (multicore_fifo_rvalid()) {
        uint32_t cmd = multicore_fifo_pop_blocking();
        if (cmd == CORE1_EV0) {
            break;
        }
    }
    sleep_ms(100);

    println("start cali");

    float* gain_map = (float*)rawbuf;
    static struct BadPt badpts[MAX_BADPT];
    int caliret = cali_ref_stream(calibuf[CALIBUF_IMG_H_IDX],
                                  calibuf[CALIBUF_IMG_L_IDX],
                                  gain_map,
                                  badpts,
                                  MAX_BADPT);
    if (caliret < 0) {
        println("cali failed: ret = %d", caliret);
        return false;
    }

    const uint32_t gain_size = SENSOR_W * SENSOR_H * sizeof(float);
    const uint32_t badpt_size = MAX_BADPT * sizeof(struct BadPt);
    static struct FlashDataInfo fheader;
    memset(&fheader, 0x00, sizeof(fheader));
    fheader.has_data = FLASH_HAS_DATA_F;
    fheader.imgbg_ofs = FLASH_OFS_IMGBG;
    fheader.imgbg_size = CALIBUFSIZE;
    fheader.gain_ofs = FLASH_OFS_GAIN;
    fheader.gain_size = gain_size;
    fheader.badpts_ofs = FLASH_OFS_BADPTS;
    fheader.badpts_num = MAX_BADPT;
    fheader.badpts_size = badpt_size;

    const uint8_t header[2] = {0xff, 0xff};
    println("send img_l...");
    uart_write_blocking(OUART, header, sizeof(header));
    uart_write_blocking(OUART, calibuf[CALIBUF_IMG_L_IDX], CALIBUFSIZE);
    sleep_ms(5);

    println("send img_h...");
    uart_write_blocking(OUART, header, sizeof(header));
    uart_write_blocking(OUART, calibuf[CALIBUF_IMG_H_IDX], CALIBUFSIZE);
    sleep_ms(5);

    be_u8_to_le_u16_inplace(calibuf[CALIBUF_IMG_L_IDX], CALIBUFSIZE);
    println("flash: imgbg...");
    {
        uint8_t retry = 0;
        struct FlashOpInfo fop;
        fop.ofs = fheader.imgbg_ofs;
        fop.data = calibuf[CALIBUF_IMG_L_IDX];
        fop.size = fheader.imgbg_size;
    flash_imgbg_start:
        while (retry++ < FLASH_RETRY_MAX) {
            if (!flash_write2(&fop)) {
                goto flash_imgbg_start;
            }
            const uint16_t* data =
                (const uint16_t*)(calibuf[CALIBUF_IMG_L_IDX]);
            for (uint32_t i = 0; i < CALIBUFSIZE / sizeof(uint16_t); ++i) {
                if (data[i] != flash_imgbg[i]) {
                    eprintln(
                        "falsh: imgbg check failed ram %llu != %llu flash in "
                        "[%llu]",
                        (uint64_t)data[i],
                        (uint64_t)flash_imgbg[i],
                        (uint64_t)i);
                    goto flash_imgbg_start;
                }
            }
            break;
        }
        if (retry >= FLASH_RETRY_MAX) {
            eprintln("flash failed");
            return false;
        }
    }
    println("send img_bg...");
    uart_write_blocking(OUART, header, sizeof(header));
    uart_write_blocking(OUART, calibuf[CALIBUF_IMG_L_IDX], CALIBUFSIZE);
    sleep_ms(5);

    println("flash: gain...");
    {
        uint8_t retry = 0;
        struct FlashOpInfo fop;
        fop.ofs = fheader.gain_ofs;
        fop.data = (uint8_t*)gain_map;
        fop.size = fheader.gain_size;
    flash_gain_start:
        while (retry++ < FLASH_RETRY_MAX) {
            if (!flash_write2(&fop)) {
                goto flash_gain_start;
            }
            const float* data = gain_map;
            for (uint32_t i = 0; i < gain_size / sizeof(float); ++i) {
                if (!float_equal(data[i], flash_gain[i])) {
                    eprintln(
                        "falsh: check failed ram %0.2f != %0.2f flash in "
                        "[%llu]",
                        data[i],
                        flash_gain[i],
                        (uint64_t)i);
                    goto flash_gain_start;
                }
            }
            break;
        }
        if (retry >= FLASH_RETRY_MAX) {
            eprintln("flash failed");
            return false;
        }
    }
    println("send gain...");
    uart_write_blocking(OUART, header, sizeof(header));
    uart_write_blocking(OUART, (const uint8_t*)gain_map, gain_size);
    sleep_ms(5);

    println("flash badpts");
    {
        uint8_t retry = 0;
        struct FlashOpInfo fop;
        fop.ofs = fheader.badpts_ofs;
        fop.data = (uint8_t*)badpts;
        fop.size = fheader.badpts_size;
    flash_badpts_start:
        while (retry++ < FLASH_RETRY_MAX) {
            if (!flash_write2(&fop)) {
                goto flash_badpts_start;
            }
            const struct BadPt* data = badpts;
            for (uint32_t i = 0; i < fheader.badpts_num; ++i) {
                if (data[i].x != flash_badpt[i].x
                    || data[i].y != flash_badpt[i].y) {
                    eprintln(
                        "falsh: check failed ram %u:%u != %u:%u flash in "
                        "[%llu]",
                        data[i].x,
                        data[i].y,
                        flash_badpt[i].x,
                        flash_badpt[i].y,
                        (uint64_t)i);
                    goto flash_badpts_start;
                }
            }
            break;
        }
        if (retry >= FLASH_RETRY_MAX) {
            eprintln("flash failed");
            return false;
        }
    }
    println("send badpts...");
    uart_write_blocking(OUART, header, sizeof(header));
    uart_write_blocking(OUART, (const uint8_t*)badpts, badpt_size);
    sleep_ms(5);

    println("flash header");
    {
        uint8_t retry = 0;
        struct FlashOpInfo fop;
        fop.ofs = FLASH_OFS_INFO;
        fop.data = (uint8_t*)&fheader;
        fop.size = sizeof(fheader);
    flash_header_start:
        while (retry++ < FLASH_RETRY_MAX) {
            if (!flash_write2(&fop)) {
                goto flash_header_start;
            }
            if (0
                != memcmp(
                    &fheader, flash_header, sizeof(struct FlashDataInfo))) {
                eprintln("flash: check failed in header");
                dump_buf_8bytes((uint8_t*)&fheader,
                                sizeof(struct FlashDataInfo));
                dump_buf_8bytes((uint8_t*)flash_header,
                                sizeof(struct FlashDataInfo));
                goto flash_header_start;
            }
            break;
        }
        if (retry >= FLASH_RETRY_MAX) {
            eprintln("flash failed");
            return false;
        }
    }

    println("send finish...");
    const uint8_t finish[2] = {0x78, 0x78};
    uart_write_blocking(OUART, header, sizeof(header));
    uart_write_blocking(OUART, (const uint8_t*)finish, 2);
    sleep_ms(5);

    return true;
}

static inline __attribute__((always_inline)) bool dump_cli()
{
    multicore_fifo_push_blocking(CORE0_EV0);
    while (multicore_fifo_rvalid()) {
        uint32_t cmd = multicore_fifo_pop_blocking();
        if (cmd == CORE1_EV0) {
            break;
        }
    }
    sleep_ms(100);

    println("dump cli");

    if (flash_header->has_data != FLASH_HAS_DATA_F) {
        return false;
    }

    sleep_ms(500);

    const uint8_t header[2] = {0xff, 0xff};

    println("send img_bg...");
    uart_write_blocking(OUART, header, sizeof(header));
    uart_write_blocking(
        OUART, (const uint8_t*)flash_imgbg, flash_header->imgbg_size);
    sleep_ms(5);

    println("send gain...");
    uart_write_blocking(OUART, header, sizeof(header));
    uart_write_blocking(
        OUART, (const uint8_t*)flash_gain, flash_header->gain_size);
    sleep_ms(5);

    println("send badpts...");
    uart_write_blocking(OUART, header, sizeof(header));
    uart_write_blocking(
        OUART, (const uint8_t*)flash_badpt, flash_header->badpts_size);
    sleep_ms(5);

    println("send finish...");
    const uint8_t finish[2] = {0x78, 0x78};
    uart_write_blocking(OUART, header, sizeof(header));
    uart_write_blocking(OUART, (const uint8_t*)finish, 2);
    sleep_ms(5);

    return true;
}

static inline __attribute__((always_inline)) void binary_calib_frame(
    const uint8_t* raw,
    const uint16_t* calib,
    const float* calig,
    const struct BadPt* calip,
    uint8_t* out,
    size_t px_cnt)
{
    uint16_t low, high;
    static uint16_t raw14[PX_CNT];
    bool debad = false;

    if (calib != nullptr && calig == nullptr) {
        for (size_t i = 0; i < px_cnt; i++) {
            uint16_t r = be16_to_u14(&raw[i * 2]);
            r = gray14_to_bin(r);
            int32_t diff = (int32_t)r - (int32_t)calib[i];
            raw14[i] = clip_u16(diff);
        }
        debad = true;
    } else if (calib != nullptr && calig != nullptr) {
        for (size_t i = 0; i < px_cnt; i++) {
            uint16_t r = be16_to_u14(&raw[i * 2]);
            r = gray14_to_bin(r);
            int32_t diff = (int32_t)r - (int32_t)calib[i];
            raw14[i] = clip_u16(diff);
            raw14[i] = (uint16_t)(calig[i] * raw14[i]);
        }
        debad = true;
    } else {
        for (size_t i = 0; i < px_cnt; i++) {
            uint16_t r = be16_to_u14(&raw[i * 2]);
            raw14[i] = gray14_to_bin(r);
        }
    }
    if (debad) {
        // FIXME: 修复坏点
        (void)calip;
    }
    calc_percentile_u14(raw14, PX_CNT, &low, &high);

    out[0] = 0xff;
    for (int i = 0; i < PX_CNT; i++) {
        out[i + 1] = norm_u14_to_u8(raw14[i], low, high);
    }
}

static inline __attribute__((always_inline)) void binary_frame(
    const uint8_t* raw, uint8_t* out, size_t px_cnt)
{
#if 0
    for (size_t i = 0; i < px_cnt; i++) {
        uint16_t r = be16_to_u14(&raw[i * 2]);
        uint16_t pixu16 = gray14_to_bin(r);
        out[i * 2] = (uint8_t)((pixu16 >> 8) & 0xFF);
        out[i * 2 + 1] = (uint8_t)(pixu16 & 0xFF);
    }
#else
    uint8_t* dst = out;
    const size_t line_stride = SENSOR_W * 2;
    const size_t px_per_line = SENSOR_W;
    for (size_t y = 0; y < SENSOR_H; ++y) {
        const uint8_t* line = raw + y * line_stride;
        for (size_t x = 0; x < px_per_line; ++x) {
            const uint8_t* src = &line[x * 2];

            uint16_t r = be16_to_u14(src);
            r = gray14_to_bin(r);
            size_t idx = (y * px_per_line + x) * 2;
            dst[idx] = (uint8_t)(r >> 8);
            dst[idx + 1] = (uint8_t)(r & 0xFF);
        }
    }
#endif
}

static inline __attribute__((always_inline)) void normalize_frame_corrected_be(
    const uint8_t* raw, const uint16_t* calib, uint8_t* out)
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

static inline __attribute__((always_inline)) int txbuf_pick_free(void)
{
    for (int i = 0; i < TXBUF_CNT; i++) {
        if (tx_state[i] == TXBUF_FREE) {
            return i;
        }
    }

    for (int i = 0; i < TXBUF_CNT; i++) {
        if (tx_state[i] == TXBUF_READY) {
            return i;
        }
    }

    return -1;
}

static inline __attribute__((always_inline)) void uart_tx_dma_start(
    const uint8_t* buf, size_t len)
{
    dma_busy = 1;

    dma_channel_set_read_addr(uart_tx_dma_chan, buf, false);
    dma_channel_set_trans_count(uart_tx_dma_chan, len, true);
}

static inline __attribute__((always_inline)) void try_start_dma(void)
{
    if (dma_busy) {
        return;
    }

    int b = newest_ready;
    if (b < 0) {
        return;
    }

    if (tx_state[b] != TXBUF_READY) {
        return;
    }

    tx_state[b] = TXBUF_SENDING;
    newest_ready = -1;
    sending = b;

    __dmb();
    dma_busy = 1;

    if (ouomode == UOM_RAW_U16) {
        uart_tx_dma_start(norm_buf[b], TX_PAYLOAD);
    } else if (ouomode == UOM_CALI_U8) {
        uart_tx_dma_start(norm_buf[b], PX_CNT + 1);
    }
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

static inline __attribute__((always_inline)) void fill_txbuf(int tx_idx,
                                                             int raw_idx)
{
    if (ouomode == UOM_RAW_U16) {
        norm_buf[tx_idx][0] = 0xFF;
        norm_buf[tx_idx][1] = 0xFF;
        raw16_decode_to_normbuf(rawbuf[raw_idx], norm_buf[tx_idx], PX_CNT);
    } else if (ouomode == UOM_CALI_U8) {
        if (cmode == CM_OFF) {
            // extern const uint16_t calib[];
            // binary_calib_frame(rawbuf[raw_idx], calib, norm_buf[tx_idx],
            // PX_CNT);
            binary_calib_frame(rawbuf[raw_idx],
                               calibg,
                               caligain,
                               calibadpt,
                               norm_buf[tx_idx],
                               PX_CNT);
        } else if (cmode == CM_ON) {
            if (caliidx < CALIBUF_CNT) {
                binary_frame(rawbuf[raw_idx], calibuf[caliidx], PX_CNT);
            }
        }
    }
}

static uint32_t curr_uartb;
static bool flash_dm = false;
static inline __attribute__((always_inline)) uint32_t baud_get2(uint8_t cmd)
{
    uint32_t baud = 0;
    switch (cmd) {
    case 0x11:
        baud = UBM11;
        ouomode = UOM_CALI_U8;
        cmode = CM_OFF;
        flash_dm = false;
        break;
    case 0x22:
        baud = UBM22;
        ouomode = UOM_CALI_U8;
        cmode = CM_OFF;
        flash_dm = false;
        break;
    case 0x33:
        baud = UBM33;
        ouomode = UOM_CALI_U8;
        cmode = CM_OFF;
        flash_dm = false;
        break;
    case 0x44:
        baud = UBM44;
        ouomode = UOM_CALI_U8;
        cmode = CM_OFF;
        flash_dm = false;
        break;
    case 0x55:
        baud = UBM55;
        ouomode = UOM_CALI_U8;
        cmode = CM_OFF;
        flash_dm = false;
        break;
    case 0x66:
        baud = UBM66;
        ouomode = UOM_CALI_U8;
        cmode = CM_OFF;
        flash_dm = false;
        break;
    case 0x77:
        baud = UBM77;
        ouomode = UOM_CALI_U8;
        cmode = CM_OFF;
        flash_dm = false;
        break;
    case 0x88:
        baud = UBM88;
        ouomode = UOM_CALI_U8;
        cmode = CM_OFF;
        flash_dm = false;
        break;
    case 0xAA:
        baud = UBMAA;
        ouomode = UOM_RAW_U16;
        cmode = CM_OFF;
        flash_dm = false;
        break;
    case 0xBB:
        baud = UBMBB;
        ouomode = UOM_RAW_U16;
        cmode = CM_OFF;
        flash_dm = false;
        break;
    case 0xCC:
        baud = UBMCC;
        ouomode = UOM_CALI_U8;
        cmode = CM_ON;
        flash_dm = false;
        led_off();
        break;
    case 0xDD:
        baud = UBMDD;
        ouomode = UOM_CALI_U8;
        cmode = CM_OFF;
        flash_dm = true;
        break;
    default:
        break;
    };
    return baud;
}

static inline __attribute__((always_inline)) void ouart_init(const uint8_t cmd)
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

static inline __attribute__((always_inline)) void uart_poll2(
    uint32_t (*sel)(uint8_t))
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

static inline __attribute__((always_inline)) uint32_t now()
{
    return to_ms_since_boot(get_absolute_time());
}

enum {
    UVCBUF_FREE = 0,
    UVCBUF_FILLING,
    UVCBUF_READY,
    UVCBUF_SENDING,
};

#define UVC_BUF_CNT 2
static uint8_t uvc_buf[UVC_BUF_CNT][FRAME_SIZE];
static volatile uint8_t uvc_state[UVC_BUF_CNT];
static volatile int uvc_newest_ready = -1;
static volatile int uvc_sending = -1;
static volatile uint8_t uvc_tx_busy = 0;
static volatile uint32_t uvc_frame_num = 0;
static volatile uint32_t uvc_inv_ms = 1000 / FRAME_RATE;
static enum UartOutMode uvc_ouomode_bak = UART_OUT_MODE_DEF;

static inline __attribute__((always_inline)) void uvc_buf_init()
{
    for (int i = 0; i < UVC_BUF_CNT; ++i) {
        uvc_state[i] = UVCBUF_FREE;
    }

    uvc_newest_ready = -1;
    uvc_sending = -1;
    uvc_tx_busy = 0;
}

static inline __attribute__((always_inline)) int find_free_uvc_buf(void)
{
    for (int i = 0; i < UVC_BUF_CNT; ++i) {
        if (uvc_state[i] == UVCBUF_FREE) {
            return i;
        }
    }
    return -1;
}

static inline __attribute__((always_inline)) int find_ready_uvc_buf(void)
{
    int b = uvc_newest_ready;
    if (b >= 0 && uvc_state[b] == UVCBUF_READY) {
        return b;
    }
    return -1;
}

void tud_mount_cb()
{
    uvc_ouomode_bak = ouomode;
    ouomode = UOM_CALI_U8;
    dataomode = DOM_UVC;
}
void tud_umount_cb()
{
    dataomode = DOM_UART;
    ouomode = uvc_ouomode_bak;
}
void tud_resume_cb() {}
void tud_suspend_cb(bool remote_wakeup_en) { (void)remote_wakeup_en; }

static inline __attribute__((always_inline)) void fill_random_yuy2(uint8_t* buf,
                                                                   size_t len)
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

static inline __attribute__((always_inline)) void fill_smpte_yuy2(uint8_t* buf,
                                                                  size_t len)
{
    extern const uint8_t frame_FF0000[];
    extern const uint8_t frame_00FF00[];
    extern const uint8_t frame_0000FF[];
    extern const uint8_t frame_FFFFFF[];
    static uint8_t sel = 0;

    switch (sel) {
    case 0x01:
        memcpy(buf, frame_FF0000, len);
        break;
    case 0x02:
        memcpy(buf, frame_00FF00, len);
        break;
    case 0x03:
        memcpy(buf, frame_0000FF, len);
        break;
    default:
        sel = 0;
        memcpy(buf, frame_FFFFFF, len);
        break;
    }
    sel++;
}

static inline __attribute__((always_inline)) void fill_uvc_frame(
    const uint8_t* pixbuf, const uint8_t* cmap, uint8_t* buf, size_t len)
{
    if (!pixbuf || !cmap || !buf || len < 4) {
        return;
    }

    const uint8_t* src = pixbuf + 1;
    size_t out_idx = 0;

    while (out_idx + 3 < len) {
        uint8_t g0 = *src++;
        uint8_t g1 = *src++;

        uint8_t r0 = cmap[g0 * 3 + 0];
        uint8_t g0c = cmap[g0 * 3 + 1];
        uint8_t b0 = cmap[g0 * 3 + 2];

        uint8_t r1 = cmap[g1 * 3 + 0];
        uint8_t g1c = cmap[g1 * 3 + 1];
        uint8_t b1 = cmap[g1 * 3 + 2];

        uint8_t y0 =
            (uint8_t)(((66 * r0 + 129 * g0c + 25 * b0 + 128) >> 8) + 16);
        uint8_t y1 =
            (uint8_t)(((66 * r1 + 129 * g1c + 25 * b1 + 128) >> 8) + 16);

        uint8_t u = (uint8_t)(((-38 * r0 - 74 * g0c + 112 * b0 + -38 * r1
                                - 74 * g1c + 112 * b1 + 256)
                               >> 8)
                              + 128);
        uint8_t v = (uint8_t)(((112 * r0 - 94 * g0c - 18 * b0 + 112 * r1
                                - 94 * g1c - 18 * b1 + 256)
                               >> 8)
                              + 128);

        buf[out_idx++] = y0;
        buf[out_idx++] = u;
        buf[out_idx++] = y1;
        buf[out_idx++] = v;
    }
}

void tud_video_frame_xfer_complete_cb(uint_fast8_t ctl_idx,
                                      uint_fast8_t stm_idx)
{
    (void)ctl_idx;
    (void)stm_idx;
#if 0
    uvc_tx_busy = 0;
    ++uvc_frame_num;
#else
    int b = uvc_sending;
    if (b >= 0) {
        uvc_state[b] = UVCBUF_FREE;
        uvc_sending = -1;
    }

    __dmb();
    uvc_tx_busy = 0;
    ++uvc_frame_num;

    __sev();
#endif
}

int tud_video_commit_cb(uint_fast8_t ctl_idx,
                        uint_fast8_t stm_idx,
                        video_probe_and_commit_control_t const* parameters)
{
    (void)ctl_idx;
    (void)stm_idx;
    uvc_inv_ms = parameters->dwFrameInterval / 10000;
    return VIDEO_ERROR_NONE;
}

static inline __attribute__((always_inline)) void uvc_init() { tusb_init(); }

static inline __attribute__((always_inline)) void fake_uvc_fill(uint8_t* buf,
                                                                size_t size)
{
    int b = newest_ready;
    if (b < 0) {
        return;
    }

    if (tx_state[b] != TXBUF_READY) {
        return;
    }

    tx_state[b] = TXBUF_SENDING;
    newest_ready = -1;
    sending = b;

    __dmb();

    fill_random_yuy2(buf, size);

    b = sending;
    tx_state[b] = TXBUF_FREE;
    __dmb();
    sending = -1;
    __sev();
}

static inline __attribute__((always_inline)) void smpte_uvc_fill(uint8_t* buf,
                                                                 size_t size)
{
    int b = newest_ready;
    if (b < 0) {
        return;
    }

    if (tx_state[b] != TXBUF_READY) {
        return;
    }

    tx_state[b] = TXBUF_SENDING;
    newest_ready = -1;
    sending = b;

    __dmb();

    fill_smpte_yuy2(buf, size);

    b = sending;
    tx_state[b] = TXBUF_FREE;
    __dmb();
    sending = -1;
    __sev();
}

static inline __attribute__((always_inline)) void uvc_fill(uint8_t* buf,
                                                           size_t size)
{
    int b = newest_ready;
    if (b < 0) {
        return;
    }

    if (tx_state[b] != TXBUF_READY) {
        return;
    }

    tx_state[b] = TXBUF_SENDING;
    newest_ready = -1;
    sending = b;

    __dmb();

    fill_uvc_frame(norm_buf[b], colormap_hot, buf, size);

    b = sending;
    tx_state[b] = TXBUF_FREE;
    __dmb();
    sending = -1;
    __sev();
}

static inline __attribute__((always_inline)) void uvc_prepare_next(void)
{
    int src = newest_ready;
    if (src < 0) {
        return;
    }

    if (tx_state[src] != TXBUF_READY) {
        return;
    }

    int b = find_free_uvc_buf();
    if (b < 0) {
        return;
    }

    tx_state[src] = TXBUF_SENDING;
    newest_ready = -1;

    uvc_state[b] = UVCBUF_FILLING;
    __dmb();

    fill_uvc_frame(norm_buf[src], colormap_hot, uvc_buf[b], FRAME_SIZE);

    __dmb();
    uvc_state[b] = UVCBUF_READY;
    uvc_newest_ready = b;

    tx_state[src] = TXBUF_FREE;
    __dmb();

    __sev();
}

#if 0
uint32_t uvc_task(void (*fill)(uint8_t*, size_t))
{
    static uint8_t frame_buffer[FRAME_SIZE];
    static uint32_t start_ms = 0;
    static uint32_t already_sent = 0;

    if (fill == nullptr) {
        return UINT32_MAX;
    }

    if (!tud_video_n_streaming(0, 0)) {
        already_sent = 0;
        uvc_frame_num = 0;
        return UINT32_MAX;
    }

    if (!already_sent) {
        already_sent = 1;
        uvc_tx_busy = 1;
        start_ms = now();
        fill(frame_buffer, FRAME_SIZE);
        tud_video_n_frame_xfer(0, 0, (void*)frame_buffer, FRAME_SIZE);
    }

    uint32_t cur = now();
    if (cur - start_ms < uvc_inv_ms || uvc_tx_busy) {
        return UINT32_MAX;
    }
    start_ms += uvc_inv_ms;
    uvc_tx_busy = 1;
    fill(frame_buffer, FRAME_SIZE);
    tud_video_n_frame_xfer(0, 0, (void*)frame_buffer, FRAME_SIZE);

    return now() - cur;
}
#else
static inline __attribute__((always_inline)) uint32_t uvc_task(void)
{
    if (!tud_video_n_streaming(0, 0)) {
        return UINT32_MAX;
    }

    if (uvc_tx_busy) {
        return UINT32_MAX;
    }

    int b = find_ready_uvc_buf();
    if (b < 0) {
        return UINT32_MAX;
    }

    uvc_state[b] = UVCBUF_SENDING;
    uvc_sending = b;
    uvc_newest_ready = -1;

    __dmb();
    uvc_tx_busy = 1;

    tud_video_n_frame_xfer(0, 0, uvc_buf[b], FRAME_SIZE);

    return 0;
}

#endif

void core1_rawbuf_consumer()
{
    int idx;
    for (int i = 0; i < TXBUF_CNT; i++) {
        tx_state[i] = TXBUF_FREE;
    }

    newest_ready = -1;
    sending = -1;
    dma_busy = 0;
    struct GpioBtn boot;
    boot.gpio = 0;
    boot.event = 0;
    boot.last_change_ms = 0;
    boot.last_level = 1;
    boot.stable_level = 1;
    boot.press_ms = 0;
    gpio_init(boot.gpio);
    gpio_set_dir(boot.gpio, GPIO_IN);
    gpio_set_pulls(boot.gpio, true, false);

    println("Core1 consumer start");
    uvc_init();
    // button_init();
    flash_safe_execute_core_init();

    ouart_init(UART_BAUD_MODE_DEF);
    dma_busy = 0;
    buf_idx = 0;

#if CORE1_TIMEU_MODE == 1
#    define TIMEU_MAX_CNT 10
    const uint8_t timeu_max_cnt = TIMEU_MAX_CNT;
    uint8_t timeu_upoll_cnt = 0;
    uint8_t timeu_utask_cnt = 0;
    uint8_t timeu_vtask_cnt = 0;
    uint8_t timeu_bscan_cnt = 0;
    uint8_t timeu_upoll[TIMEU_MAX_CNT];
    uint8_t timeu_utask[TIMEU_MAX_CNT];
    uint8_t timeu_vtask[TIMEU_MAX_CNT];
    uint8_t timeu_bscan[TIMEU_MAX_CNT];
    memset(timeu_upoll, 0x00, TIMEU_MAX_CNT);
    memset(timeu_utask, 0x00, TIMEU_MAX_CNT);
    memset(timeu_vtask, 0x00, TIMEU_MAX_CNT);
    memset(timeu_bscan, 0x00, TIMEU_MAX_CNT);
#    undef TIMEU_MAX_CNT
#endif

    while (true) {
#if CORE1_TIMEU_MODE == 1
        uint32_t upoll_lt = now();
#endif
        uart_poll2(baud_get2);
#if CORE1_TIMEU_MODE == 1
        uint32_t upoll_dt = now() - upoll_lt;
        if (timeu_upoll_cnt < timeu_max_cnt && uvc_frame_num >= 2) {
            timeu_upoll[timeu_upoll_cnt++] = (uint8_t)upoll_dt;
        }
#endif
        if (flash_dm) {
            goto dump_start;
        }
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

#if CORE1_TIMEU_MODE == 1
        uint32_t utask_lt = now();
#endif
        if (dataomode == DOM_UVC) {
            uvc_prepare_next();
        }
        tud_task();
#if CORE1_TIMEU_MODE == 1
        uint32_t utask_dt = now() - utask_lt;
        if (timeu_utask_cnt < timeu_max_cnt && uvc_frame_num >= 2) {
            timeu_utask[timeu_utask_cnt++] = (uint8_t)utask_dt;
        }
#endif

        if (dataomode == DOM_UART) {
            if (cmode == CM_OFF) {
                try_start_dma();
            } else if (cmode == CM_ON) {
                if (caliidx < CALIBUF_CNT) {
                    button_scan(&boot);
                    uint8_t boot_ev = boot.event;
                    if (boot_ev) {
                        boot.event = 0;
                        if (boot_ev & GB_EV_RELEASE) {
                            caliidx++;
                            led_toggle();
                        }
                    }
                } else {
                    goto cali_start;
                }
            }
        } else if (dataomode == DOM_UVC) {
#if 0
            uint32_t vut = uvc_task(uvc_fill);
            // uvc_task(smpte_uvc_fill);
#else
            uvc_task();
#endif
#if CORE1_TIMEU_MODE == 1
            if (vut != UINT32_MAX) {
                uint32_t vtask_dt = vut;
                if (timeu_vtask_cnt < timeu_max_cnt && uvc_frame_num >= 2) {
                    timeu_vtask[timeu_vtask_cnt++] = (uint8_t)vtask_dt;
                }
            }
#endif

#if CORE1_TIMEU_MODE == 1
            uint32_t bscan_lt = now();
#endif
            button_scan(&boot);
            uint8_t boot_ev = boot.event;
            if (boot_ev) {
                boot.event = 0;
                if (boot_ev & GB_EV_RELEASE) {
                    led_toggle();
                    // TODO: cmap sw
                }
            }
#if CORE1_TIMEU_MODE == 1
            uint32_t bscan_dt = now() - bscan_lt;
            if (timeu_bscan_cnt < timeu_max_cnt && uvc_frame_num >= 2) {
                timeu_bscan[timeu_bscan_cnt++] = (uint8_t)bscan_dt;
            }
#endif
        }

        if (!did_work) {
            __wfe();
        }

#if CORE1_TIMEU_MODE == 1
        if (timeu_upoll_cnt == timeu_max_cnt) {
            uint32_t total = 0;
            for (uint8_t i = 0; i < timeu_upoll_cnt; ++i) {
                total += timeu_upoll[i];
            }
            float avg = total / (float)timeu_upoll_cnt;
            println("upoll: %0.2f", avg);
            timeu_upoll_cnt = 0xFF;
        }
        if (timeu_utask_cnt == timeu_max_cnt) {
            uint32_t total = 0;
            for (uint8_t i = 0; i < timeu_utask_cnt; ++i) {
                total += timeu_utask[i];
            }
            float avg = total / (float)timeu_utask_cnt;
            println("utask: %0.2f", avg);
            timeu_utask_cnt = 0xFF;
        }
        if (timeu_vtask_cnt == timeu_max_cnt) {
            uint32_t total = 0;
            for (uint8_t i = 0; i < timeu_vtask_cnt; ++i) {
                total += timeu_vtask[i];
            }
            float avg = total / (float)timeu_vtask_cnt;
            println("vtask: %0.2f", avg);
            timeu_vtask_cnt = 0xFF;
        }
        if (timeu_bscan_cnt == timeu_max_cnt) {
            uint32_t total = 0;
            for (uint8_t i = 0; i < timeu_bscan_cnt; ++i) {
                total += timeu_bscan[i];
            }
            float avg = total / (float)timeu_bscan_cnt;
            println("bscan: %0.2f", avg);
            timeu_bscan_cnt = 0xFF;
        }
#endif
    }

cali_start:
    if (start_cali()) {
        led_on_succ(CALI_LED_T_NUM);
    } else {
        led_on_failed(CALI_LED_T_NUM);
    }
    goto core1_restart;

dump_start:
    if (dump_cli()) {
        led_on_succ(CALI_LED_T_NUM);
    } else {
        led_on_failed(CALI_LED_T_NUM);
    }

    goto core1_restart;

core1_restart:
    watchdog_reboot(0, 0, 0);
    while (true) {
        tight_loop_contents();
    }
}

static inline __attribute__((always_inline)) void uart_tx_dma_wait_done()
{
    dma_channel_wait_for_finish_blocking(uart_tx_dma_chan);
    dma_busy = 0;
}

static inline __attribute__((always_inline)) void shutter_on()
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

static inline __attribute__((always_inline)) void shutter_off()
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

static inline __attribute__((always_inline)) uint16_t
adc_read_trimmed_mean_from_buf(const uint16_t* data, const uint32_t size)
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

        if (v < min) {
            min = v;
        }
        if (v > max) {
            max = v;
        }
    }

    sum -= min;
    sum -= max;

    return (uint16_t)(sum / (size - 2));
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
    clock_configure(clk_usb,
                    0,
                    CLOCKS_CLK_USB_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
                    48 * MHZ,
                    48 * MHZ);
    clock_configure(clk_adc,
                    0,
                    CLOCKS_CLK_ADC_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
                    48 * MHZ,
                    48 * MHZ);
    extern void dutx_init();
    dutx_init();
    pico_get_unique_board_id_string(uid, UID_LEN);
    flash_safe_execute_core_init();

    println("MODE: %s", __PRETTY_FUNCTION__);
    println("==========start==========");
    led_init();

    if (flash_header->has_data == FLASH_HAS_DATA_F) {
        calibg = flash_imgbg;
        // FIXME:
        caligain = flash_gain;
        calibadpt = flash_badpt;
    }

#define ADC_SAMPLES 1280
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
    bool shutter_is_on = false;

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

                    if (ouomode == UOM_RAW_U16) {
                        adc_run(true);
                        dma_channel_start(adcdch);
                    }

                    rawbuf_copy_line(cur_buf, lidx, &b->data[14], SENSOR_W * 2);
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

                    while (multicore_fifo_rvalid()) {
                        uint32_t cmd = multicore_fifo_pop_blocking();
                        if (cmd == CORE0_EV0) {
                            multicore_fifo_push_blocking(CORE1_EV0);
                            goto core0_stop;
                        }
                    }

                    if (ouomode == UOM_RAW_U16) {
                        dma_channel_wait_for_finish_blocking(adcdch);
                        adc_run(false);
                        dma_channel_configure(adcdch,
                                              &adcdcfg,
                                              adcbuf,
                                              &adc_hw->fifo,
                                              adcsamp,
                                              false);
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
                    }
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
core0_stop:
    while (true) {
        tight_loop_contents();
    }
    return 0;
}

int sclk_io()
{
#if MAIN_FUNC_MODE == MAIN_FUNC_MODE_SCLK_IO
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
#endif

    errh(nullptr);
    return 0;
}

#if MAIN_FUNC_MODE == MAIN_FUNC_MODE_BAUD_SEL
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
#endif

int baud_sel()
{
#if MAIN_FUNC_MODE == MAIN_FUNC_MODE_BAUD_SEL
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
#endif

    errh(nullptr);
    return 0;
}

int adc_func()
{
#if MAIN_FUNC_MODE == MAIN_FUNC_MODE_ADC
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
#endif

    errh(nullptr);
    return 0;
}

int shutter_func()
{
#if MAIN_FUNC_MODE == MAIN_FUNC_MODE_SHUTTER
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
#endif

    errh(nullptr);
    return 0;
}

#if MAIN_FUNC_MODE == MAIN_FUNC_MODE_FLASH
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

void __not_in_flash_func(__flash_write__)(void* param)
{
    const struct FlashOpInfo* opinfo = (const struct FlashOpInfo*)param;
    if (opinfo == nullptr) {
        return;
    }
    uint32_t ofs = FLASH_TARGET_OFFSET + opinfo->ofs;
    flash_range_erase(ofs, opinfo->size);
    flash_range_program(ofs, opinfo->data, opinfo->size);
}

bool flash_write(const struct FlashOpInfo* flashop)
{
    static uint8_t buffer[FLASH_SECTOR_SIZE];
    int rc = -1;
    const uint32_t fsector = flashop->size / FLASH_SECTOR_SIZE;
    const uint32_t lsector = flashop->size % FLASH_SECTOR_SIZE;
    const uint8_t* data = flash_target_contents + flashop->ofs;

    for (uint32_t i = 0; i < fsector; ++i) {
        println("flash #%llu...", (uint64_t)i);
        memcpy(
            buffer, flashop->data + i * FLASH_SECTOR_SIZE, FLASH_SECTOR_SIZE);
        for (uint32_t i = 0; i < FLASH_SECTOR_SIZE; ++i) {
            if (buffer[i] != 0x34) {
                println("[%llu] = %02x", (uint64_t)i, buffer[i]);
            }
        }
        struct FlashOpInfo op;
        op.ofs = flashop->ofs + i * FLASH_SECTOR_SIZE;
        op.data = buffer;
        op.size = FLASH_SECTOR_SIZE;
        rc = flash_safe_execute(__flash_write__, (void*)&op, 500);
        if (rc != PICO_OK) {
            eprintln("flash failed, rc = %d", rc);
            return false;
        }
        const uint8_t* flashdata = data + i * FLASH_SECTOR_SIZE;
        for (uint32_t j = 0; j < FLASH_SECTOR_SIZE; ++j) {
            if (flashdata[j] != buffer[j]) {
                eprintln("flash check failed in %llu %02x != %02x",
                         (uint64_t)j,
                         flashdata[j],
                         buffer[j]);
                return false;
            }
        }
    }

    if (lsector != 0) {
        println("flash lsector %llu bytes", (uint64_t)lsector);
        memset(buffer, 0xFF, FLASH_SECTOR_SIZE);
        memcpy(buffer, flashop->data + fsector * FLASH_SECTOR_SIZE, lsector);
        struct FlashOpInfo op;
        op.ofs = flashop->ofs + fsector * FLASH_SECTOR_SIZE;
        op.data = buffer;
        op.size = FLASH_SECTOR_SIZE;
        rc = flash_safe_execute(__flash_write__, (void*)&op, 500);
        if (rc != PICO_OK) {
            eprintln("flash failed, rc = %d", rc);
            return false;
        }
        const uint8_t* flashdata = data + fsector * FLASH_SECTOR_SIZE;
        for (uint32_t j = 0; j < FLASH_SECTOR_SIZE; ++j) {
            if (flashdata[j] != buffer[j]) {
                eprintln("flash check failed in %llu %02x != %02x",
                         (uint64_t)j,
                         flashdata[j],
                         buffer[j]);
                return false;
            }
        }
    }

    return true;
}

#endif

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
    sleep_ms(50);

    println("%d", __LINE__);

#    if 0
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
#    else

#        define FLASH_TEST_DATA_SIZE 81920
    // uint8_t random_data[FLASH_TEST_DATA_SIZE];
    uint8_t* random_data = malloc(FLASH_TEST_DATA_SIZE);
    const uint8_t value = 0x34;
    for (uint32_t i = 0; i < FLASH_TEST_DATA_SIZE; ++i) {
        // random_data[i] = (uint8_t)(rand() >> 8);
        random_data[i] = value;
    }
    struct FlashOpInfo testflashop;
    testflashop.ofs = 0x00;
    testflashop.data = random_data;
    testflashop.size = FLASH_TEST_DATA_SIZE;
    if (flash_write(&testflashop)) {
        println("flash succ");
    } else {
        eprintln("flash failed");
    }
    free(random_data);

    // const uint8_t* flashdata = flash_target_contents + testflashop.ofs;
    // for (uint32_t i = 0; i < FLASH_TEST_DATA_SIZE; ++i) {
    //     if (flashdata[i] != value) {
    //         eprintln("failed in %llu", (uint64_t)i);
    //         dump_buf_8bytes(&flashdata[i], 8);
    //         break;
    //     }
    // }

#    endif

    while (true) {
        sleep_ms(1000);
        gpio_put(IO_LED, !gpio_get(IO_LED));
    }
#endif

    errh(nullptr);
    return 0;
}

int gpio0_func()
{
#if MAIN_FUNC_MODE == MAIN_FUNC_MODE_GPIO0
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
    gpio_put(IO_LED, 1);

    gpio_init(IO_PLED);
    gpio_set_dir(IO_PLED, true);
    gpio_put(IO_PLED, 1);

    gpio_init(IO_BTN);
    gpio_set_dir(IO_BTN, false);
    gpio_pull_up(IO_BTN);
    // gpio_disable_pulls(IO_BTN);

    uint8_t cnt = 0;
    while (true) {
        bool value = gpio_get(IO_BTN);
        gpio_put(IO_LED, value);
        gpio_put(IO_PLED, value);
        tight_loop_contents();
        sleep_ms(100);
        cnt++;
        if (cnt >= 10) {
            println("GPIO0: %d", value);
            cnt = 0;
        }
    }
#endif

    errh(nullptr);
    return 0;
}

int uvc_max_func()
{
#if MAIN_FUNC_MODE == MAIN_FUNC_MODE_UVC_MAX
    set_sys_clock_khz(160000, true);
    sleep_ms(10);
    clock_configure(clk_peri,
                    0,
                    CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                    160 * MHZ,
                    160 * MHZ);
    clock_configure(clk_usb,
                    0,
                    CLOCKS_CLK_USB_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
                    48 * MHZ,
                    48 * MHZ);
    extern void dutx_init();
    dutx_init();

    println("MODE: %s", __PRETTY_FUNCTION__);
    println("==========start==========");
    uvc_init();

    while (true) {
        tud_task();
        uvc_task(fill_smpte_yuy2);
        tight_loop_contents();
    }

#endif

    errh(nullptr);
    return 0;
}

int board_id_func()
{
#if MAIN_FUNC_MODE == MAIN_FUNC_MODE_BOARD_ID
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

    flash_read_unique_id();
    println("Flash UID:");
    for (uint8_t i = 0; i < 8; ++i) {
        print("%02X ", uid[i]);
    }
    println("");

#endif
    errh(nullptr);
    return 0;
}

int board_id_board_func()
{
#if MAIN_FUNC_MODE == MAIN_FUNC_MODE_BOARD_ID_BOARD
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

    char id[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    pico_get_unique_board_id_string(id, sizeof(id));
    println("Board id: %s", id);

#endif
    errh(nullptr);
    return 0;
}
