/*
 * Known-answer tests for MODBUS-LIB, run by mayhem/test.sh. The library's real slave/master task
 * loops run on the host shim (mayhem/shim); every expectation is a fixed byte string from the Modbus
 * RTU / TCP specification (CRCs are computed by an independent reference implementation here).
 */
#include <stdio.h>

#include "harness_common.h"

static int passed, failed;

#define CHECK(name, cond) do { \
        if (cond) { passed++; printf("PASS %s\n", name); } \
        else { failed++; printf("FAIL %s (%s:%d)\n", name, __FILE__, __LINE__); } \
    } while (0)

static modbusHandler_t h;
static UART_HandleTypeDef huart;
static USART_TypeDef usart;
static GPIO_TypeDef gpio;
static uint16_t regs[8];
static uint8_t expect_tid_lo;

static uint16_t ref_crc(const uint8_t *p, size_t n)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
    }
    return crc;
}

static size_t with_crc(uint8_t *out, const uint8_t *pdu, size_t n)
{
    memcpy(out, pdu, n);
    uint16_t c = ref_crc(pdu, n);
    out[n] = c & 0xff;
    out[n + 1] = c >> 8;
    return n + 2;
}

static void setup(mb_masterslave_t type)
{
    expect_tid_lo = type == MB_SLAVE ? 1 : 0;
    shim_reset();
    memset(&h, 0, sizeof h);
    memset(mHandlers, 0, sizeof mHandlers);
    numberHandlers = 0;
    usart.SR = usart.ISR = 0x40;
    huart.Instance = &usart;
    huart.Init.BaudRate = 19200;
    h.port = &huart;
    h.uModbusType = type;
    h.u8id = type == MB_SLAVE ? 1 : 0;
    h.u16timeOut = 1000;
#ifdef MB_SHIM_TCP
    h.xTypeHW = TCP_HW;
    shim_net_recv_empty_parks(type == MB_SLAVE);
#else
    h.xTypeHW = USART_HW;
#endif
}

static void slave_start(void)
{
    static const uint16_t init[8] = {0x1234, 0xABCD, 0, 0, 0, 0, 0, 0x7777};
    memcpy(regs, init, sizeof regs);
    setup(MB_SLAVE);
    h.u16regs = regs;
    h.u16regsize = 8;
    ModbusInit(&h);
    ModbusStart(&h);
    shim_task_run();
}

/* Present one request PDU (unit id first) to the slave. */
static void slave_send(const uint8_t *pdu, size_t n)
{
    uint8_t frame[160];
    shim_tx_clear();
#ifdef MB_SHIM_TCP
    frame[0] = 0x00; frame[1] = 0x01; frame[2] = 0; frame[3] = 0; frame[4] = 0; frame[5] = (uint8_t)n;
    memcpy(frame + 6, pdu, n);
    shim_net_set_rx(frame, n + 6, ERR_OK);
#else
    size_t len = with_crc(frame, pdu, n);
    uart_deliver(&h, frame, len);
    shim_timer_fire(h.xTimerT35);
#endif
    shim_task_run();
}

/* Does the slave's transmitted answer equal this PDU (RTU: + CRC, TCP: + MBAP header)? */
static bool answered(const uint8_t *pdu, size_t n)
{
    uint8_t want[160];
    size_t wl;
#ifdef MB_SHIM_TCP
    want[0] = 0x00; want[1] = expect_tid_lo; want[2] = 0; want[3] = 0; want[4] = 0; want[5] = (uint8_t)n;
    memcpy(want + 6, pdu, n);
    wl = n + 6;
#else
    wl = with_crc(want, pdu, n);
#endif
    size_t got;
    const uint8_t *tx = shim_tx_data(&got);
    return got == wl && memcmp(tx, want, wl) == 0;
}

static bool silent(void)
{
    size_t got;
    shim_tx_data(&got);
    return got == 0;
}

static void test_slave(void)
{
    {
        static const uint8_t rq[] = {1, 3, 0, 0, 0, 2};
        static const uint8_t rsp[] = {1, 3, 4, 0x12, 0x34, 0xAB, 0xCD};
        slave_start();
        slave_send(rq, sizeof rq);
        CHECK("slave FC3 read holding registers", answered(rsp, sizeof rsp));
    }
    {
        static const uint8_t rq[] = {1, 4, 0, 7, 0, 1};
        static const uint8_t rsp[] = {1, 4, 2, 0x77, 0x77};
        slave_start();
        slave_send(rq, sizeof rq);
        CHECK("slave FC4 read input register", answered(rsp, sizeof rsp));
    }
    {
        static const uint8_t rq[] = {1, 1, 0, 0, 0, 8};
        static const uint8_t rsp[] = {1, 1, 1, 0x34};
        slave_start();
        slave_send(rq, sizeof rq);
        CHECK("slave FC1 read coils", answered(rsp, sizeof rsp));
    }
    {
        static const uint8_t rq[] = {1, 2, 0, 12, 0, 4};
        static const uint8_t rsp[] = {1, 2, 1, 0x01};
        slave_start();
        slave_send(rq, sizeof rq);
        CHECK("slave FC2 read discrete inputs", answered(rsp, sizeof rsp));
    }
    {
        static const uint8_t rq[] = {1, 6, 0, 1, 0x00, 0xFF};
        slave_start();
        slave_send(rq, sizeof rq);
        CHECK("slave FC6 echoes the request", answered(rq, sizeof rq));
        CHECK("slave FC6 stores the register", regs[1] == 0x00FF && regs[0] == 0x1234);
    }
    {
        static const uint8_t rq[] = {1, 5, 0, 17, 0xFF, 0x00};
        slave_start();
        slave_send(rq, sizeof rq);
        CHECK("slave FC5 echoes the request", answered(rq, sizeof rq));
        CHECK("slave FC5 sets the coil", regs[1] == (0xABCD | (1u << 1)));
    }
    {
        static const uint8_t rq[] = {1, 16, 0, 2, 0, 2, 4, 0xDE, 0xAD, 0xBE, 0xEF};
        static const uint8_t rsp[] = {1, 16, 0, 2, 0, 2};
        slave_start();
        slave_send(rq, sizeof rq);
        CHECK("slave FC16 acknowledges address and count", answered(rsp, sizeof rsp));
        CHECK("slave FC16 stores the registers", regs[2] == 0xDEAD && regs[3] == 0xBEEF);
    }
    {
        static const uint8_t rq[] = {1, 15, 0, 16, 0, 10, 2, 0xA5, 0x02};
        static const uint8_t rsp[] = {1, 15, 0, 16, 0, 10};
        slave_start();
        slave_send(rq, sizeof rq);
        CHECK("slave FC15 acknowledges address and count", answered(rsp, sizeof rsp));
        CHECK("slave FC15 stores the coils", regs[1] == 0xAAA5);
    }
    {
        static const uint8_t rq[] = {1, 3, 0, 7, 0, 2};
        static const uint8_t rsp[] = {1, 0x83, 2};
        slave_start();
        slave_send(rq, sizeof rq);
        CHECK("slave answers an out-of-range read with exception 2", answered(rsp, sizeof rsp));
    }
    {
        static const uint8_t rq[] = {1, 0x2B, 0, 0, 0, 1};
        static const uint8_t rsp[] = {1, 0xAB, 1};
        slave_start();
        slave_send(rq, sizeof rq);
        CHECK("slave answers an unsupported function with exception 1", answered(rsp, sizeof rsp));
    }
#ifndef MB_SHIM_TCP
    {
        static const uint8_t rq[] = {7, 6, 0, 0, 0, 9};
        slave_start();
        slave_send(rq, sizeof rq);
        CHECK("slave ignores another unit's request", silent() && regs[0] == 0x1234);
    }
#endif
    {
        static const uint8_t rq[] = {0, 6, 0, 0, 0, 9};
        slave_start();
        slave_send(rq, sizeof rq);
        CHECK("slave applies a broadcast write without answering", silent() && regs[0] == 9);
    }
#ifndef MB_SHIM_TCP
    {
        static const uint8_t pdu[] = {1, 6, 0, 0, 0, 9};
        uint8_t frame[16];
        size_t len = with_crc(frame, pdu, sizeof pdu);
        frame[len - 1] ^= 0x40;
        slave_start();
        shim_tx_clear();
        uart_deliver(&h, frame, len);
        shim_timer_fire(h.xTimerT35);
        shim_task_run();
        CHECK("slave rejects a bad CRC without applying the write", regs[0] == 0x1234 && h.u16errCnt == 1);
    }
    {
        static const uint8_t pdu[] = {1, 6, 0, 0, 0, 9};
        uint8_t frame[16];
        size_t len = with_crc(frame, pdu, sizeof pdu);
        slave_start();
        shim_tx_clear();
        uart_deliver(&h, frame, 4);
        shim_timer_fire(h.xTimerT35);
        shim_task_run();
        CHECK("slave rejects a truncated frame", silent() && regs[0] == 0x1234 && h.u16errCnt == 1);
        uart_deliver(&h, frame, len);
        shim_timer_fire(h.xTimerT35);
        shim_task_run();
        CHECK("slave recovers and serves the next frame", regs[0] == 9 && h.u16OutCnt == 1);
    }
    CHECK("CRC of 01 03 00 00 00 02 is C4 0B",
          calcCRC((uint8_t[]){1, 3, 0, 0, 0, 2}, 6) == 0xC40B);
#endif
}

/* Start a master and queue one telegram; the task sends it and waits for the answer. */
static uint16_t mregs[8];

static void master_query(mb_functioncode_t fct, uint16_t add, uint16_t n)
{
    setup(MB_MASTER);
    memset(mregs, 0, sizeof mregs);
    ModbusInit(&h);
    ModbusStart(&h);
    modbus_t t;
    memset(&t, 0, sizeof t);
    t.u8id = 1;
    t.u8fct = fct;
    t.u16RegAdd = add;
    t.u16CoilsNo = n;
    t.u16reg = mregs;
#ifdef MB_SHIM_TCP
    t.xIpAddress = 0x0100007f;
    t.u16Port = 502;
    t.u8clientID = 0;
#endif
    shim_tx_clear();
    ModbusQuery(&h, t);
}

static void master_answer(const uint8_t *pdu, size_t n)
{
#ifdef MB_SHIM_TCP
    uint8_t frame[160];
    frame[0] = 0; frame[1] = 1; frame[2] = 0; frame[3] = 0; frame[4] = 0; frame[5] = (uint8_t)n;
    memcpy(frame + 6, pdu, n);
    shim_net_set_rx(frame, n + 6, ERR_OK);
    shim_task_run();
#else
    uint8_t frame[160];
    shim_task_run();
    size_t len = with_crc(frame, pdu, n);
    uart_deliver(&h, frame, len);
    shim_timer_fire(h.xTimerT35);
    shim_task_run();
#endif
}

static void test_master(void)
{
    uint32_t note;
    {
        static const uint8_t rq[] = {1, 3, 0, 4, 0, 2};
        static const uint8_t rsp[] = {1, 3, 4, 0x12, 0x34, 0xAB, 0xCD};
        master_query(MB_FC_READ_REGISTERS, 4, 2);
        master_answer(rsp, sizeof rsp);
        CHECK("master FC3 query frame", answered(rq, sizeof rq));
        CHECK("master FC3 stores the registers", mregs[0] == 0x1234 && mregs[1] == 0xABCD && mregs[2] == 0);
        CHECK("master FC3 reports success", shim_main_notified(&note) && note == OP_OK_QUERY && h.i8state == COM_IDLE);
    }
    {
        static const uint8_t rq[] = {1, 6, 0, 9, 0xBE, 0xEF};
        static const uint8_t rsp[] = {1, 6, 0, 9, 0xBE, 0xEF};
        master_query(MB_FC_WRITE_REGISTER, 9, 1);
        mregs[0] = 0xBEEF;
        master_answer(rsp, sizeof rsp);
        CHECK("master FC6 query frame", answered(rq, sizeof rq));
        CHECK("master FC6 reports success", shim_main_notified(&note) && note == OP_OK_QUERY);
    }
    {
        static const uint8_t rsp[] = {1, 1, 2, 0x34, 0x12};
        master_query(MB_FC_READ_COILS, 0, 16);
        master_answer(rsp, sizeof rsp);
        CHECK("master FC1 unpacks the coil bytes", mregs[0] == 0x1234);
    }
#ifndef MB_SHIM_TCP
    {
        static const uint8_t pdu[] = {1, 3, 4, 0x12, 0x34, 0xAB, 0xCD};
        uint8_t frame[32];
        size_t len = with_crc(frame, pdu, sizeof pdu);
        frame[len - 2] ^= 1;
        master_query(MB_FC_READ_REGISTERS, 4, 2);
        shim_task_run();
        uart_deliver(&h, frame, len);
        shim_timer_fire(h.xTimerT35);
        shim_task_run();
        CHECK("master rejects a bad CRC", shim_main_notified(&note) && note == ERR_BAD_CRC && mregs[0] == 0);
    }
    {
        master_query(MB_FC_READ_REGISTERS, 4, 2);
        shim_task_run();
        shim_timer_fire(h.xTimerTimeout);
        shim_task_run();
        CHECK("master reports a query timeout", shim_main_notified(&note) && note == ERR_TIME_OUT && h.i8state == COM_IDLE);
    }
#else
    {
        master_query(MB_FC_READ_REGISTERS, 4, 2);
        shim_task_run();
        CHECK("master reports a TCP receive timeout", shim_main_notified(&note) && note == ERR_TIME_OUT);
    }
#endif
}

int main(void)
{
    test_slave();
    test_master();
    shim_reset();
    printf("selftest: %d passed, %d failed\n", passed, failed);
    return failed != 0;
}
