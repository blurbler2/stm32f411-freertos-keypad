# Keypad library

Two layers, so the library can be reused with or without an RTOS:

| Files | Needs | Purpose |
|---|---|---|
| `keypad.h`, `keypad.c` | STM32 HAL (GPIO) | Scan, debounce, events, FIFO, callback |
| `keypad_rtos.h`, `keypad_rtos.c` | FreeRTOS | Scan task that copies the events into a queue |

## How the matrix is scanned

- Rows are open-drain outputs and idle high. Columns are inputs with pull-ups.
- One row at a time is pulled low. A pressed key connects its row to its column, so that column
  reads low.
- Every key has its own debounce timer: a level must be stable for `debounce_ms` before it counts.

Open-drain rows matter: with push-pull outputs, two pressed keys in one column would connect a
high row directly to a low row.

## Configuration

```c
typedef struct {
    const keypad_pin_t *rows;  uint8_t num_rows;    /* outputs                         */
    const keypad_pin_t *cols;  uint8_t num_cols;    /* inputs                          */
    const char *keymap;                             /* num_rows * num_cols characters  */
    uint16_t debounce_ms;                           /* typically 20                    */
    uint16_t hold_ms;                               /* 0 = no HOLD events              */
    uint16_t repeat_ms;                             /* 0 = no REPEAT events            */
    bool     configure_gpio;                        /* true: library sets pin modes    */
    uint16_t settle_loops;                          /* wait after switching a row      */
    keypad_callback_t callback;  void *user;        /* optional                        */
} keypad_config_t;
```

- `configure_gpio = true`: `keypad_init()` configures the pins itself. The GPIO port clocks must
  already be enabled. `false`: the pins come from CubeMX (rows *Output Open Drain*, level high;
  columns *Input*, *Pull-up*). This project uses `false`.
- Ready-made keymaps: `KEYPAD_KEYMAP_4X4` (`123A 456B 789C *0#D`) and `KEYPAD_KEYMAP_4X3`.
- For more than 4x4 keys define `KEYPAD_MAX_ROWS` / `KEYPAD_MAX_COLS` in the compiler settings.

## Events

| Event | When |
|---|---|
| `KEYPAD_EVT_PRESS` | Key is down and debounced |
| `KEYPAD_EVT_RELEASE` | Key is up and debounced |
| `KEYPAD_EVT_HOLD` | Key has been down for `hold_ms` (once per press) |
| `KEYPAD_EVT_REPEAT` | Every `repeat_ms` after HOLD while the key stays down |

Each event carries the key character, row, column and type (`keypad_event_t`).

## Use with FreeRTOS

```c
static keypad_rtos_t keypad;                       /* static or global, never on the stack */

keypad_rtos_start(&keypad, &cfg,
                  NULL, 10,                        /* create a queue for 10 events */
                  10,                              /* scan period in ms            */
                  tskIDLE_PRIORITY + 3, 128);      /* priority, stack in words     */

keypad_event_t evt;
if (keypad_rtos_wait_event(&keypad, &evt, portMAX_DELAY) == pdTRUE) {
    /* evt.key, evt.type */
}

char key = keypad_rtos_wait_key(&keypad, pdMS_TO_TICKS(5000));   /* next PRESS or KEYPAD_NO_KEY */
```

Pass your own queue instead of `NULL` if the events should go into an existing queue. Its item
size must be `sizeof(keypad_event_t)`.

## Use without an RTOS

```c
keypad_t kp;
keypad_init(&kp, &cfg);

while (1) {
    keypad_scan(&kp, HAL_GetTick());
    char key = keypad_get_key(&kp);
    if (key != KEYPAD_NO_KEY) {
        /* handle key */
    }
    HAL_Delay(10);
}
```

## API overview

| Function | Purpose |
|---|---|
| `keypad_init(kp, cfg)` | Initialise an instance |
| `keypad_scan(kp, now_ms)` | Scan once, returns the number of new events |
| `keypad_get_event(kp, &evt)` | Pop the oldest event from the FIFO |
| `keypad_get_key(kp)` | Character of the next PRESS, or `KEYPAD_NO_KEY` |
| `keypad_is_pressed(kp, 'A')`, `keypad_is_pressed_rc(kp, r, c)` | Current debounced state |
| `keypad_pressed_count(kp)` | Number of keys currently down |
| `keypad_flush(kp)` | Empty the FIFO |
| `keypad_rtos_start(...)` | Initialise and start the scan task |
| `keypad_rtos_wait_event(...)`, `keypad_rtos_wait_key(...)` | Block until an event or key arrives |

## Limits

- Call `keypad_scan()` from one context only (one task or the main loop), not from an interrupt.
- Keypads without diodes show a phantom key when three pressed keys form three corners of a
  rectangle. Two keys at once always work.
- If wrong columns are detected now and then, increase `settle_loops`.
