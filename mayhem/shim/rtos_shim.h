/*
 * Host-side stand-ins for the STM32 HAL, CMSIS-RTOS2/FreeRTOS and lwIP netconn APIs that
 * MODBUS-LIB/Src/*.c includes. Only the surface the library uses is provided.
 *
 * The library's tasks (StartTaskModbusSlave/Master) are the real, unmodified code: they run on one
 * worker thread that hands control back to the caller ("parks") at every blocking call, so a harness
 * drives them deterministically: feed bytes, fire a timer, shim_task_run(), inspect the result.
 */
#ifndef MB_RTOS_SHIM_H
#define MB_RTOS_SHIM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

/* ---- FreeRTOS ---------------------------------------------------------------------------- */
typedef int32_t BaseType_t;
typedef uint32_t UBaseType_t;
typedef uint32_t TickType_t;
#define pdFALSE 0
#define pdTRUE 1
#define pdPASS 1
#define pdFAIL 0
#define portMAX_DELAY 0xffffffffu
#define portYIELD_FROM_ISR(x) ((void)(x))

typedef struct shim_task *TaskHandle_t;
typedef void *QueueHandle_t;
typedef struct shim_timer *TimerHandle_t;
typedef TimerHandle_t xTimerHandle;
/* Real FreeRTOS takes TimerHandle_t; the library's callbacks are declared with TimerHandle_t *, and a
   mismatched indirect call trips UBSan's function check, so the typedef matches the callbacks. */
typedef void (*TimerCallbackFunction_t)(TimerHandle_t *);

typedef enum {
    eNoAction = 0,
    eSetBits,
    eIncrement,
    eSetValueWithOverwrite,
    eSetValueWithoutOverwrite
} eNotifyAction;

BaseType_t xTaskNotify(TaskHandle_t task, uint32_t value, eNotifyAction action);
BaseType_t xTaskNotifyFromISR(TaskHandle_t task, uint32_t value, eNotifyAction action, BaseType_t *woken);
uint32_t ulTaskNotifyTake(BaseType_t clearOnExit, TickType_t ticks);

BaseType_t xQueueSendToBack(QueueHandle_t q, const void *item, TickType_t ticks);
BaseType_t xQueueSendToFront(QueueHandle_t q, const void *item, TickType_t ticks);
BaseType_t xQueueReset(QueueHandle_t q);
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t ticks);

TimerHandle_t xTimerCreate(const char *name, TickType_t period, UBaseType_t reload, void *id,
                           TimerCallbackFunction_t cb);
BaseType_t xTimerStop(TimerHandle_t t, TickType_t ticks);
BaseType_t xTimerReset(TimerHandle_t t, TickType_t ticks);
BaseType_t xTimerResetFromISR(TimerHandle_t t, BaseType_t *woken);

BaseType_t xSemaphoreTake(void *sem, TickType_t ticks);
BaseType_t xSemaphoreGive(void *sem);

/* ---- CMSIS-RTOS2 ------------------------------------------------------------------------- */
typedef enum { osPriorityNormal = 24 } osPriority_t;
typedef void *osThreadId_t;
typedef void *osMessageQueueId_t;
typedef void *osSemaphoreId_t;
typedef void (*osThreadFunc_t)(void *argument);
typedef struct { const char *name; osPriority_t priority; uint32_t stack_size; } osThreadAttr_t;
typedef struct { const char *name; } osMessageQueueAttr_t;
typedef struct { const char *name; } osSemaphoreAttr_t;

osThreadId_t osThreadNew(osThreadFunc_t fn, void *arg, const osThreadAttr_t *attr);
osThreadId_t osThreadGetId(void);
osMessageQueueId_t osMessageQueueNew(uint32_t count, uint32_t size, const osMessageQueueAttr_t *attr);
osSemaphoreId_t osSemaphoreNew(uint32_t max, uint32_t initial, const osSemaphoreAttr_t *attr);
int osDelay(uint32_t ticks);

/* ---- STM32 HAL (F4-style: status register is SR) ----------------------------------------- */
typedef struct { volatile uint32_t SR; volatile uint32_t ISR; } USART_TypeDef;
#define USART_SR_TC 0x40u
#define USART_ISR_TC 0x40u
typedef struct { uint32_t BaudRate; } UART_InitTypeDef;
typedef struct { USART_TypeDef *Instance; UART_InitTypeDef Init; } UART_HandleTypeDef;
typedef struct { int unused; } GPIO_TypeDef;
typedef enum { GPIO_PIN_RESET = 0, GPIO_PIN_SET = 1 } GPIO_PinState;
typedef enum { HAL_OK = 0, HAL_ERROR, HAL_BUSY, HAL_TIMEOUT } HAL_StatusTypeDef;
typedef enum { HAL_UART_STATE_READY = 0x20 } HAL_UART_StateTypeDef;

HAL_StatusTypeDef HAL_UART_Receive_IT(UART_HandleTypeDef *h, uint8_t *data, uint16_t n);
HAL_StatusTypeDef HAL_UART_Transmit_IT(UART_HandleTypeDef *h, uint8_t *data, uint16_t n);
HAL_StatusTypeDef HAL_UART_AbortReceive_IT(UART_HandleTypeDef *h);
HAL_UART_StateTypeDef HAL_UART_GetState(UART_HandleTypeDef *h);
void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState state);
HAL_StatusTypeDef HAL_HalfDuplex_EnableTransmitter(UART_HandleTypeDef *h);
HAL_StatusTypeDef HAL_HalfDuplex_EnableReceiver(UART_HandleTypeDef *h);

/* ---- lwIP netconn ------------------------------------------------------------------------ */
typedef int8_t err_t;
typedef uint8_t u8_t;
typedef uint16_t u16_t;
typedef enum {
    ERR_OK = 0, ERR_MEM = -1, ERR_BUF = -2, ERR_TIMEOUT = -3, ERR_RTE = -4, ERR_INPROGRESS = -5,
    ERR_VAL = -6, ERR_WOULDBLOCK = -7, ERR_USE = -8, ERR_ALREADY = -9, ERR_ISCONN = -10,
    ERR_CONN = -11, ERR_IF = -12, ERR_ABRT = -13, ERR_RST = -14, ERR_CLSD = -15, ERR_ARG = -16
} err_enum_t;
typedef struct { uint32_t addr; } ip_addr_t;
enum netconn_type { NETCONN_TCP = 0x10 };
#define NETCONN_COPY 0x01
struct netconn { int unused; };
struct netbuf { uint8_t *data; uint16_t len; };
struct netvector { const void *ptr; size_t len; };

struct netconn *netconn_new(enum netconn_type t);
err_t netconn_bind(struct netconn *c, const ip_addr_t *addr, u16_t port);
err_t netconn_listen(struct netconn *c);
err_t netconn_connect(struct netconn *c, const ip_addr_t *addr, u16_t port);
err_t netconn_accept(struct netconn *c, struct netconn **out);
err_t netconn_recv(struct netconn *c, struct netbuf **out);
err_t netconn_err(struct netconn *c);
err_t netconn_close(struct netconn *c);
err_t netconn_delete(struct netconn *c);
void netconn_set_recvtimeout(struct netconn *c, int32_t ms);
void netconn_set_sendtimeout(struct netconn *c, int32_t ms);
err_t netconn_write_vectors_partly(struct netconn *c, struct netvector *vec, u16_t n, u8_t flags, size_t *written);
err_t netbuf_data(struct netbuf *b, void **data, u16_t *len);
void netbuf_delete(struct netbuf *b);

/* ---- harness control --------------------------------------------------------------------- */
/* Let the worker task run until it blocks again. */
void shim_task_run(void);
/* Invoke the library's callback for a timer (its expiry). */
void shim_timer_fire(TimerHandle_t t);
/* Drop pending notifications and any pending/queued input from an earlier iteration. */
void shim_reset(void);
/* The harness thread's notification state: set once the master task reports a result. */
bool shim_main_notified(uint32_t *value);
/* Bytes handed to HAL_UART_Transmit_IT / netconn_write_vectors_partly since shim_reset(). */
const uint8_t *shim_tx_data(size_t *len);
void shim_tx_clear(void);
/* What the next netconn_recv() delivers: a copy of data, or the error `err` when err != ERR_OK. */
void shim_net_set_rx(const uint8_t *data, size_t len, err_t err);
/* One-shot failures of the next netconn_accept()/netconn_connect()/netconn_write_vectors_partly(). */
void shim_net_fail_accept(void);
void shim_net_fail_connect(void);
void shim_net_fail_write(void);
/* An empty netconn_recv() either parks the task (slave server loop) or times out (master client). */
void shim_net_recv_empty_parks(bool parks);

#endif
