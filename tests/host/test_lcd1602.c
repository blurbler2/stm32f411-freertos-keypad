/*
 * Host unit test for the lcd1602 driver (no hardware needed).
 *
 * White-box test: the driver source is included, so the test can call the
 * static functions of the gatekeeper task (display_init, apply, refresh, ...)
 * step by step instead of running the endless task loop. FreeRTOS is replaced
 * by the mocks in rtos_mock/, the HAL by main.h and the functions below.
 *
 * Every byte that would go over I2C is fed into an HD44780 simulator behind a
 * simulated PCF8574 (as in test_lcd.c). The simulator also checks the timing
 * diagram of the lecture (slide 10): RS valid before E rises (t_AS), data and
 * RS still unchanged when E falls (t_H, t_AH).
 *
 * Build and run:  make -C tests/host
 */
#include "../../Core/Src/lcd1602.c"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Set by test_lcd1602_hook.c, which replaces the weak lcd1602_ready_changed() */
extern int  hook_calls;
extern bool hook_last;

#define SIM_ADDR   0x27u
#define PIN_RS     0x01u
#define PIN_RW     0x02u
#define PIN_E      0x04u
#define PIN_LIGHT  0x08u
#define HOLD_MASK  0xF3u        /* D7..D4, R/W, RS: must not change when E falls */

/* ---------------- HD44780 + PCF8574 simulator ---------------- */

static struct {
    uint8_t port;               /* last byte on the PCF8574 outputs */
    int     four_bit, have_high;
    uint8_t high;
    int     two_line, display_on, cursor_on, blink_on, increment;
    uint8_t addr;
    uint8_t ddram[0x80];
    int     func_8bit_count;    /* number of "8-bit mode" instructions */
} sim;

static void sim_reset(void)
{
    memset(&sim, 0, sizeof(sim));
    memset(sim.ddram, ' ', sizeof(sim.ddram));
}

static void sim_execute(uint8_t value, int rs)
{
    if (rs) {                                   /* data write */
        sim.ddram[sim.addr & 0x7Fu] = value;
        sim.addr = (uint8_t)((sim.addr + 1u) & 0x7Fu);
        return;
    }
    if (value & 0x80u)      { sim.addr = value & 0x7Fu; }
    else if (value & 0x40u) { assert(0); }       /* CGRAM: not used by lcd1602 */
    else if (value & 0x20u) {
        if (value & 0x10u)  { sim.four_bit = 0; sim.func_8bit_count++; }
        else                { sim.four_bit = 1; }
        sim.two_line = (value & 0x08u) != 0;
    }
    else if (value & 0x08u) {
        sim.display_on = (value & 0x04u) != 0;
        sim.cursor_on  = (value & 0x02u) != 0;
        sim.blink_on   = (value & 0x01u) != 0;
    }
    else if (value & 0x04u) { sim.increment = (value & 0x02u) != 0; }
    else if (value & 0x02u) { sim.addr = 0; }
    else if (value & 0x01u) { memset(sim.ddram, ' ', sizeof(sim.ddram)); sim.addr = 0; }
}

/* One I2C byte = new state of the 8 PCF8574 outputs (90 us at 100 kHz) */
static void sim_port_write(uint8_t value)
{
    const uint8_t old     = sim.port;
    const int     rising  = !(old & PIN_E) && (value & PIN_E);
    const int     falling = (old & PIN_E) && !(value & PIN_E);

    /* t_AS: RS and R/W are already valid one byte (90 us) before E rises */
    if (rising) {
        assert((old & (PIN_RS | PIN_RW)) == (value & (PIN_RS | PIN_RW)));
    }
    /* t_H, t_AH: data, RS and R/W are unchanged in the byte where E falls */
    if (falling) {
        assert((old & HOLD_MASK) == (value & HOLD_MASK));
    }
    sim.port = value;
    if (!falling) {
        return;
    }
    assert((old & PIN_RW) == 0);                /* the driver only writes */

    const int     rs     = old & PIN_RS;
    const uint8_t nibble = old & 0xF0u;

    if (!sim.four_bit) {
        sim_execute(nibble, rs);                /* D3..D0 are not connected */
        if (sim.four_bit) { sim.have_high = 0; }
    } else if (!sim.have_high) {
        sim.high = nibble; sim.have_high = 1;
    } else {
        sim.have_high = 0;
        sim_execute((uint8_t)(sim.high | (nibble >> 4)), rs);
    }
}

/* ---------------- HAL mocks ---------------- */

I2C_HandleTypeDef        hi2c1     = { .id = 1, .hdmatx = NULL };
static I2C_HandleTypeDef other_i2c = { .id = 2, .hdmatx = NULL };
static int               dummy_dma;

static uint32_t   nvic_enabled[64], nvic_prio[64];
static int        i2c_fail;                     /* 1 = no ACK */
static int        dma_hang;                     /* 1 = the DMA interrupt never comes */
static int        transfers, dma_transfers, recovers, writes;
static uint32_t   bytes, delay_total, delay_before_first_write;
static TickType_t tick;

static void count_reset(void)
{
    transfers = 0; dma_transfers = 0; bytes = 0;
}

static void feed(const uint8_t *data, uint16_t size)
{
    assert(s_bus_mutex->count == 0);            /* every transfer holds the bus mutex */
    if (writes++ == 0) { delay_before_first_write = delay_total; }
    transfers++;
    bytes += size;
    for (uint16_t i = 0; i < size; i++) { sim_port_write(data[i]); }
}

HAL_StatusTypeDef HAL_I2C_Master_Transmit(I2C_HandleTypeDef *hi2c, uint16_t dev_address,
                                          uint8_t *data, uint16_t size, uint32_t timeout)
{
    assert(hi2c == &hi2c1 && timeout > 0);
    if (i2c_fail || dev_address != (SIM_ADDR << 1)) { return HAL_ERROR; }
    feed(data, size);
    return HAL_OK;
}

HAL_StatusTypeDef HAL_I2C_Master_Transmit_DMA(I2C_HandleTypeDef *hi2c, uint16_t dev_address,
                                              uint8_t *data, uint16_t size)
{
    assert(hi2c == &hi2c1 && s_tx_active);      /* the driver waits for this transfer */
    if (dev_address != (SIM_ADDR << 1)) { return HAL_ERROR; }
    dma_transfers++;
    if (dma_hang) {
        return HAL_OK;                          /* started, but no interrupt follows */
    }
    if (i2c_fail) {
        lcd1602_i2c_error_isr(hi2c);            /* NACK, reported by the ER interrupt */
        return HAL_OK;
    }
    feed(data, size);
    lcd1602_i2c_tx_done_isr(hi2c);              /* "transfer complete" interrupt */
    return HAL_OK;
}

HAL_StatusTypeDef HAL_I2C_IsDeviceReady(I2C_HandleTypeDef *hi2c, uint16_t dev_address,
                                        uint32_t trials, uint32_t timeout)
{
    (void)trials; (void)timeout;
    assert(hi2c == &hi2c1 && s_bus_mutex->count == 0);
    return (!i2c_fail && dev_address == (SIM_ADDR << 1)) ? HAL_OK : HAL_ERROR;
}

HAL_StatusTypeDef HAL_I2C_DeInit(I2C_HandleTypeDef *hi2c) { assert(hi2c == &hi2c1); return HAL_OK; }
HAL_StatusTypeDef HAL_I2C_Init(I2C_HandleTypeDef *hi2c)   { assert(hi2c == &hi2c1); recovers++; return HAL_OK; }

/* The driver must never busy-wait with HAL_Delay() (slide 27) */
void HAL_Delay(uint32_t ms) { (void)ms; assert(0); }

uint32_t NVIC_GetEnableIRQ(IRQn_Type irq) { return nvic_enabled[irq]; }
uint32_t NVIC_GetPriority(IRQn_Type irq)  { return nvic_prio[irq]; }

/* ---------------- FreeRTOS mocks (1 tick = 1 ms) ---------------- */

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *b)  { b->count = 1; return b; }
SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *b) { b->count = 0; return b; }

BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks)
{
    if (sem->count > 0) { sem->count = 0; return pdTRUE; }
    tick += ticks;                              /* nobody gives it: the timeout passes */
    return pdFALSE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t sem)
{
    if (sem == s_bus_mutex) { assert(sem->count == 0); }   /* released only once */
    sem->count = 1;
    return pdTRUE;
}

BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t sem, BaseType_t *woken)
{
    sem->count = 1;
    *woken = pdTRUE;
    return pdTRUE;
}

QueueHandle_t xQueueCreateStatic(UBaseType_t length, UBaseType_t item_size,
                                 uint8_t *storage, StaticQueue_t *q)
{
    q->storage = storage; q->item_size = item_size; q->length = length;
    q->head = 0; q->count = 0;
    return q;
}

BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t ticks)
{
    assert(ticks == 0);                         /* senders never wait (slide 29) */
    if (q->count == q->length) { return pdFALSE; }
    memcpy(&q->storage[((q->head + q->count) % q->length) * q->item_size], item, q->item_size);
    q->count++;
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t ticks)
{
    if (q->count == 0) { tick += ticks; return pdFALSE; }
    memcpy(item, &q->storage[q->head * q->item_size], q->item_size);
    q->head = (q->head + 1u) % q->length;
    q->count--;
    return pdTRUE;
}

static TaskFunction_t created_task;

TaskHandle_t xTaskCreateStatic(TaskFunction_t code, const char *name, uint32_t stack_depth,
                               void *param, UBaseType_t priority, StackType_t *stack,
                               StaticTask_t *tcb)
{
    (void)name; (void)param; (void)stack;
    assert(stack_depth == LCD1602_TASK_STACK && priority == LCD1602_TASK_PRIORITY);
    created_task = code;
    return tcb;
}

void       vTaskDelay(TickType_t ticks) { tick += ticks; delay_total += ticks; }
TickType_t xTaskGetTickCount(void)      { return tick; }

/* ---------------- helpers ---------------- */

/** What the gatekeeper task does with the queue */
static void drain(void)
{
    lcd1602_msg_t msg;
    while (xQueueReceive(s_queue, &msg, 0) == pdTRUE) {
        handle_status(apply(&msg));
    }
}

/** The whole row must be spaces except text at col */
static void expect_row(uint8_t row, uint8_t col, const char *text)
{
    char want[LCD1602_COLS + 1];
    char got[LCD1602_COLS + 1];

    memset(want, ' ', LCD1602_COLS);
    memcpy(&want[col], text, strlen(text));
    want[LCD1602_COLS] = '\0';
    memcpy(got, &sim.ddram[row_offset[row]], LCD1602_COLS);
    got[LCD1602_COLS] = '\0';
    if (strcmp(got, want) != 0) {
        printf("row %u is \"%s\", expected \"%s\"\n", (unsigned)row, got, want);
        assert(0);
    }
}

int main(void)
{
    /* 1) DMA only when CubeMX has set it up, otherwise blocking with a reason */
    assert(strcmp(port_dma_problem(), "no DMA channel for I2C TX") == 0);
    hi2c1.hdmatx = &dummy_dma;
    assert(strcmp(port_dma_problem(), "I2C event/error interrupt off") == 0);
    nvic_enabled[I2C1_EV_IRQn] = 1; nvic_enabled[I2C1_ER_IRQn] = 1;
    nvic_prio[I2C1_EV_IRQn] = 4;    nvic_prio[I2C1_ER_IRQn] = 4;
    assert(strcmp(port_dma_problem(), "I2C interrupt priority < 5") == 0);
    nvic_prio[I2C1_EV_IRQn] = 5;    nvic_prio[I2C1_ER_IRQn] = 5;
    assert(port_dma_problem() == NULL);

    /* 2) start */
    assert(lcd1602_write_line(0, "x") == LCD1602_ERR_RTOS);     /* not started yet */
    assert(lcd1602_start(NULL) == LCD1602_OK);
    assert(created_task == lcd1602_task);
    assert(lcd1602_dma_active() && lcd1602_dma_problem() == NULL);
    assert(lcd1602_bus_mutex() == s_bus_mutex && s_bus_mutex->count == 1);
    assert(lcd1602_start(NULL) == LCD1602_OK);                  /* 2nd call: no effect */

    /* 3) parameter checks; senders never wait, a full queue drops the message */
    assert(lcd1602_print(LCD1602_ROWS, 0, "x") == LCD1602_ERR_PARAM);
    assert(lcd1602_print(0, LCD1602_COLS, "x") == LCD1602_ERR_PARAM);
    assert(lcd1602_print(0, 0, NULL) == LCD1602_ERR_PARAM);
    assert(lcd1602_write_line(LCD1602_ROWS, "x") == LCD1602_ERR_PARAM);
    for (unsigned i = 0; i < LCD1602_QUEUE_LENGTH; i++) {
        assert(lcd1602_clear() == LCD1602_OK);
    }
    assert(lcd1602_clear() == LCD1602_ERR_QUEUE_FULL);
    drain();

    /* 4) init sequence (over DMA) */
    sim_reset();
    assert(display_init() == LCD1602_OK);
    assert(delay_before_first_write >= 40);                     /* power-up wait */
    assert(sim.func_8bit_count == 3);                           /* 0x28, not 0x38 */
    assert(sim.four_bit && sim.two_line && sim.display_on && sim.increment);
    assert(!sim.cursor_on && !sim.blink_on);
    assert(sim.port & PIN_LIGHT);
    assert(dma_transfers > 0 && s_bus_mutex->count == 1);       /* mutex free again */
    set_ready(true);
    assert(lcd1602_is_ready() && hook_calls == 1 && hook_last);

    /* 5) one transfer per changed row, 20x4 row addresses, clipping */
    assert(lcd1602_write_line(0, "Hello") == LCD1602_OK);
    assert(lcd1602_print(1, 18, "abcdef") == LCD1602_OK);       /* only "ab" fits */
    assert(lcd1602_write_line(2, "third row") == LCD1602_OK);
    assert(lcd1602_write_line(3, "0123456789ABCDEFGHIJKLMN") == LCD1602_OK);
    drain();
    count_reset();
    assert(refresh() == LCD1602_OK);
    assert(dma_transfers == 4 && transfers == 4);
    assert(bytes == (1 + 5 + 1 + 2 + 1 + 9 + 1 + 20) * BYTES_PER_LCD_BYTE);
    expect_row(0, 0, "Hello");
    expect_row(1, 18, "ab");
    expect_row(2, 0, "third row");                              /* DDRAM 0x14 */
    expect_row(3, 0, "0123456789ABCDEFGHIJ");                   /* DDRAM 0x54 */

    /* 6) only the changed characters are sent */
    assert(lcd1602_write_line(0, "Help!") == LCD1602_OK);       /* "lo" -> "p!" */
    drain();
    count_reset();
    assert(refresh() == LCD1602_OK);
    assert(transfers == 1 && bytes == (1 + 2) * BYTES_PER_LCD_BYTE);
    expect_row(0, 0, "Help!");

    /* 7) nothing changed -> nothing on the bus */
    count_reset();
    assert(refresh() == LCD1602_OK && transfers == 0);

    /* 8) write_line clears the rest of the row, clear() everything */
    assert(lcd1602_write_line(2, "x") == LCD1602_OK);
    drain();
    assert(refresh() == LCD1602_OK);
    expect_row(2, 0, "x");
    assert(lcd1602_clear() == LCD1602_OK);
    assert(lcd1602_print(3, 5, "abc") == LCD1602_OK);
    drain();
    assert(refresh() == LCD1602_OK);
    expect_row(0, 0, "");
    expect_row(1, 0, "");
    expect_row(2, 0, "");
    expect_row(3, 5, "abc");

    /* 9) "done" from another I2C peripheral is ignored */
    s_tx_active = true;
    lcd1602_i2c_tx_done_isr(&other_i2c);
    assert(s_tx_done->count == 0 && s_tx_active);
    s_tx_active = false;

    /* 10) DMA interrupt never comes -> timeout, I2C reset, mutex free, not ready */
    dma_hang = 1;
    assert(lcd1602_write_line(0, "after error") == LCD1602_OK);
    drain();
    TickType_t before = tick;
    assert(refresh() == LCD1602_ERR_TIMEOUT);
    assert(tick - before >= LCD1602_BUS_TIMEOUT_MS);
    assert(recovers == 1 && s_bus_mutex->count == 1);
    handle_status(LCD1602_ERR_TIMEOUT);
    assert(!lcd1602_is_ready() && hook_calls == 2 && !hook_last);
    dma_hang = 0;

    /* 11) after a new init the whole frame is sent again */
    sim_reset();                                                /* display lost everything */
    assert(display_init() == LCD1602_OK);
    set_ready(true);
    assert(hook_calls == 3 && hook_last);
    assert(refresh() == LCD1602_OK);
    expect_row(0, 0, "after error");
    expect_row(3, 5, "abc");

    /* 12) NACK reported by the error interrupt: error, no reset, sent again later */
    i2c_fail = 1;
    assert(lcd1602_write_line(1, "nack") == LCD1602_OK);
    drain();
    assert(refresh() == LCD1602_ERR_I2C && recovers == 1);
    i2c_fail = 0;
    assert(refresh() == LCD1602_OK);
    expect_row(1, 0, "nack");

    /* 13) blocking fallback gives the same result without DMA */
    s_use_dma = false;
    count_reset();
    assert(lcd1602_write_line(1, "blocking") == LCD1602_OK);
    drain();
    assert(refresh() == LCD1602_OK);
    assert(transfers == 1 && dma_transfers == 0);
    expect_row(1, 0, "blocking");

    /* 14) backlight off stays off for the following characters */
    assert(lcd1602_backlight(false) == LCD1602_OK);
    drain();
    assert(!(sim.port & PIN_LIGHT));
    assert(lcd1602_write_line(1, "dark") == LCD1602_OK);
    drain();
    assert(refresh() == LCD1602_OK && !(sim.port & PIN_LIGHT));
    expect_row(1, 0, "dark");
    assert(lcd1602_backlight(true) == LCD1602_OK);
    drain();
    assert(sim.port & PIN_LIGHT);

    puts("lcd1602: all tests passed");
    return 0;
}
