#pragma once

#include <hardware/dma.h>
#include <hardware/pio.h>
#include <pico/types.h>
#include <stddef.h>
#include <stdint.h>

#define BUFSIZE  348
#define BUFCOUNT 4
#define BUFOF    0

struct RxBuf {
    uint8_t data[BUFSIZE + BUFOF];
    uint32_t len;
};

struct RxRing {
    volatile uint32_t widx;
    volatile uint32_t ridx;
    struct RxBuf buf[BUFCOUNT];
};

struct RxDma {
    uint chan;
    uint32_t buf_idx;
};

extern uint spi1clk;
extern struct RxRing rxring __attribute__((section(".scratch_x")));
extern PIO rxpio;
extern uint rxsm;
extern uint rxofs;
extern bool rxfinish;
extern uint32_t dirqc;
extern uint32_t pirqc;
extern int sdch;
extern dma_channel_config clkdcfg;
extern const uint8_t Z;

void init();
void rx_poll();
