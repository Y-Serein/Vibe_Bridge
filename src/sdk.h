#ifndef __j5mSt0yNPrFBx__
#define __j5mSt0yNPrFBx__

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

enum UartBaudMode : uint32_t {
    UBM11 = 2000000,
    UBM22 = 2500000,
    UBM33 = 3000000,
    UBM44 = 4000000,
    UBM55 = 5000000,
    UBM66 = 6000000,
    UBM77 = 7000000,
    UBM88 = 8000000,
    UBMAA = 5000000,
};
#define UART_BAUD_MODE_DEF 0xAA

enum UartOutMode : uint32_t {
    UOM_CALI_U8 = 0,
    UOM_RAW_U16 = 1,
};

#define UART_OUT_MODE_DEF UOM_RAW_U16

#define RXPKTDBG 0

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

#define print(fmt, ...)    printf(fmt, ##__VA_ARGS__)
#define println(fmt, ...)  printf(fmt "\r\n", ##__VA_ARGS__)
#define eprintln(fmt, ...) printf("[E]" fmt "\n", ##__VA_ARGS__)

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
#define MAIN_FUNC_MODE                       MAIN_FUNC_MODE_SPI_INIT_40MHZ_4

#if MAIN_FUNC_MODE == MAIN_FUNC_MODE_SPI_INIT_40MHZ_4
#    define IO_LED 19
#else
#    define IO_LED 25
#endif

#define RAW_OUT_MODE 0
#define RAW16_MODE   1

#define SENSOR_W 160
#define SENSOR_H 120

#define FLASH_SIZE          (4 * 1024 * 1024)
#define FLASH_TARGET_SIZE   (128 * 1024)
#define FLASH_TARGET_OFFSET (FLASH_SIZE - FLASH_TARGET_SIZE)

extern const uint8_t* flash_target_contents;

void errh(const char* msg);
void dump_buf_8bytes(const uint8_t* buf, size_t len);

#endif
