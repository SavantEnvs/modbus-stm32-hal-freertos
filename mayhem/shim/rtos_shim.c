#include "rtos_shim.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

struct shim_task { bool notified; uint32_t value; };
struct shim_timer { TimerCallbackFunction_t cb; };
struct shim_queue { uint32_t cap, size, count; uint8_t items[1024]; };

#define MAX_TIMERS 8
#define MAX_QUEUES 4
#define MAX_CONNS 16
#define TX_CAP 2048
#define RX_CAP 1024

static struct shim_task main_task, worker_task;
static struct shim_task *current = &main_task;
static struct shim_timer timers[MAX_TIMERS];
static unsigned n_timers;
static struct shim_queue queues[MAX_QUEUES];
static unsigned n_queues;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
enum { TURN_MAIN, TURN_WORKER };
static int turn = TURN_MAIN;
static bool worker_created, worker_exit, worker_done;
static pthread_t worker_tid;
static osThreadFunc_t worker_fn;
static void *worker_arg;

static uint8_t tx_buf[TX_CAP];
static size_t tx_len;

static struct netconn conns[MAX_CONNS];
static unsigned n_conns;
static struct netbuf netbuf_cur;
static uint8_t netbuf_data_store[RX_CAP];
static bool rx_pending, rx_parks;
static uint8_t rx_data[RX_CAP];
static size_t rx_len;
static err_t rx_err;
static bool accept_fail, connect_fail, write_fail;

static void die(const char *msg)
{
    fprintf(stderr, "rtos_shim: %s\n", msg);
    abort();
}

/* Hand control to the harness thread and sleep until it runs the worker again. */
static void park(void)
{
    pthread_mutex_lock(&mu);
    turn = TURN_MAIN;
    current = &main_task;
    pthread_cond_broadcast(&cv);
    while (turn != TURN_WORKER)
        pthread_cond_wait(&cv, &mu);
    current = &worker_task;
    bool leave = worker_exit;
    pthread_mutex_unlock(&mu);
    if (leave)
        pthread_exit(NULL);
}

static void *worker_main(void *unused)
{
    (void)unused;
    pthread_mutex_lock(&mu);
    while (turn != TURN_WORKER)
        pthread_cond_wait(&cv, &mu);
    current = &worker_task;
    bool leave = worker_exit;
    pthread_mutex_unlock(&mu);
    if (!leave)
        worker_fn(worker_arg);
    pthread_mutex_lock(&mu);
    worker_done = true;
    turn = TURN_MAIN;
    current = &main_task;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    return NULL;
}

void shim_task_run(void)
{
    if (worker_fn == NULL || worker_done)
        return;
    if (!worker_created) {
        if (pthread_create(&worker_tid, NULL, worker_main, NULL) != 0)
            die("pthread_create failed");
        worker_created = true;
    }
    pthread_mutex_lock(&mu);
    turn = TURN_WORKER;
    pthread_cond_broadcast(&cv);
    while (turn != TURN_MAIN)
        pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);
}

void shim_reset(void)
{
    if (worker_created) {
        pthread_mutex_lock(&mu);
        worker_exit = true;
        turn = TURN_WORKER;
        pthread_cond_broadcast(&cv);
        pthread_mutex_unlock(&mu);
        pthread_join(worker_tid, NULL);
    }
    worker_created = worker_exit = worker_done = false;
    worker_fn = NULL;
    worker_arg = NULL;
    turn = TURN_MAIN;
    current = &main_task;
    memset(&main_task, 0, sizeof main_task);
    memset(&worker_task, 0, sizeof worker_task);
    n_timers = n_queues = n_conns = 0;
    tx_len = 0;
    rx_pending = rx_parks = false;
    accept_fail = connect_fail = write_fail = false;
}

bool shim_main_notified(uint32_t *value)
{
    if (value)
        *value = main_task.value;
    return main_task.notified;
}

const uint8_t *shim_tx_data(size_t *len)
{
    *len = tx_len;
    return tx_buf;
}

void shim_tx_clear(void) { tx_len = 0; }

void shim_timer_fire(TimerHandle_t t)
{
    if (t && t->cb)
        t->cb((TimerHandle_t *)t);
}

/* ---- FreeRTOS ---------------------------------------------------------------------------- */
static BaseType_t notify(TaskHandle_t task, uint32_t value, eNotifyAction action)
{
    if (task == NULL)
        return pdFAIL;
    switch (action) {
    case eSetValueWithOverwrite:
        task->value = value;
        break;
    case eSetBits:
        task->value |= value;
        break;
    case eIncrement:
        task->value++;
        break;
    case eSetValueWithoutOverwrite:
        if (task->notified)
            return pdFAIL;
        task->value = value;
        break;
    case eNoAction:
        break;
    }
    task->notified = true;
    return pdPASS;
}

BaseType_t xTaskNotify(TaskHandle_t task, uint32_t value, eNotifyAction action)
{
    return notify(task, value, action);
}

BaseType_t xTaskNotifyFromISR(TaskHandle_t task, uint32_t value, eNotifyAction action, BaseType_t *woken)
{
    if (woken)
        *woken = pdFALSE;
    return notify(task, value, action);
}

uint32_t ulTaskNotifyTake(BaseType_t clearOnExit, TickType_t ticks)
{
    struct shim_task *self = current;
    while (!self->notified) {
        if (ticks != portMAX_DELAY || self != &worker_task)
            return 0;
        park();
    }
    uint32_t v = self->value;
    if (clearOnExit) {
        self->value = 0;
        self->notified = false;
    } else {
        if (self->value)
            self->value--;
        self->notified = self->value != 0;
    }
    return v;
}

static BaseType_t queue_send(QueueHandle_t h, const void *item, bool front)
{
    struct shim_queue *q = h;
    if (q->count == q->cap)
        return pdFALSE;
    if (front)
        memmove(q->items + q->size, q->items, (size_t)q->count * q->size);
    memcpy(q->items + (front ? 0 : (size_t)q->count * q->size), item, q->size);
    q->count++;
    return pdTRUE;
}

BaseType_t xQueueSendToBack(QueueHandle_t q, const void *item, TickType_t ticks)
{
    (void)ticks;
    return queue_send(q, item, false);
}

BaseType_t xQueueSendToFront(QueueHandle_t q, const void *item, TickType_t ticks)
{
    (void)ticks;
    return queue_send(q, item, true);
}

BaseType_t xQueueReset(QueueHandle_t h)
{
    ((struct shim_queue *)h)->count = 0;
    return pdPASS;
}

BaseType_t xQueueReceive(QueueHandle_t h, void *item, TickType_t ticks)
{
    struct shim_queue *q = h;
    while (q->count == 0) {
        if (ticks != portMAX_DELAY || current != &worker_task)
            return pdFALSE;
        park();
    }
    memcpy(item, q->items, q->size);
    q->count--;
    memmove(q->items, q->items + q->size, (size_t)q->count * q->size);
    return pdTRUE;
}

TimerHandle_t xTimerCreate(const char *name, TickType_t period, UBaseType_t reload, void *id,
                           TimerCallbackFunction_t cb)
{
    (void)name; (void)period; (void)reload; (void)id;
    if (n_timers == MAX_TIMERS)
        die("timer pool exhausted");
    struct shim_timer *t = &timers[n_timers++];
    t->cb = cb;
    return t;
}

BaseType_t xTimerStop(TimerHandle_t t, TickType_t ticks) { (void)t; (void)ticks; return pdPASS; }
BaseType_t xTimerReset(TimerHandle_t t, TickType_t ticks) { (void)t; (void)ticks; return pdPASS; }
BaseType_t xTimerResetFromISR(TimerHandle_t t, BaseType_t *woken)
{
    (void)t;
    if (woken)
        *woken = pdFALSE;
    return pdPASS;
}

BaseType_t xSemaphoreTake(void *sem, TickType_t ticks) { (void)sem; (void)ticks; return pdTRUE; }
BaseType_t xSemaphoreGive(void *sem) { (void)sem; return pdTRUE; }

/* ---- CMSIS-RTOS2 ------------------------------------------------------------------------- */
osThreadId_t osThreadNew(osThreadFunc_t fn, void *arg, const osThreadAttr_t *attr)
{
    (void)attr;
    if (worker_fn != NULL)
        die("only one task per harness iteration is supported");
    worker_fn = fn;
    worker_arg = arg;
    return &worker_task;
}

osThreadId_t osThreadGetId(void) { return current; }

osMessageQueueId_t osMessageQueueNew(uint32_t count, uint32_t size, const osMessageQueueAttr_t *attr)
{
    (void)attr;
    if (n_queues == MAX_QUEUES || (size_t)count * size > sizeof queues[0].items)
        die("queue pool exhausted");
    struct shim_queue *q = &queues[n_queues++];
    q->cap = count;
    q->size = size;
    q->count = 0;
    return q;
}

osSemaphoreId_t osSemaphoreNew(uint32_t max, uint32_t initial, const osSemaphoreAttr_t *attr)
{
    (void)max; (void)initial; (void)attr;
    static int token;
    return &token;
}

int osDelay(uint32_t ticks) { (void)ticks; return 0; }

/* ---- STM32 HAL --------------------------------------------------------------------------- */
HAL_StatusTypeDef HAL_UART_Receive_IT(UART_HandleTypeDef *h, uint8_t *data, uint16_t n)
{
    (void)h; (void)data; (void)n;
    return HAL_OK;
}

extern void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart);

HAL_StatusTypeDef HAL_UART_Transmit_IT(UART_HandleTypeDef *h, uint8_t *data, uint16_t n)
{
    if (tx_len + n > TX_CAP)
        tx_len = 0;
    memcpy(tx_buf + tx_len, data, n);
    tx_len += n;
    h->Instance->SR = USART_SR_TC;
    h->Instance->ISR = USART_ISR_TC;
    HAL_UART_TxCpltCallback(h);
    return HAL_OK;
}

HAL_StatusTypeDef HAL_UART_AbortReceive_IT(UART_HandleTypeDef *h) { (void)h; return HAL_OK; }
HAL_UART_StateTypeDef HAL_UART_GetState(UART_HandleTypeDef *h) { (void)h; return HAL_UART_STATE_READY; }
void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState state) { (void)port; (void)pin; (void)state; }
HAL_StatusTypeDef HAL_HalfDuplex_EnableTransmitter(UART_HandleTypeDef *h) { (void)h; return HAL_OK; }
HAL_StatusTypeDef HAL_HalfDuplex_EnableReceiver(UART_HandleTypeDef *h) { (void)h; return HAL_OK; }

/* ---- lwIP netconn ------------------------------------------------------------------------ */
struct netconn *netconn_new(enum netconn_type t)
{
    (void)t;
    if (n_conns == MAX_CONNS)
        die("netconn pool exhausted");
    return &conns[n_conns++];
}

err_t netconn_bind(struct netconn *c, const ip_addr_t *addr, u16_t port)
{
    (void)c; (void)addr; (void)port;
    return ERR_OK;
}

err_t netconn_listen(struct netconn *c) { (void)c; return ERR_OK; }

err_t netconn_connect(struct netconn *c, const ip_addr_t *addr, u16_t port)
{
    (void)c; (void)addr; (void)port;
    if (connect_fail) {
        connect_fail = false;
        return ERR_RTE;
    }
    return ERR_OK;
}

err_t netconn_accept(struct netconn *c, struct netconn **out)
{
    (void)c;
    if (accept_fail) {
        accept_fail = false;
        return ERR_TIMEOUT;
    }
    *out = netconn_new(NETCONN_TCP);
    return ERR_OK;
}

err_t netconn_recv(struct netconn *c, struct netbuf **out)
{
    (void)c;
    while (!rx_pending) {
        if (!rx_parks || current != &worker_task)
            return ERR_TIMEOUT;
        park();
    }
    rx_pending = false;
    if (rx_err != ERR_OK)
        return rx_err;
    memcpy(netbuf_data_store, rx_data, rx_len);
    netbuf_cur.data = netbuf_data_store;
    netbuf_cur.len = (uint16_t)rx_len;
    *out = &netbuf_cur;
    return ERR_OK;
}

err_t netconn_err(struct netconn *c) { (void)c; return ERR_OK; }
err_t netconn_close(struct netconn *c) { (void)c; return ERR_OK; }

err_t netconn_delete(struct netconn *c)
{
    (void)c;
    if (n_conns)
        n_conns--;
    return ERR_OK;
}

void netconn_set_recvtimeout(struct netconn *c, int32_t ms) { (void)c; (void)ms; }
void netconn_set_sendtimeout(struct netconn *c, int32_t ms) { (void)c; (void)ms; }

err_t netconn_write_vectors_partly(struct netconn *c, struct netvector *vec, u16_t n, u8_t flags, size_t *written)
{
    (void)c; (void)flags;
    if (write_fail) {
        write_fail = false;
        return ERR_RTE;
    }
    size_t total = 0;
    for (u16_t i = 0; i < n; i++) {
        if (tx_len + vec[i].len > TX_CAP)
            tx_len = 0;
        memcpy(tx_buf + tx_len, vec[i].ptr, vec[i].len);
        tx_len += vec[i].len;
        total += vec[i].len;
    }
    if (written)
        *written = total;
    return ERR_OK;
}

err_t netbuf_data(struct netbuf *b, void **data, u16_t *len)
{
    *data = b->data;
    *len = b->len;
    return ERR_OK;
}

void netbuf_delete(struct netbuf *b) { (void)b; }

void shim_net_set_rx(const uint8_t *data, size_t len, err_t err)
{
    if (len > RX_CAP)
        len = RX_CAP;
    if (len)
        memcpy(rx_data, data, len);
    rx_len = len;
    rx_err = err;
    rx_pending = true;
}

void shim_net_fail_accept(void) { accept_fail = true; }
void shim_net_fail_connect(void) { connect_fail = true; }
void shim_net_fail_write(void) { write_fail = true; }
void shim_net_recv_empty_parks(bool parks) { rx_parks = parks; }
