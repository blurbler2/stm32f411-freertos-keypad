/**
 * @file    keypad.h
 * @brief   Reusable key-matrix (keypad) driver for STM32 HAL, e.g. 4x4 or 4x3 keypads.
 *
 * Features
 *  - Any matrix size up to KEYPAD_MAX_ROWS x KEYPAD_MAX_COLS
 *  - Every row/column may sit on any port and pin
 *  - Several keypads at once (one keypad_t instance each)
 *  - Per-key, time-based debouncing
 *  - Events: PRESS, RELEASE, HOLD (long press) and optional REPEAT (auto-repeat)
 *  - Event FIFO and/or callback
 *  - No RTOS required: call keypad_scan() periodically (every 5..10 ms).
 *    For FreeRTOS there is an optional wrapper, see keypad_rtos.h.
 *
 * Electrical concept
 *  - Rows    = outputs, open-drain, idle level HIGH (high impedance)
 *  - Columns = inputs with pull-up
 *  - Exactly one row is pulled LOW at a time; a pressed key then pulls its
 *    column LOW.
 *  Open-drain rows avoid a short circuit between two rows when two keys of
 *  the same column are pressed at the same time.
 */
#ifndef KEYPAD_H
#define KEYPAD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "main.h"   /* pulls in the matching stm32xxxx_hal.h of the CubeMX project */

/* ---------- Compile-time configuration (override before including) ---------- */
#ifndef KEYPAD_MAX_ROWS
#define KEYPAD_MAX_ROWS         4u
#endif
#ifndef KEYPAD_MAX_COLS
#define KEYPAD_MAX_COLS         4u
#endif
#ifndef KEYPAD_EVENT_FIFO_LEN   /* buffered events per keypad */
#define KEYPAD_EVENT_FIFO_LEN   8u
#endif

#define KEYPAD_MAX_KEYS         (KEYPAD_MAX_ROWS * KEYPAD_MAX_COLS)
#define KEYPAD_NO_KEY           '\0'

/* ---------- Types ---------- */

/** One GPIO pin (port + pin mask, as used by HAL_GPIO_WritePin). */
typedef struct {
    GPIO_TypeDef *port;
    uint16_t      pin;
} keypad_pin_t;

typedef enum {
    KEYPAD_EVT_PRESS = 0,   /**< key was pressed (debounced)                  */
    KEYPAD_EVT_RELEASE,     /**< key was released                             */
    KEYPAD_EVT_HOLD,        /**< key has been down for longer than hold_ms    */
    KEYPAD_EVT_REPEAT       /**< auto-repeat while held (every repeat_ms)     */
} keypad_evt_type_t;

typedef struct {
    char              key;   /**< character from the keymap, e.g. '5' or 'A' */
    uint8_t           row;   /**< 0-based */
    uint8_t           col;   /**< 0-based */
    keypad_evt_type_t type;
} keypad_event_t;

/** Optional callback, called from inside keypad_scan() (same context!). */
typedef void (*keypad_callback_t)(const keypad_event_t *evt, void *user);

/** Configuration - can (and should) be const and live in flash. */
typedef struct {
    const keypad_pin_t *rows;       /**< array of num_rows row pins (outputs)     */
    uint8_t             num_rows;
    const keypad_pin_t *cols;       /**< array of num_cols column pins (inputs)   */
    uint8_t             num_cols;
    const char         *keymap;     /**< num_rows*num_cols characters, row by row */

    uint16_t debounce_ms;   /**< debounce time, typically 20 ms                    */
    uint16_t hold_ms;       /**< 0 = no HOLD event, otherwise e.g. 800 ms          */
    uint16_t repeat_ms;     /**< 0 = no auto-repeat, otherwise e.g. 200 ms (after HOLD) */

    bool     configure_gpio;/**< true : keypad_init() configures the pins itself
                                        (the GPIO port clocks must still be enabled!)
                                 false: pins were configured in CubeMX             */
    uint16_t settle_loops;  /**< busy-wait iterations after switching a row (line
                                 capacitance + pull-up), typically 20..100         */

    keypad_callback_t callback; /**< optional, NULL = FIFO only                    */
    void             *user;     /**< passed through to the callback                */
} keypad_config_t;

/** Runtime state of one keypad. Treat the content as private. */
typedef struct {
    const keypad_config_t *cfg;
    uint8_t  flags[KEYPAD_MAX_KEYS];     /* bit0 raw, bit1 stable, bit2 hold sent   */
    uint32_t t_change[KEYPAD_MAX_KEYS];  /* time of the last raw-level change       */
    uint32_t t_next[KEYPAD_MAX_KEYS];    /* time of the next HOLD/REPEAT event      */
    keypad_event_t fifo[KEYPAD_EVENT_FIFO_LEN];
    uint8_t  fifo_head;
    uint8_t  fifo_tail;
    uint8_t  fifo_count;
    uint16_t fifo_overflows;             /* events lost because the FIFO was full   */
    bool     initialized;
} keypad_t;

typedef enum {
    KEYPAD_OK = 0,
    KEYPAD_ERR_PARAM,       /**< NULL pointer, 0 rows/columns, ...   */
    KEYPAD_ERR_SIZE         /**< matrix larger than KEYPAD_MAX_*     */
} keypad_status_t;

/* ---------- API ---------- */

/** Initialise a keypad instance. cfg must stay valid as long as kp is used. */
keypad_status_t keypad_init(keypad_t *kp, const keypad_config_t *cfg);

/**
 * Scan the whole matrix once and generate events.
 * Call periodically (recommended: every 5..10 ms).
 * @param now_ms current time in ms, e.g. HAL_GetTick() or
 *               xTaskGetTickCount() * portTICK_PERIOD_MS
 * @return number of events generated by this call
 */
uint8_t keypad_scan(keypad_t *kp, uint32_t now_ms);

/** Pop the oldest event from the FIFO. @return true if there was one. */
bool keypad_get_event(keypad_t *kp, keypad_event_t *evt);

/**
 * Convenience: returns the character of the next PRESS event (other events
 * are discarded) or KEYPAD_NO_KEY.
 */
char keypad_get_key(keypad_t *kp);

/** Is the key with this character currently pressed (debounced)? */
bool keypad_is_pressed(const keypad_t *kp, char key);

/** Is the key at (row, col) currently pressed (debounced)? */
bool keypad_is_pressed_rc(const keypad_t *kp, uint8_t row, uint8_t col);

/** Number of keys currently pressed (debounced). */
uint8_t keypad_pressed_count(const keypad_t *kp);

/** Empty the event FIFO. */
void keypad_flush(keypad_t *kp);

/* ---------- Ready-made keymaps ---------- */
extern const char KEYPAD_KEYMAP_4X4[16];   /* "123A456B789C*0#D" */
extern const char KEYPAD_KEYMAP_4X3[12];   /* "123456789*0#"     */

#ifdef __cplusplus
}
#endif

#endif /* KEYPAD_H */
