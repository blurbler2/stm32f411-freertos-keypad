/**
 * @file    keypad.c
 * @brief   Key-matrix driver implementation. See keypad.h.
 */
#include "keypad.h"
#include <string.h>

#define FLAG_RAW     0x01u   /* last raw sample (1 = pressed)             */
#define FLAG_STABLE  0x02u   /* debounced state (1 = pressed)             */
#define FLAG_HOLD    0x04u   /* HOLD already sent for this press          */

const char KEYPAD_KEYMAP_4X4[16] = {
    '1', '2', '3', 'A',
    '4', '5', '6', 'B',
    '7', '8', '9', 'C',
    '*', '0', '#', 'D'
};

const char KEYPAD_KEYMAP_4X3[12] = {
    '1', '2', '3',
    '4', '5', '6',
    '7', '8', '9',
    '*', '0', '#'
};

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void settle_delay(uint16_t loops)
{
    /* volatile so the compiler does not optimise the loop away */
    for (volatile uint16_t i = 0; i < loops; i++) {
        /* nothing */
    }
}

static void all_rows_idle(const keypad_config_t *cfg)
{
    for (uint8_t r = 0; r < cfg->num_rows; r++) {
        HAL_GPIO_WritePin(cfg->rows[r].port, cfg->rows[r].pin, GPIO_PIN_SET);
    }
}

static void configure_pins(const keypad_config_t *cfg)
{
    GPIO_InitTypeDef init;

    /* set the idle level before switching to output -> no glitch */
    all_rows_idle(cfg);

    for (uint8_t r = 0; r < cfg->num_rows; r++) {
        memset(&init, 0, sizeof(init));
        init.Pin   = cfg->rows[r].pin;
        init.Mode  = GPIO_MODE_OUTPUT_OD;
        init.Pull  = GPIO_NOPULL;
        init.Speed = GPIO_SPEED_FREQ_LOW;
        HAL_GPIO_Init(cfg->rows[r].port, &init);
    }
    for (uint8_t c = 0; c < cfg->num_cols; c++) {
        memset(&init, 0, sizeof(init));
        init.Pin   = cfg->cols[c].pin;
        init.Mode  = GPIO_MODE_INPUT;
        init.Pull  = GPIO_PULLUP;
        init.Speed = GPIO_SPEED_FREQ_LOW;
        HAL_GPIO_Init(cfg->cols[c].port, &init);
    }
}

static void emit(keypad_t *kp, uint8_t idx, keypad_evt_type_t type, uint8_t *count)
{
    const keypad_config_t *cfg = kp->cfg;
    keypad_event_t evt;

    evt.row  = (uint8_t)(idx / cfg->num_cols);
    evt.col  = (uint8_t)(idx % cfg->num_cols);
    evt.key  = cfg->keymap ? cfg->keymap[idx] : KEYPAD_NO_KEY;
    evt.type = type;

    if (kp->fifo_count < KEYPAD_EVENT_FIFO_LEN) {
        kp->fifo[kp->fifo_head] = evt;
        kp->fifo_head = (uint8_t)((kp->fifo_head + 1u) % KEYPAD_EVENT_FIFO_LEN);
        kp->fifo_count++;
    } else {
        kp->fifo_overflows++;   /* older events are kept */
    }

    if (cfg->callback != NULL) {
        cfg->callback(&evt, cfg->user);
    }
    (*count)++;
}

/* Debounce and event logic for a single key */
static void process_key(keypad_t *kp, uint8_t idx, bool pressed, uint32_t now, uint8_t *count)
{
    const keypad_config_t *cfg = kp->cfg;
    uint8_t f = kp->flags[idx];
    bool raw    = (f & FLAG_RAW) != 0u;
    bool stable = (f & FLAG_STABLE) != 0u;

    if (pressed != raw) {
        /* raw level changed -> restart the debounce timer */
        f = (uint8_t)(pressed ? (f | FLAG_RAW) : (f & (uint8_t)~FLAG_RAW));
        kp->t_change[idx] = now;
    } else if (pressed != stable) {
        /* raw level is steady but not accepted yet */
        if ((uint32_t)(now - kp->t_change[idx]) >= cfg->debounce_ms) {
            if (pressed) {
                f |= FLAG_STABLE;
                f &= (uint8_t)~FLAG_HOLD;
                kp->t_next[idx] = now + cfg->hold_ms;
                kp->flags[idx] = f;
                emit(kp, idx, KEYPAD_EVT_PRESS, count);
            } else {
                f &= (uint8_t)~(FLAG_STABLE | FLAG_HOLD);
                kp->flags[idx] = f;
                emit(kp, idx, KEYPAD_EVT_RELEASE, count);
            }
            return;
        }
    } else if (stable && cfg->hold_ms > 0u) {
        /* key is held down -> check HOLD / REPEAT.
         * The (int32_t) comparison is safe across tick wrap-around. */
        if ((int32_t)(now - kp->t_next[idx]) >= 0) {
            if ((f & FLAG_HOLD) == 0u) {
                f |= FLAG_HOLD;
                kp->flags[idx] = f;
                kp->t_next[idx] = now + cfg->repeat_ms;
                emit(kp, idx, KEYPAD_EVT_HOLD, count);
                return;
            } else if (cfg->repeat_ms > 0u) {
                kp->t_next[idx] = now + cfg->repeat_ms;
                emit(kp, idx, KEYPAD_EVT_REPEAT, count);
            }
        }
    }
    kp->flags[idx] = f;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

keypad_status_t keypad_init(keypad_t *kp, const keypad_config_t *cfg)
{
    if (kp == NULL || cfg == NULL || cfg->rows == NULL || cfg->cols == NULL ||
        cfg->num_rows == 0u || cfg->num_cols == 0u) {
        return KEYPAD_ERR_PARAM;
    }
    if (cfg->num_rows > KEYPAD_MAX_ROWS || cfg->num_cols > KEYPAD_MAX_COLS) {
        return KEYPAD_ERR_SIZE;
    }

    memset(kp, 0, sizeof(*kp));
    kp->cfg = cfg;

    if (cfg->configure_gpio) {
        configure_pins(cfg);
    } else {
        all_rows_idle(cfg);
    }

    kp->initialized = true;
    return KEYPAD_OK;
}

uint8_t keypad_scan(keypad_t *kp, uint32_t now_ms)
{
    uint8_t count = 0;

    if (kp == NULL || !kp->initialized) {
        return 0;
    }
    const keypad_config_t *cfg = kp->cfg;

    for (uint8_t r = 0; r < cfg->num_rows; r++) {
        /* activate exactly this row (LOW) */
        HAL_GPIO_WritePin(cfg->rows[r].port, cfg->rows[r].pin, GPIO_PIN_RESET);
        settle_delay(cfg->settle_loops);

        for (uint8_t c = 0; c < cfg->num_cols; c++) {
            bool pressed =
                (HAL_GPIO_ReadPin(cfg->cols[c].port, cfg->cols[c].pin) == GPIO_PIN_RESET);
            process_key(kp, (uint8_t)(r * cfg->num_cols + c), pressed, now_ms, &count);
        }

        HAL_GPIO_WritePin(cfg->rows[r].port, cfg->rows[r].pin, GPIO_PIN_SET);
    }
    return count;
}

bool keypad_get_event(keypad_t *kp, keypad_event_t *evt)
{
    if (kp == NULL || evt == NULL || kp->fifo_count == 0u) {
        return false;
    }
    *evt = kp->fifo[kp->fifo_tail];
    kp->fifo_tail = (uint8_t)((kp->fifo_tail + 1u) % KEYPAD_EVENT_FIFO_LEN);
    kp->fifo_count--;
    return true;
}

char keypad_get_key(keypad_t *kp)
{
    keypad_event_t evt;
    while (keypad_get_event(kp, &evt)) {
        if (evt.type == KEYPAD_EVT_PRESS) {
            return evt.key;
        }
    }
    return KEYPAD_NO_KEY;
}

bool keypad_is_pressed_rc(const keypad_t *kp, uint8_t row, uint8_t col)
{
    if (kp == NULL || !kp->initialized ||
        row >= kp->cfg->num_rows || col >= kp->cfg->num_cols) {
        return false;
    }
    return (kp->flags[row * kp->cfg->num_cols + col] & FLAG_STABLE) != 0u;
}

bool keypad_is_pressed(const keypad_t *kp, char key)
{
    if (kp == NULL || !kp->initialized || kp->cfg->keymap == NULL) {
        return false;
    }
    uint8_t n = (uint8_t)(kp->cfg->num_rows * kp->cfg->num_cols);
    for (uint8_t i = 0; i < n; i++) {
        if (kp->cfg->keymap[i] == key) {
            return (kp->flags[i] & FLAG_STABLE) != 0u;
        }
    }
    return false;
}

uint8_t keypad_pressed_count(const keypad_t *kp)
{
    uint8_t cnt = 0;
    if (kp == NULL || !kp->initialized) {
        return 0;
    }
    uint8_t n = (uint8_t)(kp->cfg->num_rows * kp->cfg->num_cols);
    for (uint8_t i = 0; i < n; i++) {
        if (kp->flags[i] & FLAG_STABLE) {
            cnt++;
        }
    }
    return cnt;
}

void keypad_flush(keypad_t *kp)
{
    if (kp == NULL) {
        return;
    }
    kp->fifo_head = kp->fifo_tail = kp->fifo_count = 0;
}
