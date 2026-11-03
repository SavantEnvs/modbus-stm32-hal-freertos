/*
 * Modbus slave (server) harness. Drives the real StartTaskModbusSlave loop: each input frame is
 * received as the target would receive it (UART ISR bytes + T3.5 expiry for RTU, a netconn buffer for
 * TCP), the task validates it, touches the register image and builds the response.
 *
 * Input: cfg, n01, n23, s01, s23 (slave id, shared/separate register regions and their sizes/starts),
 * then frames as [len:7 | fix-crc:1][bytes] (up to 8). Seeds are valid CRC-correct requests.
 */
#include "harness_common.h"

static modbusHandler_t h;
static UART_HandleTypeDef huart;
static USART_TypeDef usart;
static GPIO_TypeDef gpio;

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 6)
        return 0;
    struct rd r = {data, size};
    uint8_t cfg = take(&r), n01 = take(&r), n23 = take(&r), s01 = take(&r), s23 = take(&r);

    shim_reset();
    memset(&h, 0, sizeof h);
    memset(mHandlers, 0, sizeof mHandlers);
    numberHandlers = 0;

    usart.SR = usart.ISR = 0x40;
    huart.Instance = &usart;
    huart.Init.BaudRate = 19200;

    size_t shared = 1u + (n01 & 0x3f);
    uint16_t *regs = calloc(shared, sizeof(uint16_t));
    fill_words(regs, shared, 1);
    h.u16regs = regs;
    h.u16regsize = (uint16_t)shared;

    uint16_t *sep[4] = {0};
    unsigned mask = (cfg >> 2) & 15;
    for (unsigned k = 0; k < 4; k++) {
        if (!(mask & (1u << k)))
            continue;
        uint8_t nb = (k < 2 ? n01 : n23) >> (4 * (k & 1));
        uint8_t sb = (k < 2 ? s01 : s23) >> (4 * (k & 1));
        size_t n = 1u + (nb & 15);
        sep[k] = calloc(n, sizeof(uint16_t));
        fill_words(sep[k], n, k + 2);
        uint16_t start = (uint16_t)((sb & 15) * 8);
        switch (k) {
        case 0: h.u16coils = sep[k]; h.u16coilsNregs = (uint16_t)n; h.u16coilsStartAdd = start; break;
        case 1: h.u16discreteInputs = sep[k]; h.u16discreteInputsNregs = (uint16_t)n; h.u16discreteInputsStartAdd = start; break;
        case 2: h.u16inputRegs = sep[k]; h.u16inputRegsNregs = (uint16_t)n; h.u16inputRegsStartAdd = start; break;
        default: h.u16holdingRegs = sep[k]; h.u16holdingRegsNregs = (uint16_t)n; h.u16holdingRegsStartAdd = start; break;
        }
    }

    h.uModbusType = MB_SLAVE;
    h.port = &huart;
    h.u8id = (uint8_t)(1 + (cfg >> 6));
    h.u16timeOut = 1000;
    if (cfg & 2) {
        h.EN_Port = &gpio;
        h.EN_Pin = 1;
    }
#ifdef MB_SHIM_TCP
    h.xTypeHW = TCP_HW;
    shim_net_recv_empty_parks(true);
#else
    h.xTypeHW = USART_HW;
#endif

    ModbusInit(&h);
    ModbusStart(&h);
    shim_task_run();

    uint8_t frame[MAX_BUFFER + 16];
    for (int i = 0; i < 8 && r.n > 0; i++) {
#ifdef MB_SHIM_TCP
        uint8_t kind = take(&r);
        size_t len = take_frame(&r, frame, sizeof frame, true);
        if (kind & 1)
            shim_net_fail_accept();
        switch ((kind >> 1) & 3) {
        case 1: shim_net_set_rx(NULL, 0, ERR_CLSD); break;
        case 2: shim_net_set_rx(NULL, 0, ERR_TIMEOUT); break;
        default: shim_net_set_rx(frame, len, ERR_OK); break;
        }
#else
        size_t len = take_frame(&r, frame, sizeof frame, false);
        uart_deliver(&h, frame, len);
        shim_timer_fire(h.xTimerT35);
#endif
        shim_task_run();
    }

    shim_reset();
    free(regs);
    for (unsigned k = 0; k < 4; k++)
        free(sep[k]);
    return 0;
}
