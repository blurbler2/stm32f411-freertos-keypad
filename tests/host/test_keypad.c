/*
 * Host unit test for the keypad library (no hardware needed).
 * A simulated 4x4 matrix replaces the GPIO pins: a column reads LOW when the
 * active (LOW) row has a pressed key in that column.
 *
 * Build and run:  make -C tests/host
 */
#include "keypad.h"
#include <assert.h>
#include <stdio.h>

GPIO_TypeDef mock_port_rows = { 1 }, mock_port_cols = { 2 };

static int row_low[4];        /* 1 = row is driven LOW                    */
static int pressed[4][4];     /* simulated key state                      */
static int bounce;            /* 1 = pressed keys read alternately 0/1    */
static int bounce_toggle;
static int init_calls;

void HAL_GPIO_Init(GPIO_TypeDef *port, GPIO_InitTypeDef *init)
{
    (void)port; (void)init;
    init_calls++;
}

void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState state)
{
    assert(port == GPIOD);
    for (int r = 0; r < 4; r++) {
        if (pin == (1u << r)) {
            row_low[r] = (state == GPIO_PIN_RESET);
        }
    }
}

GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *port, uint16_t pin)
{
    int c = -1, active = 0;

    assert(port == GPIOE);
    for (int i = 0; i < 4; i++) {
        if (pin == (1u << (7 + i))) { c = i; }
    }
    assert(c >= 0);
    for (int r = 0; r < 4; r++) { active += row_low[r]; }
    assert(active == 1);                      /* exactly one row may be active */

    for (int r = 0; r < 4; r++) {
        if (row_low[r] && pressed[r][c]) {
            if (bounce) {
                bounce_toggle ^= 1;
                if (bounce_toggle) { return GPIO_PIN_SET; }
            }
            return GPIO_PIN_RESET;
        }
    }
    return GPIO_PIN_SET;
}

static const keypad_pin_t rows[4] = { {GPIOD, 1u << 0}, {GPIOD, 1u << 1}, {GPIOD, 1u << 2}, {GPIOD, 1u << 3} };
static const keypad_pin_t cols[4] = { {GPIOE, 1u << 7}, {GPIOE, 1u << 8}, {GPIOE, 1u << 9}, {GPIOE, 1u << 10} };

static int cb_count;
static void on_event(const keypad_event_t *evt, void *user) { (void)evt; (*(int *)user)++; }

static const keypad_config_t cfg = {
    .rows = rows, .num_rows = 4, .cols = cols, .num_cols = 4,
    .keymap = KEYPAD_KEYMAP_4X4,
    .debounce_ms = 20, .hold_ms = 800, .repeat_ms = 200,
    .configure_gpio = true, .settle_loops = 0,
    .callback = on_event, .user = &cb_count
};

static uint32_t now;

/* Simulate ms milliseconds with one scan every 10 ms */
static void run(keypad_t *kp, uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i += 10) {
        now += 10;
        keypad_scan(kp, now);
    }
}

int main(void)
{
    keypad_t kp;
    keypad_event_t e;

    now = 0xFFFFFF00u;                        /* also covers the tick wrap-around */

    assert(keypad_init(&kp, &cfg) == KEYPAD_OK);
    assert(init_calls == 8);                  /* 4 rows + 4 columns configured */
    for (int r = 0; r < 4; r++) { assert(!row_low[r]); }

    /* 1) a bouncing contact produces no event */
    pressed[1][1] = 1; bounce = 1;
    run(&kp, 200);
    assert(!keypad_get_event(&kp, &e));

    /* 2) steady contact -> exactly one PRESS '5' */
    bounce = 0;
    run(&kp, 50);
    assert(keypad_get_event(&kp, &e));
    assert(e.type == KEYPAD_EVT_PRESS && e.key == '5' && e.row == 1 && e.col == 1);
    assert(!keypad_get_event(&kp, &e));
    assert(keypad_is_pressed(&kp, '5') && keypad_is_pressed_rc(&kp, 1, 1));
    assert(keypad_pressed_count(&kp) == 1);

    /* 3) holding -> one HOLD after 800 ms, then REPEAT every 200 ms */
    run(&kp, 1500);
    int hold = 0, repeat = 0;
    while (keypad_get_event(&kp, &e)) {
        assert(e.key == '5');
        if (e.type == KEYPAD_EVT_HOLD)   { hold++; }
        if (e.type == KEYPAD_EVT_REPEAT) { repeat++; }
    }
    assert(hold == 1 && repeat >= 3 && repeat <= 4);

    /* 4) release -> RELEASE */
    pressed[1][1] = 0;
    run(&kp, 50);
    assert(keypad_get_event(&kp, &e) && e.type == KEYPAD_EVT_RELEASE && e.key == '5');
    assert(keypad_pressed_count(&kp) == 0);

    /* 5) two keys of the same column at once: '3' and '9' */
    pressed[0][2] = 1; pressed[2][2] = 1;
    run(&kp, 50);
    assert(keypad_get_key(&kp) == '3');
    assert(keypad_get_key(&kp) == '9');
    pressed[0][2] = 0; pressed[2][2] = 0;
    run(&kp, 50);
    keypad_flush(&kp);

    /* 6) last key of the matrix */
    pressed[3][3] = 1;
    run(&kp, 50);
    assert(keypad_get_key(&kp) == 'D');
    pressed[3][3] = 0;
    run(&kp, 50);

    /* 7) FIFO overflow is counted, the callback still sees every event */
    keypad_flush(&kp);
    int before = cb_count;
    for (int i = 0; i < 12; i++) {
        pressed[0][0] = 1; run(&kp, 40);
        pressed[0][0] = 0; run(&kp, 40);
    }
    assert(cb_count - before == 24);
    assert(kp.fifo_count == KEYPAD_EVENT_FIFO_LEN);
    assert(kp.fifo_overflows == 24 - KEYPAD_EVENT_FIFO_LEN);

    /* 8) parameter checks */
    keypad_config_t bad = cfg;
    bad.num_rows = KEYPAD_MAX_ROWS + 1;
    assert(keypad_init(&kp, &bad) == KEYPAD_ERR_SIZE);
    assert(keypad_init(NULL, &cfg) == KEYPAD_ERR_PARAM);

    puts("keypad: all tests passed");
    return 0;
}
