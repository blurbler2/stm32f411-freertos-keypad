# LCD library

> The application now uses the FreeRTOS driver `lcd1602` (gatekeeper task, I2C bus mutex, DMA):
> see the comment at the top of `Core/Inc/lcd1602.h` and [architecture.md](architecture.md).
> This page describes the older `lcd_i2c` library. It stays in the project as a standalone,
> blocking driver without RTOS and is still unit-tested in `tests/host`.

Driver for HD44780 character displays (16x2, 20x4, ...) that are connected through a PCF8574 I2C
backpack. Files: `lcd_i2c.h`, `lcd_i2c.c`. It needs the STM32 HAL with the I2C module and no RTOS.

## Hardware

| Backpack | Microcontroller |
|---|---|
| GND | GND |
| VCC | 5V |
| SDA | I2C SDA (in this project PB7) |
| SCL | I2C SCL (in this project PB8) |

The backpack connects the PCF8574 outputs to the display in the usual way: P0 = RS, P1 = RW,
P2 = EN, P3 = backlight, P4..P7 = D4..D7. The library is written for this layout.

- Address: `0x27` (PCF8574) or `0x3F` (PCF8574A) unless the solder jumpers A0..A2 are closed.
  `lcd_i2c_find()` searches all possible addresses.
- With 5 V supply the backpack pulls SDA/SCL to 5 V. Use 5 V tolerant pins.
- Contrast is set with the potentiometer on the backpack.

## Configuration

```c
typedef struct {
    I2C_HandleTypeDef *hi2c;       /* e.g. &hi2c1                                   */
    uint8_t            address;    /* 7-bit address, not shifted                    */
    uint8_t            cols;       /* 16                                            */
    uint8_t            rows;       /* 2                                             */
    uint32_t           timeout_ms; /* I2C timeout per transfer, 0 = 20 ms           */
    lcd_delay_fn_t     delay_ms;   /* NULL = HAL_Delay                              */
} lcd_i2c_config_t;
```

In CubeMX enable an I2C peripheral in *Standard Mode* (100 kHz). The library does not configure
the I2C peripheral itself.

## Basic use

```c
static lcd_i2c_t lcd;

lcd_i2c_config_t cfg = { .hi2c = &hi2c1, .cols = 16, .rows = 2 };
cfg.address = lcd_i2c_find(&hi2c1);
if (cfg.address == 0 || lcd_init(&lcd, &cfg) != LCD_OK) {
    /* no display */
}

lcd_write_line(&lcd, 0, "Temperature");
lcd_print_at(&lcd, 0, 1, "23.5 C");
```

Every function returns `LCD_OK`, `LCD_ERR_PARAM`, `LCD_ERR_I2C` or `LCD_ERR_NOT_INIT`.

## API overview

| Function | Purpose |
|---|---|
| `lcd_i2c_find(hi2c)` | Search the bus, returns the 7-bit address or 0 |
| `lcd_init(lcd, cfg)` | Initialise: 4-bit mode, display on, cleared, backlight on (about 70 ms) |
| `lcd_clear(lcd)`, `lcd_home(lcd)` | Clear everything / cursor to the top left |
| `lcd_set_cursor(lcd, col, row)` | Position, both 0-based |
| `lcd_putc(lcd, c)`, `lcd_puts(lcd, text)` | Write at the cursor |
| `lcd_print_at(lcd, col, row, text)` | Position and write, clipped at the end of the row |
| `lcd_write_line(lcd, row, text)` | Write a whole row and fill the rest with spaces |
| `lcd_backlight(lcd, on)` | Backlight on/off |
| `lcd_display(lcd, on)`, `lcd_cursor(lcd, on)`, `lcd_blink(lcd, on)` | Display, underline cursor, blinking cursor |
| `lcd_create_char(lcd, slot, pattern)` | Define one of 8 custom characters |

`lcd_write_line()` is the function for status displays: the old content of the row disappears
without `lcd_clear()`, so the display does not flicker.

The display has no automatic line wrap. `lcd_puts()` simply continues in the display memory, so
position the cursor for each row or use the two functions that clip.

### Numbers

The library has no `printf`. Format the text first:

```c
char line[17];
snprintf(line, sizeof(line), "Task %d: %05lu", 1, (unsigned long)counter);
lcd_write_line(&lcd, 0, line);
```

Under FreeRTOS, a task that calls `snprintf` needs a larger stack (256 words or more).

### Custom characters

```c
static const uint8_t degree[8] = { 0x06, 0x09, 0x09, 0x06, 0x00, 0x00, 0x00, 0x00 };
lcd_create_char(&lcd, 1, degree);
lcd_print_at(&lcd, 0, 1, "23.5 \x01" "C");
```

Each row of the pattern uses bits 4..0 for the five pixels. Prefer slots 1..7, because character
0 ends a C string.

## Use with FreeRTOS

Two rules:

1. **One task owns the display.** The library has no locking. The usual pattern is a display
   manager task that receives texts from a queue. This project does exactly that, see
   `DisplayTask` and `App_Display()` in `app.c`.
2. **Use a delay that lets other tasks run.** Otherwise `HAL_Delay` busy-waits during the
   initialisation.

```c
static void lcd_delay(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms) + 1); }

typedef struct { uint8_t row; char text[17]; } display_msg_t;

static void DisplayTask(void *arg)
{
    static lcd_i2c_t lcd;
    lcd_i2c_config_t cfg = { .hi2c = &hi2c1, .cols = 16, .rows = 2, .delay_ms = lcd_delay };
    display_msg_t msg;

    cfg.address = lcd_i2c_find(&hi2c1);
    lcd_init(&lcd, &cfg);                    /* call it inside the task, not before the scheduler runs */

    for (;;) {
        if (xQueueReceive(displayQueue, &msg, portMAX_DELAY) == pdPASS) {
            lcd_write_line(&lcd, msg.row, msg.text);
        }
    }
}
```

Any number of producer tasks can then send `display_msg_t` messages to the queue.

## Timing

One character is one I2C transfer of 4 bytes, about 0.45 ms at 100 kHz. A full 16-character row
takes about 8 ms. `lcd_clear()` and `lcd_home()` add a 2 ms wait.

## Limits

- Write only. The busy flag of the display is not read; the library relies on the timing.
- Displays with other backpack pin layouts need changed `PIN_*` constants in `lcd_i2c.c`.
- Characters outside ASCII (ä, ö, ü, °) depend on the character ROM of the display and are not
  translated.
