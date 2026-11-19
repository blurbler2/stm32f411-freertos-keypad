/**
 * @file    lcd1602.c
 * @brief   HD44780 (20x4) over PCF8574T with FreeRTOS, I2C mutex, DMA and a
 *          gatekeeper task. See lcd1602.h for the overview and the CubeMX settings.
 */
#include "lcd1602.h"

#include <string.h>
#include "task.h"
#include "queue.h"

/* HD44780 instructions */
#define CMD_CLEAR           0x01u   /* 1.52 ms execution time */
#define CMD_ENTRY_INC       0x06u   /* cursor moves right, no display shift */
#define CMD_DISPLAY_OFF     0x08u
#define CMD_DISPLAY_ON      0x0Cu   /* display on, cursor off, blink off */
#define CMD_FUNCTION_4BIT   0x28u   /* 4-bit bus, "2 lines" (= 4 rows), 5x8 font */
#define CMD_SET_DDRAM       0x80u   /* cursor position: 0x80 | address */

/* One HD44780 byte = 5 PCF8574 bytes, see encode_byte() */
#define BYTES_PER_LCD_BYTE  5u
/* Largest transfer: cursor command + one full row */
#define TXBUF_SIZE          ((1u + LCD1602_COLS) * BYTES_PER_LCD_BYTE)

#define STR_(x)             #x
#define STR(x)              STR_(x)

static const uint8_t row_offset[LCD1602_ROWS] = LCD1602_ROW_OFFSETS;

/* RTOS objects - all statically allocated, no heap needed */
static SemaphoreHandle_t s_bus_mutex;          /* protects the I2C bus (may be shared) */
static SemaphoreHandle_t s_tx_done;            /* given by the I2C/DMA interrupt       */
static QueueHandle_t     s_queue;              /* messages from the application tasks  */
static bool              s_started;

static StaticSemaphore_t s_bus_mutex_buf;
static StaticSemaphore_t s_tx_done_buf;
static StaticQueue_t     s_queue_buf;
static StaticTask_t      s_task_tcb;
static StackType_t       s_task_stack[LCD1602_TASK_STACK];

/* Transfer mode, decided once in lcd1602_start() */
static bool        s_use_dma;
static const char *s_dma_problem = "driver not started";

/* Transfer state shared with the interrupt */
static volatile bool s_tx_active;              /* a transfer of THIS driver is running */
static volatile bool s_tx_error;

/* Owned by the gatekeeper task only */
static char       s_frame[LCD1602_ROWS][LCD1602_COLS];  /* "Soll-Bild": what should be shown */
static char       s_shown[LCD1602_ROWS][LCD1602_COLS];  /* "Ist-Bild": what is on the glass  */
static uint8_t    s_txbuf[TXBUF_SIZE];                  /* DMA reads from here: must stay
                                                           valid until the transfer is done */
static uint8_t    s_backlight = LCD1602_PIN_BACKLIGHT;
static TickType_t s_next_init;                          /* next init attempt while not ready */
static volatile bool s_ready;

/* Messages to the gatekeeper task (24 bytes for 20 columns, slide 29) */
typedef enum { MSG_TEXT, MSG_LINE, MSG_CLEAR, MSG_BACKLIGHT } msg_type_t;

typedef struct {
    uint8_t type;   /* msg_type_t                    */
    uint8_t row;
    uint8_t col;
    uint8_t len;    /* number of valid bytes in text */
    char    text[LCD1602_COLS];   /* not 0-terminated */
} lcd1602_msg_t;

static uint8_t s_queue_storage[LCD1602_QUEUE_LENGTH * sizeof(lcd1602_msg_t)];

/* ========================================================================= */
/*  SECTION 1: PORT - the only functions that use the ST HAL                  */
/*  (to port the driver to another MCU, only this section and section 1 of    */
/*  lcd1602.h have to change)                                                 */
/* ========================================================================= */

static lcd1602_status_t hal_status(HAL_StatusTypeDef st)
{
    switch (st) {
    case HAL_OK:    return LCD1602_OK;
    case HAL_ERROR: return LCD1602_ERR_I2C;       /* e.g. no ACK                  */
    default:        return LCD1602_ERR_TIMEOUT;   /* HAL_BUSY, HAL_TIMEOUT: stuck */
    }
}

/** Does the PCF8574 answer with ACK? (short blocking poll, < 1 ms) */
static bool port_probe(void)
{
    return HAL_I2C_IsDeviceReady(LCD1602_I2C_HANDLE,
                                 (uint16_t)(LCD1602_I2C_ADDR << 1), 2, 5) == HAL_OK;
}

/** Checks the CubeMX settings the HAL needs for DMA transfers.
 *  @return NULL if DMA can be used, otherwise the reason why not */
static const char *port_dma_problem(void)
{
#if LCD1602_USE_DMA
    const uint32_t min_prio = (uint32_t)configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY;

    if (LCD1602_I2C_HANDLE->hdmatx == NULL) {
        return "no DMA channel for I2C TX";
    }
    if (NVIC_GetEnableIRQ(LCD1602_I2C_EV_IRQn) == 0u ||
        NVIC_GetEnableIRQ(LCD1602_I2C_ER_IRQn) == 0u) {
        return "I2C event/error interrupt off";
    }
    if (NVIC_GetPriority(LCD1602_I2C_EV_IRQn) < min_prio ||
        NVIC_GetPriority(LCD1602_I2C_ER_IRQn) < min_prio) {
        return "I2C interrupt priority < " STR(configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY);
    }
    return NULL;
#else
    return "LCD1602_USE_DMA is 0";
#endif
}

/** Start a DMA transfer. Returns at once; the interrupt reports the end. */
static lcd1602_status_t port_start_dma(uint8_t *buf, uint16_t len)
{
#if LCD1602_USE_DMA
    return hal_status(HAL_I2C_Master_Transmit_DMA(LCD1602_I2C_HANDLE,
                                                  (uint16_t)(LCD1602_I2C_ADDR << 1), buf, len));
#else
    (void)buf;
    (void)len;
    return LCD1602_ERR_PARAM;          /* never called: s_use_dma is always false */
#endif
}

/** Blocking transfer: returns when all bytes are sent (the HAL polls the flags). */
static lcd1602_status_t port_transmit_blocking(uint8_t *buf, uint16_t len)
{
    return hal_status(HAL_I2C_Master_Transmit(LCD1602_I2C_HANDLE,
                                              (uint16_t)(LCD1602_I2C_ADDR << 1), buf, len,
                                              LCD1602_BUS_TIMEOUT_MS));
}

/** Reset the I2C peripheral after a transfer that never finished. */
static void port_recover(void)
{
    (void)HAL_I2C_DeInit(LCD1602_I2C_HANDLE);
    (void)HAL_I2C_Init(LCD1602_I2C_HANDLE);   /* MspInit links the DMA stream again */
}

/** Wait without blocking the CPU. "+ 1" because vTaskDelay(1) can be anything
 *  between 0 and 1 tick (slide 27). */
static void port_delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms) + 1u);
}

/* ========================================================================= */
/*  SECTION 2: HD44780 protocol over the PCF8574                              */
/* ========================================================================= */

/*
 * Write cycle from the HD44780 timing diagram ("LCD-Anzeigen schreiben", slide 10):
 *
 *            ___________________                ____________
 *   Data  --X___________________X--     RS  ___/            \___ (RS, R/W valid
 *               ________                                          around E)
 *   E     _____/        \______
 *          t_AS|<t_PWH >|t_H, t_AH
 *
 *   t_AS  >= 140 ns  RS, R/W valid before E rises
 *   t_PWH >= 450 ns  E high time
 *   t_DSW >= 195 ns  data valid before E falls
 *   t_H, t_AH >= 10 ns  data, RS, R/W held after E falls
 *
 * The PCF8574 sets all 8 outputs at once with every I2C byte it receives, so
 * every step of the diagram is one I2C byte. At 100 kHz one byte (8 bits + ACK)
 * takes 90 us, so every minimum time is exceeded by a factor of more than 200
 * and no extra delay is needed between the bytes (slide 26). The delay
 * function is only needed for the execution times of the controller
 * (Clear Display 1.52 ms, power-up and init sequence).
 *
 *   PCF8574 byte:  P7..P4 = D7..D4 | P3 = light | P2 = E | P1 = R/W (0) | P0 = RS
 *
 *   byte 1   E=0  high nibble   RS/R/W become valid, E still low   -> t_AS
 *   byte 2   E=1  high nibble   E rises; data valid during t_PWH   -> t_PWH, t_DSW
 *   byte 3   E=0  high nibble   E falls: controller takes the high nibble;
 *                               data and RS still unchanged        -> t_H, t_AH
 *   byte 4   E=1  low nibble    second E pulse (RS unchanged, so t_AS is met)
 *   byte 5   E=0  low nibble    E falls: controller takes the low nibble
 *
 * The 37 us execution time of a normal instruction has passed before the next
 * E pulse (at least one I2C byte = 90 us later).
 */
static uint16_t encode_byte(uint8_t *out, uint8_t value, uint8_t rs)
{
    const uint8_t ctrl = (uint8_t)(rs | s_backlight);           /* R/W = 0: write */
    const uint8_t hi   = (uint8_t)((value & 0xF0u) | ctrl);
    const uint8_t lo   = (uint8_t)(((uint8_t)(value << 4) & 0xF0u) | ctrl);

    out[0] = hi;                          /* t_AS: RS valid, E low      */
    out[1] = hi | LCD1602_PIN_E;          /* E rising edge              */
    out[2] = hi;                          /* E falling edge: latch D7-4 */
    out[3] = lo | LCD1602_PIN_E;          /* E rising edge              */
    out[4] = lo;                          /* E falling edge: latch D3-0 */
    return BYTES_PER_LCD_BYTE;
}

/** Only one nibble (upper 4 bits of value), used by the init sequence while
 *  the controller may still be in 8-bit mode. */
static uint16_t encode_nibble(uint8_t *out, uint8_t value)
{
    const uint8_t b = (uint8_t)((value & 0xF0u) | s_backlight);  /* RS = 0 */

    out[0] = b;
    out[1] = b | LCD1602_PIN_E;
    out[2] = b;
    return 3u;
}

/* ========================================================================= */
/*  SECTION 3: bus access - mutex + DMA + semaphore (slide 28)                */
/* ========================================================================= */

static void signal_from_isr(I2C_HandleTypeDef *hi2c, bool error)
{
    BaseType_t woken = pdFALSE;

    /* Ignore transfers of other drivers on the same I2C peripheral */
    if (hi2c != LCD1602_I2C_HANDLE || !s_tx_active) {
        return;
    }
    s_tx_active = false;
    s_tx_error  = error;
    (void)xSemaphoreGiveFromISR(s_tx_done, &woken);   /* step 5: "fertig"      */
    portYIELD_FROM_ISR(woken);                        /* run the LCD task now  */
}

void lcd1602_i2c_tx_done_isr(I2C_HandleTypeDef *hi2c)
{
    signal_from_isr(hi2c, false);
}

void lcd1602_i2c_error_isr(I2C_HandleTypeDef *hi2c)
{
    signal_from_isr(hi2c, true);
}

#if LCD1602_DEFINE_HAL_CALLBACKS
void HAL_I2C_MasterTxCpltCallback(I2C_HandleTypeDef *hi2c)
{
    lcd1602_i2c_tx_done_isr(hi2c);
}

void HAL_I2C_ErrorCallback(I2C_HandleTypeDef *hi2c)
{
    lcd1602_i2c_error_isr(hi2c);
}
#endif

/**
 * Steps 2-5 of slide 28: start the DMA transfer and sleep on the semaphore
 * until the interrupt reports the end. The CPU runs the other tasks in the
 * meantime - only the I2C bus stays occupied.
 */
static lcd1602_status_t dma_transfer(uint8_t *buf, uint16_t len)
{
    lcd1602_status_t st;

    (void)xSemaphoreTake(s_tx_done, 0);              /* drop a stale "done" */
    s_tx_error  = false;
    s_tx_active = true;

    st = port_start_dma(buf, len);                   /* step 2: returns at once */
    if (st != LCD1602_OK) {
        s_tx_active = false;
        return st;
    }
    /* step 3: block; steps 4 and 5 are done by the DMA controller and the IRQ */
    if (xSemaphoreTake(s_tx_done, pdMS_TO_TICKS(LCD1602_BUS_TIMEOUT_MS)) != pdTRUE) {
        s_tx_active = false;
        return LCD1602_ERR_TIMEOUT;
    }
    return s_tx_error ? LCD1602_ERR_I2C : LCD1602_OK;
}

/** Send one block of PCF8574 bytes while holding the bus mutex. */
static lcd1602_status_t bus_write(uint8_t *buf, uint16_t len)
{
    lcd1602_status_t st;

    /* step 1: take the bus */
    if (xSemaphoreTake(s_bus_mutex, pdMS_TO_TICKS(LCD1602_BUS_TIMEOUT_MS)) != pdTRUE) {
        return LCD1602_ERR_BUSY;
    }

    st = s_use_dma ? dma_transfer(buf, len)
                   : port_transmit_blocking(buf, len);   /* fallback: this task waits */
    if (st == LCD1602_ERR_TIMEOUT) {
        port_recover();                                   /* reset a stuck peripheral */
    }

    /* step 6: release the bus for the next task */
    (void)xSemaphoreGive(s_bus_mutex);
    return st;
}

static lcd1602_status_t bus_probe(void)
{
    bool ok;

    if (xSemaphoreTake(s_bus_mutex, pdMS_TO_TICKS(LCD1602_BUS_TIMEOUT_MS)) != pdTRUE) {
        return LCD1602_ERR_BUSY;
    }
    ok = port_probe();
    (void)xSemaphoreGive(s_bus_mutex);
    return ok ? LCD1602_OK : LCD1602_ERR_I2C;
}

static lcd1602_status_t send_nibble(uint8_t value, uint32_t wait_ms)
{
    lcd1602_status_t st = bus_write(s_txbuf, encode_nibble(s_txbuf, value));
    port_delay_ms(wait_ms);
    return st;
}

static lcd1602_status_t send_command(uint8_t cmd, uint32_t wait_ms)
{
    lcd1602_status_t st = bus_write(s_txbuf, encode_byte(s_txbuf, cmd, 0));
    if (wait_ms > 0u) {
        port_delay_ms(wait_ms);
    }
    return st;
}

/**
 * "Initialization by instruction" (HD44780 data sheet, figure 24). Runs in the
 * gatekeeper task, so the delays let the other tasks run.
 */
static lcd1602_status_t display_init(void)
{
    lcd1602_status_t st;

    port_delay_ms(50);                  /* > 40 ms after VDD rises (slide 21: 40-50 ms) */

    st = bus_probe();
    if (st != LCD1602_OK) {
        return st;
    }

    /* After power-up all PCF8574 outputs are high (also E). Set them low. */
    s_txbuf[0] = s_backlight;
    st = bus_write(s_txbuf, 1);
    if (st != LCD1602_OK) {
        return st;
    }

    /* 3 x "8-bit mode" brings the controller into a known state from any state,
     * then switch to 4-bit mode. Only single nibbles here. */
    if ((st = send_nibble(0x30u, 5)) != LCD1602_OK) { return st; }   /* wait > 4.1 ms */
    if ((st = send_nibble(0x30u, 1)) != LCD1602_OK) { return st; }   /* wait > 100 us */
    if ((st = send_nibble(0x30u, 1)) != LCD1602_OK) { return st; }
    if ((st = send_nibble(0x20u, 1)) != LCD1602_OK) { return st; }   /* 4-bit mode    */

    /* From here on every instruction is sent as two nibbles.
     * 0x28 = 4-bit, 2 lines. (The original driver sends 0x38 here, which sets
     * DL = 1 and switches the controller back to 8-bit mode.) */
    if ((st = send_command(CMD_FUNCTION_4BIT, 0)) != LCD1602_OK) { return st; }
    if ((st = send_command(CMD_DISPLAY_OFF,   0)) != LCD1602_OK) { return st; }
    if ((st = send_command(CMD_CLEAR,         2)) != LCD1602_OK) { return st; }   /* 1.52 ms */
    if ((st = send_command(CMD_ENTRY_INC,     0)) != LCD1602_OK) { return st; }
    if ((st = send_command(CMD_DISPLAY_ON,    0)) != LCD1602_OK) { return st; }

    /* The display is empty now; whatever is in the frame will be sent by the
     * next refresh */
    memset(s_shown, ' ', sizeof(s_shown));
    return LCD1602_OK;
}

/* ========================================================================= */
/*  SECTION 4: gatekeeper task with frame buffer (slide 29)                   */
/* ========================================================================= */

/**
 * Compare frame and shown and send only what changed: per row the range from
 * the first to the last changed column, together with the cursor command, in
 * ONE transfer. A full row (21 bytes x 5) takes about 9.5 ms on the bus, a
 * single changed character about 1 ms.
 */
static lcd1602_status_t refresh(void)
{
    for (uint8_t row = 0; row < LCD1602_ROWS; row++) {
        int first = -1;
        int last  = -1;

        for (int c = 0; c < (int)LCD1602_COLS; c++) {
            if (s_frame[row][c] != s_shown[row][c]) {
                if (first < 0) {
                    first = c;
                }
                last = c;
            }
        }
        if (first < 0) {
            continue;                                   /* row unchanged */
        }

        uint16_t n = encode_byte(s_txbuf,
                                 (uint8_t)(CMD_SET_DDRAM | (row_offset[row] + (unsigned)first)), 0);
        for (int c = first; c <= last; c++) {
            n = (uint16_t)(n + encode_byte(&s_txbuf[n], (uint8_t)s_frame[row][c], LCD1602_PIN_RS));
        }

        lcd1602_status_t st = bus_write(s_txbuf, n);
        if (st != LCD1602_OK) {
            return st;
        }
        memcpy(&s_shown[row][first], &s_frame[row][first], (size_t)(last - first + 1));
    }
    return LCD1602_OK;
}

static lcd1602_status_t apply(const lcd1602_msg_t *msg)
{
    switch ((msg_type_t)msg->type) {
    case MSG_LINE:
        memset(s_frame[msg->row], ' ', LCD1602_COLS);
        memcpy(s_frame[msg->row], msg->text, msg->len);
        break;
    case MSG_TEXT:
        memcpy(&s_frame[msg->row][msg->col], msg->text, msg->len);
        break;
    case MSG_CLEAR:
        memset(s_frame, ' ', sizeof(s_frame));
        break;
    case MSG_BACKLIGHT:
        s_backlight = msg->len ? LCD1602_PIN_BACKLIGHT : 0u;
        if (s_ready) {
            s_txbuf[0] = s_backlight;                   /* E low, only the light changes */
            return bus_write(s_txbuf, 1);
        }
        break;                                          /* used by the next init */
    default:
        break;
    }
    return LCD1602_OK;
}

/** Empty default: the application can define its own version.
 *  (__WEAK comes from CMSIS and works with every Cortex-M compiler.) */
__WEAK void lcd1602_ready_changed(bool ready)
{
    (void)ready;
}

/** Remember the state and report changes to the application. */
static void set_ready(bool ready)
{
    static int8_t reported = -1;                        /* -1: nothing reported yet */

    s_ready = ready;
    if (reported != (int8_t)ready) {
        reported = (int8_t)ready;
        lcd1602_ready_changed(ready);
    }
}

/** Bus errors mean the display is gone or confused: initialise it again (at
 *  once, then every LCD1602_RETRY_MS). "Busy" only means another device had
 *  the bus - the next refresh simply tries again. */
static void handle_status(lcd1602_status_t st)
{
    if ((st == LCD1602_ERR_I2C || st == LCD1602_ERR_TIMEOUT) && s_ready) {
        set_ready(false);
        s_next_init = xTaskGetTickCount();
    }
}

static void lcd1602_task(void *arg)
{
    TickType_t    next_refresh = xTaskGetTickCount();
    lcd1602_msg_t msg;
    (void)arg;

    s_next_init = xTaskGetTickCount();                  /* first attempt at once */

    for (;;) {
        /* (Re)initialise the display while it is not ready */
        if (!s_ready && (int32_t)(xTaskGetTickCount() - s_next_init) >= 0) {
            set_ready(display_init() == LCD1602_OK);
            s_next_init  = xTaskGetTickCount() + pdMS_TO_TICKS(LCD1602_RETRY_MS);
            next_refresh = xTaskGetTickCount();         /* show the frame at once */
        }

        /* Sleep until a message arrives or the next refresh / init attempt is
         * due. Messages also update the frame while the display is missing, so
         * it shows the latest texts as soon as it answers. */
        TickType_t deadline = s_ready ? next_refresh : s_next_init;
        TickType_t now      = xTaskGetTickCount();
        TickType_t wait     = ((int32_t)(deadline - now) > 0) ? (deadline - now) : 0u;

        if (xQueueReceive(s_queue, &msg, wait) == pdTRUE) {
            do {
                handle_status(apply(&msg));
            } while (xQueueReceive(s_queue, &msg, 0) == pdTRUE);
        }

        if (s_ready && (int32_t)(xTaskGetTickCount() - next_refresh) >= 0) {
            next_refresh = xTaskGetTickCount() + pdMS_TO_TICKS(LCD1602_REFRESH_MS);
            handle_status(refresh());
        }
    }
}

/* ========================================================================= */
/*  Public API                                                                */
/* ========================================================================= */

lcd1602_status_t lcd1602_start(SemaphoreHandle_t bus_mutex)
{
    if (s_started) {
        return LCD1602_OK;
    }

    s_bus_mutex = (bus_mutex != NULL) ? bus_mutex : xSemaphoreCreateMutexStatic(&s_bus_mutex_buf);
    s_tx_done   = xSemaphoreCreateBinaryStatic(&s_tx_done_buf);
    s_queue     = xQueueCreateStatic(LCD1602_QUEUE_LENGTH, sizeof(lcd1602_msg_t),
                                     s_queue_storage, &s_queue_buf);
    if (s_bus_mutex == NULL || s_tx_done == NULL || s_queue == NULL) {
        return LCD1602_ERR_RTOS;
    }

    /* DMA if CubeMX has set it up (MX_I2C1_Init() has run), otherwise blocking */
    s_dma_problem = port_dma_problem();
    s_use_dma     = (s_dma_problem == NULL);

    memset(s_frame, ' ', sizeof(s_frame));
    memset(s_shown, ' ', sizeof(s_shown));

    if (xTaskCreateStatic(lcd1602_task, "LCD", LCD1602_TASK_STACK, NULL,
                          LCD1602_TASK_PRIORITY, s_task_stack, &s_task_tcb) == NULL) {
        return LCD1602_ERR_RTOS;
    }
    s_started = true;
    return LCD1602_OK;
}

static lcd1602_status_t post(const lcd1602_msg_t *msg)
{
    if (!s_started) {
        return LCD1602_ERR_RTOS;
    }
    /* Timeout 0: the caller never waits for the display */
    return (xQueueSend(s_queue, msg, 0) == pdTRUE) ? LCD1602_OK : LCD1602_ERR_QUEUE_FULL;
}

lcd1602_status_t lcd1602_print(uint8_t row, uint8_t col, const char *text)
{
    lcd1602_msg_t msg = { .type = MSG_TEXT, .row = row, .col = col };

    if (text == NULL || row >= LCD1602_ROWS || col >= LCD1602_COLS) {
        return LCD1602_ERR_PARAM;
    }
    msg.len = (uint8_t)strnlen(text, (size_t)(LCD1602_COLS - col));   /* clip at row end */
    memcpy(msg.text, text, msg.len);
    return post(&msg);
}

lcd1602_status_t lcd1602_write_line(uint8_t row, const char *text)
{
    lcd1602_msg_t msg = { .type = MSG_LINE, .row = row };

    if (text == NULL || row >= LCD1602_ROWS) {
        return LCD1602_ERR_PARAM;
    }
    msg.len = (uint8_t)strnlen(text, LCD1602_COLS);
    memcpy(msg.text, text, msg.len);
    return post(&msg);
}

lcd1602_status_t lcd1602_clear(void)
{
    const lcd1602_msg_t msg = { .type = MSG_CLEAR };
    return post(&msg);
}

lcd1602_status_t lcd1602_backlight(bool on)
{
    const lcd1602_msg_t msg = { .type = MSG_BACKLIGHT, .len = on ? 1u : 0u };
    return post(&msg);
}

bool lcd1602_is_ready(void)
{
    return s_ready;
}

bool lcd1602_dma_active(void)
{
    return s_use_dma;
}

const char *lcd1602_dma_problem(void)
{
    return s_use_dma ? NULL : s_dma_problem;
}

SemaphoreHandle_t lcd1602_bus_mutex(void)
{
    return s_bus_mutex;
}
