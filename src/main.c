#include <bsp/board_api.h>
#include <cmap/cmap.h>
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
int shutter_func();
int flash_func();
int gpio0_func();
int uvc_max_func();
int board_id_func();
int board_id_board_func();

int blink_func()
{
#if MAIN_FUNC_MODE == MAIN_FUNC_MODE_BLINK
    gpio_init(IO_LED);
    gpio_init(IO_PLED);
    gpio_set_dir(IO_LED, true);
    gpio_set_dir(IO_PLED, true);
    gpio_put(IO_LED, 0);
    gpio_put(IO_PLED, 0);

    for (;;) {
        gpio_put(IO_LED, !gpio_get(IO_LED));
        gpio_put(IO_PLED, !gpio_get(IO_PLED));
        sleep_ms(500);
    }
#endif
    return 0;
}

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
#elif MAIN_FUNC_MODE == MAIN_FUNC_MODE_BLINK
    return blink_func();
#endif
}

enum rawbuf_state {
    RAWBUF_FREE = 0,
    RAWBUF_WRITING,
    RAWBUF_READY,
    RAWBUF_READING,
};

#define RAWBUFCNT        3
#define RAWBUFSIZE       (SENSOR_W * SENSOR_H * 2)
#define RAWBUFSIZEOFS    (SENSOR_H * 4)
#define RAWBUFSIZEOFSTAIL 2 /* trailing 2 bytes for VTEMP (BE 14-bit) */
#define RAWBUFSIZETOTAL  (RAWBUFSIZE + RAWBUFSIZEOFS + RAWBUFSIZEOFSTAIL)
/* Trailing metadata appended to each rawbuf after the pixel data:
 *   RAWBUF_OFS_VTEMP : Sensor-internal FPA temperature register D166
 *                      (2 bytes, BE, 14-bit).  Read from the SPI data
 *                      stream, NOT from any Pico ADC.
 *                      T_FPA = 25 + (vtemp - 8192) / 70  [°C, factory cal].
 *                      FFC drift detection uses this field. */
#define RAWBUF_OFS_VTEMP (RAWBUFSIZE + RAWBUFSIZEOFS)
static uint8_t rawbuf[RAWBUFCNT][RAWBUFSIZETOTAL];
static volatile uint8_t rawbuf_state[RAWBUFCNT];
static volatile int rawbuf_last_ready;
static enum UartOutMode ouomode = UART_OUT_MODE_DEF;

/* Per-frame VTEMP accumulator.  The sensor emits 2 u16 VTEMP samples at the
 * end of every row (bytes 324-325 in each per-row SPI buffer = the first u16
 * of the 4-byte per-row tail).  Averaging all 120 rows reduces VTEMP noise
 * ~11× (√120) vs. reading a single row at end-of-frame.  Without this the
 * FPA-temperature signal is buried under single-sample noise — apparent
 * VTEMP swings are too small to reliably track sensor self-heating. */
static uint32_t rawbuf_vtemp_sum[RAWBUFCNT];
static uint16_t rawbuf_vtemp_cnt[RAWBUFCNT];

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

    /* Per-line VTEMP accumulation.  Row tail starts at src + SENSOR_W*2 + 4
     * (after the 4 skipped bytes between pixels and row-ext); first u16 is
     * the FPA temperature register (same as the single-sample extraction
     * that the end-of-frame block does at b->data[338..339]). */
    if (line == 0) {
        rawbuf_vtemp_sum[buf_idx] = 0;
        rawbuf_vtemp_cnt[buf_idx] = 0;
    }
    const uint16_t line_vtemp =
        ((((uint16_t)src[SENSOR_W * 2 + 4]) << 8) | src[SENSOR_W * 2 + 5])
        & 0x3FFF;
    rawbuf_vtemp_sum[buf_idx] += line_vtemp;
    rawbuf_vtemp_cnt[buf_idx]++;

    if (ouomode == UOM_RAW_U16) {
        const size_t line_stride = SENSOR_W * 2 + 4;
        uint8_t* dst = &rawbuf[buf_idx][line * line_stride];
        // memcpy(dst, src, SENSOR_W * 2);
        uint16_t* d16 = (uint16_t*)dst;
        const uint16_t* s16 = (const uint16_t*)src;
        for (size_t i = 0; i < (SENSOR_W * 2) / 2; ++i) {
            d16[i] = s16[i];
        }
        memcpy(dst + SENSOR_W * 2, src + SENSOR_W * 2 + 4, 4);
    } else if (ouomode == UOM_CALI_U8) {
        uint8_t* dst = &rawbuf[buf_idx][line * SENSOR_W * 2];
        // memcpy(dst, src, len);
        uint16_t* d16 = (uint16_t*)dst;
        const uint16_t* s16 = (const uint16_t*)src;
        for (size_t i = 0; i < len / 2; ++i) {
            d16[i] = s16[i];
        }
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

    if (dataomode == DOM_UVC) {
        tud_task();
    }

    return -1;
}

#define HIST_BUCKETS 1024
#define HIST_SHIFT   4

static inline __attribute__((always_inline)) uint16_t
be16_to_u14(const uint8_t* p)
{
    // uint16_t v;
    // v = ((uint16_t)p[0] << 8) | p[1];
    // return v & 0x3fff;
    uint16_t val;
    memcpy(&val, p, 2);
    return __builtin_bswap16(val) & 0x3fff;
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

static inline __attribute__((always_inline)) uint8_t
norm_u14_to_u8(uint16_t v, uint16_t low, uint16_t high)
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
[[maybe_unused]] static int tx_buf_idx;
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
static uint16_t ram_calibg[PX_CNT];
static struct BadPt ram_badpts[MAX_BADPT];
static float ram_mean_diff = 0.0f;
static float ram_t_ref_low = 0.0f;
static int32_t ram_l_ref_mean = 0;   /* mean of low-ref frame; fixed at boot, never changes */
static int32_t ram_temp_anchor = 0;  /* temperature zero-point in corrected ADU; updated each FFC */
#define T_REF_HIGH 50.0f
#define BODY_ONLY_RESYNC_THRES    8u
#define STARTUP_HEAD_SYNC_TIMEOUT 24u

/* FFC: lifted from binary_calib_frame so FFC can reset them */
static uint16_t g_smooth_low = 0;
static uint16_t g_smooth_high = 0;
static int32_t g_calib_mean = 0;
static const uint16_t* g_last_calib = nullptr;

/* FFC timing and state.
 *
 * Warmup schedule: the first ~25 s after boot sees ~2000 ADU of VTEMP travel
 * from bolometer self-heating, which directly drives scene-reading drift.
 * To cancel this we need K auto-fit (requires ≥ 3 FFCs) to activate BEFORE
 * warmup ends — so we force three rapid FFCs during warmup (t≈3, 6, 10 s),
 * one more at the tail end (t≈30 s) to freshen calibg at the settled FPA,
 * and only then switch to the 120 s stable cadence. */
#define FFC_INTERVAL_FIRST_MS  3000u    /* 1st FFC at t≈3 s */
#define FFC_INTERVAL_WARMUP_2  3000u    /* 2nd FFC 3 s after 1st → t≈6 s */
#define FFC_INTERVAL_WARMUP_3  4000u    /* 3rd FFC 4 s after 2nd → t≈10 s (K-fit activates) */
#define FFC_INTERVAL_WARMUP_4  20000u   /* 4th FFC 20 s after 3rd → t≈30 s (post-warmup) */
#define FFC_INTERVAL_STABLE_MS 120000u
#define FFC_VTEMP_DELTA_ADU     150u
static uint32_t ffc_last_ms = 0;
static uint16_t ffc_last_fpa_vtemp = 0;
static int32_t  ffc_last_g_calib_mean = 0;  /* g_calib_mean at last FFC */
static uint16_t baseline_fpa_vtemp = 0;      /* VTEMP at 2nd FFC; K-fit reference */
static int32_t  baseline_g_calib_mean = 0;   /* g_calib_mean at 2nd FFC; K-fit reference */
static uint16_t ffc_count = 0;               /* number of FFCs run since boot */
static bool     ffc_done_first = false;

/* Runtime-fit FPA-drift compensation.
 *
 * Problem: with near-uniform per-pixel gain, the FFC anchor formula
 *   anchor = mean(gain*(flash_imgbg - ram_calibg)) + g_calib_mean
 * collapses to mean(flash_imgbg) = ram_l_ref_mean, a compile-time constant.
 * Between FFCs, ram_calibg and g_calib_mean are frozen, so corrected_mean of
 * a fixed scene drifts linearly with FPA self-emission (≈ with VTEMP). The
 * reported T therefore drifts without bound across the 2-min FFC interval.
 *
 * Fix: eff_anchor = ram_temp_anchor + K * (cur_vtemp - ffc_last_fpa_vtemp)
 * where K is auto-fit at each FFC (≥ 3rd) as dg_calib_mean/dVTEMP across FFCs.
 * g_calib_mean is the mean of the shutter frame; the shutter is a uniform
 * FPA-temperature blackbody, so its drift across FFCs directly measures
 * dmean(raw)/dVTEMP for a fixed scene — which is exactly what eff_anchor
 * must cancel between FFCs to hold the report steady. */

/* Diagnostic tail appended after the 6-byte temperature tail (VTEMP+T_lo+T_hi):
 *    [PX_CNT+ 7 .. +10] ram_temp_anchor     int32 BE
 *    [PX_CNT+11 .. +12] g_smooth_low        u16   BE
 *    [PX_CNT+13 .. +14] g_smooth_high       u16   BE
 *    [PX_CNT+15 .. +18] ram_mean_diff       f32   BE (IEEE 754)
 *    [PX_CNT+19 .. +20] baseline_fpa_vtemp  u16   BE (2nd-FFC VTEMP; 0 before)
 * Total frame bytes sent in CALI_U8 mode: PX_CNT + 7 + FRAME_DIAG_TAIL. */
#define FRAME_DIAG_TAIL     14

static float runtime_k_ancomp = 0.0f;   /* dg_calib_mean / dVTEMP, fit across FFCs */
static bool  runtime_k_valid  = false;

/* Diagnostic tail appended after the 6-byte temperature tail (VTEMP+T_lo+T_hi):
 *    [PX_CNT+ 7 .. +10] ram_temp_anchor     int32 BE
 *    [PX_CNT+11 .. +12] g_smooth_low        u16   BE
 *    [PX_CNT+13 .. +14] g_smooth_high       u16   BE
 *    [PX_CNT+15 .. +18] ram_mean_diff       f32   BE (IEEE 754)
 *    [PX_CNT+19 .. +20] baseline_fpa_vtemp  u16   BE (first-FFC VTEMP; 0 before)
 * Total frame bytes sent in CALI_U8 mode: PX_CNT + 7 + FRAME_DIAG_TAIL. */
#define FRAME_DIAG_TAIL     14

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
    static uint32_t hist[HIST_BUCKETS] = {0};
    size_t i;
    uint32_t acc;
    uint32_t p1_cnt = px_cnt / 100;
    uint32_t p99_cnt = px_cnt - p1_cnt;

    memset(hist, 0, sizeof(uint32_t) * HIST_BUCKETS);
    for (i = 0; i < px_cnt; i++) {
        uint32_t idx = raw14[i] >> HIST_SHIFT;
        if (unlikely(idx >= HIST_BUCKETS)) {
            idx = HIST_BUCKETS - 1;
        }
        hist[idx]++;
        if (dataomode == DOM_UVC && (i & 0x3FF) == 0) {
            tud_task();
        }
    }

    acc = 0;
    for (i = 0; i < HIST_BUCKETS; i++) {
        acc += hist[i];
        if (acc >= p1_cnt) {
            *low = (i << HIST_SHIFT) + (1 << (HIST_SHIFT - 1));
            break;
        }
    }

    if (dataomode == DOM_UVC) {
        tud_task();
    }

    acc = 0;
    for (i = 0; i < HIST_BUCKETS; i++) {
        acc += hist[i];
        if (acc >= p99_cnt) {
            // *high = (i << HIST_SHIFT) | 0xff;
            *high = (i << HIST_SHIFT) + (1 << (HIST_SHIFT - 1));
            break;
        }
    }

    if (dataomode == DOM_UVC) {
        tud_task();
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
    const uint8_t* raw, uint8_t* out, [[maybe_unused]] size_t size)
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
    uint8_t* badpts_size_max)
{
    const uint8_t badpts_size = *badpts_size_max;
    const size_t line_stride = SENSOR_W * 2;
    const size_t px_per_line = SENSOR_W;
    const uint8_t* rawl_src = raw_l;
    const uint8_t* rawh_src = raw_h;

    float diff_sum = 0.0f;
    size_t diff_count = 0;

    for (uint8_t i = 0; i < badpts_size; ++i) {
        badpts[i].x = UINT8_MAX;
        badpts[i].y = UINT8_MAX;
    }

    for (size_t y = 0; y < SENSOR_H; ++y) {
        const uint8_t* line_h = rawh_src + y * line_stride;
        const uint8_t* line_l = rawl_src + y * line_stride;

        for (size_t x = 0; x < SENSOR_W; ++x) {
            const uint8_t* src_h = line_h + x * 2;
            const uint8_t* src_l = line_l + x * 2;

            uint16_t h = be16_to_u14(src_h);
            uint16_t l = be16_to_u14(src_l);

            if (unlikely(h > HOT_PIXEL_THRES || l > HOT_PIXEL_THRES)) {
                if (gain_map != nullptr) {
                    gain_map[y * px_per_line + x] = -1.0f;
                }
                continue;
            }

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

                if (unlikely(diff < 0.0f)) {
                    gain_map[y * px_per_line + x] = 1.0f;

                    if (bad_idx >= badpts_size) {
#if CALI_PASS_MODE != 1
                        *badpts_size_max = bad_idx + 1;
                        return -3;
#else
                        continue;
#endif
                    }
                    badpts[bad_idx].y = y;
                    badpts[bad_idx].x = x;
                    bad_idx++;
                    continue;
                }

                gain_map[y * px_per_line + x] = avg_diff / diff;
                if (unlikely(diff < avg_diff * 0.6f)) {
                    if (bad_idx >= badpts_size) {
#if CALI_PASS_MODE != 1
                        *badpts_size_max = bad_idx + 1;
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
            *badpts_size_max = bad_idx + 1;
            return -4;
        }
    }
#endif

    *badpts_size_max = bad_idx + 1;
    return 0;
}

static inline __attribute__((always_inline)) int cali_ref_stream_v2(
    const uint8_t* raw_h,
    const uint8_t* raw_l,
    float* gain_map,
    struct BadPt* badpts,
    uint8_t* badpts_size_max)
{
    const uint8_t badpts_size = *badpts_size_max;
    const size_t line_stride = SENSOR_W * 2;
    const size_t px_per_line = SENSOR_W;
    const uint8_t* rawl_src = raw_l;
    const uint8_t* rawh_src = raw_h;

    float diff_sum = 0.0f;
    float diff_sq_sum = 0.0f;
    size_t diff_count = 0;

    for (uint8_t i = 0; i < badpts_size; ++i) {
        badpts[i].x = UINT8_MAX;
        badpts[i].y = UINT8_MAX;
    }

    for (size_t y = 0; y < SENSOR_H; ++y) {
        const uint8_t* line_h = rawh_src + y * line_stride;
        const uint8_t* line_l = rawl_src + y * line_stride;

        for (size_t x = 0; x < SENSOR_W; ++x) {
            const uint8_t* src_h = line_h + x * 2;
            const uint8_t* src_l = line_l + x * 2;

            uint16_t h = be16_to_u14(src_h);
            uint16_t l = be16_to_u14(src_l);

            if (unlikely(h > HOT_PIXEL_THRES || l > HOT_PIXEL_THRES)) {
                if (gain_map != nullptr) {
                    gain_map[y * px_per_line + x] = -1.0f;
                }
                continue;
            }

            int32_t diff = (int32_t)h - (int32_t)l;
            if (diff == 0) {
                diff = 1;
            }

            float f_diff = (float)diff;
            diff_sum += f_diff;
            diff_sq_sum += (f_diff * f_diff);
            diff_count++;

            if (gain_map != nullptr) {
                gain_map[y * px_per_line + x] = f_diff;
            }

#if CALI_PASS_MODE != 1
            if (unlikely(
                    (y == 0 || y == SENSOR_H - 1 || x == 0 || x == SENSOR_W - 1)
                    && diff < 0)) {
                return -1;
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

    const float avg_diff = diff_sum / (float)diff_count;
    const float variance =
        (diff_sq_sum / (float)diff_count) - (avg_diff * avg_diff);
    const float std_dev = variance > 0.0f ? sqrtf(variance) : 0.0f;
    const float lower_bound = avg_diff - 3.0f * std_dev;
    const float upper_bound = avg_diff + 3.0f * std_dev;

    size_t bad_idx = 0;

    if (gain_map != nullptr) {
        for (size_t y = 0; y < SENSOR_H; ++y) {
            for (size_t x = 0; x < px_per_line; ++x) {
                float diff = gain_map[y * px_per_line + x];

                if (unlikely(diff < 0.0f)) {
                    gain_map[y * px_per_line + x] = 1.0f;
                    goto mark_bad_pixel;
                }

                gain_map[y * px_per_line + x] = avg_diff / diff;

                if (unlikely(diff < lower_bound || diff > upper_bound)) {
                    goto mark_bad_pixel;
                }

                continue;

            mark_bad_pixel:
                if (bad_idx >= badpts_size) {
#if CALI_PASS_MODE != 1
                    *badpts_size_max = bad_idx + 1;
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

#if CALI_PASS_MODE != 1
    // for (size_t i = 0; i < bad_idx - 1; ++i) {
    //     int dy = (int)badpts[i].y - (int)badpts[i + 1].y;
    //     int dx = (int)badpts[i].x - (int)badpts[i + 1].x;
    //     if (abs(dy) <= 1 && abs(dx) <= 1) {
    //         *badpts_size_max = bad_idx + 1;
    //         return -4;
    //     }
    // }
#endif

    *badpts_size_max = bad_idx + 1;
    return 0;
}

static inline __attribute__((always_inline)) int cali_ref_stream_v3(
    const uint8_t* raw_h,
    const uint8_t* raw_l,
    float* gain_map,
    struct BadPt* badpts,
    uint8_t* badpts_size_max)
{
    const uint8_t badpts_size = *badpts_size_max;
    const size_t line_stride = SENSOR_W * 2;
    const size_t px_per_line = SENSOR_W;
    const uint8_t* rawl_src = raw_l;
    const uint8_t* rawh_src = raw_h;

    float diff_sum = 0.0f;
    size_t diff_count = 0;

    for (uint8_t i = 0; i < badpts_size; ++i) {
        badpts[i].x = UINT8_MAX;
        badpts[i].y = UINT8_MAX;
    }

    for (size_t y = 0; y < SENSOR_H; ++y) {
        const uint8_t* line_h = rawh_src + y * line_stride;
        const uint8_t* line_l = rawl_src + y * line_stride;

        for (size_t x = 0; x < SENSOR_W; ++x) {
            const uint8_t* src_h = line_h + x * 2;
            const uint8_t* src_l = line_l + x * 2;

            const uint16_t h = be16_to_u14(src_h);
            const uint16_t l = be16_to_u14(src_l);

            if (unlikely(h > HOT_PIXEL_THRES || l > HOT_PIXEL_THRES)) {
                if (gain_map != nullptr) {
                    gain_map[y * px_per_line + x] = -1.0f;
                }
                continue;
            }

            int32_t diff = (int32_t)h - (int32_t)l;
            if (diff == 0) {
                diff = 1;
            }

            const float f_diff = (float)diff;
            diff_sum += f_diff;
            diff_count++;

            if (gain_map != nullptr) {
                gain_map[y * px_per_line + x] = f_diff;
            }

#if CALI_PASS_MODE != 1
            if (unlikely(
                    (y == 0 || y == SENSOR_H - 1 || x == 0 || x == SENSOR_W - 1)
                    && diff < 0)) {
                return -1;
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

    const float avg_diff = diff_sum / (float)diff_count;
    const float lower_bound = avg_diff * 0.5f;
    const float upper_bound = avg_diff * 1.5f;
    size_t bad_idx = 0;

    if (gain_map != nullptr) {
        for (size_t y = 0; y < SENSOR_H; ++y) {
            for (size_t x = 0; x < px_per_line; ++x) {
                float diff = gain_map[y * px_per_line + x];
                uint16_t h = be16_to_u14(rawh_src + y * line_stride + x * 2);
                if (unlikely(diff <= 0.0f || h > HOT_PIXEL_THRES)) {
                    gain_map[y * px_per_line + x] = 1.0f;
                    goto mark_bad_pixel;
                }
                gain_map[y * px_per_line + x] = avg_diff / diff;
                if (unlikely(diff < lower_bound || diff > upper_bound)) {
                    goto mark_bad_pixel;
                }
                continue;
            mark_bad_pixel:
                if (bad_idx >= badpts_size) {
#if CALI_PASS_MODE != 1
                    *badpts_size_max = bad_idx + 1;
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

#if CALI_PASS_MODE != 1
#    define EMPTY_POINT_VAL 255
    for (size_t i = 0; i < bad_idx; ++i) {
        if (badpts[i].x == EMPTY_POINT_VAL || badpts[i].y == EMPTY_POINT_VAL) {
            continue;
        }

        for (size_t j = i + 1; j < bad_idx; ++j) {
            int dy, dx;

            if (badpts[j].x == EMPTY_POINT_VAL
                || badpts[j].y == EMPTY_POINT_VAL) {
                continue;
            }

            dy = (int)badpts[i].y - (int)badpts[j].y;

            if (dy < -1 || dy > 1) {
                continue;
            }

            dx = (int)badpts[i].x - (int)badpts[j].x;

            if (abs(dy) <= 1 && abs(dx) <= 1) {
                println(
                    "Adjacent bad pixels detected: pt1(%u,%u), pt2(%u,%u)\n",
                    badpts[i].x,
                    badpts[i].y,
                    badpts[j].x,
                    badpts[j].y);

                *badpts_size_max = bad_idx + 1;
                return -4;
            }
        }
    }
#    undef EMPTY_POINT_VAL
#endif

    *badpts_size_max = bad_idx + 1;
    return 0;
}

static inline __attribute__((always_inline)) int cali_ref_stream_v4(
    const uint8_t* raw_h,
    const uint8_t* raw_l,
    float* gain_map,
    struct BadPt* badpts,
    uint8_t* badpts_size_max,
    float* out_mean_diff)
{
    const uint8_t badpts_size = *badpts_size_max;
    const size_t line_stride = SENSOR_W * 2;
    const size_t px_per_line = SENSOR_W;

    float diff_sum = 0.0f;
    size_t diff_count = 0;
    size_t bad_idx = 0;

    for (uint8_t i = 0; i < badpts_size; ++i) {
        badpts[i].x = UINT8_MAX;
        badpts[i].y = UINT8_MAX;
    }

    for (size_t y = 0; y < SENSOR_H; ++y) {
        for (size_t x = 0; x < SENSOR_W; ++x) {
            const uint8_t* src_h = raw_h + y * line_stride + x * 2;
            const uint8_t* src_l = raw_l + y * line_stride + x * 2;

            uint16_t h = be16_to_u14(src_h);
            uint16_t l = be16_to_u14(src_l);

            if (__builtin_expect(h > HOT_PIXEL_THRES || l > HOT_PIXEL_THRES,
                                 0)) {
                if (gain_map != nullptr) {
                    gain_map[y * px_per_line + x] = -1.0f;
                }
                continue;
            }

            int32_t diff = (int32_t)h - (int32_t)l;
            if (diff <= 0) {
                diff = 1;
            }

            diff_sum += (float)diff;
            diff_count++;

            if (gain_map != nullptr) {
                gain_map[y * px_per_line + x] = (float)diff;
            }
        }
    }

    if (diff_count == 0) {
        return -2;
    }
    const float avg_diff = diff_sum / (float)diff_count;

    if (gain_map != NULL) {
        for (size_t y = 0; y < SENSOR_H; ++y) {
            for (size_t x = 0; x < px_per_line; ++x) {
                float diff = gain_map[y * px_per_line + x];

                if (__builtin_expect(diff <= 0.0f, 0)) {
                    gain_map[y * px_per_line + x] = 1.0f;
                    if (bad_idx < badpts_size) {
                        badpts[bad_idx].x = (uint8_t)x;
                        badpts[bad_idx].y = (uint8_t)y;
                        bad_idx++;
                    }
                    continue;
                }

                float gain = avg_diff / diff;

                if (__builtin_expect(gain < 0.2f || gain > 4.0f, 0)) {
                    gain_map[y * px_per_line + x] = 1.0f;
                    if (bad_idx < badpts_size) {
                        badpts[bad_idx].x = (uint8_t)x;
                        badpts[bad_idx].y = (uint8_t)y;
                        bad_idx++;
                    }
                    continue;
                }

                gain_map[y * px_per_line + x] = gain;
            }
        }

        const float MAX_SPATIAL_GAIN_DIFF = 0.4f;

        for (int y = 0; y < SENSOR_H; ++y) {
            for (int x = 0; x < SENSOR_W; ++x) {
                float my_gain = gain_map[y * SENSOR_W + x];
                if (my_gain == 1.0f) {
                    continue;
                }

                float neighbor_sum = 0.0f;
                int valid_count = 0;

                int y_min = (y > 0) ? y - 1 : 0;
                int y_max = (y < SENSOR_H - 1) ? y + 1 : SENSOR_H - 1;
                int x_min = (x > 0) ? x - 1 : 0;
                int x_max = (x < SENSOR_W - 1) ? x + 1 : SENSOR_W - 1;

                for (int ny = y_min; ny <= y_max; ++ny) {
                    for (int nx = x_min; nx <= x_max; ++nx) {
                        if (ny == y && nx == x) {
                            continue;
                        }
                        float ng = gain_map[ny * SENSOR_W + nx];
                        if (ng > 0.0f && ng != 1.0f) {
                            neighbor_sum += ng;
                            valid_count++;
                        }
                    }
                }

                if (valid_count > 0) {
                    float avg_neighbor = neighbor_sum / (float)valid_count;
                    if (__builtin_expect(fabsf(my_gain - avg_neighbor)
                                             > MAX_SPATIAL_GAIN_DIFF,
                                         0)) {
                        gain_map[y * SENSOR_W + x] = 1.0f;
                        if (bad_idx < badpts_size) {
                            badpts[bad_idx].x = (uint8_t)x;
                            badpts[bad_idx].y = (uint8_t)y;
                            bad_idx++;
                        }
                    }
                }
            }
        }
    }

    *badpts_size_max = bad_idx;
    if (out_mean_diff != nullptr) {
        *out_mean_diff = avg_diff;
    }
    return 0;
}

static inline __attribute__((always_inline)) int cali_ref_stream_v5(
    const uint8_t* raw_h,
    const uint8_t* raw_l,
    float* gain_map,
    struct BadPt* badpts,
    uint8_t* badpts_size_max)
{
    const uint8_t badpts_size = *badpts_size_max;
    const size_t line_stride = SENSOR_W * 2;
    const size_t px_per_line = SENSOR_W;

    float diff_sum = 0.0f;
    size_t diff_count = 0;
    size_t bad_idx = 0;

    for (uint8_t i = 0; i < badpts_size; ++i) {
        badpts[i].x = UINT8_MAX;
        badpts[i].y = UINT8_MAX;
    }

    for (size_t y = 0; y < SENSOR_H; ++y) {
        for (size_t x = 0; x < SENSOR_W; ++x) {
            const uint8_t* src_h = raw_h + y * line_stride + x * 2;
            const uint8_t* src_l = raw_l + y * line_stride + x * 2;

            uint16_t h = be16_to_u14(src_h);
            uint16_t l = be16_to_u14(src_l);

            if (__builtin_expect(h > HOT_PIXEL_THRES || l > HOT_PIXEL_THRES,
                                 0)) {
                if (gain_map != nullptr) {
                    gain_map[y * px_per_line + x] = -1.0f;
                }
                continue;
            }

            int32_t diff = (int32_t)h - (int32_t)l;

            if (__builtin_expect(diff <= 5, 0)) {
                if (gain_map != nullptr) {
                    gain_map[y * px_per_line + x] = -1.0f;
                }
                continue;
            }

            diff_sum += (float)diff;
            diff_count++;

            if (gain_map != nullptr) {
                gain_map[y * px_per_line + x] = (float)diff;
            }
        }
    }

    if (diff_count == 0) {
        return -2;
    }
    const float avg_diff = diff_sum / (float)diff_count;

    if (gain_map != NULL) {
        for (size_t y = 0; y < SENSOR_H; ++y) {
            for (size_t x = 0; x < px_per_line; ++x) {
                float val = gain_map[y * px_per_line + x];
                if (val > 0.0f) {
                    gain_map[y * px_per_line + x] = avg_diff / val;
                }
            }
        }

        const float MAX_LOCAL_DEVIATION = 0.15f;

        for (int y = 0; y < SENSOR_H; ++y) {
            for (int x = 0; x < SENSOR_W; ++x) {
                float my_gain = gain_map[y * SENSOR_W + x];

                if (my_gain < 0.0f) {
                    if (bad_idx < badpts_size) {
                        badpts[bad_idx].x = (uint8_t)x;
                        badpts[bad_idx].y = (uint8_t)y;
                        bad_idx++;
                    }
                    continue;
                }

                float neighbor_sum = 0.0f;
                int valid_count = 0;

                int y_min = (y > 0) ? y - 1 : 0;
                int y_max = (y < SENSOR_H - 1) ? y + 1 : SENSOR_H - 1;
                int x_min = (x > 0) ? x - 1 : 0;
                int x_max = (x < SENSOR_W - 1) ? x + 1 : SENSOR_W - 1;

                for (int ny = y_min; ny <= y_max; ++ny) {
                    for (int nx = x_min; nx <= x_max; ++nx) {
                        if (ny == y && nx == x) {
                            continue;
                        }
                        float ng = gain_map[ny * SENSOR_W + nx];
                        if (ng > 0.0f) {
                            neighbor_sum += ng;
                            valid_count++;
                        }
                    }
                }

                if (valid_count > 0) {
                    float avg_neighbor = neighbor_sum / (float)valid_count;
                    if (__builtin_expect(
                            fabsf(my_gain - avg_neighbor) / avg_neighbor
                                > MAX_LOCAL_DEVIATION,
                            0)) {
                        gain_map[y * SENSOR_W + x] = -2.0f;
                        if (bad_idx < badpts_size) {
                            badpts[bad_idx].x = (uint8_t)x;
                            badpts[bad_idx].y = (uint8_t)y;
                            bad_idx++;
                        }
                    }
                }
            }
        }

        for (size_t y = 0; y < SENSOR_H; ++y) {
            for (size_t x = 0; x < px_per_line; ++x) {
                if (gain_map[y * px_per_line + x] < 0.0f) {
                    gain_map[y * px_per_line + x] = 1.0f;
                }
            }
        }
    }

    *badpts_size_max = bad_idx;
    return 0;
}

static inline __attribute__((always_inline)) void be_u8_to_le_u16_inplace(
    uint8_t* buf, const size_t len)
{
    if (unlikely(!buf || len < 2)) {
        return;
    }

    for (size_t i = 0; i + 1 < len; i += 2) {
        const uint8_t tmp = buf[i];
        buf[i] = buf[i + 1];
        buf[i + 1] = tmp;
    }
}

void __not_in_flash_func(__flash_erase_cali__)(void* param)
{
    (void)param;
    flash_range_erase(FLASH_TARGET_OFFSET, FLASH_SECTOR_SIZE);
}

void __not_in_flash_func(__flash_write2__)(void* param)
{
    const struct FlashOpInfo* opinfo = (const struct FlashOpInfo*)param;
    if (opinfo == nullptr) {
        return;
    }
    const uint32_t ofs = FLASH_TARGET_OFFSET + opinfo->ofs;
    flash_range_erase(ofs, opinfo->size);
    flash_range_program(ofs, opinfo->data, opinfo->size);
}

static inline __attribute__((always_inline)) void* hidep(void* ptr)
{
    __asm__ __volatile__("" : "+r"(ptr));
    return ptr;
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
        const void* src = hidep(flashop->data + i * FLASH_SECTOR_SIZE);
        memcpy(buffer, src, FLASH_SECTOR_SIZE);
        struct FlashOpInfo op;
        op.ofs = flashop->ofs + i * FLASH_SECTOR_SIZE;
        op.data = buffer;
        op.size = FLASH_SECTOR_SIZE;
        rc = flash_safe_execute(__flash_write2__, (void*)&op, 500);
        if (rc != PICO_OK) {
            return false;
        }
        const uint8_t* flashdata = data + i * FLASH_SECTOR_SIZE;
        for (uint32_t j = 0; j < FLASH_SECTOR_SIZE; ++j) {
            if (flashdata[j] != buffer[j]) {
                return false;
            }
        }
    }

    if (lsector != 0) {
        memset(buffer, 0xFF, FLASH_SECTOR_SIZE);
        const void* src = hidep(flashop->data + fsector * FLASH_SECTOR_SIZE);
        memcpy(buffer, src, lsector);
        struct FlashOpInfo op;
        op.ofs = flashop->ofs + fsector * FLASH_SECTOR_SIZE;
        op.data = buffer;
        op.size = FLASH_SECTOR_SIZE;
        rc = flash_safe_execute(__flash_write2__, (void*)&op, 500);
        if (rc != PICO_OK) {
            return false;
        }
        const uint8_t* flashdata = data + fsector * FLASH_SECTOR_SIZE;
        for (uint32_t j = 0; j < FLASH_SECTOR_SIZE; ++j) {
            if (flashdata[j] != buffer[j]) {
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

    const float diff = fabsf(a - b);
    if (diff <= abs_eps) {
        return true;
    }

    return diff <= rel_eps * fmaxf(fabsf(a), fabsf(b));
}

static inline __attribute__((always_inline)) const char* get_cali_error_str(
    const int code)
{
    switch (code) {
#define X(name, val) \
    case name: \
        return #name;
        CALI_ERROR_LIST
#undef X
    default:
        return "UNKNOWN_ERROR";
    }
}

static inline __attribute__((always_inline)) void send_cali_error_with_str(
    int err_code)
{
    const char* err_str = get_cali_error_str(err_code);
    const uint8_t str_len = (uint8_t)strlen(err_str);
    const uint8_t header[5] = {0xFF, 0xFF, 0xEE, (uint8_t)err_code, str_len};
    uart_write_blocking(OUART, header, sizeof(header));
    uart_write_blocking(OUART, (const uint8_t*)err_str, str_len);
    uart_tx_wait_blocking(OUART);
}

static inline __attribute__((always_inline)) bool start_cali()
{
    multicore_fifo_push_blocking(CORE0_EV0);
    while (multicore_fifo_rvalid()) {
        const uint32_t cmd = multicore_fifo_pop_blocking();
        if (cmd == CORE1_EV0) {
            break;
        }
    }
    sleep_ms(100);

    println("start cali");

    float* gain_map = (float*)rawbuf;
    static struct BadPt badpts[MAX_BADPT];
    uint8_t badpt_num = MAX_BADPT;
    float mean_diff = 0.0f;
    const int caliret = cali_ref_stream_v4(calibuf[CALIBUF_IMG_H_IDX],
                                           calibuf[CALIBUF_IMG_L_IDX],
                                           gain_map,
                                           badpts,
                                           &badpt_num,
                                           &mean_diff);
    println("badpt_num: %u", badpt_num);
    if (caliret < 0) {
        println("cali failed: ret = %d", caliret);
        send_cali_error_with_str(-caliret);
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
    fheader.mean_diff = mean_diff;
    fheader.t_ref_low = 0.0f;

    const uint8_t header[3] = {0xff, 0xff, 0xdd};
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
            send_cali_error_with_str(ERR_FLASH_IMGBG);
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
            send_cali_error_with_str(ERR_FLASH_GAIN);
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
            send_cali_error_with_str(ERR_FLASH_BADPT);
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
            send_cali_error_with_str(ERR_FLASH_HDR);
            return false;
        }
    }

    println("send finish...");
    send_cali_error_with_str(ERR_NONE);

    return true;
}

static inline __attribute__((always_inline)) bool dump_cli()
{
    multicore_fifo_push_blocking(CORE0_EV0);
    while (multicore_fifo_rvalid()) {
        const uint32_t cmd = multicore_fifo_pop_blocking();
        if (cmd == CORE1_EV0) {
            break;
        }
    }
    sleep_ms(100);

    println("dump cli");

    if (flash_header->has_data != FLASH_HAS_DATA_F) {
        send_cali_error_with_str(ERR_FLASH_EMPTY);
        return false;
    }

    sleep_ms(500);

    const uint8_t header[3] = {0xff, 0xff, 0xdd};

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
    send_cali_error_with_str(ERR_NONE);

    return true;
}

static inline __attribute__((always_inline)) void repair_bad_pixels(
    uint16_t* raw, const struct BadPt* pts, const int count)
{
    int i;
    uint32_t sum;
    uint32_t offset;
    uint16_t y, x;

    if (unlikely(!raw || !pts || count <= 0)) {
        return;
    }

    for (i = 0; i < count; i++) {
        y = pts[i].y;
        x = pts[i].x;
        if (y == UINT8_MAX || x == UINT8_MAX) {
            break;
        }
        offset = y * SENSOR_W + x;
        sum = raw[offset - SENSOR_W - 1] + raw[offset - SENSOR_W]
            + raw[offset - SENSOR_W + 1] + raw[offset - 1] + raw[offset + 1]
            + raw[offset + SENSOR_W - 1] + raw[offset + SENSOR_W]
            + raw[offset + SENSOR_W + 1];
        raw[offset] = (uint16_t)(sum >> 3);

        if (dataomode == DOM_UVC) {
            tud_task();
        }
    }
}

static inline __attribute__((always_inline)) void binary_calib_frame(
    const uint8_t* raw,
    const uint16_t* calib,
    const float* calig,
    const struct BadPt* calip,
    uint8_t* out,
    const size_t px_cnt)
{
    static uint8_t gamma_lut[256];
    static uint16_t raw14[PX_CNT];

    uint16_t low = 0, high = 0;
    bool debad = false;

    if (unlikely(calib != nullptr && calib != g_last_calib)) {
        uint64_t sum = 0;
        for (size_t i = 0; i < px_cnt; ++i) {
            sum += calib[i];
        }
        g_calib_mean = (int32_t)(sum / px_cnt);
        g_last_calib = calib;
    }

    if (calib != nullptr && calig == nullptr) {
        for (size_t i = 0; i < px_cnt; i++) {
            uint16_t r = be16_to_u14(&raw[i * 2]);
            r = gray14_to_bin(r);
            const int32_t diff = (int32_t)r - (int32_t)calib[i] + g_calib_mean;
            raw14[i] = clip_u16(diff);
            if (dataomode == DOM_UVC && (i & 0x3FF) == 0) {
                tud_task();
            }
        }
        debad = true;
    } else if (calib != nullptr && calig != nullptr) {
        for (size_t i = 0; i < px_cnt; i++) {
            uint16_t r = be16_to_u14(&raw[i * 2]);
            r = gray14_to_bin(r);
            const int32_t diff = (int32_t)r - (int32_t)calib[i];
            const int32_t val =
                (int32_t)(calig[i] * (float)diff) + g_calib_mean;
            raw14[i] = clip_u16(val);
            if (dataomode == DOM_UVC && (i & 0x3FF) == 0) {
                tud_task();
            }
        }
        debad = true;
    } else {
        for (size_t i = 0; i < px_cnt; i++) {
            const uint16_t r = be16_to_u14(&raw[i * 2]);
            raw14[i] = gray14_to_bin(r);
            if (dataomode == DOM_UVC && (i & 0x3FF) == 0) {
                tud_task();
            }
        }
    }
    if (debad) {
        repair_bad_pixels(raw14, calip, MAX_BADPT);
        if (dataomode == DOM_UVC) {
            tud_task();
        }
    }
    calc_percentile_u14(raw14, PX_CNT, &low, &high);

    if (unlikely(g_smooth_low == 0 && g_smooth_high == 0)) {
        g_smooth_low = low;
        g_smooth_high = high;
    } else {
        g_smooth_low = (uint16_t)(((uint32_t)g_smooth_low * 7 + low) >> 3);
        g_smooth_high = (uint16_t)(((uint32_t)g_smooth_high * 7 + high) >> 3);
    }

    if (unlikely(g_smooth_high <= g_smooth_low)) {
        g_smooth_high = g_smooth_low + 1;
    }

    out[0] = 0xff;

    /* Floor the displayed scene window to MIN_THERMAL_RANGE *before* computing
     * t_lo/t_hi and the gamma LUT, so color-bar labels, LUT, and per-pixel
     * mapping all use the same window.  Without this floor, static / uniform
     * scenes (occluded lens, blank wall) with Δsmooth ≈ 100 ADU would stretch
     * the full color map over that tiny range, exposing FPN and ghost
     * artifacts that are otherwise masked by scene dynamics. */
    {
        const uint32_t range0 = g_smooth_high - g_smooth_low;
        if (unlikely(range0 < MIN_THERMAL_RANGE)) {
            const uint32_t center = g_smooth_low + (range0 >> 1);
            g_smooth_low = (uint16_t)(center > (MIN_THERMAL_RANGE >> 1)
                                          ? center - (MIN_THERMAL_RANGE >> 1)
                                          : 0);
            g_smooth_high = (uint16_t)(g_smooth_low + MIN_THERMAL_RANGE);
        }
    }

    /* compute frame temperature range from calibration mapping.
     * Bolometer ADU ∝ scene radiance ∝ T_K^4 (Stefan-Boltzmann), so two-point
     * interpolation must be done in T^4 space, not linear-in-°C, otherwise a
     * 0°C / 50°C calibration under-reads midrange by several degrees. */
    /* VTEMP is the last two bytes of the rawbuf (BE 14-bit).  Used for
     * between-FFC anchor drift compensation via runtime_k_ancomp. */
    const uint16_t cur_vtemp = be16_to_u14(&raw[RAWBUF_OFS_VTEMP]);

    int16_t t_lo_x10 = INT16_MAX;
    int16_t t_hi_x10 = INT16_MAX;
    float t_lo_c_f = 0.0f, t_hi_c_f = 0.0f;
    bool temp_map_ok = false;
    int32_t eff_anchor = ram_temp_anchor;
    if (ram_mean_diff > 0.0f && ram_temp_anchor != 0 && calib != nullptr) {
        if (runtime_k_valid && ffc_last_fpa_vtemp != 0) {
            const int32_t dv =
                (int32_t)cur_vtemp - (int32_t)ffc_last_fpa_vtemp;
            eff_anchor += (int32_t)(runtime_k_ancomp * (float)dv);
        }
        const float T_L_K = ram_t_ref_low + 273.15f;
        const float T_H_K = T_REF_HIGH + 273.15f;
        const float T_L_K4 = T_L_K * T_L_K * T_L_K * T_L_K;
        const float T_H_K4 = T_H_K * T_H_K * T_H_K * T_H_K;
        const float dK4 = T_H_K4 - T_L_K4;
        const float inv_md = 1.0f / ram_mean_diff;
        const float f_lo =
            (float)((int32_t)g_smooth_low - eff_anchor) * inv_md;
        const float f_hi =
            (float)((int32_t)g_smooth_high - eff_anchor) * inv_md;
        float lo_K4 = T_L_K4 + f_lo * dK4;
        float hi_K4 = T_L_K4 + f_hi * dK4;
        if (lo_K4 < 1.0f) lo_K4 = 1.0f;
        if (hi_K4 < 1.0f) hi_K4 = 1.0f;
        t_lo_c_f = sqrtf(sqrtf(lo_K4)) - 273.15f;
        t_hi_c_f = sqrtf(sqrtf(hi_K4)) - 273.15f;
        t_lo_x10 = (int16_t)(t_lo_c_f * 10.0f);
        t_hi_x10 = (int16_t)(t_hi_c_f * 10.0f);
        temp_map_ok = true;
    }

    /* Per-frame temperature-linear LUT: map the ADU-linear intensity 0..254
     * (which is linear in radiance within the scene window) to an output that
     * is linear in °C within [t_lo_c_f, t_hi_c_f]. Replaces the old static
     * gamma=0.6 LUT. Falls back to identity when no temperature mapping. */
    {
        const float t_span_c = t_hi_c_f - t_lo_c_f;
        if (temp_map_ok && t_span_c > 0.01f) {
            const float t_lo_k = t_lo_c_f + 273.15f;
            const float t_hi_k = t_hi_c_f + 273.15f;
            const float lo_K4 = t_lo_k * t_lo_k * t_lo_k * t_lo_k;
            const float hi_K4 = t_hi_k * t_hi_k * t_hi_k * t_hi_k;
            const float dK4 = hi_K4 - lo_K4;
            const float inv_span = 254.0f / t_span_c;
            for (int i = 0; i < 256; ++i) {
                const float f = (float)i * (1.0f / 254.0f);
                float k4 = lo_K4 + f * dK4;
                if (k4 < 1.0f) k4 = 1.0f;
                const float t_c = sqrtf(sqrtf(k4)) - 273.15f;
                float o = (t_c - t_lo_c_f) * inv_span;
                if (o < 0.0f) o = 0.0f;
                if (o > 254.0f) o = 254.0f;
                gamma_lut[i] = (uint8_t)o;
            }
        } else {
            for (int i = 0; i < 256; ++i) {
                gamma_lut[i] = (uint8_t)(i > 254 ? 254 : i);
            }
        }
    }

    /* range is already ≥ MIN_THERMAL_RANGE (floored above). */
    const uint32_t range = g_smooth_high - g_smooth_low;
    const uint32_t mult = (254 << 16) / range;

    for (int i = 0; i < PX_CNT; i++) {
        const uint16_t v = raw14[i];
        uint8_t linear_val;

        if (v <= g_smooth_low) {
            linear_val = 0;
        } else if (v >= g_smooth_high) {
            linear_val = 254;
        } else {
            linear_val = (uint8_t)(((v - g_smooth_low) * mult) >> 16);
        }

        out[i + 1] = gamma_lut[linear_val];

        if (dataomode == DOM_UVC && (i & 0x3FF) == 0) {
            tud_task();
        }
    }

    /* temperature range bytes at [PX_CNT+3..PX_CNT+6]; INT16_MAX = N/A */
    out[PX_CNT + 3] = (uint8_t)((t_lo_x10 >> 8) & 0xFF);
    out[PX_CNT + 4] = (uint8_t)(t_lo_x10 & 0xFF);
    out[PX_CNT + 5] = (uint8_t)((t_hi_x10 >> 8) & 0xFF);
    out[PX_CNT + 6] = (uint8_t)(t_hi_x10 & 0xFF);

    /* diagnostic tail — all big-endian, see FRAME_DIAG_TAIL comment */
    const uint32_t anch_u = (uint32_t)ram_temp_anchor;
    out[PX_CNT +  7] = (uint8_t)(anch_u >> 24);
    out[PX_CNT +  8] = (uint8_t)(anch_u >> 16);
    out[PX_CNT +  9] = (uint8_t)(anch_u >>  8);
    out[PX_CNT + 10] = (uint8_t)(anch_u      );
    out[PX_CNT + 11] = (uint8_t)(g_smooth_low  >> 8);
    out[PX_CNT + 12] = (uint8_t)(g_smooth_low      );
    out[PX_CNT + 13] = (uint8_t)(g_smooth_high >> 8);
    out[PX_CNT + 14] = (uint8_t)(g_smooth_high     );
    uint32_t md_u;
    memcpy(&md_u, &ram_mean_diff, sizeof(md_u));
    out[PX_CNT + 15] = (uint8_t)(md_u >> 24);
    out[PX_CNT + 16] = (uint8_t)(md_u >> 16);
    out[PX_CNT + 17] = (uint8_t)(md_u >>  8);
    out[PX_CNT + 18] = (uint8_t)(md_u      );
    out[PX_CNT + 19] = (uint8_t)(baseline_fpa_vtemp >> 8);
    out[PX_CNT + 20] = (uint8_t)(baseline_fpa_vtemp     );
}

static inline __attribute__((always_inline)) void binary_frame(
    const uint8_t* raw, uint8_t* out, [[maybe_unused]] const size_t px_cnt)
{
    uint8_t* dst = out;
    const size_t line_stride = SENSOR_W * 2;
    const size_t px_per_line = SENSOR_W;
    for (size_t y = 0; y < SENSOR_H; ++y) {
        const uint8_t* line = raw + y * line_stride;
        for (size_t x = 0; x < px_per_line; ++x) {
            const uint8_t* src = &line[x * 2];

            uint16_t r = be16_to_u14(src);
            r = gray14_to_bin(r);
            const size_t idx = (y * px_per_line + x) * 2;
            dst[idx] = (uint8_t)(r >> 8);
            dst[idx + 1] = (uint8_t)(r & 0xFF);
        }
    }
}

static inline __attribute__((always_inline)) void normalize_frame_corrected_be(
    const uint8_t* raw, const uint16_t* calib, uint8_t* out)
{
    uint16_t low = 0, high = 0;
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

static inline __attribute__((always_inline)) int txbuf_pick_free()
{
    for (int i = 0; i < TXBUF_CNT; i++) {
        if (tx_state[i] == TXBUF_FREE) {
            return i;
        }
    }

    if (dataomode == DOM_UVC) {
        tud_task();
    }

#ifdef MAX_SPEED
    for (int i = 0; i < TXBUF_CNT; i++) {
        if (tx_state[i] == TXBUF_READY && i != newest_ready) {
            return i;
        }
    }
#    undef MAX_SPEED
#endif

    return -1;
}

static inline __attribute__((always_inline)) void uart_tx_dma_start(
    const uint8_t* buf, const size_t len)
{
    dma_busy = 1;

    dma_channel_set_read_addr(uart_tx_dma_chan, buf, false);
    dma_channel_set_trans_count(uart_tx_dma_chan, len, true);
}

static inline __attribute__((always_inline)) void try_start_dma()
{
    if (dma_busy) {
        return;
    }

    const int b = newest_ready;
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
        uart_tx_dma_start(norm_buf[b], PX_CNT + 7 + FRAME_DIAG_TAIL);
    }
}

void __isr dma_irq1_handler()
{
    dma_hw->ints1 = 1u << uart_tx_dma_chan;

    const int b = sending;
    tx_state[b] = TXBUF_FREE;
    __dmb();
    sending = -1;
    dma_busy = 0;
    __sev();
}

static inline __attribute__((always_inline)) void fill_txbuf(const int tx_idx,
                                                             const int raw_idx)
{
    if (ouomode == UOM_RAW_U16) {
        norm_buf[tx_idx][0] = 0xFF;
        norm_buf[tx_idx][1] = 0xFF;
        raw16_decode_to_normbuf(rawbuf[raw_idx], norm_buf[tx_idx], PX_CNT);
    } else if (ouomode == UOM_CALI_U8) {
        if (cmode == CM_OFF) {
            binary_calib_frame(rawbuf[raw_idx],
                               calibg,
                               caligain,
                               calibadpt,
                               norm_buf[tx_idx],
                               PX_CNT);
            norm_buf[tx_idx][PX_CNT + 1] = rawbuf[raw_idx][RAWBUF_OFS_VTEMP];
            norm_buf[tx_idx][PX_CNT + 2] = rawbuf[raw_idx][RAWBUF_OFS_VTEMP + 1];
        } else if (cmode == CM_ON) {
            if (caliidx < CALIBUF_CNT) {
                binary_frame(rawbuf[raw_idx], calibuf[caliidx], PX_CNT);
            }
        }
    }
}

static uint32_t curr_uartb;
static bool flash_dm = false;
static inline __attribute__((always_inline)) uint32_t
baud_get2(const uint8_t cmd)
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
    // case 0x77:
    //     baud = UBM77;
    //     ouomode = UOM_CALI_U8;
    //     cmode = CM_OFF;
    //     flash_dm = false;
    //     break;
    // case 0x88:
    //     baud = UBM88;
    //     ouomode = UOM_CALI_U8;
    //     cmode = CM_OFF;
    //     flash_dm = false;
    //     break;
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
        dataomode = DOM_UART; /* override UVC mode so UART path runs */
        caliidx = CALIBUF_IMG_L_IDX; /* reset capture index on each trigger */
        flash_dm = false;
        led_off();
        break;
    case 0xDD:
        baud = UBMDD;
        ouomode = UOM_CALI_U8;
        cmode = CM_OFF;
        dataomode = DOM_UART; /* override UVC mode so dump path runs */
        flash_dm = true;
        break;
    default:
        break;
    };
    return baud;
}

static inline __attribute__((always_inline)) void ouart_init(const uint8_t cmd)
{
    static bool setup = OUART_SETUP_DEF;

    if (unlikely(!setup)) {
        const char* OUART_TEST_STRING = "sipeed PMOD_Thermal160\n";
        setup = true;
        uart_deinit(OUART);
        gpio_set_function(IO_OTX, GPIO_FUNC_UART);
        gpio_set_function(IO_ORX, GPIO_FUNC_UART);
        uart_init(OUART, 115200);
        uart_set_hw_flow(OUART, false, false);
        uart_set_format(OUART, 8, 1, UART_PARITY_NONE);
        uart_write_blocking(OUART,
                            (const uint8_t*)OUART_TEST_STRING,
                            strlen(OUART_TEST_STRING));
    }

    const uint32_t baud = baud_get2(cmd);
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
    if (dataomode == DOM_UVC) {
        tud_task();
        /* In UVC mode only accept explicit override commands (0xCC/0xDD).
         * All other bytes are silently discarded to avoid corrupting UVC. */
        if (!uart_is_readable(OUART)) {
            return;
        }
        const uint8_t cmd = uart_getc(OUART);
        if (cmd != 0xCC && cmd != 0xDD) {
            return;
        }
        /* sel() sets dataomode = DOM_UART, cmode, caliidx, etc. */
        const uint32_t baud = sel(cmd);
        if (baud == 0 || baud == curr_uartb) {
            return;
        }
        while (dma_busy) {
            tight_loop_contents();
        }
        dma_channel_abort(uart_tx_dma_chan);
        while (dma_channel_is_busy(uart_tx_dma_chan)) {
            tight_loop_contents();
        }
        uart_deinit(OUART);
        gpio_set_function(IO_OTX, GPIO_FUNC_UART);
        gpio_set_function(IO_ORX, GPIO_FUNC_UART);
        uart_init(OUART, baud);
        uart_set_hw_flow(OUART, false, false);
        uart_set_format(OUART, 8, 1, UART_PARITY_NONE);
        curr_uartb = baud;
        return;
    }

    if (!uart_is_readable(OUART)) {
        return;
    }

    const uint8_t cmd = uart_getc(OUART);
    const uint32_t baud = sel(cmd);

    if (baud == 0 || baud == curr_uartb) {
        return;
    }

    while (dma_busy) {
        tight_loop_contents();
    }

    dma_channel_abort(uart_tx_dma_chan);
    while (dma_channel_is_busy(uart_tx_dma_chan)) {
        tight_loop_contents();
    }

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
static uint8_t uvc_buf[UVC_BUF_CNT][FRAME_SIZE] __attribute__((aligned(4)));
static volatile uint8_t uvc_state[UVC_BUF_CNT];
static volatile int uvc_newest_ready = -1;
static volatile int uvc_sending = -1;
static volatile uint8_t uvc_tx_busy = 0;
static volatile uint32_t uvc_frame_num = 0;
static volatile uint32_t uvc_inv_ms = 1000 / FRAME_RATE;
static enum UartOutMode uvc_ouomode_bak = UART_OUT_MODE_DEF;
static uint8_t ram_cmaps[CMAP_NUM][CMAP_BYTES];
static volatile uint8_t cur_cmap_idx = 0;
static const uint8_t* volatile active_cmap = nullptr;
static struct yuv_cmap_entry yuv_cmap[CMAP_W];

static inline __attribute__((always_inline)) void uvc_buf_init()
{
    for (int i = 0; i < UVC_BUF_CNT; ++i) {
        uvc_state[i] = UVCBUF_FREE;
    }

    uvc_newest_ready = -1;
    uvc_sending = -1;
    uvc_tx_busy = 0;
}

static inline __attribute__((always_inline)) int find_free_uvc_buf()
{
    for (int i = 0; i < UVC_BUF_CNT; ++i) {
        if (uvc_state[i] == UVCBUF_FREE) {
            return i;
        }
    }
    return -1;
}

static inline __attribute__((always_inline)) int find_ready_uvc_buf()
{
    const int b = uvc_newest_ready;
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

static inline __attribute__((always_inline)) void fill_random_yuy2(
    uint8_t* buf, const size_t len)
{
    for (size_t i = 0; i < len; i += 4) {
        const uint8_t y0 = rand() & 0xff;
        const uint8_t y1 = rand() & 0xff;
        const uint8_t u = 128 + (rand() % 32 - 16);
        const uint8_t v = 128 + (rand() % 32 - 16);

        buf[i + 0] = y0;
        buf[i + 1] = u;
        buf[i + 2] = y1;
        buf[i + 3] = v;
    }
}

static inline __attribute__((always_inline)) void fill_smpte_yuy2(
    uint8_t* buf, const size_t len)
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

static inline __attribute__((always_inline)) void init_yuv_cmap(
    const uint8_t* rgb_cmap)
{
    if (unlikely(!rgb_cmap)) {
        return;
    }

    for (int i = 0; i < CMAP_W; ++i) {
        const uint8_t r = rgb_cmap[i * 3 + 0];
        const uint8_t g = rgb_cmap[i * 3 + 1];
        const uint8_t b = rgb_cmap[i * 3 + 2];

        yuv_cmap[i].y =
            (uint8_t)(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);

        yuv_cmap[i].u_raw = (int16_t)(-38 * r - 74 * g + 112 * b);
        yuv_cmap[i].v_raw = (int16_t)(112 * r - 94 * g - 18 * b);
    }
}

static inline __attribute__((always_inline)) void fill_uvc_frame2(
    const uint8_t* pixbuf,
    const struct yuv_cmap_entry* yuv_cmap,
    uint8_t* buf,
    const size_t len)
{
    if (unlikely(!pixbuf || !yuv_cmap || !buf || len < 4)) {
        return;
    }

    const uint8_t* src = pixbuf + 1;
    size_t out_idx = 0;
    const size_t chunk_bytes = SENSOR_W * 2 * 8;

    while (out_idx + 3 < len) {
        size_t end_idx = out_idx + chunk_bytes;
        if (end_idx > len) {
            end_idx = len;
        }

        while (out_idx + 3 < end_idx) {
            const uint8_t g0 = *src++;
            const uint8_t g1 = *src++;

            const uint8_t y0 = yuv_cmap[g0].y;
            const uint8_t y1 = yuv_cmap[g1].y;

            const uint8_t u =
                (uint8_t)(((yuv_cmap[g0].u_raw + yuv_cmap[g1].u_raw + 256) >> 9)
                          + 128);
            const uint8_t v =
                (uint8_t)(((yuv_cmap[g0].v_raw + yuv_cmap[g1].v_raw + 256) >> 9)
                          + 128);

            buf[out_idx++] = y0;
            buf[out_idx++] = u;
            buf[out_idx++] = y1;
            buf[out_idx++] = v;

            if ((out_idx & 0x1FF) == 0) {
                tud_task();
            }
        }

        tud_task();
    }

    return;
}

static inline __attribute__((always_inline)) void fill_uvc_frame(
    const uint8_t* pixbuf, const uint8_t* cmap, uint8_t* buf, const size_t len)
{
    if (unlikely(!pixbuf || !cmap || !buf || len < 4)) {
        return;
    }

    const uint8_t* src = pixbuf + 1;
    size_t out_idx = 0;

    while (out_idx + 3 < len) {
        const uint8_t g0 = *src++;
        const uint8_t g1 = *src++;
        const uint8_t r0 = cmap[g0 * 3 + 0];
        const uint8_t g0c = cmap[g0 * 3 + 1];
        const uint8_t b0 = cmap[g0 * 3 + 2];
        const uint8_t r1 = cmap[g1 * 3 + 0];
        const uint8_t g1c = cmap[g1 * 3 + 1];
        const uint8_t b1 = cmap[g1 * 3 + 2];
        const uint8_t y0 =
            (uint8_t)(((66 * r0 + 129 * g0c + 25 * b0 + 128) >> 8) + 16);
        const uint8_t y1 =
            (uint8_t)(((66 * r1 + 129 * g1c + 25 * b1 + 128) >> 8) + 16);
        const uint8_t u = (uint8_t)(((-38 * r0 - 74 * g0c + 112 * b0 + -38 * r1
                                      - 74 * g1c + 112 * b1 + 256)
                                     >> 9)
                                    + 128);
        const uint8_t v = (uint8_t)(((112 * r0 - 94 * g0c - 18 * b0 + 112 * r1
                                      - 94 * g1c - 18 * b1 + 256)
                                     >> 9)
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
    const int b = uvc_sending;
    if (b >= 0) {
        uvc_state[b] = UVCBUF_FREE;
        uvc_sending = -1;
    }
    __dmb();
    uvc_tx_busy = 0;
    ++uvc_frame_num;
    __sev();
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

static inline __attribute__((always_inline)) void fake_uvc_fill(
    uint8_t* buf, const size_t size)
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

static inline __attribute__((always_inline)) void smpte_uvc_fill(
    uint8_t* buf, const size_t size)
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

    tud_task();

    fill_uvc_frame2(norm_buf[b], yuv_cmap, buf, size);
    // fill_smpte_yuy2(buf, size);

    tud_task();

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

    uvc_state[b] = UVCBUF_FILLING;
    __dmb();

    tud_task();
    fill_uvc_frame2(norm_buf[src], yuv_cmap, uvc_buf[b], FRAME_SIZE);
    // fill_smpte_yuy2(uvc_buf[b], FRAME_SIZE);
    tud_task();

    __dmb();
    int old_ready = uvc_newest_ready;
    uvc_state[b] = UVCBUF_READY;
    uvc_newest_ready = b;

    if (old_ready >= 0 && old_ready != b
        && uvc_state[old_ready] == UVCBUF_READY) {
        uvc_state[old_ready] = UVCBUF_FREE;
    }

    tx_state[src] = TXBUF_FREE;
    if (newest_ready == src) {
        newest_ready = -1;
    }
    __dmb();
    __sev();
}

static inline __attribute__((always_inline)) uint32_t uvc_task()
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
    if (uvc_newest_ready == b) {
        uvc_newest_ready = -1;
    }

    __dmb();
    uvc_tx_busy = 1;

    tud_video_n_frame_xfer(0, 0, uvc_buf[b], FRAME_SIZE);

    return 0;
}

static void ffc_run(uint16_t cur_fpa_vtemp);
static bool ffc_should_trigger(uint16_t cur_fpa_vtemp);

void __time_critical_func(core1_rawbuf_consumer)()
{
    int idx;
    for (int i = 0; i < TXBUF_CNT; i++) {
        tx_state[i] = TXBUF_FREE;
    }

    newest_ready = -1;
    sending = -1;
    dma_busy = 0;
    struct GpioBtn boot;
    boot.gpio = IO_BTN;
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
    flash_safe_execute_core_init();

    ouart_init(UART_BAUD_MODE_DEF);
    dma_busy = 0;
    buf_idx = 0;

    memcpy(ram_cmaps[0], colormap_magma, CMAP_BYTES);
    memcpy(ram_cmaps[1], colormap_hot, CMAP_BYTES);
    memcpy(ram_cmaps[2], colormap_turbo, CMAP_BYTES);
    memcpy(ram_cmaps[3], colormap_cool, CMAP_BYTES);
    memcpy(ram_cmaps[4], colormap_deepgreen, CMAP_BYTES);
    memcpy(ram_cmaps[5], colormap_gray, CMAP_BYTES);
    active_cmap = ram_cmaps[0];
    cur_cmap_idx = 0;
    init_yuv_cmap(active_cmap);

    while (true) {
        uart_poll2(baud_get2);
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
                __dmb();

                int old_tx_ready = newest_ready;
                tx_state[b] = TXBUF_READY;
                newest_ready = b;

                if (old_tx_ready >= 0 && old_tx_ready != b
                    && tx_state[old_tx_ready] == TXBUF_READY) {
                    tx_state[old_tx_ready] = TXBUF_FREE;
                }
                __sev();
            }

            __dmb();
            rawbuf_state[idx] = RAWBUF_FREE;
            did_work = true;
        }

        if (dataomode == DOM_UVC) {
            tud_task();
            uvc_prepare_next();
        }
        tud_task();

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
            uvc_task();

            button_scan(&boot);
            uint8_t boot_ev = boot.event;
            if (boot_ev) {
                boot.event = 0;
                if (boot_ev & GB_EV_RELEASE) {
                    cur_cmap_idx = (cur_cmap_idx + 1) % CMAP_NUM;
                    active_cmap = ram_cmaps[cur_cmap_idx];
                    init_yuv_cmap(active_cmap);
                }
            }
        }

        /* FFC: check trigger once per loop iteration when in normal output.
         * Allow first FFC even without full calibration (calibg == nullptr);
         * ffc_run() will set calibg = ram_calibg for subsequent frames. */
        if (cmode == CM_OFF && (calibg != nullptr || !ffc_done_first)) {
            const uint16_t cur_fpa =
                (uint16_t)(((uint16_t)
                                rawbuf[rawbuf_last_ready < 0
                                           ? 0
                                           : rawbuf_last_ready][RAWBUF_OFS_VTEMP]
                            << 8)
                           | rawbuf[rawbuf_last_ready < 0 ? 0
                                                          : rawbuf_last_ready]
                                   [RAWBUF_OFS_VTEMP + 1]);
            if (ffc_should_trigger(cur_fpa)) {
                ffc_run(cur_fpa);
            }
        }

        if (!did_work) {
            __wfe();
        }
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

/*
 * ffc_run() — Flat-Field Correction, called from Core1.
 *
 * Closes the shutter, waits for one frame (now looking at a uniform surface),
 * updates ram_calibg[] with that frame, resets calib_mean and the percentile
 * smoother so the next output frame restarts from a clean baseline.
 * Then opens the shutter and resumes normal capture.
 *
 * The update preserves the original gain map and mean_diff; only the
 * per-pixel offset (background) and global mean are refreshed, correcting
 * for FPA drift since the last full calibration.
 */
static void ffc_run(uint16_t cur_fpa_vtemp)
{
    println("FFC start");
    led_on();

    shutter_on(); /* close shutter (~200 ms) */

    /* Discard all frames currently in the ring: they may have been captured
     * while the shutter was still mechanically closing (or entirely before
     * the pulse), and using such a frame as ram_calibg bakes the scene into
     * the background, producing a negative ghost on every subsequent frame. */
    for (int i = 0; i < RAWBUFCNT; i++) {
        if (rawbuf_state[i] == RAWBUF_READY) {
            rawbuf_state[i] = RAWBUF_FREE;
        }
    }
    __dmb();
    rawbuf_last_ready = -1;
    __sev();

    /* Wait for TWO fresh frames from Core0.  The first new frame may still
     * straddle the shutter-close boundary (started before, finished after),
     * so we drop it and use the second, which is guaranteed to be captured
     * entirely with the shutter closed. */
    int idx = -1;
    for (int pass = 0; pass < 2; pass++) {
        idx = -1;
        uint32_t timeout = 300;
        while (idx < 0 && timeout--) {
            for (int i = 0; i < RAWBUFCNT; i++) {
                if (rawbuf_state[i] == RAWBUF_READY) {
                    idx = i;
                    break;
                }
            }
            if (idx < 0) {
                sleep_ms(1);
            }
        }
        if (idx < 0) {
            println("FFC: no frame, abort");
            shutter_off();
            led_off();
            return;
        }
        if (pass == 0) {
            /* drop the straddle frame */
            rawbuf_state[idx] = RAWBUF_FREE;
            __dmb();
        }
    }
    __dmb();
    rawbuf_state[idx] = RAWBUF_READING;

    /* update per-pixel background from the shutter frame */
    const uint8_t* src = rawbuf[idx];
    uint64_t sum = 0;
    for (size_t i = 0; i < PX_CNT; i++) {
        /* must apply gray14_to_bin to match binary_calib_frame's processing */
        const uint16_t v = gray14_to_bin(be16_to_u14(&src[i * 2]));
        ram_calibg[i] = v;
        sum += v;
    }

    rawbuf_state[idx] = RAWBUF_FREE;

    /* ensure calibg points to the updated background even if no full
     * calibration was loaded at boot (FFC provides offset-only correction) */
    calibg = ram_calibg;

    /* reset calib_mean and force binary_calib_frame to recompute it */
    g_calib_mean = (int32_t)(sum / PX_CNT);
    g_last_calib = nullptr; /* triggers recompute on next frame */

    /* recompute temperature anchor to compensate for gain-weighted offset between
     * the low-ref frame and this FFC shutter frame.  Without this, a shutter at a
     * different temperature than the low-ref causes a constant offset on all
     * temperature readings proportional to (avg_gain - 1) * ΔT_shutter.
     *
     * anchor = Σ(gain[i] * (l_ref[i] - shutter[i])) / N + shutter_mean
     *
     * Derivation: for scene at T, corrected_mean = anchor + avg_diff * α,
     * so T_reading = T_L + (corrected_mean - anchor) * k = T_L + avg_diff*α*k = T_scene ✓ */
    const int32_t anchor_old = ram_temp_anchor;
    if (caligain != nullptr && ram_l_ref_mean > 0) {
        float fsum = 0.0f;
        for (size_t i = 0; i < PX_CNT; i++)
            fsum += flash_gain[i] * ((float)flash_imgbg[i] - (float)ram_calibg[i]);
        ram_temp_anchor = (int32_t)roundf(fsum / (float)PX_CNT + (float)g_calib_mean);

        /* Persist the K·ΔVTEMP compensation accumulated since last FFC into
         * ram_temp_anchor. Without this, the compensation term resets to 0
         * right after FFC (because ffc_last_fpa_vtemp gets refreshed to the
         * current VTEMP below), and each FFC silently rolls back the drift
         * correction — producing a sawtooth drift pattern instead of fixing
         * the underlying problem. */
        if (runtime_k_valid && ffc_last_fpa_vtemp != 0) {
            const int32_t dv_acc =
                (int32_t)cur_fpa_vtemp - (int32_t)ffc_last_fpa_vtemp;
            const int32_t comp_acc =
                (int32_t)(runtime_k_ancomp * (float)dv_acc);
            ram_temp_anchor += comp_acc;
            println("FFC anchor+K: base=%ld comp_acc=%ld final=%ld",
                    (long)(ram_temp_anchor - comp_acc), (long)comp_acc,
                    (long)ram_temp_anchor);
        } else {
            println("FFC anchor updated: %ld (was %ld)",
                    (long)ram_temp_anchor, (long)anchor_old);
        }
    }

    /* Auto-fit K = dg_calib_mean / dVTEMP across FFCs, taking the 1st FFC as
     * the K-fit baseline so the fit window opens as early as the 2nd FFC
     * (~t=6 s with the warmup schedule). Need enough VTEMP travel to avoid
     * fitting on ADU noise; 50 ADU ≈ ~0.7 °C FPA swing — low enough that
     * slow-warming units still trigger a fit within the warmup window, high
     * enough to reject jitter. Clamp the result to ±2 (physically plausible
     * for this sensor) — a runaway K would drive eff_anchor far off and make
     * the report worse than the un-compensated baseline. After the first
     * valid fit, apply EMA smoothing to absorb noise on subsequent fits
     * without throwing away the already-validated estimate. */
    if (ffc_count >= 1 && baseline_fpa_vtemp != 0) {
        const int32_t dv = (int32_t)cur_fpa_vtemp - (int32_t)baseline_fpa_vtemp;
        const int32_t dg = g_calib_mean - baseline_g_calib_mean;
        if (dv > 50 || dv < -50) {
            const float k_new = (float)dg / (float)dv;
            if (k_new > -2.0f && k_new < 2.0f) {
                if (runtime_k_valid) {
                    runtime_k_ancomp = 0.3f * runtime_k_ancomp + 0.7f * k_new;
                } else {
                    runtime_k_ancomp = k_new;
                    runtime_k_valid = true;
                }
                println("K fit: dv=%ld dg=%ld k_new=%.4f k=%.4f",
                        (long)dv, (long)dg, (double)k_new,
                        (double)runtime_k_ancomp);
            } else {
                println("K skip: dv=%ld dg=%ld k=%.4f out of range",
                        (long)dv, (long)dg, (double)k_new);
            }
        } else {
            println("K skip: |dv|=%ld below threshold", (long)(dv < 0 ? -dv : dv));
        }
    }

    /* Carry the percentile smoother across FFC by shifting with Δanchor,
     * instead of resetting to 0. A full reset causes a one-frame spike on
     * trend plots because the next frame reseeds g_smooth from raw percentiles.
     * Shifting keeps (g_smooth - anchor)/mean_diff continuous, so the reported
     * temperature window stays stable across the shutter event. The first FFC
     * still clears: any pre-first-FFC smoother state was computed with a stale
     * calibg and is not salvageable. */
    if (ffc_done_first && g_smooth_low != 0 && g_smooth_high != 0) {
        const int32_t delta = ram_temp_anchor - anchor_old;
        int32_t nl = (int32_t)g_smooth_low  + delta;
        int32_t nh = (int32_t)g_smooth_high + delta;
        if (nl < 0) nl = 0; else if (nl > 0xFFFF) nl = 0xFFFF;
        if (nh < 0) nh = 0; else if (nh > 0xFFFF) nh = 0xFFFF;
        g_smooth_low  = (uint16_t)nl;
        g_smooth_high = (uint16_t)nh;
    } else {
        g_smooth_low  = 0;
        g_smooth_high = 0;
    }

    /* update FFC bookkeeping */
    ffc_last_ms = to_ms_since_boot(get_absolute_time());
    ffc_last_fpa_vtemp = cur_fpa_vtemp;
    ffc_last_g_calib_mean = g_calib_mean;
    ffc_count++;
    if (ffc_count == 1) {
        /* Baseline at 1st FFC (~t=3 s). Earlier we deferred to 2nd FFC to
         * skip a settling transient, but with the warmup FFC schedule that
         * costs us the largest VTEMP-travel window (3 s → ~30 s) — the
         * window where K can actually be measured before the FPA stabilises.
         * Capturing at 1st FFC lets the 2nd FFC already attempt a fit. */
        baseline_fpa_vtemp    = cur_fpa_vtemp;
        baseline_g_calib_mean = g_calib_mean;
    }
    ffc_done_first = true;

    shutter_off(); /* open shutter (~200 ms) */
    led_off();
    println("FFC done, calib_mean=%ld", (long)g_calib_mean);
}

static bool ffc_should_trigger(uint16_t cur_fpa_vtemp)
{
    const uint32_t now_ms = to_ms_since_boot(get_absolute_time());

    if (!ffc_done_first) {
        return (now_ms >= FFC_INTERVAL_FIRST_MS);
    }

    /* Select target interval based on which FFC we're waiting for.
     * ffc_count here is the number of FFCs COMPLETED, so ffc_count==1 means
     * we're deciding when to fire FFC #2, etc. */
    uint32_t target_interval;
    switch (ffc_count) {
        case 1:  target_interval = FFC_INTERVAL_WARMUP_2; break;
        case 2:  target_interval = FFC_INTERVAL_WARMUP_3; break;
        case 3:  target_interval = FFC_INTERVAL_WARMUP_4; break;
        default: target_interval = FFC_INTERVAL_STABLE_MS; break;
    }

    const uint32_t elapsed = now_ms - ffc_last_ms;
    if (elapsed >= target_interval) {
        return true;
    }

    const uint16_t delta = (cur_fpa_vtemp > ffc_last_fpa_vtemp)
        ? (cur_fpa_vtemp - ffc_last_fpa_vtemp)
        : (ffc_last_fpa_vtemp - cur_fpa_vtemp);
    return (delta >= FFC_VTEMP_DELTA_ADU);
}

int __time_critical_func(spi_init_40MHz4)()
{
#if OCLK_180 == 0
    set_sys_clock_khz(160000, true);
#else
    vreg_set_voltage(VREG_VOLTAGE_1_15);
    sleep_ms(10);
    set_sys_clock_khz(180000, true);
#endif

    sleep_ms(10);
#if OCLK_180 == 0
    clock_configure(clk_peri,
                    0,
                    CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                    160 * MHZ,
                    160 * MHZ);
#else
    clock_configure(clk_peri,
                    0,
                    CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                    180 * MHZ,
                    180 * MHZ);
#endif

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

#if CLEAR_CALI_ON_BOOT
    /* core1 not yet started — no multicore coordination needed,
     * call erase function directly with interrupts disabled */
    {
        uint32_t save = save_and_disable_interrupts();
        __flash_erase_cali__(nullptr);
        restore_interrupts(save);
    }
    println("CLEAR_CALI_ON_BOOT: calibration flash erased.");
#endif

    if (flash_header->has_data == FLASH_HAS_DATA_F) {
        memcpy(ram_badpts, flash_badpt, MAX_BADPT * sizeof(struct BadPt));
        calibadpt = ram_badpts;
        memcpy(ram_calibg, flash_imgbg, sizeof(ram_calibg));
        calibg = ram_calibg;
        {
            uint64_t sum = 0;
            for (size_t i = 0; i < PX_CNT; i++) sum += ram_calibg[i];
            ram_l_ref_mean  = (int32_t)(sum / PX_CNT);
            ram_temp_anchor = ram_l_ref_mean; /* before first FFC, calibg == l_ref so anchor == l_ref_mean */
        }
        memcpy(calibuf, flash_gain, PX_CNT * sizeof(float));
        caligain = (float*)calibuf;
        if (flash_header->mean_diff > 0.0f) {
            ram_mean_diff = flash_header->mean_diff;
            ram_t_ref_low = flash_header->t_ref_low;
        }
    }

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

    extern uint spi1clk;
    spi1clk = 45000000;
    int32_t lidx = -1;
    int cur_buf = -1;
    uint8_t frame_group_cnt = 0;
    uint8_t body_only_sync_cnt = 0;
    uint8_t startup_sync_retry = 0;
    bool sync_locked = false;
    bool startup_synced = false;

    rawbuf_init();

    multicore_launch_core1(core1_rawbuf_consumer);
    sleep_ms(5);

    extern void wspi_reinit1();
    extern void wspi_init1();
    extern void wspi_stop1();
    wspi_init1();
    println("spiclk = %llu", (uint64_t)spi1clk);

    // Serein_Y
    shutter_on();

    while (true) {
        rx_poll();
        if (rxring.ridx != rxring.widx) {
            struct RxBuf* b = &rxring.buf[rxring.ridx];

            if (b->data[2] == 0xff && b->data[3] == 0x00
                && b->data[BUFSIZE - 6] == 0xff
                && b->data[BUFSIZE - 5] == 0x00) {
                if (!startup_synced && !sync_locked) {
                    if (b->data[5] == 0xec && b->data[BUFSIZE - 3] == 0xda) {
                        sync_locked = true;
                        startup_sync_retry = 0;
                        body_only_sync_cnt = 0;
                    } else {
                        if (++startup_sync_retry >= STARTUP_HEAD_SYNC_TIMEOUT) {
                            startup_sync_retry = 0;
                            body_only_sync_cnt = 0;
                            lidx = -1;
                            cur_buf = -1;
                            rawbuf_init();
                            rxring.ridx = rxring.widx;
                            wspi_stop1();
                            sleep_ms(5);
                            wspi_reinit1();
                        }
                        rxring.ridx = (rxring.ridx + 1) % BUFCOUNT;
                        continue;
                    }
                }

                if (b->data[5] == 0xec && b->data[BUFSIZE - 3] == 0xda) {
                    if (!startup_synced) {
                        body_only_sync_cnt = 0;
                    }
                    frame_group_cnt++;

                    if (false
                        && (frame_group_cnt >= 5
                            && ((dataomode == DOM_UART && curr_uartb > UBM33)
                                || (dataomode == DOM_UVC)))) {
                        frame_group_cnt = 0;
                        lidx = -1;
                    } else {
                        lidx = 0;
                        if (cur_buf < 0) {
                            cur_buf = rawbuf_find_free();
                            if (cur_buf >= 0) {
                                rawbuf_state[cur_buf] = RAWBUF_WRITING;
                                __dmb();
                            }
                        }

                        if (cur_buf >= 0) {
                            rawbuf_copy_line(
                                cur_buf, lidx, &b->data[14], SENSOR_W * 2);
                            lidx++;
                        } else {
                            lidx = -1;
                        }
                    }

                } else if (b->data[5] == 0xc7 && b->data[BUFSIZE - 3] == 0xda) {
                    if (!startup_synced && (lidx < 0 || cur_buf < 0)) {
                        if (++body_only_sync_cnt >= BODY_ONLY_RESYNC_THRES) {
                            body_only_sync_cnt = 0;
                            lidx = -1;
                            cur_buf = -1;
                            rawbuf_init();
                            rxring.ridx = rxring.widx;
                            wspi_stop1();
                            sleep_ms(2);
                            wspi_reinit1();
                            rxring.ridx = rxring.widx;
                            continue;
                        }
                    } else if (!startup_synced) {
                        body_only_sync_cnt = 0;
                    }
                    if (lidx >= 0 && cur_buf >= 0) {
                        if (lidx < SENSOR_H) {
                            rawbuf_copy_line(
                                cur_buf, lidx, &b->data[14], SENSOR_W * 2);
                        }
                        lidx++;
                    }
                } else if (b->data[5] == 0xc7 && b->data[BUFSIZE - 3] == 0xf1) {
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

                    if (lidx == SENSOR_H && cur_buf >= 0) {
                        /* VTEMP = mean of per-row D166 samples accumulated
                         * in rawbuf_copy_line (≈120 samples, √120 noise
                         * reduction vs. single-row extraction).
                         * T(FPA) = 25 + (vtemp - 8192) / 70  [°C, factory cal] */
                        const uint16_t vtemp =
                            rawbuf_vtemp_cnt[cur_buf] > 0
                                ? (uint16_t)(rawbuf_vtemp_sum[cur_buf]
                                             / rawbuf_vtemp_cnt[cur_buf])
                                : 0;
                        rawbuf[cur_buf][RAWBUF_OFS_VTEMP] =
                            (uint8_t)(vtemp >> 8);
                        rawbuf[cur_buf][RAWBUF_OFS_VTEMP + 1] =
                            (uint8_t)(vtemp & 0xFF);
                        __dmb();
                        rawbuf_state[cur_buf] = RAWBUF_READY;
                        rawbuf_last_ready = cur_buf;
                        __sev();

                        startup_synced = true;
                        cur_buf = -1;
                        lidx = -1;
                    } else {
                        lidx = -1;
                    }

                    wspi_reinit1();
                }
            }
            rxring.ridx = (rxring.ridx + 1) % BUFCOUNT;
        }
    }

    errh(nullptr);
core0_stop:
    while (true) {
        tight_loop_contents();
    }
    return 0;
}

///////// TEST CODE UNUSED START

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
        if (tud_video_n_streaming(0, 0) && !uvc_tx_busy) {
            fill_smpte_yuy2(uvc_buf[0], FRAME_SIZE);
            uvc_tx_busy = 1;
            tud_video_n_frame_xfer(0, 0, uvc_buf[0], FRAME_SIZE);
        }
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
///////////////// TEST CODE UNUSED END
