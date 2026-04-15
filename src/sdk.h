#ifndef __j5mSt0yNPrFBx__
#define __j5mSt0yNPrFBx__

#include <sys/cdefs.h>
#if defined(__STDC__)
#elifdef __GNUC__
#else
#    error Unsupported compiler
#endif

#if !defined(__GNUC__)
#    error "GNU compiler required"
#endif

#ifdef __STRICT_ANSI__
#    error "GNU C required"
#endif

#ifdef nullptr
#    error "Macro-defined"
#endif

#include <stdint.h>
#include <stdio.h>

#define THDEBUG   1
#define MAX_SPEED 1

enum UartBaudMode : uint32_t {
    UBM11 = 2000000,
    UBM22 = 2500000,
    UBM33 = 3000000,
    UBM44 = 4000000,
    UBM55 = 5000000,
    UBM66 = 6000000,
    UBM77 = 7000000,
    UBM88 = 8000000,
    UBMAA = 2000000,
    UBMBB = 10000000,
    UBMCC = 5000000,
    UBMDD = 5000000,
};
#define UART_BAUD_MODE_DEF 0x11
#define SHUTTER_ENABLE     1

enum UartOutMode : uint32_t {
    UOM_CALI_U8 = 0,
    UOM_RAW_U16 = 1,
};

#define UART_OUT_MODE_DEF UOM_RAW_U16

enum DataOutMode : uint8_t {
    DOM_UART = 0,
    DOM_UVC,
};
#define DATA_OUT_MODE_DEF DOM_UART

enum CaliMode : uint8_t {
    CM_OFF = 0,
    CM_ON,
};
#define CALIMODE_DEF CM_OFF

struct Button {
    uint8_t pin;
    uint8_t state;
    uint32_t last_tick;
    volatile bool event;
};
#define DEBOUNCE       20
#define BUTTON_NUM_MAX 1

struct GpioBtn {
    uint8_t gpio;
    uint8_t stable_level;
    uint8_t last_level;
    uint32_t last_change_ms;
    uint32_t press_ms;
    uint8_t event;
};

enum GpioBtnEvent {
    GB_EV_PRESS = 1 << 0,
    GB_EV_RELEASE = 1 << 1,
    GB_EV_LONG = 1 << 2,
};
#define GB_LONG_PRESS 800

struct __attribute__((packed)) BadPt {
    uint8_t y;
    uint8_t x;
};
#define MAX_BADPT       5
#define HOT_PIXEL_THRES 15000
#ifndef unlikely
#    define unlikely(x) __builtin_expect(!!(x), 0)
#endif

#define CALI_ERROR_LIST \
    X(ERR_NONE, 0) \
    X(ERR_CALI_ALGO_BORDER_NEG_DIFF, 1) \
    X(ERR_CALI_ALGO_NO_VALID_PIXEL, 2) \
    X(ERR_CALI_ALGO_TOO_MANY_BAD_PTS, 3) \
    X(ERR_CALI_ALGO_ADJACENT_BAD_PTS, 4) \
    X(ERR_FLASH_IMGBG, 5) \
    X(ERR_FLASH_GAIN, 6) \
    X(ERR_FLASH_BADPT, 7) \
    X(ERR_FLASH_HDR, 8) \
    X(ERR_FLASH_EMPTY, 9)

#define X(name, val) name = val,
enum {
    CALI_ERROR_LIST
};
#undef X

enum : uint32_t {
    CORE0_EV0 = 0xdeadc0e0,
    CORE1_EV0 = 0xdeadc1e0,
};

struct yuv_cmap_entry {
    int16_t u_raw;
    int16_t v_raw;
    uint8_t y;
};

struct yuv_cmap_simd {
    uint32_t uv_raw;
    uint32_t y;
};

#define RXPKTDBG 0

#define LED_STATE_DEF  0
#define LED_BOTH       1
#define LED_ON_VAL     true
#define LED_OFF_VAL    false
#define LED_FAILED_INV 200
#define LED_SUCC_INV   1000

#define CALI_LED_T_NUM 3

#define IO_ISDA 1
#define IO_ICLK 2
#define IO_RSTB 12
#define IO_EN   14
#define IO_OSDA 10
#define IO_OCLK 20
#define IO_OFS  13

#define IO_UTX 16
#define IO_URX 17
#define UUP    uart0

#define IO_FUTX 4
#define IO_FURX 5
#define FUP     uart1

#if THDEBUG == 1
#    define print(fmt, ...)    printf(fmt, ##__VA_ARGS__)
#    define println(fmt, ...)  printf(fmt "\r\n", ##__VA_ARGS__)
#    define eprintln(fmt, ...) printf("[E]" fmt "\n", ##__VA_ARGS__)
#else
#    define print(fmt, ...)
#    define println(fmt, ...)
#    define eprintln(fmt, ...)
#endif

#define IO_PLED 25
#define IO_LED  19
#define IO_DUTX 20
#define IO_SSDA 11
#define IO_SCLK 10
#define IO_SCS  13
#define IO_SCTL 14
#define IO_SRST 12
#define IO_PCLK 2
#define IO_PSDA 1
#define IO_OTX  16
#define IO_ORX  17
#define OUART   uart0
#define IO_NTC  26
#define IO_SVMS 3
#define IO_SVM1 4
#define IO_SVM2 5
#define IO_BTN  0

#define MAIN_FUNC_MODE_SPI_INIT_25MHZ        0
#define MAIN_FUNC_MODE_240MHZ                1
#define MAIN_FUNC_MODE_SCLK_IO               2
#define MAIN_FUNC_MODE_SPI_INIT_40MHZ        3
#define MAIN_FUNC_MODE_160M_UVC              4
#define MAIN_FUNC_MODE_160M_CDC              5
#define MAIN_FUNC_MODE_SPI_INIT_40MHZ_NOINIT 6
#define MAIN_FUNC_MODE_SPI_INIT_40MHZ_3      7
#define MAIN_FUNC_MODE_SPI_INIT_40MHZ_4      8
#define MAIN_FUNC_MODE_BAUD_SEL              9
#define MAIN_FUNC_MODE_ADC                   10
#define MAIN_FUNC_MODE_SHUTTER               11
#define MAIN_FUNC_MODE_FLASH                 12
#define MAIN_FUNC_MODE_GPIO0                 13
#define MAIN_FUNC_MODE_UVC_MAX               14
#define MAIN_FUNC_MODE_BOARD_ID              15
#define MAIN_FUNC_MODE_BOARD_ID_BOARD        16
#define MAIN_FUNC_MODE_BLINK                 17
#define MAIN_FUNC_MODE                       MAIN_FUNC_MODE_SPI_INIT_40MHZ_4

#define RAW_OUT_MODE      0
#define LED_BTN_SYNC_MODE 0
#define CORE1_TIMEU_MODE  0
#define CALI_PASS_MODE    0
#define OCLK_180          1
#define OUART_SETUP_DEF   true

#define SENSOR_W          160
#define SENSOR_H          120
#define MIN_THERMAL_RANGE 128

#define FLASH_SIZE          (4 * 1024 * 1024)
#define FLASH_TARGET_SIZE   (512 * 1024)
#define FLASH_TARGET_OFFSET (FLASH_SIZE - FLASH_TARGET_SIZE)
#define FLASH_OFS_INFO      0x000000
#define FLASH_OFS_IMGBG     0x002000
#define FLASH_OFS_BADPTS    0x00e000
#define FLASH_OFS_GAIN      0x01a000
#define FLASH_RETRY_MAX     3
#define FLASH_HAS_DATA_F    0x78

struct __attribute__((packed)) FlashDataInfo {
    uint8_t has_data;
    uint8_t _pad0[3];
    uint32_t imgbg_ofs;
    uint32_t imgbg_size;
    uint32_t badpts_ofs;
    uint32_t badpts_size;
    uint32_t badpts_num;
    uint32_t gain_ofs;
    uint32_t gain_size;
    uint8_t _pad1[32];
};

struct FlashOpInfo {
    uint32_t ofs;
    uint32_t size;
    uint8_t* data;
};

extern const uint8_t* flash_target_contents;
extern const struct FlashDataInfo* flash_header;
extern const uint16_t* flash_imgbg;
extern const float* flash_gain;
extern const struct BadPt* flash_badpt;
extern enum DataOutMode dataomode;
extern struct Button buttons[BUTTON_NUM_MAX];
#if MAIN_FUNC_MODE == MAIN_FUNC_MODE_BOARD_ID
extern uint8_t uid[8];
#endif

#define UID_LEN (2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1)
extern char uid[];

void errh(const char* msg);
void dump_buf_8bytes(const uint8_t* buf, size_t len);
void button_init();
void button_scan(struct GpioBtn* btn);
#if MAIN_FUNC_MODE == MAIN_FUNC_MODE_BOARD_ID
void flash_read_unique_id();
#endif

static_assert(sizeof(struct BadPt) == 2, "BadPt size must be 2 bytes!");
static_assert(sizeof(struct FlashDataInfo) == 64,
              "FlashDataInfo size must be 64 bytes!");
#define FLOAT_ABS_EPS 1e-7f
#define FLOAT_REL_EPS 1e-6f

#endif
