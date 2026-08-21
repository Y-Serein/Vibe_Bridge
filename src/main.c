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

#include "thermal_ncm.h"
#include "thermal_ota.h"
#include "usb_descriptors.h"

#if UVC_MJPEG640_STATIC_TEST
#    include "uvc_mjpeg_test_frame.h"
#endif

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
static uint8_t rawbuf[RAWBUFCNT][RAWBUFSIZETOTAL]
    __attribute__((aligned(__alignof__(uint16_t))));
static volatile uint8_t rawbuf_state[RAWBUFCNT];
static volatile int rawbuf_last_ready;
/* Core1 advances the capture epoch only after a shutter movement has fully
 * completed.  Core0 snapshots it at frame start, so a frame straddling the
 * mechanical boundary remains permanently identifiable even if it reaches
 * READY much later. */
static volatile uint32_t rawbuf_capture_epoch = 1u;
static volatile uint32_t rawbuf_epoch[RAWBUFCNT];
static enum UartOutMode ouomode = UART_OUT_MODE_DEF;

/* Per-frame VTEMP accumulator.  The sensor emits 2 u16 VTEMP samples at the
 * end of every row (bytes 324-325 in each per-row SPI buffer = the first u16
 * of the 4-byte per-row tail).  Averaging all 120 rows reduces VTEMP noise
 * ~11× (√120) vs. reading a single row at end-of-frame.  Without this the
 * FPA-temperature signal is buried under single-sample noise — apparent
 * VTEMP swings are too small to reliably track sensor self-heating. */
static uint32_t rawbuf_vtemp_sum[RAWBUFCNT];
static uint16_t rawbuf_vtemp_cnt[RAWBUFCNT];
static uint16_t rawbuf_uniform_rows[RAWBUFCNT];
static uint16_t rawbuf_uniform_max_run[RAWBUFCNT];
volatile uint16_t g_capture_last_frame_uniform_rows = 0;
volatile uint16_t g_capture_last_frame_uniform_max_run = 0;
uint8_t g_capture_raw_diag[RAWBUFSIZE] __attribute__((aligned(4)));
volatile bool g_capture_raw_diag_frozen = false;
int8_t g_capture_row_phase[SENSOR_H];
static uint16_t rawbuf_row_ref[RAWBUFCNT][SENSOR_H];
uint16_t g_capture_row_ref_diag[SENSOR_H];

enum adc16_snapshot_state {
    ADC16_SNAPSHOT_IDLE = 0,
    ADC16_SNAPSHOT_REQUESTED,
    ADC16_SNAPSHOT_READY,
    ADC16_SNAPSHOT_SENDING,
};
static uint8_t adc16_snapshot_state;




/* Core0 capture diagnostics are copied into the NCM metadata row by Core1.
 * They are monotonic, so a bad image can be correlated with an exact receive
 * overflow or frame-boundary resync without a serial console. */
volatile uint32_t g_capture_incomplete_frames = 0;
volatile uint32_t g_capture_unexpected_heads = 0;

/* Statistics for the most recent averaged shutter background candidate. */
volatile uint16_t g_ffc_bg_max_row_jump_adu = 0;
volatile uint8_t g_ffc_bg_max_row_jump_line = 0;
volatile uint16_t g_ffc_bg_min_row_mean = 0;
volatile uint16_t g_ffc_bg_max_row_mean = 0;

/* H1617 packet-sequence diagnostics. A complete image is one EC/DA head,
 * 119 C7/DA middle rows and one C7/F1 tail. These counters are observational:
 * they do not change the existing capture/resync decisions. */
volatile uint32_t g_capture_packet_heads = 0;
volatile uint32_t g_capture_packet_mids = 0;
volatile uint32_t g_capture_packet_tails = 0;
volatile uint32_t g_capture_packet_invalid = 0;
volatile uint16_t g_capture_runtime_resyncs = 0;
volatile uint16_t g_capture_last_resync_invalid_run = 0;
volatile uint16_t g_capture_max_invalid_run = 0;
volatile uint32_t g_capture_complete_sequences = 0;
volatile uint32_t g_capture_bad_tail_sequences = 0;
volatile uint32_t g_capture_head_after_head = 0;
volatile uint32_t g_capture_tail_without_head = 0;
volatile uint16_t g_capture_active_mid_count = 0;
volatile uint16_t g_capture_last_interrupted_mid_count = 0;
volatile uint16_t g_capture_last_tail_mid_count = 0;
volatile uint16_t g_capture_max_interrupted_mid_count = 0;
volatile uint32_t g_capture_interrupt_mid_0_31 = 0;
volatile uint32_t g_capture_interrupt_mid_32_63 = 0;
volatile uint32_t g_capture_interrupt_mid_64_95 = 0;
volatile uint32_t g_capture_interrupt_mid_96_plus = 0;

static inline __attribute__((always_inline)) void
capture_note_interrupted_sequence(uint16_t mid_count)
{
    g_capture_last_interrupted_mid_count = mid_count;
    if (mid_count > g_capture_max_interrupted_mid_count) {
        g_capture_max_interrupted_mid_count = mid_count;
    }
    if (mid_count <= 31) {
        g_capture_interrupt_mid_0_31++;
    } else if (mid_count <= 63) {
        g_capture_interrupt_mid_32_63++;
    } else if (mid_count <= 95) {
        g_capture_interrupt_mid_64_95++;
    } else {
        g_capture_interrupt_mid_96_plus++;
    }
}

static inline __attribute__((always_inline)) void rawbuf_init(void)
{
    for (int i = 0; i < RAWBUFCNT; i++) {
        rawbuf_state[i] = RAWBUF_FREE;
        rawbuf_epoch[i] = 0;
    }
    rawbuf_last_ready = -1;
}

static uint32_t rawbuf_advance_capture_epoch(void)
{
    uint32_t next = rawbuf_capture_epoch + 1u;
    if (next == 0) {
        next = 1u;
    }
    __dmb();
    rawbuf_capture_epoch = next;
    __dmb();
    return next;
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
    static uint16_t uniform_run[RAWBUFCNT];

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
        rawbuf_uniform_rows[buf_idx] = 0;
        rawbuf_uniform_max_run[buf_idx] = 0;
        uniform_run[buf_idx] = 0;
    }
    uint16_t refs[6];
    for (int k = 0; k < 6; ++k) {
        const uint8_t* p = k < 4
                               ? src - 8 + k * 2
                               : src + SENSOR_W * 2 + (k - 4) * 2;
        uint16_t value =
            ((((uint16_t)p[0]) << 8) | (uint16_t)p[1]) & 0x3FFFu;
        value ^= value >> 1;
        value ^= value >> 2;
        value ^= value >> 4;
        value ^= value >> 8;
        refs[k] = value & 0x3FFFu;
    }
    for (int i = 1; i < 6; ++i) {
        const uint16_t value = refs[i];
        int j = i;
        while (j > 0 && refs[j - 1] > value) {
            refs[j] = refs[j - 1];
            --j;
        }
        refs[j] = value;
    }
    rawbuf_row_ref[buf_idx][line] =
        (uint16_t)((refs[1] + refs[2] + refs[3] + refs[4] + 2u) / 4u);
    /* Equality is endian-independent. A real thermal row always has spatial
     * variation; failure packets repeat one code across all active columns. */
    const uint16_t* row16 = (const uint16_t*)src;
    bool uniform = true;
    for (size_t x = 1; x < SENSOR_W; ++x) {
        if (row16[x] != row16[0]) {
            uniform = false;
            break;
        }
    }
    if (uniform) {
        rawbuf_uniform_rows[buf_idx]++;
        uniform_run[buf_idx]++;
        if (uniform_run[buf_idx] > rawbuf_uniform_max_run[buf_idx]) {
            rawbuf_uniform_max_run[buf_idx] = uniform_run[buf_idx];
        }
    } else {
        uniform_run[buf_idx] = 0;
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
#if THERMAL_NCM_MODE
/* NCM v1 transmits temp8 directly. Keep only the normalized pixels and the
 * 70-byte internal per-frame diagnostics consumed while constructing Meta. */
#define TX_PAYLOAD (PX_CNT + 72)
#else
#define TX_PAYLOAD (RAWBUFSIZETOTAL + 2)
#endif
/* Core1 fills processed frames.  UART can hold one buffer in DMA while
 * Core1 prepares the next; UVC copies a READY buffer synchronously before
 * releasing it.  A third processed-frame buffer has no distinct owner. */
#define TXBUF_CNT  2
static int uart_tx_dma_chan = -1;
static uint8_t norm_buf[TXBUF_CNT][TX_PAYLOAD] __attribute__((aligned(4)));
static_assert(TX_PAYLOAD > PX_CNT + 70,
              "processed frame buffer must include internal diagnostics");
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
/* Normal output caches the float gain map in this 76.8 KiB workspace.  FFC
 * pauses the renderer, so the same storage can safely hold one uint32 sum per
 * pixel for the 8-frame shutter average, then reload the gain cache before
 * output resumes.  A union keeps peak SRAM unchanged. */
static union {
    uint8_t frames[CALIBUF_CNT][CALIBUFSIZE];
    uint32_t ffc_sum[PX_CNT];
} calib_workspace;
#define calibuf calib_workspace.frames
static_assert(sizeof(calib_workspace.frames) == sizeof(calib_workspace.ffc_sum),
              "FFC sum must reuse the calibration workspace without extra SRAM");
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
#define FFC_INTERVAL_WARMUP_3  4000u    /* 3rd FFC 4 s after 2nd → t≈10 s */
#define FFC_INTERVAL_WARMUP_4  20000u   /* 4th FFC 20 s after 3rd → t≈30 s */
/* Extended warmup window (0…180 s after boot). The shutter blade, lens
 * housing, and other body parts thermally settle on a ~50 s time
 * constant — during this window scene drift rate is ~0.13 °C/s even
 * with K_NTC compensation enabled (see trend_20260427_150825.png:
 * scene drops 23 °C in the first 3 min, then only 2 °C in the next
 * 2 min). At 15 s cadence the worst-case inter-FFC drift is bounded
 * to ~2 °C; at 30 s it is ~4 °C. Pay the extra shutter wear during
 * warmup to keep the displayed reading honest. */
#define FFC_INTERVAL_EXT_WARMUP_MS 15000u
#define FFC_EXT_WARMUP_END_MS      180000u
/* Stable-cadence FFC after extended warmup: 30 s caps the worst-case
 * inter-FFC bolometer drift in the quasi-equilibrium regime where the
 * remaining drift is small. */
#define FFC_INTERVAL_STABLE_MS 30000u
#define FFC_VTEMP_TRIGGER_ENABLE 0
#define FFC_VTEMP_DELTA_ADU     150u
#define FFC_CLOSED_DISCARD_FRAMES 2u
#define FFC_AVERAGE_FRAMES        8u
#define FFC_OPEN_DISCARD_FRAMES   4u  /* minimum fresh scene frames */
#define FFC_OPEN_MAX_DISCARD_FRAMES 12u
#define FFC_OPEN_STABLE_FRAMES      2u
#define FFC_OPEN_STABLE_DELTA_ADU   128u
#define FFC_POST_GUARD_MAX_REJECT_FRAMES 12u
#define FFC_FRAME_TIMEOUT_MS       300u
/* Minimum elapsed time before the |ΔVTEMP| trigger can fire — prevents
 * back-to-back FFCs during fast warmup where VTEMP can swing > 150 ADU
 * within 1–2 s, which would otherwise machine-gun the shutter. */
#define FFC_MIN_ELAPSED_MS      2000u
/* FFC sanity check: reject the FFC outright if the new shutter frame mean
 * (or VTEMP) differs from the previous FFC's value by more than these
 * thresholds. Between FFCs (≤ 30 s) the bolometer + FPA can realistically
 * change by ~3 °C ≈ 200 ADU VTEMP and at most ~K·200 ≈ 1500 ADU shutter
 * mean (for K ≤ 5). Anything larger is a sensor sync glitch, partial-
 * shutter capture, or VTEMP register glitch — committing such an FFC
 * poisons calibg/anchor and drives subsequent scene reads wildly off
 * (see trend_20260427_110503.png: scene crashed to −55 °C after one bad
 * FFC absorbed a +2000 ADU VTEMP glitch). Rejecting keeps the previous
 * known-good calibration in use until the next FFC attempt. */
#define FFC_REJECT_DG_ADU       2500
#define FFC_REJECT_DV_ADU       1000
/* Thermal-settled gate: defer FFC entirely when |raw_vtemp − smooth_vtemp|
 * exceeds this threshold. During a fast FPA transient (e.g. external heat
 * shock) raw races ahead of the EMA smoother by 100s of ADU; committing
 * FFC at that moment bakes a wildly off K_V·ΔV into ram_temp_anchor and
 * permanently corrupts the scene reading. Wait for the smoother to catch
 * up (~6 s @ τ=3 s) before allowing FFC. 100 ADU ≈ 1.4 °C-equivalent. */
#define FFC_VTEMP_SETTLED_ADU   100
/* Thermal compensation model selector.  TABLE is the bounded cold-start
 * correction identified from trend_20260730_092834.csv.  Keep OFF and LEGACY
 * available for a one-line rollback during A/B validation. */
#define THERMAL_COMP_MODEL_OFF     0
#define THERMAL_COMP_MODEL_LEGACY  1
#define THERMAL_COMP_MODEL_TABLE   2

#define THERMAL_COMP_MODEL THERMAL_COMP_MODEL_TABLE
#define THERMAL_COMP_TABLE_LIMIT_ADU 600

struct ThermalCompTablePoint {
    uint32_t elapsed_ms;
    int16_t anchor_comp_adu;
};

static const struct ThermalCompTablePoint thermal_comp_table[] = {
    {     0u,    0},
    { 30000u, -230},
    { 60000u, -345},
    { 90000u, -395},
    {120000u, -415},
    {180000u, -445},
    {240000u, -455},
    {320000u, -457},
};

#if THERMAL_COMP_MODEL < THERMAL_COMP_MODEL_OFF \
    || THERMAL_COMP_MODEL > THERMAL_COMP_MODEL_TABLE
#error "Invalid THERMAL_COMP_MODEL"
#endif

static inline bool thermal_comp_legacy_enabled(void)
{
    return THERMAL_COMP_MODEL == THERMAL_COMP_MODEL_LEGACY;
}

static inline bool thermal_comp_table_enabled(void)
{
    return THERMAL_COMP_MODEL == THERMAL_COMP_MODEL_TABLE;
}

/* Between-FFC drift compensation: STATIC (compile-time) coefficients
 * for two independent compensators, applied to eff_anchor as
 *   eff_anchor = ram_temp_anchor + K_V·(V_now - V_lastFFC)
 *                                + K_N·(NTC_now - NTC_lastFFC)
 *
 * K_V (VTEMP-based, FPA-coupled drift):
 *   Disabled for temperature-anchor compensation.  The sensor VTEMP register
 *   is useful for FFC scheduling/sanity checks, but bench tests show it can
 *   move independently of a fixed blackbody scene and can also glitch.  Feeding
 *   that signal into eff_anchor creates false absolute-temperature drift
 *   (50°C blackbody falling toward ~41°C), so keep K_V at zero unless VTEMP is
 *   proven correlated on this hardware revision.
 *
 * K_N (NTC-based, shutter/body-coupled drift):
 *   Targets shutter-blade and lens-housing thermal lag — these heat
 *   on a much longer time constant (~50 s) than the FPA, and the
 *   bolometer raw counts respond to (T_shutter - T_scene) via the
 *   (raw - calibg) term. trend_20260427_143902.png shows the canonical
 *   case: FPA stable at 130 °C from t=0–130 s but scene reading drops
 *   31 °C as shutter blade warms toward equilibrium with the hot FPA.
 *   K_V·ΔV = 0 in this regime; only NTC tracks the slow body warming
 *   and so only K_N can cancel the residual.
 *
 * Sign of K_N depends on the NTC voltage divider topology (thermistor
 * pull-up vs pull-down). If shutter heating produces a scene drift
 * opposite to the compensation (i.e. the residual gets WORSE, not
 * better), flip the sign. Magnitude estimated from the trend:
 *   Δscene ≈ 31 °C → Δsmooth ≈ md/50.4·31 ≈ 120 ADU (md ≈ 200)
 *   ΔNTC ≈ 500–1000 ADU over the 130 s warming window
 *   |K_N| ≈ 0.12–0.24 → start at 0.20.
 *
 * Cap K_COMP_MAX_ADU bounds glitch amplification on either signal. */
#define K_ANCHOR_COMP_STATIC    0.0f    /* K_V — disabled: VTEMP can be false */
#define K_NTC_COMP_STATIC       (+2.3f) /* K_N — normal NTC-based */
#define K_NTC_COMP_LATE_STATIC  (+3.0f) /* K_N — after warmup curve hands off */
/* dV input clamp for K_V·ΔV. The K_V model (linear bolometer drift with
 * FPA temperature) holds for slow drift (ΔV < ~300 ADU between FFCs) but
 * breaks for thermal shocks (ΔV > 1000 ADU): the bolometer doesn't actually
 * follow the linear extrapolation, so K_V·ΔV over-corrects and crashes the
 * scene reading. Clamping dV at the input bounds the per-frame and per-FFC
 * scene shift to ≤ K_V·CLAMP / mean_diff·range ≈ 8 °C even on a 30 °C FPA
 * jump. Normal operation (dV < 400) is unaffected. */
#define DV_CLAMP_ADU            400
/* NTC plausibility cross-check for dV. The VTEMP register can spontaneously
 * glitch (jumps 30 °C-equivalent without any real die heating — confirmed
 * by user: putting hand on lens does NOT change FPA reading, yet FPA jumps
 * by itself in static conditions). NTC measures real PCB body temp via a
 * physical thermistor; it cannot glitch. Real thermal events show NTC and
 * VTEMP changing together (die heats faster than body, so dV/dN is bounded
 * but positive). VTEMP glitches show dN≈0 with huge dV. Bounding |dV| by
 * DV_DN_RATIO_MAX·|dN| makes K_V·dV completely inert during glitches while
 * preserving full strength for real thermal drift. Ratio 10 covers the
 * worst-case warmup phase where die leads body. */
#define DV_DN_RATIO_MAX         10
/* FFC hard plausibility gate. Board NTC uses a 10K pull-up, so ADC falls when
 * body temperature rises, while VTEMP rises when the die/FPA warms. Real
 * thermal motion should therefore drive dV and dN in opposite signs. A
 * same-sign large move (for example VTEMP falling while NTC temperature rises)
 * is a VTEMP-register false event; do not commit an FFC in that window. */
#define FFC_VTEMP_NTC_FREE_ADU      80
#define FFC_VTEMP_NTC_RATIO_MAX     6
#define FFC_NTC_SIGN_DEADBAND_ADU   3
#define FFC_RETRY_COOLDOWN_MS       15000u
#define NTC_WARMUP_CURVE_COMP_ADU   120
#define NTC_WARMUP_CURVE_TAU_MS     12000u
#define NTC_WARMUP_FADE_START_MS    180000u
#define NTC_WARMUP_FADE_END_MS      320000u
#define NTC_WARMUP_NTC_FADE_START_ADU 80
#define NTC_WARMUP_NTC_FADE_END_ADU   160
/* Jump guard for runtime FFC.  A single large predicted scene jump is often a
 * partial shutter/glitch and should be rejected.  However, repeatedly rejecting
 * the same direction also starves runtime FFC and lets the temperature window
 * walk away for minutes.  After a few soft-jump rejects, accept a bounded
 * recovery candidate as long as hard dG/dV gates have already passed. */
#define FFC_SOFT_JUMP_X10       50  /* first reject if predicted jump > 5.0°C */
#define FFC_HARD_JUMP_X10       250 /* always reject if predicted jump > 25°C */
#define FFC_RECOVER_AFTER_JUMP_REJECTS 2
#define K_COMP_MAX_ADU          1500
static uint32_t ffc_last_ms = 0;
static uint32_t ffc_retry_after_ms = 0;
static uint16_t ffc_last_fpa_vtemp = 0;
static uint16_t ffc_last_ntc = 0;            /* NTC ADC at last FFC */
static uint16_t boot_ntc = 0;                /* valid NTC ADC captured at power-on */
static uint32_t boot_ntc_ms = 0;             /* timestamp of the first valid NTC */
static int32_t  ffc_last_g_calib_mean = 0;  /* g_calib_mean at last FFC */
static uint16_t ffc_count = 0;               /* number of FFCs run since boot */
static uint8_t  ffc_consec_reject = 0;        /* consecutive sanity-rejects */
static bool     ffc_done_first = false;

/* Bounded piecewise-linear correction relative to the first valid board-NTC
 * sample.  It is intentionally independent of each FFC epoch: resetting the
 * curve at every shutter event would replay cold-start compensation and cause
 * a sawtooth temperature error. */
static int32_t thermal_comp_table_for_now(void)
{
    if (!thermal_comp_table_enabled()) {
        return 0;
    }

    const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    const uint32_t start_ms = boot_ntc_ms ? boot_ntc_ms : now_ms;
    const uint32_t elapsed_ms = now_ms - start_ms;
    const size_t point_count =
        sizeof(thermal_comp_table) / sizeof(thermal_comp_table[0]);
    int32_t comp = thermal_comp_table[point_count - 1u].anchor_comp_adu;

    for (size_t i = 1; i < point_count; i++) {
        if (elapsed_ms <= thermal_comp_table[i].elapsed_ms) {
            const struct ThermalCompTablePoint* const p0 =
                &thermal_comp_table[i - 1u];
            const struct ThermalCompTablePoint* const p1 =
                &thermal_comp_table[i];
            const uint32_t span_ms = p1->elapsed_ms - p0->elapsed_ms;
            const uint32_t offset_ms = elapsed_ms - p0->elapsed_ms;
            const int32_t delta =
                (int32_t)p1->anchor_comp_adu - p0->anchor_comp_adu;
            comp = p0->anchor_comp_adu
                   + (int32_t)((delta * (int32_t)offset_ms)
                               / (int32_t)span_ms);
            break;
        }
    }

    if (comp > THERMAL_COMP_TABLE_LIMIT_ADU) {
        comp = THERMAL_COMP_TABLE_LIMIT_ADU;
    } else if (comp < -THERMAL_COMP_TABLE_LIMIT_ADU) {
        comp = -THERMAL_COMP_TABLE_LIMIT_ADU;
    }
    return comp;
}

enum FfcEventDecision : uint8_t {
    FFC_EVT_ACCEPT = 0,
    FFC_EVT_REJECT_VN,
    FFC_EVT_REJECT_LIMIT,
    FFC_EVT_REJECT_JUMP,
    FFC_EVT_ABORT_NO_FRAME,
    FFC_EVT_NONE = 255,
};

/* Diagnostic tail appended after the 6-byte temperature tail (VTEMP+T_lo+T_hi):
 *    [PX_CNT+ 7 .. +10] ram_temp_anchor     int32 BE
 *    [PX_CNT+11 .. +12] g_smooth_low        u16   BE
 *    [PX_CNT+13 .. +14] g_smooth_high       u16   BE
 *    [PX_CNT+15 .. +18] ram_mean_diff       f32   BE (IEEE 754)
 *    [PX_CNT+19 .. +20] ffc_last_ntc        u16   BE (0 before first FFC)
 *    [PX_CNT+21 .. +22] cur_ntc             u16   BE
 *    [PX_CNT+23]         last FFC/NUC event decision
 *    [PX_CNT+24]         ffc_count after the last accepted NUC
 *    [PX_CNT+25 .. +26] last NUC dg         i16   BE
 *    [PX_CNT+27 .. +28] last NUC dv         i16   BE
 *    [PX_CNT+29 .. +30] last NUC dn         i16   BE
 *    [PX_CNT+31 .. +32] boot_ntc            u16   BE
 *    [PX_CNT+33 .. +34] |cur_ntc-boot_ntc|  u16   BE (0xffff if invalid)
 *    [PX_CNT+35 .. +36] warmup NTC comp     i16   BE
 *    [PX_CNT+37 .. +38] normal NTC comp     i16   BE
 *    [PX_CNT+39 .. +40] total anchor comp   i16   BE
 *    [PX_CNT+41 .. +44] effective anchor    int32 BE
 *    [PX_CNT+45 .. +46] smooth VTEMP        u16   BE
 *    [PX_CNT+47 .. +48] center raw temp     i16   BE (x0.1°C, raw corrected ADU)
 *    [PX_CNT+49 .. +50] center 5x5 temp     i16   BE (x0.1°C, raw corrected ADU)
 *    [PX_CNT+51 .. +52] center raw ADU      u16   BE
 *    [PX_CNT+53 .. +54] center 5x5 ADU      u16   BE
 *    [PX_CNT+55 .. +58] temperature low     f32   BE (degrees Celsius)
 *    [PX_CNT+59 .. +62] temperature high    f32   BE (degrees Celsius)
 *    [PX_CNT+63 .. +64] scene minimum        i16   BE (x0.1°C)
 *    [PX_CNT+65 .. +66] scene maximum        i16   BE (x0.1°C)
 *    [PX_CNT+67]         scene minimum X
 *    [PX_CNT+68]         scene minimum Y
 *    [PX_CNT+69]         scene maximum X
 *    [PX_CNT+70]         scene maximum Y
 * Total frame bytes sent in CALI_U8 mode: PX_CNT + 7 + FRAME_DIAG_TAIL. */
#define FRAME_DIAG_TAIL     64

/* VTEMP EMA smoother (x256 fixed-point). The raw VTEMP register has
 * frame-to-frame ADC jitter of ~50–100 ADU (~0.7–1.4 °C-equivalent),
 * which K_V·ΔV would otherwise turn into visible scene jitter. EMA with
 * α = 1/32 gives τ ≈ 3 s @ 10 fps — fast enough to track FPA settling
 * over tens of seconds, slow enough to reject per-frame ADC noise. */
static volatile uint32_t g_vtemp_smooth_x256 = 0;

/* NTC sampling. ADC2 = GPIO28 on the current board.  EMA-smoothed sample is
 * exposed in the diag tail at out[PX_CNT+21..22] and feeds K_NTC compensation
 * only when it is not rail-saturated. */
static volatile uint16_t g_cur_ntc_adc = 0;
static volatile uint32_t g_ntc_smooth_x256 = 0;
static volatile uint16_t g_adc_temp_adc = 0;
static volatile uint16_t g_adc_temp_cs_low16 = 0;
static volatile uint16_t g_ntc_adc_cs_low16 = 0;
#define ADC_DIAG_CFG_WORD \
    ((((uint16_t)ADC_BASE_PIN) << 8) \
     | (((uint16_t)NUM_ADC_CHANNELS) << 4) \
     | ((uint16_t)ADC_TEMPERATURE_CHANNEL_NUM))
static volatile uint16_t g_adc_cfg_word = ADC_DIAG_CFG_WORD;
#define NTC_ADC_SAMPLES 256
#define NTC_ADC_DMA_TIMEOUT_MS 5
#define NTC_FAST_TRACK_MS 60000u
#define NTC_EMA_FAST_SHIFT 3
#define NTC_EMA_SLOW_SHIFT 5
static uint16_t ntc_adcbuf[NTC_ADC_SAMPLES];
static int ntc_adc_dma_chan = -1;
static dma_channel_config ntc_adc_dma_cfg;
static bool ntc_adc_dma_ready = false;
static volatile uint8_t g_ffc_last_decision = FFC_EVT_NONE;
static volatile int16_t g_ffc_last_dg = 0;
static volatile int16_t g_ffc_last_dv = 0;
static volatile int16_t g_ffc_last_dn = 0;

static inline bool ntc_adc_valid(uint16_t v)
{
    return v > 8 && v < 4088;
}

static bool ffc_vtemp_ntc_plausible(
    uint16_t cur_fpa_vtemp,
    uint16_t cur_ntc,
    int32_t* dv_out,
    int32_t* dn_out)
{
    if (ffc_last_fpa_vtemp == 0
        || !ntc_adc_valid(ffc_last_ntc)
        || !ntc_adc_valid(cur_ntc)) {
        return true;
    }

    const int32_t dv = (int32_t)cur_fpa_vtemp - (int32_t)ffc_last_fpa_vtemp;
    const int32_t dn = (int32_t)cur_ntc - (int32_t)ffc_last_ntc;
    if (dv_out != nullptr) *dv_out = dv;
    if (dn_out != nullptr) *dn_out = dn;

    const int32_t adv = dv < 0 ? -dv : dv;
    if (adv <= FFC_VTEMP_NTC_FREE_ADU) {
        return true;
    }

    const int32_t adn = dn < 0 ? -dn : dn;
    if (adn <= FFC_NTC_SIGN_DEADBAND_ADU) {
        return false;
    }

    if ((dv > 0 && dn > 0) || (dv < 0 && dn < 0)) {
        return false;
    }

    return adv <= (FFC_VTEMP_NTC_FREE_ADU
                   + adn * FFC_VTEMP_NTC_RATIO_MAX);
}

static inline uint16_t u16_absdiff(uint16_t a, uint16_t b)
{
    return (a > b) ? (a - b) : (b - a);
}

static inline int32_t ntc_warmup_curve_comp_for(uint16_t cur_ntc)
{
    if (!ntc_adc_valid(boot_ntc) || !ntc_adc_valid(cur_ntc)) {
        return 0;
    }

    /* The board NTC lags the shutter/lens during cold start.  The latest
     * trend shows scene keeps falling even after NTC has started moving, so
     * this curve exits only after a larger body-temperature rise or a long
     * timeout, then the normal K_NTC path owns the residual drift. */
    const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    const uint32_t start_ms = boot_ntc_ms ? boot_ntc_ms : now_ms;
    const uint32_t elapsed_ms = now_ms - start_ms;
    if (elapsed_ms >= NTC_WARMUP_FADE_END_MS) {
        return 0;
    }

    uint32_t mag = ((uint32_t)NTC_WARMUP_CURVE_COMP_ADU * elapsed_ms)
                   / (elapsed_ms + NTC_WARMUP_CURVE_TAU_MS);

    if (elapsed_ms > NTC_WARMUP_FADE_START_MS) {
        mag = mag * (NTC_WARMUP_FADE_END_MS - elapsed_ms)
              / (NTC_WARMUP_FADE_END_MS - NTC_WARMUP_FADE_START_MS);
    }

    const uint16_t boot_delta = u16_absdiff(cur_ntc, boot_ntc);
    if (boot_delta > NTC_WARMUP_NTC_FADE_START_ADU) {
        if (boot_delta >= NTC_WARMUP_NTC_FADE_END_ADU) {
            return 0;
        }
        mag = mag * (uint32_t)(NTC_WARMUP_NTC_FADE_END_ADU - boot_delta)
              / (uint32_t)(NTC_WARMUP_NTC_FADE_END_ADU
                           - NTC_WARMUP_NTC_FADE_START_ADU);
    }

    return -(int32_t)mag;
}

static inline float ntc_normal_comp_k_for(uint16_t cur_ntc)
{
    float k = K_NTC_COMP_STATIC;
    if (ntc_adc_valid(boot_ntc)) {
        const uint16_t boot_delta = u16_absdiff(cur_ntc, boot_ntc);
        if (boot_delta >= NTC_WARMUP_NTC_FADE_END_ADU) {
            k = K_NTC_COMP_LATE_STATIC;
        } else if (boot_delta > NTC_WARMUP_NTC_FADE_START_ADU) {
            const float w = (float)(boot_delta - NTC_WARMUP_NTC_FADE_START_ADU)
                            * (1.0f / (float)(NTC_WARMUP_NTC_FADE_END_ADU
                                               - NTC_WARMUP_NTC_FADE_START_ADU));
            k += (K_NTC_COMP_LATE_STATIC - K_NTC_COMP_STATIC) * w;
        }
    }
    return k;
}

static inline int32_t ntc_normal_comp_between(uint16_t ref_ntc, uint16_t cur_ntc)
{
    if (!ntc_adc_valid(ref_ntc) || !ntc_adc_valid(cur_ntc)) {
        return 0;
    }

    const int32_t dn = (int32_t)cur_ntc - (int32_t)ref_ntc;
    return (int32_t)(ntc_normal_comp_k_for(cur_ntc) * (float)dn);
}

static inline int32_t ntc_normal_comp_for(uint16_t cur_ntc)
{
    return ntc_normal_comp_between(ffc_last_ntc, cur_ntc);
}

static inline int32_t ntc_anchor_comp_for(uint16_t cur_ntc)
{
    if (ntc_adc_valid(boot_ntc)) {
        return ntc_normal_comp_between(boot_ntc, cur_ntc);
    }
    return ntc_normal_comp_for(cur_ntc);
}

static inline int32_t ntc_frame_comp_for(uint16_t cur_ntc)
{
    int32_t comp = ntc_normal_comp_for(cur_ntc);
    comp += ntc_warmup_curve_comp_for(cur_ntc);
    return comp;
}

static inline int32_t i32_abs(int32_t v)
{
    return v < 0 ? -v : v;
}

static inline int16_t clip_i16_i32(int32_t v)
{
    if (v < INT16_MIN) {
        return INT16_MIN;
    }
    if (v > INT16_MAX) {
        return INT16_MAX;
    }
    return (int16_t)v;
}

static int16_t temp_x10_from_corrected_adu(int32_t adu, int32_t eff_anchor)
{
    if (ram_mean_diff <= 0.0f || ram_temp_anchor == 0) {
        return INT16_MAX;
    }

    const float T_L_K = ram_t_ref_low + 273.15f;
    const float T_H_K = T_REF_HIGH + 273.15f;
    const float T_L_K4 = T_L_K * T_L_K * T_L_K * T_L_K;
    const float T_H_K4 = T_H_K * T_H_K * T_H_K * T_H_K;
    const float f = (float)(adu - eff_anchor) / ram_mean_diff;
    float k4 = T_L_K4 + f * (T_H_K4 - T_L_K4);
    if (k4 < 1.0f) {
        k4 = 1.0f;
    }
    const float t_c = sqrtf(sqrtf(k4)) - 273.15f;
    const int32_t x10 = (int32_t)roundf(t_c * 10.0f);
    if (x10 < INT16_MIN) {
        return INT16_MIN;
    }
    if (x10 > INT16_MAX) {
        return INT16_MAX;
    }
    return (int16_t)x10;
}

static uint16_t center_roi_mean_u14(const uint16_t* img, int radius)
{
    const int cx = SENSOR_W / 2;
    const int cy = SENSOR_H / 2;
    uint32_t sum = 0;
    uint32_t count = 0;
    for (int dy = -radius; dy <= radius; ++dy) {
        const int y = cy + dy;
        if (y < 0 || y >= SENSOR_H) {
            continue;
        }
        for (int dx = -radius; dx <= radius; ++dx) {
            const int x = cx + dx;
            if (x < 0 || x >= SENSOR_W) {
                continue;
            }
            sum += img[(uint32_t)y * SENSOR_W + (uint32_t)x];
            count++;
        }
    }
    return count != 0 ? (uint16_t)(sum / count) : img[(uint32_t)cy * SENSOR_W + (uint32_t)cx];
}

static void ffc_record_event(uint8_t decision,
                             int32_t dg,
                             int32_t dv,
                             int32_t dn,
                             int32_t old_anchor,
                             int32_t new_anchor,
                             int16_t jump_x10,
                             int32_t new_mean,
                             uint16_t vtemp,
                             uint16_t ntc)
{
    g_ffc_last_decision = decision;
    g_ffc_last_dg = clip_i16_i32(dg);
    g_ffc_last_dv = clip_i16_i32(dv);
    g_ffc_last_dn = clip_i16_i32(dn);
    println("FFC evt dec=%u cnt=%u dg=%ld dv=%ld dn=%ld oldA=%ld newA=%ld jump_x10=%d mean=%ld V=%u N=%u",
            (unsigned)decision,
            (unsigned)ffc_count,
            (long)dg,
            (long)dv,
            (long)dn,
            (long)old_anchor,
            (long)new_anchor,
            (int)jump_x10,
            (long)new_mean,
            (unsigned)vtemp,
            (unsigned)ntc);
}

static inline uint16_t adc_trimmed_mean_from_buf(
    const uint16_t* data, uint32_t size)
{
    uint32_t sum = 0;
    uint16_t min = 0x0FFF;
    uint16_t max = 0;

    if (data == nullptr || size < 3) return 0;

    for (uint32_t i = 0; i < size; i++) {
        const uint16_t v = data[i] & 0x0FFF;
        sum += v;
        if (v < min) min = v;
        if (v > max) max = v;
    }

    sum -= min;
    sum -= max;
    return (uint16_t)(sum / (size - 2));
}

static inline void adc_clear_err_sticky(void)
{
    const uint32_t keep = adc_hw->cs
        & (ADC_CS_EN_BITS | ADC_CS_TS_EN_BITS | ADC_CS_AINSEL_BITS
           | ADC_CS_RROBIN_BITS);
    adc_hw->cs = keep | ADC_CS_ERR_STICKY_BITS;
}

static inline uint16_t adc_read_single_avg_input(uint input, int n)
{
    if (n < 1) n = 1;
    adc_select_input(input);
    adc_clear_err_sticky();
    (void)adc_read();
    uint32_t acc = 0;
    for (int i = 0; i < n; i++) acc += adc_read();
    return (uint16_t)(acc / (uint32_t)n);
}

static uint16_t adc_read_dma_trimmed_input(uint input)
{
    if (!ntc_adc_dma_ready) {
        return adc_read_single_avg_input(input, 8);
    }

    adc_run(false);
    adc_select_input(input);
    adc_clear_err_sticky();
    adc_fifo_drain();
    adc_fifo_setup(true, true, 1, false, false);

    dma_channel_configure(ntc_adc_dma_chan,
                          &ntc_adc_dma_cfg,
                          ntc_adcbuf,
                          &adc_hw->fifo,
                          NTC_ADC_SAMPLES,
                          false);
    adc_run(true);
    dma_channel_start(ntc_adc_dma_chan);

    /* Never let an ADC/DREQ fault starve TinyUSB and the lwIP timers. The
     * nominal 256-sample conversion takes well below 1 ms; 5 ms leaves ample
     * margin while keeping the NCM data plane alive during the wait. */
    const absolute_time_t deadline =
        make_timeout_time_ms(NTC_ADC_DMA_TIMEOUT_MS);
    while (dma_channel_is_busy(ntc_adc_dma_chan)) {
        thermal_ncm_task();
        if (time_reached(deadline)) {
            adc_run(false);
            dma_channel_abort(ntc_adc_dma_chan);
            adc_fifo_drain();
            return 0;
        }
    }
    adc_run(false);
    adc_fifo_drain();

    return adc_trimmed_mean_from_buf(ntc_adcbuf, NTC_ADC_SAMPLES);
}

static inline uint16_t ntc_read_avg(int n)
{
    (void)n;
    const uint16_t v = adc_read_dma_trimmed_input(ADC_CH_NTC);
    g_ntc_adc_cs_low16 = (uint16_t)(adc_hw->cs & 0xFFFFu);
    return v;
}

static void ntc_adc_dma_init(void)
{
    memset(ntc_adcbuf, 0, sizeof(ntc_adcbuf));
    adc_fifo_setup(true, true, 1, false, false);

    ntc_adc_dma_chan = dma_claim_unused_channel(true);
    ntc_adc_dma_cfg = dma_channel_get_default_config(ntc_adc_dma_chan);
    channel_config_set_transfer_data_size(&ntc_adc_dma_cfg, DMA_SIZE_16);
    channel_config_set_read_increment(&ntc_adc_dma_cfg, false);
    channel_config_set_write_increment(&ntc_adc_dma_cfg, true);
    channel_config_set_dreq(&ntc_adc_dma_cfg, DREQ_ADC);
    ntc_adc_dma_ready = true;
}

static inline void ntc_tick(void)
{
    static uint8_t tick = 0;
    const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    const bool fast = (now_ms < NTC_FAST_TRACK_MS);

    tick++;
    if ((tick & (fast ? 0x03u : 0x07u)) != 0) return;

    const uint16_t v = ntc_read_avg(4);
    if (!ntc_adc_valid(v)) return;

    if (!ntc_adc_valid(boot_ntc)) {
        boot_ntc = v;
        boot_ntc_ms = now_ms;
    }

    uint32_t smooth = g_ntc_smooth_x256;
    const uint32_t target = ((uint32_t)v) << 8;
    if (smooth == 0) {
        smooth = target;
    } else {
        const uint8_t shift = fast ? NTC_EMA_FAST_SHIFT : NTC_EMA_SLOW_SHIFT;
        if (target >= smooth) {
            smooth += (target - smooth) >> shift;
        } else {
            smooth -= (smooth - target) >> shift;
        }
    }

    g_ntc_smooth_x256 = smooth;
    g_cur_ntc_adc = (uint16_t)((smooth + 128u) >> 8);
}

static void adc_probe_core(void)
{
    adc_set_temp_sensor_enabled(true);
    g_adc_temp_adc = adc_read_dma_trimmed_input(ADC_TEMPERATURE_CHANNEL_NUM);
    g_adc_temp_cs_low16 = (uint16_t)(adc_hw->cs & 0xFFFFu);
    adc_set_temp_sensor_enabled(false);
    g_adc_cfg_word = ADC_DIAG_CFG_WORD;

    adc_select_input(ADC_CH_NTC);
}

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

#define DISPLAY_ROW_STRIPE_MAX_ADU 48
#define DISPLAY_COL_STRIPE_MAX_ADU 48
#define DISPLAY_COL_STRIPE_MAX_U8  4
#define DISPLAY_DETAIL_MAX_ADU     32

static inline int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline void display_row_destripe_u14(uint16_t* img)
{
    int32_t row_mean[SENSOR_H];

    for (int y = 0; y < SENSOR_H; y++) {
        uint32_t sum = 0;
        const uint16_t* row = &img[y * SENSOR_W];
        for (int x = 0; x < SENSOR_W; x++) {
            sum += row[x];
        }
        row_mean[y] = (int32_t)(sum / SENSOR_W);
    }

    for (int y = 0; y < SENSOR_H; y++) {
        int32_t ref_sum = 0;
        int32_t ref_cnt = 0;
        for (int dy = -2; dy <= 2; dy++) {
            if (dy == 0) continue;
            const int yy = y + dy;
            if (yy < 0 || yy >= SENSOR_H) continue;
            ref_sum += row_mean[yy];
            ref_cnt++;
        }
        if (ref_cnt == 0) continue;

        const int32_t local_ref = ref_sum / ref_cnt;
        int32_t corr = row_mean[y] - local_ref;
        corr = clamp_i32(corr, -DISPLAY_ROW_STRIPE_MAX_ADU,
                         DISPLAY_ROW_STRIPE_MAX_ADU);
        corr = (corr * 3) / 4;
        if (corr == 0) continue;

        uint16_t* row = &img[y * SENSOR_W];
        for (int x = 0; x < SENSOR_W; x++) {
            row[x] = clip_u16((int32_t)row[x] - corr);
        }
    }
}

static inline void display_col_destripe_u14(uint16_t* img)
{
    int32_t col_mean[SENSOR_W];

    for (int x = 0; x < SENSOR_W; x++) {
        uint32_t sum = 0;
        for (int y = 0; y < SENSOR_H; y++) {
            sum += img[y * SENSOR_W + x];
        }
        col_mean[x] = (int32_t)(sum / SENSOR_H);
    }

    for (int x = 0; x < SENSOR_W; x++) {
        int32_t ref_sum = 0;
        int32_t ref_cnt = 0;
        for (int dx = -2; dx <= 2; dx++) {
            if (dx == 0) continue;
            const int xx = x + dx;
            if (xx < 0 || xx >= SENSOR_W) continue;
            ref_sum += col_mean[xx];
            ref_cnt++;
        }
        if (ref_cnt == 0) continue;

        const int32_t local_ref = ref_sum / ref_cnt;
        int32_t corr = col_mean[x] - local_ref;
        corr = clamp_i32(corr, -DISPLAY_COL_STRIPE_MAX_ADU,
                         DISPLAY_COL_STRIPE_MAX_ADU);
        corr = (corr * 3) / 4;
        if (corr == 0) continue;

        for (int y = 0; y < SENSOR_H; y++) {
            uint16_t* p = &img[y * SENSOR_W + x];
            *p = clip_u16((int32_t)*p - corr);
        }
    }
}

static inline uint8_t clip_u8_i32(int32_t v)
{
    if (v < 0) {
        return 0;
    }
    if (v > 255) {
        return 255;
    }
    return (uint8_t)v;
}

static inline void display_col_destripe_u8(uint8_t* img)
{
    int32_t col_mean[SENSOR_W];

    for (int x = 0; x < SENSOR_W; x++) {
        uint32_t sum = 0;
        for (int y = 0; y < SENSOR_H; y++) {
            sum += img[y * SENSOR_W + x];
        }
        col_mean[x] = (int32_t)(sum / SENSOR_H);
    }

    for (int x = 0; x < SENSOR_W; x++) {
        int32_t ref_sum = 0;
        int32_t ref_cnt = 0;
        for (int dx = -2; dx <= 2; dx++) {
            if (dx == 0) continue;
            const int xx = x + dx;
            if (xx < 0 || xx >= SENSOR_W) continue;
            ref_sum += col_mean[xx];
            ref_cnt++;
        }
        if (ref_cnt == 0) continue;

        const int32_t local_ref = ref_sum / ref_cnt;
        int32_t corr = col_mean[x] - local_ref;
        corr = clamp_i32(corr, -DISPLAY_COL_STRIPE_MAX_U8,
                         DISPLAY_COL_STRIPE_MAX_U8);
        corr = (corr * 3) / 4;
        if (corr == 0) continue;

        for (int y = 0; y < SENSOR_H; y++) {
            uint8_t* p = &img[y * SENSOR_W + x];
            *p = clip_u8_i32((int32_t)*p - corr);
        }
    }
}

static inline uint16_t display_detail_boost_u14(
    const uint16_t* img, int x, int y)
{
    const int idx = y * SENSOR_W + x;
    if (x <= 0 || x >= SENSOR_W - 1 || y <= 0 || y >= SENSOR_H - 1) {
        return img[idx];
    }

    const int32_t blur =
        ((int32_t)img[idx - 1] + (int32_t)img[idx + 1]
         + (int32_t)img[idx - SENSOR_W] + (int32_t)img[idx + SENSOR_W]) >> 2;
    int32_t detail = (int32_t)img[idx] - blur;
    detail = clamp_i32(detail, -DISPLAY_DETAIL_MAX_ADU,
                       DISPLAY_DETAIL_MAX_ADU);
    return clip_u16((int32_t)img[idx] + (detail >> 1));
}

#if !THERMAL_NCM_MODE
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
#endif

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
    if (opinfo == nullptr
        || opinfo->ofs > FLASH_CALIBRATION_DATA_SIZE
        || opinfo->size > FLASH_CALIBRATION_DATA_SIZE - opinfo->ofs) {
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
    if (flashop == nullptr || flashop->data == nullptr || flashop->size == 0
        || flashop->ofs % FLASH_SECTOR_SIZE != 0
        || flashop->ofs > FLASH_CALIBRATION_DATA_SIZE
        || flashop->size > FLASH_CALIBRATION_DATA_SIZE - flashop->ofs) {
        return false;
    }
    const uint32_t erase_size =
        (flashop->size + FLASH_SECTOR_SIZE - 1u)
        & ~(FLASH_SECTOR_SIZE - 1u);
    if (erase_size > FLASH_CALIBRATION_DATA_SIZE - flashop->ofs) {
        return false;
    }
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
    uint32_t n;
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
        if (unlikely(y >= SENSOR_H || x >= SENSOR_W)) {
            continue;
        }

        sum = 0;
        n = 0;
        for (int dy = -1; dy <= 1; dy++) {
            const int ny = (int)y + dy;
            if (ny < 0 || ny >= SENSOR_H) {
                continue;
            }
            for (int dx = -1; dx <= 1; dx++) {
                const int nx = (int)x + dx;
                if ((dx == 0 && dy == 0) || nx < 0 || nx >= SENSOR_W) {
                    continue;
                }
                sum += raw[(uint32_t)ny * SENSOR_W + (uint32_t)nx];
                n++;
            }
        }
        if (n != 0) {
            raw[(uint32_t)y * SENSOR_W + (uint32_t)x] =
                (uint16_t)(sum / n);
        }

        if (dataomode == DOM_UVC) {
            tud_task();
        }
    }
}

static inline __attribute__((always_inline)) void binary_calib_frame(
    uint8_t* raw,
    const uint16_t* calib,
    const float* calig,
    const struct BadPt* calip,
    uint8_t* out,
    const size_t px_cnt)
{
    typedef uint16_t raw14_alias_t __attribute__((may_alias));
    static uint8_t gamma_lut[256];
    /* Core1 owns this frame as RAWBUF_READING for the whole conversion.
     * Reuse its pixel area as corrected-u14 scratch; trailing VTEMP remains
     * outside RAWBUFSIZE and is read after conversion. */
    static_assert(RAWBUFSIZE == PX_CNT * sizeof(uint16_t),
                  "raw pixel area must fit the in-place u14 scratch");
    static_assert(RAWBUFSIZETOTAL % __alignof__(uint16_t) == 0,
                  "every raw frame must preserve u14 alignment");
    raw14_alias_t* const raw14 = (raw14_alias_t*)raw;

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
    uint16_t scene_min_adu = UINT16_MAX;
    uint16_t scene_max_adu = 0;
    uint32_t scene_min_idx = 0;
    uint32_t scene_max_idx = 0;
    for (uint32_t i = 0; i < PX_CNT; ++i) {
        if (raw14[i] < scene_min_adu) {
            scene_min_adu = raw14[i];
            scene_min_idx = i;
        }
        if (raw14[i] > scene_max_adu) {
            scene_max_adu = raw14[i];
            scene_max_idx = i;
        }
    }
    calc_percentile_u14(raw14, PX_CNT, &low, &high);
    const uint32_t raw_scene_range = high > low ? high - low : 0;
    const bool display_enhance = raw_scene_range < MIN_THERMAL_RANGE;

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
    /* VTEMP feeds the static K_V·ΔVTEMP between-FFC compensation;
     * NTC feeds the K_N·ΔNTC shutter/body-thermal-lag compensation. */
    const uint16_t cur_vtemp = be16_to_u14(&raw[RAWBUF_OFS_VTEMP]);
    const uint16_t cur_ntc   = g_cur_ntc_adc;

    /* Update VTEMP EMA smoother — used by K_V to avoid amplifying
     * frame-to-frame ADC jitter into visible scene swing. */
    if (g_vtemp_smooth_x256 == 0) {
        g_vtemp_smooth_x256 = (uint32_t)cur_vtemp << 8;
    } else {
        g_vtemp_smooth_x256 =
            (g_vtemp_smooth_x256 * 31 + ((uint32_t)cur_vtemp << 8)) >> 5;
    }
    const uint16_t smooth_vtemp = (uint16_t)(g_vtemp_smooth_x256 >> 8);

    int16_t t_lo_x10 = INT16_MAX;
    int16_t t_hi_x10 = INT16_MAX;
    float t_lo_c_f = 0.0f, t_hi_c_f = 0.0f;
    bool temp_map_ok = false;
    int32_t eff_anchor = ram_temp_anchor;
    if (ram_mean_diff > 0.0f && ram_temp_anchor != 0 && calib != nullptr) {
        if (thermal_comp_legacy_enabled()) {
            int32_t comp = 0;
            const bool ntc_ok =
                ntc_adc_valid(ffc_last_ntc) && ntc_adc_valid(cur_ntc);
            const int32_t dn = ntc_ok
                ? (int32_t)cur_ntc - (int32_t)ffc_last_ntc
                : 0;
            if (ffc_last_fpa_vtemp != 0) {
                int32_t dv =
                    (int32_t)smooth_vtemp - (int32_t)ffc_last_fpa_vtemp;
                if (ntc_ok) {
                    const int32_t adn = dn < 0 ? -dn : dn;
                    const int32_t dv_ntc_cap = adn * DV_DN_RATIO_MAX;
                    if (dv >  dv_ntc_cap) dv =  dv_ntc_cap;
                    if (dv < -dv_ntc_cap) dv = -dv_ntc_cap;
                }
                if (dv >  DV_CLAMP_ADU) dv =  DV_CLAMP_ADU;
                if (dv < -DV_CLAMP_ADU) dv = -DV_CLAMP_ADU;
                comp += (int32_t)(K_ANCHOR_COMP_STATIC * (float)dv);
            }
            if (ntc_ok) {
                comp += ntc_frame_comp_for(cur_ntc);
            }
            if (comp >  K_COMP_MAX_ADU) comp =  K_COMP_MAX_ADU;
            if (comp < -K_COMP_MAX_ADU) comp = -K_COMP_MAX_ADU;
            eff_anchor += comp;
        } else if (thermal_comp_table_enabled()) {
            eff_anchor += thermal_comp_table_for_now();
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

    const uint32_t center_idx =
        (uint32_t)(SENSOR_H / 2) * SENSOR_W + (uint32_t)(SENSOR_W / 2);
    const uint16_t center_raw_adu = raw14[center_idx];
    const uint16_t center_roi_adu = center_roi_mean_u14(raw14, 2);
    const int16_t center_raw_x10 =
        temp_map_ok ? temp_x10_from_corrected_adu(center_raw_adu, eff_anchor)
                    : INT16_MAX;
    const int16_t center_roi_x10 =
        temp_map_ok ? temp_x10_from_corrected_adu(center_roi_adu, eff_anchor)
                    : INT16_MAX;
    const int16_t scene_min_x10 =
        temp_map_ok ? temp_x10_from_corrected_adu(scene_min_adu, eff_anchor)
                    : INT16_MAX;
    const int16_t scene_max_x10 =
        temp_map_ok ? temp_x10_from_corrected_adu(scene_max_adu, eff_anchor)
                    : INT16_MAX;

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

    if (display_enhance) {
        display_row_destripe_u14(raw14);
        display_col_destripe_u14(raw14);
    }

    for (int i = 0, x = 0, y = 0; i < PX_CNT; i++) {
        const uint16_t v = display_enhance
                               ? display_detail_boost_u14(raw14, x, y)
                               : raw14[i];
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

        x++;
        if (x >= SENSOR_W) {
            x = 0;
            y++;
        }
    }
    display_col_destripe_u8(&out[1]);

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
    const uint16_t ntc_ref = ffc_last_ntc;
    out[PX_CNT + 19] = (uint8_t)(ntc_ref >> 8);
    out[PX_CNT + 20] = (uint8_t)(ntc_ref     );
    const uint16_t ntc_snap = g_cur_ntc_adc;
    out[PX_CNT + 21] = (uint8_t)(ntc_snap >> 8);
    out[PX_CNT + 22] = (uint8_t)(ntc_snap     );
    out[PX_CNT + 23] = g_ffc_last_decision;
    out[PX_CNT + 24] = (uint8_t)(ffc_count > 255 ? 255 : ffc_count);
    const uint16_t ffc_dg = (uint16_t)g_ffc_last_dg;
    out[PX_CNT + 25] = (uint8_t)(ffc_dg >> 8);
    out[PX_CNT + 26] = (uint8_t)(ffc_dg     );
    const uint16_t ffc_dv = (uint16_t)g_ffc_last_dv;
    out[PX_CNT + 27] = (uint8_t)(ffc_dv >> 8);
    out[PX_CNT + 28] = (uint8_t)(ffc_dv     );
    const uint16_t ffc_dn = (uint16_t)g_ffc_last_dn;
    out[PX_CNT + 29] = (uint8_t)(ffc_dn >> 8);
    out[PX_CNT + 30] = (uint8_t)(ffc_dn     );
    const uint16_t boot_ntc_snap = boot_ntc;
    uint16_t boot_delta = 0xFFFFu;
    if (ntc_adc_valid(boot_ntc_snap) && ntc_adc_valid(ntc_snap)) {
        boot_delta = u16_absdiff(ntc_snap, boot_ntc_snap);
    }
    const uint16_t warmup_comp =
        (uint16_t)clip_i16_i32(ntc_warmup_curve_comp_for(ntc_snap));
    const uint16_t normal_comp =
        (uint16_t)clip_i16_i32(ntc_normal_comp_for(ntc_snap));
    const uint16_t total_comp =
        (uint16_t)clip_i16_i32(eff_anchor - ram_temp_anchor);
    const uint32_t eff_anchor_u = (uint32_t)eff_anchor;
    out[PX_CNT + 31] = (uint8_t)(boot_ntc_snap >> 8);
    out[PX_CNT + 32] = (uint8_t)(boot_ntc_snap     );
    out[PX_CNT + 33] = (uint8_t)(boot_delta >> 8);
    out[PX_CNT + 34] = (uint8_t)(boot_delta     );
    out[PX_CNT + 35] = (uint8_t)(warmup_comp >> 8);
    out[PX_CNT + 36] = (uint8_t)(warmup_comp     );
    out[PX_CNT + 37] = (uint8_t)(normal_comp >> 8);
    out[PX_CNT + 38] = (uint8_t)(normal_comp     );
    out[PX_CNT + 39] = (uint8_t)(total_comp >> 8);
    out[PX_CNT + 40] = (uint8_t)(total_comp     );
    out[PX_CNT + 41] = (uint8_t)(eff_anchor_u >> 24);
    out[PX_CNT + 42] = (uint8_t)(eff_anchor_u >> 16);
    out[PX_CNT + 43] = (uint8_t)(eff_anchor_u >>  8);
    out[PX_CNT + 44] = (uint8_t)(eff_anchor_u      );
    out[PX_CNT + 45] = (uint8_t)(smooth_vtemp >> 8);
    out[PX_CNT + 46] = (uint8_t)(smooth_vtemp     );
    const uint16_t center_raw_u = (uint16_t)center_raw_x10;
    const uint16_t center_roi_u = (uint16_t)center_roi_x10;
    out[PX_CNT + 47] = (uint8_t)(center_raw_u >> 8);
    out[PX_CNT + 48] = (uint8_t)(center_raw_u     );
    out[PX_CNT + 49] = (uint8_t)(center_roi_u >> 8);
    out[PX_CNT + 50] = (uint8_t)(center_roi_u     );
    out[PX_CNT + 51] = (uint8_t)(center_raw_adu >> 8);
    out[PX_CNT + 52] = (uint8_t)(center_raw_adu     );
    out[PX_CNT + 53] = (uint8_t)(center_roi_adu >> 8);
    out[PX_CNT + 54] = (uint8_t)(center_roi_adu     );
    uint32_t t_lo_bits;
    uint32_t t_hi_bits;
    memcpy(&t_lo_bits, &t_lo_c_f, sizeof(t_lo_bits));
    memcpy(&t_hi_bits, &t_hi_c_f, sizeof(t_hi_bits));
    out[PX_CNT + 55] = (uint8_t)(t_lo_bits >> 24);
    out[PX_CNT + 56] = (uint8_t)(t_lo_bits >> 16);
    out[PX_CNT + 57] = (uint8_t)(t_lo_bits >>  8);
    out[PX_CNT + 58] = (uint8_t)(t_lo_bits      );
    out[PX_CNT + 59] = (uint8_t)(t_hi_bits >> 24);
    out[PX_CNT + 60] = (uint8_t)(t_hi_bits >> 16);
    out[PX_CNT + 61] = (uint8_t)(t_hi_bits >>  8);
    out[PX_CNT + 62] = (uint8_t)(t_hi_bits      );
    const uint16_t scene_min_u = (uint16_t)scene_min_x10;
    const uint16_t scene_max_u = (uint16_t)scene_max_x10;
    out[PX_CNT + 63] = (uint8_t)(scene_min_u >> 8);
    out[PX_CNT + 64] = (uint8_t)(scene_min_u);
    out[PX_CNT + 65] = (uint8_t)(scene_max_u >> 8);
    out[PX_CNT + 66] = (uint8_t)(scene_max_u);
    out[PX_CNT + 67] = (uint8_t)(scene_min_idx % SENSOR_W);
    out[PX_CNT + 68] = (uint8_t)(scene_min_idx / SENSOR_W);
    out[PX_CNT + 69] = (uint8_t)(scene_max_idx % SENSOR_W);
    out[PX_CNT + 70] = (uint8_t)(scene_max_idx / SENSOR_W);
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

#if !THERMAL_NCM_MODE
    if (ouomode == UOM_RAW_U16) {
        uart_tx_dma_start(norm_buf[b], TX_PAYLOAD);
    } else
#endif
    if (ouomode == UOM_CALI_U8) {
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
#if !THERMAL_NCM_MODE
    if (ouomode == UOM_RAW_U16) {
        norm_buf[tx_idx][0] = 0xFF;
        norm_buf[tx_idx][1] = 0xFF;
        raw16_decode_to_normbuf(rawbuf[raw_idx], norm_buf[tx_idx], PX_CNT);
    } else
#endif
    if (ouomode == UOM_CALI_U8) {
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

static void capture_diag_compute_row_phase(void)
{
    if (!calibg) {
        memset(g_capture_row_phase, 0x7f, sizeof(g_capture_row_phase));
        return;
    }
    for (int y = 0; y < SENSOR_H; ++y) {
        enum { PHASE_SAMPLES = SENSOR_W / 4 };
        uint16_t raw_samples[PHASE_SAMPLES];
        uint32_t raw_sum = 0;
        for (int k = 0; k < PHASE_SAMPLES; ++k) {
            const int x = k * 4;
            const size_t i = (size_t)y * SENSOR_W + (size_t)x;
            raw_samples[k] =
                gray14_to_bin(be16_to_u14(&g_capture_raw_diag[i * 2]));
            raw_sum += raw_samples[k];
        }

        uint64_t best_cost = UINT64_MAX;
        int best_dy = 0;
        for (int cy = 0; cy < SENSOR_H; ++cy) {
            const int dy = cy - y;
            uint32_t calib_sum = 0;
            for (int k = 0; k < PHASE_SAMPLES; ++k) {
                const int x = k * 4;
                calib_sum += calibg[(size_t)cy * SENSOR_W + (size_t)x];
            }

            uint64_t cost = 0;
            for (int k = 0; k < PHASE_SAMPLES; ++k) {
                const int x = k * 4;
                const int32_t raw_centered =
                    (int32_t)raw_samples[k] * PHASE_SAMPLES
                    - (int32_t)raw_sum;
                const int32_t calib_centered =
                    (int32_t)calibg[(size_t)cy * SENSOR_W + (size_t)x]
                        * PHASE_SAMPLES
                    - (int32_t)calib_sum;
                const int32_t delta = raw_centered - calib_centered;
                cost += (uint32_t)(delta < 0 ? -delta : delta);
            }
            if (cost < best_cost) {
                best_cost = cost;
                best_dy = dy;
            }
        }
        g_capture_row_phase[y] = (int8_t)best_dy;
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
        const char* OUART_TEST_STRING = "Sipeed T160 Thermal\n";
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

#if !THERMAL_NCM_MODE
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
static volatile int uvc_last_completed = -1;
static volatile int uvc_ffc_hold_buf = -1;
static volatile bool uvc_ffc_hold_active = false;
static volatile uint8_t uvc_tx_busy = 0;
static volatile uint32_t uvc_frame_num = 0;
#if UVC_MJPEG640_STATIC_TEST
static volatile uint8_t uvc_format_index = UVC_FORMAT_INDEX_YUY2;
#endif
#define UVC_FIXED_INTERVAL_MS (1000u / FRAME_RATE)
static volatile uint32_t uvc_inv_ms = UVC_FIXED_INTERVAL_MS;
static volatile uint32_t uvc_last_xfer_ms = 0;
static enum UartOutMode uvc_ouomode_bak = UART_OUT_MODE_DEF;
static uint8_t ram_cmaps[CMAP_NUM][CMAP_BYTES];
static volatile uint8_t cur_cmap_idx = 0;
static const uint8_t* volatile active_cmap = nullptr;
static struct yuv_cmap_entry yuv_cmap[CMAP_W];

static inline __attribute__((always_inline)) void fill_smpte_yuy2(
    uint8_t* buf, const size_t len);

static inline __attribute__((always_inline)) void uvc_buf_init()
{
    for (int i = 0; i < UVC_BUF_CNT; ++i) {
        uvc_state[i] = UVCBUF_FREE;
    }

    uvc_newest_ready = -1;
    uvc_sending = -1;
    uvc_last_completed = -1;
    uvc_ffc_hold_buf = -1;
    uvc_ffc_hold_active = false;
    uvc_tx_busy = 0;
    uvc_last_xfer_ms = 0;
#if UVC_MJPEG640_STATIC_TEST
    uvc_format_index = UVC_FORMAT_INDEX_YUY2;
#endif
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
    for (int i = 0; i < UVC_BUF_CNT; ++i) {
        if (uvc_state[i] == UVCBUF_READY) {
            return i;
        }
    }
    return -1;
}

static inline __attribute__((always_inline)) int uvc_prepare_fallback_frame()
{
    const int b = find_free_uvc_buf();
    if (b < 0) {
        return -1;
    }

    uvc_state[b] = UVCBUF_FILLING;
    __dmb();
    fill_smpte_yuy2(uvc_buf[b], FRAME_SIZE);
    __dmb();
    uvc_state[b] = UVCBUF_READY;
    uvc_newest_ready = b;
    return b;
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
        uvc_last_completed = b;
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
#if UVC_MJPEG640_STATIC_TEST
    if (parameters
        && (parameters->bFormatIndex == UVC_FORMAT_INDEX_YUY2
            || parameters->bFormatIndex == UVC_FORMAT_INDEX_MJPEG_TEST)) {
        uvc_format_index = parameters->bFormatIndex;
    }
#else
    (void)parameters;
#endif
    uvc_inv_ms = UVC_FIXED_INTERVAL_MS;
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
#if UVC_MJPEG640_STATIC_TEST
    if (uvc_format_index == UVC_FORMAT_INDEX_MJPEG_TEST) {
        return;
    }
#endif

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
        if (uvc_sending >= 0 && uvc_sending < UVC_BUF_CNT
            && uvc_state[uvc_sending] == UVCBUF_SENDING) {
            uvc_state[uvc_sending] = UVCBUF_FREE;
        }
        uvc_sending = -1;
        uvc_tx_busy = 0;
        __dmb();
        return UINT32_MAX;
    }

    if (uvc_tx_busy) {
        return UINT32_MAX;
    }

    const uint32_t t_ms = now();
    if (uvc_last_xfer_ms && (uint32_t)(t_ms - uvc_last_xfer_ms) < uvc_inv_ms) {
        return UINT32_MAX;
    }

#if UVC_MJPEG640_STATIC_TEST
    if (uvc_format_index == UVC_FORMAT_INDEX_MJPEG_TEST) {
        uvc_sending = -1;
        uvc_tx_busy = 1;
        uvc_last_xfer_ms = t_ms;
        if (!tud_video_n_frame_xfer(0,
                                    0,
                                    (void*)(uintptr_t)uvc_mjpeg_test_frame,
                                    UVC_MJPEG_TEST_FRAME_SIZE)) {
            uvc_tx_busy = 0;
            uvc_last_xfer_ms = 0;
            __dmb();
            return UINT32_MAX;
        }

        const uint32_t timeout_ms = uvc_inv_ms + 100;
        while (uvc_tx_busy && tud_video_n_streaming(0, 0)
               && (uint32_t)(now() - t_ms) < timeout_ms) {
            tud_task();
            tight_loop_contents();
        }
        return 0;
    }
#endif

    int b = find_ready_uvc_buf();
    if (b < 0) {
        b = uvc_prepare_fallback_frame();
        if (b < 0) {
            return UINT32_MAX;
        }
    }

    uvc_state[b] = UVCBUF_SENDING;
    uvc_sending = b;
    if (uvc_newest_ready == b) {
        uvc_newest_ready = -1;
    }

    __dmb();
    uvc_tx_busy = 1;
    uvc_last_xfer_ms = t_ms;

    if (!tud_video_n_frame_xfer(0, 0, uvc_buf[b], FRAME_SIZE)) {
        uvc_state[b] = UVCBUF_READY;
        uvc_newest_ready = b;
        uvc_sending = -1;
        uvc_tx_busy = 0;
        uvc_last_xfer_ms = 0;
        __dmb();
        return UINT32_MAX;
    }

    const uint32_t timeout_ms = uvc_inv_ms > 50 ? uvc_inv_ms + 100 : 150;
    while (uvc_tx_busy && tud_video_n_streaming(0, 0)
           && (uint32_t)(now() - t_ms) < timeout_ms) {
        tud_task();
        tight_loop_contents();
    }

    return 0;
}

/* Preserve the most recent real thermal frame while FFC owns the raw-frame
 * pipeline.  The completed UVC buffer remains valid after its transfer; FFC
 * reuses that storage read-only instead of allocating another 38.4 KiB frame
 * or falling back to a test pattern. */
static void uvc_ffc_hold_begin(void)
{
    if (dataomode != DOM_UVC) {
        return;
    }

    int b = find_ready_uvc_buf();
    if (b >= 0) {
        if (uvc_newest_ready == b) {
            uvc_newest_ready = -1;
        }
        uvc_state[b] = UVCBUF_FREE;
    } else {
        b = uvc_last_completed;
    }

    if (b < 0 || b >= UVC_BUF_CNT || uvc_state[b] != UVCBUF_FREE) {
        return;
    }

    for (int i = 0; i < UVC_BUF_CNT; i++) {
        if (i != b && uvc_state[i] == UVCBUF_READY) {
            uvc_state[i] = UVCBUF_FREE;
        }
    }
    uvc_newest_ready = -1;
    uvc_ffc_hold_buf = b;
    __dmb();
    uvc_ffc_hold_active = true;
}

static void uvc_ffc_hold_service(void)
{
    if (!uvc_ffc_hold_active || !tud_video_n_streaming(0, 0)
        || uvc_tx_busy) {
        return;
    }

    const uint32_t t_ms = now();
    if (uvc_last_xfer_ms
        && (uint32_t)(t_ms - uvc_last_xfer_ms) < uvc_inv_ms) {
        return;
    }

    const int b = uvc_ffc_hold_buf;
    if (b < 0 || b >= UVC_BUF_CNT || uvc_state[b] != UVCBUF_FREE) {
        return;
    }

    uvc_state[b] = UVCBUF_SENDING;
    uvc_sending = b;
    __dmb();
    uvc_tx_busy = 1;
    uvc_last_xfer_ms = t_ms;

    if (!tud_video_n_frame_xfer(0, 0, uvc_buf[b], FRAME_SIZE)) {
        uvc_state[b] = UVCBUF_FREE;
        uvc_sending = -1;
        uvc_tx_busy = 0;
        uvc_last_xfer_ms = 0;
        __dmb();
    }
}

static void uvc_ffc_hold_end(void)
{
    uvc_ffc_hold_active = false;
    __dmb();
    uvc_ffc_hold_buf = -1;
}
#else
static inline void uvc_ffc_hold_begin(void) {}
static inline void uvc_ffc_hold_end(void) {}
#endif

static bool ncm_frame_acquire(struct thermal_frame_view* view)
{
    const int b = newest_ready;
    if (!view || b < 0 || b >= TXBUF_CNT || tx_state[b] != TXBUF_READY) {
        return false;
    }

    tx_state[b] = TXBUF_SENDING;
    newest_ready = -1;
    __dmb();
    view->pixels = &norm_buf[b][1];
    view->meta = thermal_ncm_meta((uint8_t)b);
    view->token = (uint8_t)b;
    return view->meta != nullptr;
}

static void ncm_frame_release(uint8_t token)
{
    if (token >= TXBUF_CNT || tx_state[token] != TXBUF_SENDING) return;
    __dmb();
    tx_state[token] = TXBUF_FREE;
    __dmb();
    __sev();
}

/* Factory adc16 uses the exact active-pixel decode from the legacy raw UART
 * path: sensor BE Gray code -> right-aligned 14-bit binary.  The unified
 * protocol changes the wire order to little endian and excludes the legacy
 * UART frame marker, two per-row reference words and trailing auxiliary word.
 * g_capture_raw_diag is reused as the one-shot snapshot so no additional
 * 38.4 KiB frame buffer is added to BSS. */
static void ncm_capture_adc16(const uint8_t* raw)
{
    for (size_t i = 0; i < PX_CNT; ++i) {
        uint16_t value = be16_to_u14(&raw[i * 2u]);
        value = gray14_to_bin(value);
        g_capture_raw_diag[i * 2u] = (uint8_t)value;
        g_capture_raw_diag[i * 2u + 1u] = (uint8_t)(value >> 8);
    }
}

static bool ncm_adc16_frame_acquire(struct thermal_frame_view* view)
{
    if (!view) return false;
    if (adc16_snapshot_state == ADC16_SNAPSHOT_IDLE) {
        adc16_snapshot_state = ADC16_SNAPSHOT_REQUESTED;
        __dmb();
        __sev();
        return false;
    }
    if (adc16_snapshot_state != ADC16_SNAPSHOT_READY) return false;

    const struct sipeed_thermal_meta_v1* meta =
        thermal_ncm_adc16_meta();
    if (!meta) return false;
    adc16_snapshot_state = ADC16_SNAPSHOT_SENDING;
    __dmb();
    view->pixels = g_capture_raw_diag;
    view->meta = meta;
    view->token = 0;
    return true;
}

static void ncm_adc16_frame_release(uint8_t token)
{
    (void)token;
    if (adc16_snapshot_state != ADC16_SNAPSHOT_SENDING) return;
    __dmb();
    adc16_snapshot_state = ADC16_SNAPSHOT_IDLE;
    __dmb();
    __sev();
}

static void ncm_adc16_frame_cancel(void)
{
    if (adc16_snapshot_state == ADC16_SNAPSHOT_SENDING) return;
    adc16_snapshot_state = ADC16_SNAPSHOT_IDLE;
    __dmb();
}

static void ffc_run(uint16_t cur_fpa_vtemp);
static bool ffc_should_trigger(uint16_t cur_fpa_vtemp);
static bool ffc_post_guard_accept(const uint8_t* raw, uint32_t frame_epoch);

void __time_critical_func(core1_rawbuf_consumer)()
{
    int idx;
    for (int i = 0; i < TXBUF_CNT; i++) {
        tx_state[i] = TXBUF_FREE;
    }

    newest_ready = -1;
    sending = -1;
    dma_busy = 0;
    println("Core1 consumer start");
    dataomode = DOM_NCM;
    ouomode = UOM_CALI_U8;
    cmode = CM_OFF;
    adc16_snapshot_state = ADC16_SNAPSHOT_IDLE;
    thermal_ncm_init(ncm_frame_acquire, ncm_frame_release,
                     ncm_adc16_frame_acquire, ncm_adc16_frame_release,
                     ncm_adc16_frame_cancel);
    flash_safe_execute_core_init();
    dma_busy = 0;
    buf_idx = 0;

    /* Seed ADC diagnostics before the first frame. */
    g_adc_cfg_word = ADC_DIAG_CFG_WORD;
    adc_probe_core();
    for (int i = 0; i < 32; i++) {
        const uint16_t v = ntc_read_avg(8);
        if (ntc_adc_valid(v)) {
            g_cur_ntc_adc = v;
            g_ntc_smooth_x256 = ((uint32_t)v) << 8;
            boot_ntc = v;
            boot_ntc_ms = to_ms_since_boot(get_absolute_time());
            break;
        }
        sleep_ms(2);
    }
    if (!ntc_adc_valid(g_cur_ntc_adc)) {
        g_cur_ntc_adc = ntc_read_avg(8);
        if (ntc_adc_valid(g_cur_ntc_adc)) {
            g_ntc_smooth_x256 = ((uint32_t)g_cur_ntc_adc) << 8;
            boot_ntc = g_cur_ntc_adc;
            boot_ntc_ms = to_ms_since_boot(get_absolute_time());
        }
    }

    while (true) {
        ntc_tick();
        bool did_work = false;

        idx = rawbuf_pick_ready();
        if (idx >= 0) {
            __dmb();
            rawbuf_state[idx] = RAWBUF_READING;
            __dmb();
            const uint32_t frame_epoch = rawbuf_epoch[idx];

            if (ffc_post_guard_accept(rawbuf[idx], frame_epoch)) {
                int b = txbuf_pick_free();
                if (b >= 0) {
                    tx_state[b] = TXBUF_FILLING;
                    g_capture_last_frame_uniform_rows =
                        rawbuf_uniform_rows[idx];
                    g_capture_last_frame_uniform_max_run =
                        rawbuf_uniform_max_run[idx];
                    const bool adc16_captured =
                        adc16_snapshot_state == ADC16_SNAPSHOT_REQUESTED;
                    bool diag_raw_captured = false;
                    if (adc16_captured) {
                        ncm_capture_adc16(rawbuf[idx]);
                    } else if (adc16_snapshot_state == ADC16_SNAPSHOT_IDLE
                               && !g_capture_raw_diag_frozen) {
                        memcpy(g_capture_raw_diag, rawbuf[idx], RAWBUFSIZE);
                        memcpy(g_capture_row_ref_diag, rawbuf_row_ref[idx],
                               sizeof(g_capture_row_ref_diag));
                        diag_raw_captured = true;
                    }
                    fill_txbuf(b, idx);
                    if (diag_raw_captured && !g_capture_raw_diag_frozen) {
                        const uint8_t* pixels = &norm_buf[b][1];
                        uint32_t previous_sum = 0;
                        for (uint32_t x = 0; x < SENSOR_W; ++x) {
                            previous_sum += pixels[x];
                        }
                        for (uint32_t y = 1; y < SENSOR_H; ++y) {
                            uint32_t sum = 0;
                            const uint8_t* row = pixels + y * SENSOR_W;
                            for (uint32_t x = 0; x < SENSOR_W; ++x) {
                                sum += row[x];
                            }
                            const uint32_t jump = sum > previous_sum
                                                      ? sum - previous_sum
                                                      : previous_sum - sum;
                            if (jump >= SENSOR_W * 80u) {
                                capture_diag_compute_row_phase();
                                __dmb();
                                g_capture_raw_diag_frozen = true;
                                break;
                            }
                            previous_sum = sum;
                        }
                    }
                    thermal_ncm_note_frame(
                        (uint8_t)b,
                        norm_buf[b],
                        calibg != nullptr && caligain != nullptr
                            && ram_mean_diff > 0.0f);
                    if (adc16_captured) {
                        thermal_ncm_note_adc16(
                            g_capture_raw_diag,
                            thermal_ncm_meta((uint8_t)b));
                        __dmb();
                        adc16_snapshot_state = ADC16_SNAPSHOT_READY;
                    }
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
            }

            __dmb();
            rawbuf_state[idx] = RAWBUF_FREE;
            did_work = true;
        }

        thermal_ncm_task();

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
            tight_loop_contents();
        }
    }

}

static inline __attribute__((always_inline)) void uart_tx_dma_wait_done()
{
    dma_channel_wait_for_finish_blocking(uart_tx_dma_chan);
    dma_busy = 0;
}

static inline void ffc_service_usb(void)
{
    /* Core0 also calls shutter_on() once during sensor startup. TinyUSB and
     * lwIP are owned by Core1 and are not safe to enter concurrently from
     * both cores; runtime FFC itself always runs on Core1. */
    if (get_core_num() == 1) {
        thermal_ncm_task();
    }
}

static void ffc_service_delay_ms(uint32_t delay_ms)
{
    const uint32_t start_ms = now();
    while ((uint32_t)(now() - start_ms) < delay_ms) {
        /* Held-frame submission is non-blocking, so the motor pulse remains
         * bounded while TinyUSB continues at the negotiated frame cadence. */
        ffc_service_usb();
        sleep_ms(1);
    }
}

static inline __attribute__((always_inline)) void shutter_on()
{
#if SHUTTER_ENABLE == 1
    gpio_put(IO_SVMS, 1);
    gpio_put(IO_SVM1, 1);
    gpio_put(IO_SVM2, 0);
#endif
    ffc_service_delay_ms(200);
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
    ffc_service_delay_ms(200);
#if SHUTTER_ENABLE == 1
    gpio_put(IO_SVMS, 0);
    gpio_put(IO_SVM1, 0);
    gpio_put(IO_SVM2, 0);
#endif
}

/* Take ownership of one fresh raw frame.  A non-zero required_epoch also
 * drains frames that began before the corresponding shutter movement was
 * complete; those frames can never be used for FFC or normal output. */
static int ffc_wait_ready_frame(uint32_t required_epoch)
{
    uint32_t timeout = FFC_FRAME_TIMEOUT_MS;
    while (timeout--) {
        for (int i = 0; i < RAWBUFCNT; i++) {
            if (rawbuf_state[i] == RAWBUF_READY) {
                __dmb();
                rawbuf_state[i] = RAWBUF_READING;
                __dmb();
                if (required_epoch != 0
                    && rawbuf_epoch[i] != required_epoch) {
                    rawbuf_state[i] = RAWBUF_FREE;
                    __dmb();
                    __sev();
                    continue;
                }
                return i;
            }
        }
        ffc_service_usb();
        sleep_ms(1);
    }
    return -1;
}

static inline void ffc_release_frame(int idx)
{
    __dmb();
    rawbuf_state[idx] = RAWBUF_FREE;
    __dmb();
    __sev();
}

static void ffc_flush_ready_frames(void)
{
    for (int i = 0; i < RAWBUFCNT; i++) {
        if (rawbuf_state[i] == RAWBUF_READY) {
            rawbuf_state[i] = RAWBUF_FREE;
        }
    }
    __dmb();
    rawbuf_last_ready = -1;
    __sev();
}

static void ffc_restore_gain_cache(bool restore_gain_cache)
{
    if (!restore_gain_cache) {
        return;
    }
    ffc_service_usb();
    memcpy(calibuf, flash_gain, PX_CNT * sizeof(float));
    caligain = (float*)calibuf;
    ffc_service_usb();
}

/* A single global mean can miss a local shutter edge.  Compare the full
 * frame, four quadrants, and a center 16x16 ROI between consecutive fresh
 * scene frames before releasing the renderer. */
#define FFC_SCENE_SIGNATURE_VALUES 6u
#define FFC_SCENE_CENTER_SIZE      16u
struct FfcSceneSignature {
    uint16_t mean[FFC_SCENE_SIGNATURE_VALUES];
};

static void ffc_scene_signature(const uint8_t* raw,
                                struct FfcSceneSignature* signature)
{
    uint32_t quadrant_sum[4] = {0, 0, 0, 0};
    uint32_t center_sum = 0;
    const uint32_t half_w = SENSOR_W / 2u;
    const uint32_t half_h = SENSOR_H / 2u;
    const uint32_t center_x0 = (SENSOR_W - FFC_SCENE_CENTER_SIZE) / 2u;
    const uint32_t center_y0 = (SENSOR_H - FFC_SCENE_CENTER_SIZE) / 2u;
    const uint32_t center_x1 = center_x0 + FFC_SCENE_CENTER_SIZE;
    const uint32_t center_y1 = center_y0 + FFC_SCENE_CENTER_SIZE;

    for (uint32_t y = 0; y < SENSOR_H; y++) {
        const uint32_t qy = y >= half_h ? 2u : 0u;
        for (uint32_t x = 0; x < SENSOR_W; x++) {
            const size_t i = (size_t)y * SENSOR_W + x;
            const uint16_t adu =
                gray14_to_bin(be16_to_u14(&raw[i * 2u]));
            quadrant_sum[qy + (x >= half_w ? 1u : 0u)] += adu;
            if (x >= center_x0 && x < center_x1
                && y >= center_y0 && y < center_y1) {
                center_sum += adu;
            }
        }
        if ((y & 7u) == 7u) {
            ffc_service_usb();
        }
    }

    const uint32_t quadrant_pixels = half_w * half_h;
    uint32_t total_sum = 0;
    for (uint32_t q = 0; q < 4u; q++) {
        total_sum += quadrant_sum[q];
        signature->mean[q + 1u] =
            (uint16_t)(quadrant_sum[q] / quadrant_pixels);
    }
    signature->mean[0] = (uint16_t)(total_sum / PX_CNT);
    signature->mean[5] = (uint16_t)(
        center_sum / (FFC_SCENE_CENTER_SIZE * FFC_SCENE_CENTER_SIZE));
}

static uint16_t ffc_scene_signature_delta(
    const struct FfcSceneSignature* a,
    const struct FfcSceneSignature* b)
{
    uint16_t max_delta = 0;
    for (uint32_t i = 0; i < FFC_SCENE_SIGNATURE_VALUES; i++) {
        const uint16_t delta = a->mean[i] > b->mean[i]
            ? a->mean[i] - b->mean[i]
            : b->mean[i] - a->mean[i];
        if (delta > max_delta) {
            max_delta = delta;
        }
    }
    return max_delta;
}

/* The resync loop releases its last verified frame back to Core0.  A frame
 * that was already WRITING can still become READY afterwards, so the normal
 * consumer performs one final gate before any pixels reach UART/UVC.  A new
 * scene is also allowed after two mutually consistent frames, which avoids a
 * permanent video hold if the user moves the camera during FFC. */
static bool g_ffc_post_guard_active = false;
static bool g_ffc_post_guard_candidate_valid = false;
static uint32_t g_ffc_post_guard_rejects = 0;
static uint32_t g_ffc_post_guard_epoch = 0;
static struct FfcSceneSignature g_ffc_post_guard_expected = {{0}};
static struct FfcSceneSignature g_ffc_post_guard_candidate = {{0}};

static void ffc_post_guard_arm(
    const struct FfcSceneSignature* expected,
    uint32_t expected_epoch)
{
    if (expected == nullptr) {
        g_ffc_post_guard_active = false;
        g_ffc_post_guard_candidate_valid = false;
        g_ffc_post_guard_rejects = 0;
        g_ffc_post_guard_epoch = 0;
        uvc_ffc_hold_end();
        return;
    }

    g_ffc_post_guard_expected = *expected;
    g_ffc_post_guard_candidate_valid = false;
    g_ffc_post_guard_rejects = 0;
    g_ffc_post_guard_epoch = expected_epoch;
    g_ffc_post_guard_active = true;
}

static bool ffc_post_guard_accept(const uint8_t* raw, uint32_t frame_epoch)
{
    const uint32_t current_epoch = rawbuf_capture_epoch;
    if (frame_epoch != current_epoch) {
        println("FFC epoch drop: frame=%lu current=%lu",
                (unsigned long)frame_epoch,
                (unsigned long)current_epoch);
        return false;
    }

    if (!g_ffc_post_guard_active) {
        return true;
    }
    if (frame_epoch != g_ffc_post_guard_epoch) {
        println("FFC post-guard epoch drop: frame=%lu expected=%lu",
                (unsigned long)frame_epoch,
                (unsigned long)g_ffc_post_guard_epoch);
        return false;
    }

    struct FfcSceneSignature current;
    ffc_scene_signature(raw, &current);
    const uint16_t expected_delta =
        ffc_scene_signature_delta(&g_ffc_post_guard_expected, &current);
    const uint16_t candidate_delta = g_ffc_post_guard_candidate_valid
        ? ffc_scene_signature_delta(&g_ffc_post_guard_candidate, &current)
        : UINT16_MAX;

    if (expected_delta <= FFC_OPEN_STABLE_DELTA_ADU
        || candidate_delta <= FFC_OPEN_STABLE_DELTA_ADU) {
        println("FFC post-guard accept: ref=%s delta=%u rejected=%u",
                expected_delta <= FFC_OPEN_STABLE_DELTA_ADU
                    ? "scene" : "new-scene",
                (unsigned)(expected_delta <= FFC_OPEN_STABLE_DELTA_ADU
                               ? expected_delta : candidate_delta),
                (unsigned)g_ffc_post_guard_rejects);
        g_ffc_post_guard_active = false;
        g_ffc_post_guard_candidate_valid = false;
        g_ffc_post_guard_epoch = 0;
        uvc_ffc_hold_end();
        return true;
    }

    g_ffc_post_guard_rejects++;
    if (g_ffc_post_guard_rejects >= FFC_POST_GUARD_MAX_REJECT_FRAMES) {
        /* A continuously moving scene may never satisfy the signature gate.
         * Fail open after a bounded hold; isolated shutter/raw glitches are
         * still removed because they recover on the next normal frame. */
        println("FFC post-guard bounded release: scene_delta=%u candidate_delta=%u",
                (unsigned)expected_delta,
                (unsigned)candidate_delta);
        g_ffc_post_guard_active = false;
        g_ffc_post_guard_candidate_valid = false;
        g_ffc_post_guard_epoch = 0;
        uvc_ffc_hold_end();
        return true;
    }

    g_ffc_post_guard_candidate = current;
    g_ffc_post_guard_candidate_valid = true;
    println("FFC post-guard drop: scene_delta=%u candidate_delta=%u count=%u",
            (unsigned)expected_delta,
            (unsigned)candidate_delta,
            (unsigned)g_ffc_post_guard_rejects);
    return false;
}

/* Frames captured while the shutter is closed or moving must never reach the
 * renderer. Core0 normally fills the ring while FFC computes. After opening,
 * discard at least four fresh frames and require two consecutive signatures
 * to agree. A bounded maximum prevents permanent hold when the camera or
 * scene is moving. */
static void ffc_open_and_resync_scene(void)
{
    shutter_off();
    const uint32_t scene_epoch = rawbuf_advance_capture_epoch();
    ffc_flush_ready_frames();

    struct FfcSceneSignature previous = {{0}};
    bool have_previous = false;
    bool scene_stable = false;
    uint32_t stable_frames = 0;
    uint32_t discarded_frames = 0;
    uint16_t last_delta = UINT16_MAX;

    for (uint32_t pass = 0; pass < FFC_OPEN_MAX_DISCARD_FRAMES; pass++) {
        const int idx = ffc_wait_ready_frame(scene_epoch);
        if (idx < 0) {
            println("FFC: scene resync timeout after %u frame(s)",
                    (unsigned)pass);
            break;
        }
        discarded_frames = pass + 1u;

        if (pass >= FFC_OPEN_DISCARD_FRAMES) {
            struct FfcSceneSignature current;
            ffc_scene_signature(rawbuf[idx], &current);
            if (have_previous) {
                last_delta =
                    ffc_scene_signature_delta(&previous, &current);
                stable_frames = last_delta <= FFC_OPEN_STABLE_DELTA_ADU
                    ? stable_frames + 1u
                    : 1u;
            } else {
                stable_frames = 1u;
                have_previous = true;
            }
            previous = current;
        }
        ffc_release_frame(idx);

        if (stable_frames >= FFC_OPEN_STABLE_FRAMES) {
            scene_stable = true;
            break;
        }
    }
    println("FFC scene resync: %s epoch=%lu discard=%u delta=%u",
            scene_stable ? "stable" : "max/timeout",
            (unsigned long)scene_epoch,
            (unsigned)discarded_frames,
            (unsigned)last_delta);
    /* Remove READY frames accumulated while the final signature was being
     * calculated.  A concurrently WRITING frame remains tagged with its
     * start epoch and is covered by the consumer's epoch + signature gate. */
    ffc_flush_ready_frames();
    ffc_post_guard_arm(have_previous ? &previous : nullptr, scene_epoch);
}

/*
 * ffc_run() — Flat-Field Correction, called from Core1.
 *
 * Closes the shutter, discards transition frames, averages eight uniform
 * shutter frames, updates ram_calibg[], and preserves the percentile smoother
 * across the new background reference.
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

    uvc_ffc_hold_begin();

    shutter_on(); /* close shutter (~200 ms) */
    const uint32_t closed_epoch = rawbuf_advance_capture_epoch();

    /* Discard all frames currently in the ring: they may have been captured
     * while the shutter was still mechanically closing (or entirely before
     * the pulse), and using such a frame as ram_calibg bakes the scene into
     * the background, producing a negative ghost on every subsequent frame. */
    ffc_flush_ready_frames();

    const bool restore_gain_cache = (caligain != nullptr);
    uint32_t* const ffc_sum = calib_workspace.ffc_sum;
    memset(ffc_sum, 0, sizeof(calib_workspace.ffc_sum));
    uint32_t ffc_vtemp_sum = 0;

    /* Drop two fully fresh frames after the shutter-close pulse, then sum
     * eight complete shutter frames.  14-bit ADU * 8 fits in 17 bits, so the
     * uint32 accumulator has ample headroom. */
    const uint32_t capture_frames =
        FFC_CLOSED_DISCARD_FRAMES + FFC_AVERAGE_FRAMES;
    for (uint32_t pass = 0; pass < capture_frames; pass++) {
        const int idx = ffc_wait_ready_frame(closed_epoch);
        if (idx < 0) {
            println("FFC: no frame, abort");
            ffc_record_event(FFC_EVT_ABORT_NO_FRAME,
                             0,
                             0,
                             0,
                             ram_temp_anchor,
                             ram_temp_anchor,
                             0,
                             0,
                             cur_fpa_vtemp,
                             g_cur_ntc_adc);
            ffc_restore_gain_cache(restore_gain_cache);
            ffc_open_and_resync_scene();
            led_off();
            return;
        }

        if (pass >= FFC_CLOSED_DISCARD_FRAMES) {
            const uint8_t* const src = rawbuf[idx];
            for (size_t i = 0; i < PX_CNT; i++) {
                ffc_sum[i] += gray14_to_bin(be16_to_u14(&src[i * 2]));
                if ((i & 0x3FFu) == 0) {
                    ffc_service_usb();
                }
            }
            ffc_vtemp_sum += be16_to_u14(&src[RAWBUF_OFS_VTEMP]);
        }
        ffc_release_frame(idx);
    }

    /* Convert the direct sums to rounded per-pixel averages in place.  Keep
     * the candidate in the shared workspace until all sanity guards pass, so
     * a rejected FFC leaves ram_calibg untouched. */
    uint64_t pixel_sum = 0;
    for (size_t i = 0; i < PX_CNT; i++) {
        const uint32_t avg =
            (ffc_sum[i] + FFC_AVERAGE_FRAMES / 2u) / FFC_AVERAGE_FRAMES;
        ffc_sum[i] = avg;
        pixel_sum += avg;
    }

    /* Summarize shutter-background row structure. A partial/shifted frame
     * produces a large adjacent-row discontinuity here; normal scene texture
     * cannot influence this metric because the shutter is closed. */
    uint16_t bg_min_row_mean = UINT16_MAX;
    uint16_t bg_max_row_mean = 0;
    uint16_t bg_max_row_jump = 0;
    uint8_t bg_max_row_jump_line = 0;
    uint16_t previous_row_mean = 0;
    for (uint32_t y = 0; y < SENSOR_H; y++) {
        uint32_t row_sum = 0;
        for (uint32_t x = 0; x < SENSOR_W; x++) {
            row_sum += ffc_sum[y * SENSOR_W + x];
        }
        const uint16_t row_mean = (uint16_t)(row_sum / SENSOR_W);
        if (row_mean < bg_min_row_mean) bg_min_row_mean = row_mean;
        if (row_mean > bg_max_row_mean) bg_max_row_mean = row_mean;
        if (y != 0) {
            const uint16_t jump = row_mean > previous_row_mean
                ? row_mean - previous_row_mean
                : previous_row_mean - row_mean;
            if (jump > bg_max_row_jump) {
                bg_max_row_jump = jump;
                bg_max_row_jump_line = (uint8_t)y;
            }
        }
        previous_row_mean = row_mean;
    }
    g_ffc_bg_max_row_jump_adu = bg_max_row_jump;
    g_ffc_bg_max_row_jump_line = bg_max_row_jump_line;
    g_ffc_bg_min_row_mean = bg_min_row_mean;
    g_ffc_bg_max_row_mean = bg_max_row_mean;
    const int32_t new_calib_mean = (int32_t)(pixel_sum / PX_CNT);
    cur_fpa_vtemp = (uint16_t)(
        (ffc_vtemp_sum + FFC_AVERAGE_FRAMES / 2u) / FFC_AVERAGE_FRAMES);
    const uint16_t cur_ntc_at_ffc = g_cur_ntc_adc;
    int32_t dg_check = 0;
    int32_t dv_check = 0;
    int32_t dn_check = 0;
    if (ntc_adc_valid(ffc_last_ntc) && ntc_adc_valid(cur_ntc_at_ffc)) {
        dn_check = (int32_t)cur_ntc_at_ffc - (int32_t)ffc_last_ntc;
    }

    /* FFC sanity check: an FFC across a healthy 30 s interval should
     * produce only modest dG (shutter mean change) and dV (VTEMP change).
     * Anything large indicates a sensor glitch or a partial-shutter
     * capture. Reject by leaving calibg/anchor/K-fit untouched and
     * exiting; the next scheduled FFC will retry. */
    if (ffc_done_first && ffc_last_g_calib_mean != 0) {
        dg_check = new_calib_mean - ffc_last_g_calib_mean;
        dv_check = (int32_t)cur_fpa_vtemp - (int32_t)ffc_last_fpa_vtemp;
        const int32_t adg = i32_abs(dg_check);
        const int32_t adv = i32_abs(dv_check);
        int32_t dv_ntc = 0, dn_ntc = 0;
        if (K_ANCHOR_COMP_STATIC != 0.0f
            && !ffc_vtemp_ntc_plausible(
                cur_fpa_vtemp, cur_ntc_at_ffc, &dv_ntc, &dn_ntc)) {
            println("FFC defer: VTEMP/NTC mismatch dv=%ld dn=%ld",
                    (long)dv_ntc, (long)dn_ntc);
            ffc_consec_reject = 0;
            ffc_record_event(FFC_EVT_REJECT_VN,
                             dg_check,
                             dv_ntc,
                             dn_ntc,
                             ram_temp_anchor,
                             ram_temp_anchor,
                             0,
                             new_calib_mean,
                             cur_fpa_vtemp,
                             cur_ntc_at_ffc);
            ffc_restore_gain_cache(restore_gain_cache);
            ffc_open_and_resync_scene();
            led_off();
            const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
            ffc_last_ms = now_ms;
            ffc_retry_after_ms = now_ms + FFC_RETRY_COOLDOWN_MS;
            return;
        }
        const bool over_limits = (adg > FFC_REJECT_DG_ADU)
            || (K_ANCHOR_COMP_STATIC != 0.0f && adv > FFC_REJECT_DV_ADU);
        if (over_limits) {
            ffc_consec_reject++;
            println("FFC reject %u: dg=%ld dv=%ld (limits dg=%d dv=%d)",
                    (unsigned)ffc_consec_reject,
                    (long)dg_check, (long)dv_check,
                    FFC_REJECT_DG_ADU, FFC_REJECT_DV_ADU);
            ffc_record_event(FFC_EVT_REJECT_LIMIT,
                             dg_check,
                             dv_check,
                             dn_check,
                             ram_temp_anchor,
                             ram_temp_anchor,
                             0,
                             new_calib_mean,
                             cur_fpa_vtemp,
                             cur_ntc_at_ffc);
            ffc_restore_gain_cache(restore_gain_cache);
            ffc_open_and_resync_scene();
            led_off();
            /* Reset the time anchor so the next FFC waits the full
             * stable interval and avoids back-to-back retries. */
            ffc_last_ms = to_ms_since_boot(get_absolute_time());
            ffc_retry_after_ms = ffc_last_ms + FFC_RETRY_COOLDOWN_MS;
            return;
        }
    }

    /* recompute temperature anchor to compensate for gain-weighted offset between
     * the low-ref frame and this FFC shutter frame.  Without this, a shutter at a
     * different temperature than the low-ref causes a constant offset on all
     * temperature readings proportional to (avg_gain - 1) * ΔT_shutter.
     *
     * anchor = Σ(gain[i] * (l_ref[i] - shutter[i])) / N + shutter_mean
     *
     * Derivation: for scene at T, corrected_mean = anchor + avg_diff * α,
     * so T_reading = T_L + (corrected_mean - anchor) * k = T_L + avg_diff*α*k = T_scene ✓ */
    /* recompute temperature anchor to compensate for gain-weighted offset
     * between the low-ref frame (cold cal scene at T_L) and this FFC
     * shutter frame (uniform metal at shutter T). The formula transforms
     * the shutter-temperature reference back to the cold-cal-scene
     * reference, so displayed T = T_L when actual scene T = T_L. */
    const int32_t anchor_old = ram_temp_anchor;
    int32_t candidate_temp_anchor = ram_temp_anchor;
    /* eff_anchor_old = the eff_anchor the renderer used in the last
     * frame before this FFC. Includes BOTH K_V·ΔVTEMP and K_N·ΔNTC
     * accumulated since the previous FFC. The smoother shift below
     * uses eff_anchor_old (not bare anchor_old) to keep displayed
     * scene continuous. */
    const int32_t smooth_vtemp_at_ffc = (int32_t)(g_vtemp_smooth_x256 >> 8);
    const int32_t table_comp_at_ffc = thermal_comp_table_for_now();
    int32_t eff_anchor_old = anchor_old;
    if (thermal_comp_legacy_enabled()) {
        int32_t comp_pre = 0;
        const bool ntc_ok_pre =
            ntc_adc_valid(ffc_last_ntc) && ntc_adc_valid(cur_ntc_at_ffc);
        const int32_t dn_pre = ntc_ok_pre
            ? (int32_t)cur_ntc_at_ffc - (int32_t)ffc_last_ntc
            : 0;
        if (ffc_last_fpa_vtemp != 0) {
            int32_t dv_pre =
                smooth_vtemp_at_ffc - (int32_t)ffc_last_fpa_vtemp;
            if (ntc_ok_pre) {
                const int32_t adn = dn_pre < 0 ? -dn_pre : dn_pre;
                const int32_t dv_ntc_cap = adn * DV_DN_RATIO_MAX;
                if (dv_pre >  dv_ntc_cap) dv_pre =  dv_ntc_cap;
                if (dv_pre < -dv_ntc_cap) dv_pre = -dv_ntc_cap;
            }
            if (dv_pre >  DV_CLAMP_ADU) dv_pre =  DV_CLAMP_ADU;
            if (dv_pre < -DV_CLAMP_ADU) dv_pre = -DV_CLAMP_ADU;
            comp_pre += (int32_t)(K_ANCHOR_COMP_STATIC * (float)dv_pre);
        }
        if (ntc_ok_pre) {
            comp_pre += ntc_frame_comp_for(cur_ntc_at_ffc);
            dn_check = dn_pre;
        }
        if (comp_pre >  K_COMP_MAX_ADU) comp_pre =  K_COMP_MAX_ADU;
        if (comp_pre < -K_COMP_MAX_ADU) comp_pre = -K_COMP_MAX_ADU;
        eff_anchor_old += comp_pre;
    } else if (thermal_comp_table_enabled()) {
        eff_anchor_old += table_comp_at_ffc;
    }
    if (caligain != nullptr && ram_l_ref_mean > 0) {
        float fsum = 0.0f;
        for (size_t i = 0; i < PX_CNT; i++) {
            const uint16_t candidate_bg = (uint16_t)ffc_sum[i];
            fsum += flash_gain[i] * ((float)flash_imgbg[i] - (float)candidate_bg);
        }
        candidate_temp_anchor =
            (int32_t)roundf(fsum / (float)PX_CNT + (float)new_calib_mean);

        if (thermal_comp_legacy_enabled()) {
            /* Persist cumulative K_N body-warmup compensation into the
             * freshly recomputed shutter anchor.  The base anchor below is
             * derived from the current shutter frame and does not remember
             * compensation that was applied before this FFC. */
            int32_t comp_acc = 0;
            const bool ntc_ok_acc =
                ntc_adc_valid(ffc_last_ntc) && ntc_adc_valid(cur_ntc_at_ffc);
            const bool ntc_anchor_ok_acc =
                ntc_adc_valid(cur_ntc_at_ffc)
                && (ntc_adc_valid(boot_ntc) || ntc_ok_acc);
            const int32_t dn_acc = ntc_ok_acc
                ? (int32_t)cur_ntc_at_ffc - (int32_t)ffc_last_ntc
                : 0;
            if (ffc_last_fpa_vtemp != 0) {
                int32_t dv_acc =
                    smooth_vtemp_at_ffc - (int32_t)ffc_last_fpa_vtemp;
                if (ntc_ok_acc) {
                    const int32_t adn = dn_acc < 0 ? -dn_acc : dn_acc;
                    const int32_t dv_ntc_cap = adn * DV_DN_RATIO_MAX;
                    if (dv_acc >  dv_ntc_cap) dv_acc =  dv_ntc_cap;
                    if (dv_acc < -dv_ntc_cap) dv_acc = -dv_ntc_cap;
                }
                if (dv_acc >  DV_CLAMP_ADU) dv_acc =  DV_CLAMP_ADU;
                if (dv_acc < -DV_CLAMP_ADU) dv_acc = -DV_CLAMP_ADU;
                comp_acc += (int32_t)(K_ANCHOR_COMP_STATIC * (float)dv_acc);
            }
            if (ntc_anchor_ok_acc) {
                comp_acc += ntc_anchor_comp_for(cur_ntc_at_ffc);
                if (ntc_ok_acc) {
                    dn_check = dn_acc;
                }
            }
            if (comp_acc >  K_COMP_MAX_ADU) comp_acc =  K_COMP_MAX_ADU;
            if (comp_acc < -K_COMP_MAX_ADU) comp_acc = -K_COMP_MAX_ADU;
            candidate_temp_anchor += comp_acc;
            println("FFC anchor+K: base=%ld comp=%ld final=%ld ntc=%u",
                    (long)(candidate_temp_anchor - comp_acc), (long)comp_acc,
                    (long)candidate_temp_anchor, cur_ntc_at_ffc);
        }
    }

    /* Carry the percentile smoother across FFC by shifting with Δeff_anchor.
     * Using eff_anchor_old (not bare anchor_old) accounts for the K·ΔVTEMP
     * compensation already applied to the renderer; otherwise every FFC
     * introduces a momentary scene-reading offset of K·ΔVTEMP/mean_diff. */
    int16_t predicted_jump_x10 = 0;
    if (ffc_done_first && g_smooth_low != 0 && g_smooth_high != 0) {
        int32_t eff_anchor_new = candidate_temp_anchor;
        if (thermal_comp_legacy_enabled()) {
            eff_anchor_new += ntc_warmup_curve_comp_for(cur_ntc_at_ffc);
        } else if (thermal_comp_table_enabled()) {
            eff_anchor_new += table_comp_at_ffc;
        }
        const int32_t scene_mid_adu =
            ((int32_t)g_smooth_low + (int32_t)g_smooth_high) >> 1;
        const int16_t old_mid_x10 =
            temp_x10_from_corrected_adu(scene_mid_adu, eff_anchor_old);
        const int16_t new_mid_x10 =
            temp_x10_from_corrected_adu(scene_mid_adu, eff_anchor_new);
        if (old_mid_x10 != INT16_MAX && new_mid_x10 != INT16_MAX) {
            int32_t jump_tmp = (int32_t)new_mid_x10 - (int32_t)old_mid_x10;
            if (jump_tmp < INT16_MIN) {
                jump_tmp = INT16_MIN;
            } else if (jump_tmp > INT16_MAX) {
                jump_tmp = INT16_MAX;
            }
            predicted_jump_x10 = (int16_t)jump_tmp;
            const int32_t abs_jump_x10 = i32_abs(predicted_jump_x10);
            if (abs_jump_x10 > FFC_SOFT_JUMP_X10) {
                const bool hard_jump =
                    abs_jump_x10 > FFC_HARD_JUMP_X10;
                const bool still_probing =
                    ffc_consec_reject < FFC_RECOVER_AFTER_JUMP_REJECTS;
                if (hard_jump || still_probing) {
                    println("FFC reject jump: old=%d new=%d jump=%d x0.1C soft=%d hard=%d rej=%u",
                            (int)old_mid_x10,
                            (int)new_mid_x10,
                            (int)predicted_jump_x10,
                            FFC_SOFT_JUMP_X10,
                            FFC_HARD_JUMP_X10,
                            (unsigned)ffc_consec_reject + 1u);
                    ffc_consec_reject++;
                    ffc_record_event(FFC_EVT_REJECT_JUMP,
                                     dg_check,
                                     dv_check,
                                     dn_check,
                                     eff_anchor_old,
                                     eff_anchor_new,
                                     predicted_jump_x10,
                                     new_calib_mean,
                                     cur_fpa_vtemp,
                                     cur_ntc_at_ffc);
                    ffc_restore_gain_cache(restore_gain_cache);
                    ffc_open_and_resync_scene();
                    led_off();
                    ffc_last_ms = to_ms_since_boot(get_absolute_time());
                    ffc_retry_after_ms = ffc_last_ms + FFC_RETRY_COOLDOWN_MS;
                    return;
                }
                println("FFC recover jump accept: old=%d new=%d jump=%d x0.1C rej=%u",
                        (int)old_mid_x10,
                        (int)new_mid_x10,
                        (int)predicted_jump_x10,
                        (unsigned)ffc_consec_reject);
            }
        }
        const int32_t delta = eff_anchor_new - eff_anchor_old;
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

    ffc_consec_reject = 0;

    /* All guards passed — commit the averaged shutter candidate and anchor. */
    for (size_t i = 0; i < PX_CNT; i++) {
        ram_calibg[i] = (uint16_t)ffc_sum[i];
    }
    calibg = ram_calibg;
    g_calib_mean = new_calib_mean;
    g_last_calib = nullptr; /* triggers recompute on next frame */
    ram_temp_anchor = candidate_temp_anchor;
    ffc_restore_gain_cache(restore_gain_cache);
    ffc_record_event(FFC_EVT_ACCEPT,
                     dg_check,
                     dv_check,
                     dn_check,
                     eff_anchor_old,
                     ram_temp_anchor,
                     predicted_jump_x10,
                     new_calib_mean,
                     cur_fpa_vtemp,
                     cur_ntc_at_ffc);

    /* update FFC bookkeeping. ffc_last_fpa_vtemp stores the SMOOTHED VTEMP
     * so that K_V·ΔV in subsequent frames is consistent (smooth − smooth).
     * The trigger and sanity check still compare raw cur vs this stored
     * smoothed value — close enough since they're separated by ≥30 s. */
    ffc_last_ms = to_ms_since_boot(get_absolute_time());
    ffc_retry_after_ms = 0;
    ffc_last_fpa_vtemp = (uint16_t)smooth_vtemp_at_ffc;
    ffc_last_ntc = cur_ntc_at_ffc;
    ffc_last_g_calib_mean = g_calib_mean;
    ffc_count++;
    ffc_done_first = true;

    ffc_open_and_resync_scene();
    led_off();
    println("FFC done, calib_mean=%ld", (long)g_calib_mean);
}

static bool ffc_should_trigger(uint16_t cur_fpa_vtemp)
{
    const uint32_t now_ms = to_ms_since_boot(get_absolute_time());

    if (!ffc_done_first) {
        return (now_ms >= FFC_INTERVAL_FIRST_MS);
    }

    if (ffc_retry_after_ms != 0) {
        if ((int32_t)(now_ms - ffc_retry_after_ms) < 0) {
            return false;
        }
        ffc_retry_after_ms = 0;
    }

    /* Select target interval based on which FFC we're waiting for.
     * ffc_count here is the number of FFCs COMPLETED, so ffc_count==1 means
     * we're deciding when to fire FFC #2, etc. After the explicit warmup
     * schedule (FFCs #1–#4 over t=0…30 s), use 15 s extended-warmup
     * cadence until t=180 s, then switch to 30 s stable cadence. */
    uint32_t target_interval;
    switch (ffc_count) {
        case 1:  target_interval = FFC_INTERVAL_WARMUP_2; break;
        case 2:  target_interval = FFC_INTERVAL_WARMUP_3; break;
        case 3:  target_interval = FFC_INTERVAL_WARMUP_4; break;
        default:
            target_interval = (now_ms < FFC_EXT_WARMUP_END_MS)
                                  ? FFC_INTERVAL_EXT_WARMUP_MS
                                  : FFC_INTERVAL_STABLE_MS;
            break;
    }

    /* Post-warmup thermal-settled gate: if VTEMP raw is far from its EMA
     * smoothed value, FPA is mid-transient (e.g. external thermal shock).
     * Defer FFC entirely until smoother catches up — committing now would
     * bake an enormous K_V·ΔV into ram_temp_anchor and shift the scene
     * reading permanently. The first three warmup FFCs still lock cold-start
     * calibration quickly; from the fourth attempt onward, require the signals
     * to be settled before firing the shutter. */
    if (ffc_count >= 3) {
        const int32_t smooth = (int32_t)(g_vtemp_smooth_x256 >> 8);
        int32_t vdiff = (int32_t)cur_fpa_vtemp - smooth;
        if (vdiff < 0) vdiff = -vdiff;
        if (K_ANCHOR_COMP_STATIC != 0.0f && vdiff > FFC_VTEMP_SETTLED_ADU) {
            return false;
        }
        if (K_ANCHOR_COMP_STATIC != 0.0f
            && !ffc_vtemp_ntc_plausible(
                cur_fpa_vtemp, g_cur_ntc_adc, nullptr, nullptr)) {
            return false;
        }
    }

    const uint32_t elapsed = now_ms - ffc_last_ms;
    if (elapsed >= target_interval) {
        return true;
    }

    if (elapsed < FFC_MIN_ELAPSED_MS) {
        return false;
    }

    /* This sensor revision has shown large VTEMP register jumps while NTC and
     * the scene remain continuous.  Keep VTEMP in diagnostics, but do not let
     * it machine-gun the shutter between the periodic FFC deadlines. */
    if (!FFC_VTEMP_TRIGGER_ENABLE) {
        return false;
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

    /* Init board NTC on ADC2 / GPIO28 for diagnostics and K_NTC.
     * Keep the old working ADC0 init sequence: deinit GPIO before switching
     * it to ADC, then sample through FIFO + DMA instead of single adc_read(). */
    gpio_deinit(IO_NTC);
    adc_init();
    adc_gpio_init(IO_NTC);
    adc_select_input(ADC_CH_NTC);
    ntc_adc_dma_init();

    extern void dutx_init();
    dutx_init();
    pico_get_unique_board_id_string(uid, UID_LEN);
    thermal_ota_init();
#if THERMAL_OTA_BOOT_PROBE
    /* One-shot isolation: prove a staging-sector erase before Core1, TinyUSB,
     * sensor DMA, or the application FIFO protocol has started. */
    watchdog_enable(8000u, true);
    watchdog_hw->scratch[0] = 0x544f5441u;
    watchdog_hw->scratch[1] = 0xb001u;
    watchdog_hw->scratch[2] = FLASH_OTA_HEADER_OFFSET;
    const int ota_probe_rc = thermal_ota_boot_probe();
    watchdog_hw->scratch[3] = (uint32_t)ota_probe_rc;
    watchdog_hw->scratch[1] = ota_probe_rc == 0 ? 0xb002u : 0xb0eeu;
    watchdog_disable();
#endif
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
    bool capture_sequence_active = false;
    uint16_t capture_sequence_mid_count = 0;
    uint16_t capture_invalid_run = 0;

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

    /* The startup shutter delay intentionally leaves the first completed DMA
     * packet waiting before Core0 enters the receive loop. Do not let that
     * one-time delay saturate the runtime rearm diagnostic. */
    rx_rearm_max_us = 0;
    rx_pio_stall_count = 0;

    while (true) {
        if (__builtin_expect(g_thermal_ota_core0_flash_state
                                 == THERMAL_OTA_CORE0_FLASH_REQUEST,
                             false)) {
            thermal_ota_core0_flash_service();
        }
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
                            capture_sequence_active = false;
                            capture_sequence_mid_count = 0;
                            g_capture_active_mid_count = 0;
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
                    capture_invalid_run = 0;
                    g_capture_packet_heads++;
                    const bool interrupted_sequence = capture_sequence_active;
                    if (interrupted_sequence) {
                        g_capture_unexpected_heads++;
                        g_capture_incomplete_frames++;
                        g_capture_head_after_head++;
                        capture_note_interrupted_sequence(
                            capture_sequence_mid_count);
                    }
                    capture_sequence_active = true;
                    capture_sequence_mid_count = 0;
                    g_capture_active_mid_count = 0;
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
                                rawbuf_epoch[cur_buf] = rawbuf_capture_epoch;
                                __dmb();
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
                    capture_invalid_run = 0;
                    g_capture_packet_mids++;
                    if (capture_sequence_active
                        && capture_sequence_mid_count != UINT16_MAX) {
                        capture_sequence_mid_count++;
                        g_capture_active_mid_count =
                            capture_sequence_mid_count;
                    }
                    if (!startup_synced && (lidx < 0 || cur_buf < 0)) {
                        if (++body_only_sync_cnt >= BODY_ONLY_RESYNC_THRES) {
                            body_only_sync_cnt = 0;
                            lidx = -1;
                            cur_buf = -1;
                            rawbuf_init();
                            capture_sequence_active = false;
                            capture_sequence_mid_count = 0;
                            g_capture_active_mid_count = 0;
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
                    capture_invalid_run = 0;
                    g_capture_packet_tails++;
                    if (capture_sequence_active) {
                        g_capture_last_tail_mid_count =
                            capture_sequence_mid_count;
                        if (capture_sequence_mid_count == SENSOR_H - 1) {
                            g_capture_complete_sequences++;
                        } else {
                            g_capture_bad_tail_sequences++;
                        }
                    } else {
                        g_capture_tail_without_head++;
                    }
                    capture_sequence_active = false;
                    capture_sequence_mid_count = 0;
                    g_capture_active_mid_count = 0;
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
                } else {
                    g_capture_packet_invalid++;
                    capture_invalid_run++;
                }
            } else {
                g_capture_packet_invalid++;
                capture_invalid_run++;
            }

            if (capture_invalid_run > g_capture_max_invalid_run) {
                g_capture_max_invalid_run = capture_invalid_run;
            }
            if (startup_synced
                && capture_invalid_run >= BODY_ONLY_RESYNC_THRES) {
                g_capture_runtime_resyncs++;
                g_capture_last_resync_invalid_run = capture_invalid_run;
                capture_invalid_run = 0;
                lidx = -1;
                if (cur_buf >= 0) {
                    g_capture_incomplete_frames++;
                    rawbuf_state[cur_buf] = RAWBUF_FREE;
                    cur_buf = -1;
                    __dmb();
                }
                capture_sequence_active = false;
                capture_sequence_mid_count = 0;
                g_capture_active_mid_count = 0;
                rxring.ridx = rxring.widx;
                wspi_stop1();
                sleep_ms(2);
                wspi_reinit1();
                rxring.ridx = rxring.widx;
                continue;
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

    uint32_t last_attempt_ms = 0;
    uint32_t xfer_start_ms = 0;
    while (true) {
        tud_task();
        const uint32_t t_ms = now();
        if (uvc_tx_busy && (uint32_t)(t_ms - xfer_start_ms) > 1000) {
            uvc_tx_busy = 0;
            uvc_sending = -1;
            uvc_state[0] = UVCBUF_FREE;
            __dmb();
        }
        if (tud_video_n_streaming(0, 0)
            && !uvc_tx_busy
            && (uint32_t)(t_ms - last_attempt_ms) >= (1000 / FRAME_RATE)) {
            last_attempt_ms = t_ms;
            fill_smpte_yuy2(uvc_buf[0], FRAME_SIZE);
            uvc_state[0] = UVCBUF_SENDING;
            uvc_sending = 0;
            uvc_tx_busy = 1;
            xfer_start_ms = t_ms;
            if (!tud_video_n_frame_xfer(0, 0, uvc_buf[0], FRAME_SIZE)) {
                uvc_tx_busy = 0;
                uvc_sending = -1;
                uvc_state[0] = UVCBUF_FREE;
                __dmb();
            }
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
