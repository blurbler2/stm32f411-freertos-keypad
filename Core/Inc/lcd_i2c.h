/**
 * @file    lcd_i2c.h
 * @brief   Reusable driver for HD44780 character LCDs (16x2, 20x4, ...) with a
 *          PCF8574 I2C backpack, for STM32 HAL.
 *
 * Features
 *  - Any size up to 4 rows (16x2, 20x2, 20x4, ...)
 *  - Several displays at once (one lcd_i2c_t instance each)
 *  - Text, cursor positioning, whole-line output with padding, custom characters
 *  - Backlight, display, cursor and blink control
 *  - Address search for the usual backpack addresses (0x27, 0x3F, ...)
 *  - No RTOS required. The delay function can be replaced, e.g. by one based
 *    on vTaskDelay(), so that the driver does not busy-wait under an RTOS.
 *
 * Wiring of the backpack (the standard for nearly all of these modules)
 *      PCF8574 P0 = RS      P4 = D4
 *              P1 = RW      P5 = D5
 *              P2 = EN      P6 = D6
 *              P3 = backlight   P7 = D7
 *
 * Thread safety: the driver has no locking. Under an RTOS use each display
 * from ONE task only (a "display manager" task fed by a queue) or protect all
 * calls with a mutex.
 */
#ifndef LCD_I2C_H
#define LCD_I2C_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "main.h"   /* pulls in the matching stm32xxxx_hal.h (I2C module must be enabled) */

#define LCD_I2C_MAX_ROWS        4u
#define LCD_I2C_ADDR_PCF8574    0x27u   /* default of PCF8574  backpacks (A0..A2 open) */
#define LCD_I2C_ADDR_PCF8574A   0x3Fu   /* default of PCF8574A backpacks (A0..A2 open) */

typedef enum {
    LCD_OK = 0,
    LCD_ERR_PARAM,      /**< NULL pointer or invalid size / position   */
    LCD_ERR_I2C,        /**< I2C transfer failed (no ACK, bus error, timeout) */
    LCD_ERR_NOT_INIT    /**< lcd_init() has not succeeded yet           */
} lcd_status_t;

/** Delay function, e.g. HAL_Delay or a wrapper around vTaskDelay. */
typedef void (*lcd_delay_fn_t)(uint32_t ms);

/** Configuration - can be const and live in flash. */
typedef struct {
    I2C_HandleTypeDef *hi2c;      /**< I2C handle from CubeMX, e.g. &hi2c1              */
    uint8_t            address;   /**< 7-bit address (0x27, 0x3F, ...), NOT shifted     */
    uint8_t            cols;      /**< characters per row, e.g. 16                      */
    uint8_t            rows;      /**< number of rows, e.g. 2                           */
    uint32_t           timeout_ms;/**< I2C timeout per transfer, 0 = default (20 ms)    */
    lcd_delay_fn_t     delay_ms;  /**< NULL = HAL_Delay                                 */
} lcd_i2c_config_t;

/** Runtime state of one display. Treat the content as private. */
typedef struct {
    lcd_i2c_config_t cfg;
    uint8_t backlight;      /* current backlight bit            */
    uint8_t display_ctrl;   /* display / cursor / blink bits    */
    bool    initialized;
} lcd_i2c_t;

/* ---------- Setup ---------- */

/**
 * Search the bus for a backpack. Probes 0x27, 0x3F and then the remaining
 * PCF8574 (0x20..0x26) and PCF8574A (0x38..0x3E) addresses.
 * @return 7-bit address, or 0 if nothing answered
 */
uint8_t lcd_i2c_find(I2C_HandleTypeDef *hi2c);

/**
 * Initialise the display (4-bit mode, display on, cursor off, cleared,
 * backlight on). Takes about 70 ms because of the delays the HD44780 needs
 * after power-up.
 */
lcd_status_t lcd_init(lcd_i2c_t *lcd, const lcd_i2c_config_t *cfg);

/* ---------- Text output ---------- */

lcd_status_t lcd_clear(lcd_i2c_t *lcd);                       /**< clear all, cursor to 0,0 */
lcd_status_t lcd_home(lcd_i2c_t *lcd);                        /**< cursor to 0,0            */
lcd_status_t lcd_set_cursor(lcd_i2c_t *lcd, uint8_t col, uint8_t row);   /**< both 0-based */
lcd_status_t lcd_putc(lcd_i2c_t *lcd, char c);                /**< one character at the cursor */
lcd_status_t lcd_puts(lcd_i2c_t *lcd, const char *text);      /**< string at the cursor (no line wrap) */

/** Set the cursor and print a string (clipped at the end of the row). */
lcd_status_t lcd_print_at(lcd_i2c_t *lcd, uint8_t col, uint8_t row, const char *text);

/**
 * Write a whole row: the text is printed from column 0 and the rest of the
 * row is filled with spaces, so old content disappears without lcd_clear()
 * (no flicker). Text longer than the row is clipped.
 */
lcd_status_t lcd_write_line(lcd_i2c_t *lcd, uint8_t row, const char *text);

/* ---------- Control ---------- */

lcd_status_t lcd_backlight(lcd_i2c_t *lcd, bool on);
lcd_status_t lcd_display(lcd_i2c_t *lcd, bool on);            /**< content is kept while off */
lcd_status_t lcd_cursor(lcd_i2c_t *lcd, bool on);             /**< underline cursor          */
lcd_status_t lcd_blink(lcd_i2c_t *lcd, bool on);              /**< blinking block cursor     */

/**
 * Define a custom character.
 * @param slot    0..7; print it later with lcd_putc(lcd, (char)slot)
 *                (use slots 1..7 if you print with lcd_puts, because 0 ends a C string)
 * @param pattern 8 rows, bits 4..0 = pixels from left to right
 * The cursor is at 0,0 afterwards.
 */
lcd_status_t lcd_create_char(lcd_i2c_t *lcd, uint8_t slot, const uint8_t pattern[8]);

#ifdef __cplusplus
}
#endif

#endif /* LCD_I2C_H */
