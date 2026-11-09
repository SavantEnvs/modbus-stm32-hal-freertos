/* Shared plumbing for the Modbus harnesses: input reader, frame/CRC helpers, byte injection. */
#ifndef MB_HARNESS_COMMON_H
#define MB_HARNESS_COMMON_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "Modbus.h"
#include "rtos_shim.h"

extern void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart);

struct rd { const uint8_t *p; size_t n; };

static inline uint8_t take(struct rd *r)
{
    if (r->n == 0)
        return 0;
    r->n--;
    return *r->p++;
}

static inline uint16_t take16(struct rd *r)
{
    uint16_t hi = take(r);
    return (uint16_t)(hi << 8 | take(r));
}

/* Next wire frame from the input: [len:7 | crc:1][len bytes]. With crc set the correct Modbus CRC is
   appended (RTU); `mbap` prepends a Modbus TCP header instead. Returns the frame length. */
static inline size_t take_frame(struct rd *r, uint8_t *out, size_t cap, bool mbap)
{
    uint8_t b = take(r);
    size_t len = b & 0x7f;
    bool fix = b & 0x80;
    if (len > r->n)
        len = r->n;
    size_t off = 0;
    if (mbap) {
        out[0] = 0x12;
        out[1] = 0x34;
        out[2] = 0;
        out[3] = 0;
        out[4] = (uint8_t)(len >> 8);
        out[5] = (uint8_t)len;
        off = 6;
    }
    if (off + len + 2 > cap)
        len = cap - off - 2;
    memcpy(out + off, r->p, len);
    r->p += len;
    r->n -= len;
    size_t total = off + len;
    if (fix && !mbap) {
        uint16_t crc = calcCRC(out, (uint8_t)total);
        out[total++] = crc >> 8;
        out[total++] = crc & 0xff;
    }
    return total;
}

/* Clock a received frame into the UART ISR path byte by byte, then let T3.5 expire. */
static inline void uart_deliver(modbusHandler_t *h, const uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        h->dataRX = buf[i];
        HAL_UART_RxCpltCallback(h->port);
    }
}

static inline void fill_words(uint16_t *p, size_t n, unsigned salt)
{
    for (size_t i = 0; i < n; i++)
        p[i] = (uint16_t)(0x1111u * salt + i);
}

#endif
