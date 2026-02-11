#include "utils.h"

#include <sdk.h>
#include <stdint.h>

#define U8PKTSIZE 161

static inline uint8_t u14_to_u8_no_ff(uint16_t v)
{
    v &= 0x3fff;
    uint8_t out = v >> 6;
    out -= (out == 0xff);
    return out;
}

int32_t make_line(const uint16_t* line, uint8_t** out, uint32_t* outs)
{
    static uint8_t u8pkt[U8PKTSIZE];
    static int32_t lidx = -1;
    u8pkt[0] = 0xFF;

    if (lidx >= 120) {
        lidx = -1;
    }

    do {
        if (line[1] == 0x00ff && line[171] == 0x00ff) {
            if (line[2] == 0xc700 && line[172] == 0xda00) {
                if (lidx >= 0) {
                    lidx++;
                }
                break;
            } else if (line[2] == 0xec00 && line[172] == 0xda00) {
                lidx = 0;
                break;
            } else if (line[2] == 0xc700 && line[172] == 0xf100) {
                if (lidx >= 0) {
                    lidx++;
                }
                break;
            }
        }
        return -1;
    } while (0);

    for (int i = 0; i < 160; ++i) {
        u8pkt[i + 1] = u14_to_u8_no_ff(line[i + 4]);
    }

    if (lidx == 0) {
        *out = u8pkt;
        *outs = U8PKTSIZE;
    } else {
        *out = u8pkt + 1;
        *outs = U8PKTSIZE - 1;
    }
    return lidx;
}

int32_t make_line0(const uint16_t* line, uint8_t** out, uint32_t* outs)
{
    static uint8_t u8pkt[U8PKTSIZE];

    for (int i = 0; i < 160; ++i) {
        u8pkt[i + 1] = u14_to_u8_no_ff(line[i + 4]);
    }

    *out = u8pkt + 1;
    *outs = 160;

    return 0;
}

static inline uint16_t be16_to_le14(const uint8_t* p)
{
    uint16_t v;
    v = ((uint16_t)p[0] << 8) | p[1];
    return v & 0x3FFF;
}

#if 0
static inline uint8_t sensor_x_to_u8(uint16_t x)
{
    const uint16_t x_min = 5042;
    const uint16_t x_max = 10992;

    if (x <= x_min)
        return 0;
    if (x >= x_max)
        return 254;

    return (uint8_t)(((uint32_t)(x - x_min) * 2798u) >> 16);
}
#else
static inline uint8_t sensor_x_to_u8(uint16_t x)
{
    const uint16_t x_min = 5042;
    const uint16_t x_max = 10992;
    const uint16_t x_range = x_max - x_min;

    if (x <= x_min)
        return 0;
    if (x >= x_max)
        return 254;
    /* linear mapping: -20℃ -> 0, 65℃ -> 254 */
    return (uint8_t)(((uint32_t)(x - x_min) * 254u + x_range / 2) / x_range);
}
#endif

void calil(const uint8_t* in, uint8_t** out, uint32_t* outsiz)
{
    static uint8_t line[SENSOR_W];
    const uint32_t size = SENSOR_W;
    const uint32_t start = 14;

    for (uint32_t i = 0; i < size; ++i) {
        uint16_t value = be16_to_le14(&in[start + i * 2]);
        line[i] = sensor_x_to_u8(value);
    }

    *out = line;
    *outsiz = SENSOR_W;
}
