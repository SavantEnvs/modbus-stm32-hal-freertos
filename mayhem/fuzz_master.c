/*
 * Modbus master (client) harness. Queues one telegram, lets the real StartTaskModbusMaster loop send
 * it, then plays the slave's response back (UART bytes + T3.5 expiry for RTU, a netconn buffer for
 * TCP) or lets the query time out. The caller's register image is allocated exactly as large as the
 * API contract requires for the query, so a response that writes past it is a heap overflow.
 *
 * Input: cfg, id (mapped to 1..247), fct-select, address(2), count, then response frames [len:7 | fix-crc:1][bytes].
 */
#include "harness_common.h"

static modbusHandler_t h;
static UART_HandleTypeDef huart;
static USART_TypeDef usart;
static GPIO_TypeDef gpio;

static const uint8_t valid_fct[8] = {1, 2, 3, 4, 5, 6, 15, 16};

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 8)
        return 0;
    struct rd r = {data, size};
    uint8_t cfg = take(&r);
    modbus_t t;
    memset(&t, 0, sizeof t);
    t.u8id = (uint8_t)(1 + take(&r) % 247); /* the API contract: a slave address in 1..247 */
    uint8_t sel = take(&r);
    t.u8fct = (sel & 0x80) ? (mb_functioncode_t)take(&r) : (mb_functioncode_t)valid_fct[sel & 7];
    t.u16RegAdd = take16(&r);
    t.u16CoilsNo = take(&r);

    shim_reset();
    memset(&h, 0, sizeof h);
    memset(mHandlers, 0, sizeof mHandlers);
    numberHandlers = 0;

    usart.SR = usart.ISR = 0x40;
    huart.Instance = &usart;
    huart.Init.BaudRate = (cfg & 4) ? 115200 : 9600;

    size_t words;
    switch (t.u8fct) {
    case MB_FC_READ_COILS:
    case MB_FC_READ_DISCRETE_INPUT:
    case MB_FC_WRITE_MULTIPLE_COILS:
        words = (t.u16CoilsNo + 15u) / 16u;
        break;
    case MB_FC_READ_REGISTERS:
    case MB_FC_READ_INPUT_REGISTER:
    case MB_FC_WRITE_MULTIPLE_REGISTERS:
        words = t.u16CoilsNo;
        break;
    default:
        words = 1;
        break;
    }
    if (words == 0)
        words = 1;
    uint16_t *regs = calloc(words, sizeof(uint16_t));
    fill_words(regs, words, 3);
    t.u16reg = regs;

    h.uModbusType = MB_MASTER;
    h.port = &huart;
    h.u8id = 0;
    h.u16timeOut = 1000;
    if (cfg & 2) {
        h.EN_Port = &gpio;
        h.EN_Pin = 1;
    }
#ifdef MB_SHIM_TCP
    h.xTypeHW = TCP_HW;
    t.xIpAddress = 0x0100007f;
    t.u16Port = 502;
    t.u8clientID = take(&r);
    shim_net_recv_empty_parks(false);
    if (cfg & 8)
        shim_net_fail_connect();
    if (cfg & 16)
        shim_net_fail_write();
#else
    h.xTypeHW = USART_HW;
#endif

    ModbusInit(&h);
    ModbusStart(&h);
    ModbusQuery(&h, t);

    uint8_t frame[MAX_BUFFER + 16];
#ifdef MB_SHIM_TCP
    size_t len = take_frame(&r, frame, sizeof frame, true);
    switch (cfg >> 6) {
    case 1: shim_net_set_rx(NULL, 0, ERR_CLSD); break;
    case 2: shim_net_set_rx(NULL, 0, ERR_TIMEOUT); break;
    default: shim_net_set_rx(frame, len, ERR_OK); break;
    }
    shim_task_run();
#else
    shim_task_run();
    for (int i = 0; i < 2 && r.n > 0; i++) {
        size_t len = take_frame(&r, frame, sizeof frame, false);
        uart_deliver(&h, frame, len);
    }
    if ((cfg >> 6) == 1)
        shim_timer_fire(h.xTimerTimeout);
    else
        shim_timer_fire(h.xTimerT35);
    shim_task_run();
#endif

    shim_reset();
    free(regs);
    return 0;
}
