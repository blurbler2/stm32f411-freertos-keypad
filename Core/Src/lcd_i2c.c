/**
 * @file    lcd_i2c.c
 * @brief   HD44780 LCD with PCF8574 I2C backpack. See lcd_i2c.h.
 */
#include "lcd_i2c.h"

/* PCF8574 bits */
#define PIN_RS          0x01u
#define PIN_RW          0x02u   /* always 0: the driver only writes */
#define PIN_EN          0x04u
#define PIN_BACKLIGHT   0x08u

/* HD44780 commands */
#define CMD_CLEAR           0x01u
#define CMD_HOME            0x02u
#define CMD_ENTRY_MODE      0x04u
#define CMD_DISPLAY_CTRL    0x08u
#define CMD_FUNCTION_SET    0x20u
#define CMD_SET_CGRAM       0x40u
#define CMD_SET_DDRAM       0x80u

#define ENTRY_INCREMENT     0x02u   /* cursor moves right, no display shift */
#define DISPLAY_ON          0x04u
#define CURSOR_ON           0x02u
#define BLINK_ON            0x01u
#define FUNCTION_2LINE      0x08u   /* also correct for 4-row displays; 4-bit bus, 5x8 font */

#define DEFAULT_TIMEOUT_MS  20u

/* DDRAM start address of each row (rows 2 and 3 continue rows 0 and 1) */
static uint8_t row_address(const lcd_i2c_t *lcd, uint8_t row)
{
    switch (row) {
    case 0:  return 0x00u;
    case 1:  return 0x40u;
    case 2:  return (uint8_t)(0x00u + lcd->cfg.cols);
    default: return (uint8_t)(0x40u + lcd->cfg.cols);
    }
}

static void delay(const lcd_i2c_t *lcd, uint32_t ms)
{
    if (lcd->cfg.delay_ms != NULL) {
        lcd->cfg.delay_ms(ms);
    } else {
        HAL_Delay(ms);
    }
}

static lcd_status_t i2c_write(const lcd_i2c_t *lcd, uint8_t *data, uint16_t len)
{
    HAL_StatusTypeDef st = HAL_I2C_Master_Transmit(lcd->cfg.hi2c,
                                                   (uint16_t)(lcd->cfg.address << 1),
                                                   data, len, lcd->cfg.timeout_ms);
    return (st == HAL_OK) ? LCD_OK : LCD_ERR_I2C;
}

/* One nibble (in the upper 4 bits) with an EN pulse. The PCF8574 latches every
 * byte, so two bytes in one transfer give EN high -> EN low. At 100 kHz the
 * pulse is about 90 us long, far above the 450 ns the HD44780 needs. */
static lcd_status_t write_nibble(const lcd_i2c_t *lcd, uint8_t nibble, uint8_t rs)
{
    uint8_t base = (uint8_t)((nibble & 0xF0u) | rs | lcd->backlight);
    uint8_t buf[2] = { (uint8_t)(base | PIN_EN), base };
    return i2c_write(lcd, buf, 2);
}

/* One byte = high nibble, then low nibble, in a single I2C transfer. The
 * transfer itself takes longer than the 37 us execution time of a command. */
static lcd_status_t write_byte(const lcd_i2c_t *lcd, uint8_t value, uint8_t rs)
{
    uint8_t hi = (uint8_t)((value & 0xF0u) | rs | lcd->backlight);
    uint8_t lo = (uint8_t)(((value << 4) & 0xF0u) | rs | lcd->backlight);
    uint8_t buf[4] = { (uint8_t)(hi | PIN_EN), hi, (uint8_t)(lo | PIN_EN), lo };
    return i2c_write(lcd, buf, 4);
}

static lcd_status_t command(const lcd_i2c_t *lcd, uint8_t cmd)
{
    return write_byte(lcd, cmd, 0);
}

static lcd_status_t check(const lcd_i2c_t *lcd)
{
    if (lcd == NULL) {
        return LCD_ERR_PARAM;
    }
    return lcd->initialized ? LCD_OK : LCD_ERR_NOT_INIT;
}

/* ------------------------------------------------------------------ */
/* Setup                                                               */
/* ------------------------------------------------------------------ */

uint8_t lcd_i2c_find(I2C_HandleTypeDef *hi2c)
{
    static const uint8_t first[] = { LCD_I2C_ADDR_PCF8574, LCD_I2C_ADDR_PCF8574A };

    if (hi2c == NULL) {
        return 0;
    }
    for (size_t i = 0; i < sizeof(first); i++) {
        if (HAL_I2C_IsDeviceReady(hi2c, (uint16_t)(first[i] << 1), 2, 10) == HAL_OK) {
            return first[i];
        }
    }
    for (uint8_t a = 0x20u; a <= 0x3Eu; a++) {
        if (a == 0x27u) {
            a = 0x38u;                       /* skip to the PCF8574A range */
        }
        if (HAL_I2C_IsDeviceReady(hi2c, (uint16_t)(a << 1), 2, 10) == HAL_OK) {
            return a;
        }
    }
    return 0;
}

lcd_status_t lcd_init(lcd_i2c_t *lcd, const lcd_i2c_config_t *cfg)
{
    lcd_status_t st;

    if (lcd == NULL || cfg == NULL || cfg->hi2c == NULL ||
        cfg->cols == 0u || cfg->rows == 0u || cfg->rows > LCD_I2C_MAX_ROWS ||
        cfg->address > 0x7Fu) {
        return LCD_ERR_PARAM;
    }

    lcd->cfg = *cfg;
    if (lcd->cfg.timeout_ms == 0u) {
        lcd->cfg.timeout_ms = DEFAULT_TIMEOUT_MS;
    }
    lcd->backlight    = PIN_BACKLIGHT;
    lcd->display_ctrl = DISPLAY_ON;
    lcd->initialized  = false;

    /* The controller needs > 40 ms after power-up */
    delay(lcd, 50);

    /* "Initialisation by instruction" from the HD44780 data sheet: three times
     * 0x3 brings the controller into 8-bit mode from any state, then 0x2
     * switches to the 4-bit bus. */
    st = write_nibble(lcd, 0x30u, 0);
    if (st != LCD_OK) { return st; }
    delay(lcd, 5);
    st = write_nibble(lcd, 0x30u, 0);
    if (st != LCD_OK) { return st; }
    delay(lcd, 5);
    st = write_nibble(lcd, 0x30u, 0);
    if (st != LCD_OK) { return st; }
    delay(lcd, 1);
    st = write_nibble(lcd, 0x20u, 0);
    if (st != LCD_OK) { return st; }
    delay(lcd, 1);

    st = command(lcd, (uint8_t)(CMD_FUNCTION_SET | (cfg->rows > 1u ? FUNCTION_2LINE : 0u)));
    if (st != LCD_OK) { return st; }
    st = command(lcd, CMD_DISPLAY_CTRL);                 /* display off */
    if (st != LCD_OK) { return st; }
    st = command(lcd, CMD_CLEAR);
    if (st != LCD_OK) { return st; }
    delay(lcd, 2);
    st = command(lcd, CMD_ENTRY_MODE | ENTRY_INCREMENT);
    if (st != LCD_OK) { return st; }
    st = command(lcd, (uint8_t)(CMD_DISPLAY_CTRL | lcd->display_ctrl));
    if (st != LCD_OK) { return st; }

    lcd->initialized = true;
    return LCD_OK;
}

/* ------------------------------------------------------------------ */
/* Text output                                                         */
/* ------------------------------------------------------------------ */

lcd_status_t lcd_clear(lcd_i2c_t *lcd)
{
    lcd_status_t st = check(lcd);
    if (st != LCD_OK) { return st; }
    st = command(lcd, CMD_CLEAR);
    delay(lcd, 2);                       /* clear needs 1.52 ms */
    return st;
}

lcd_status_t lcd_home(lcd_i2c_t *lcd)
{
    lcd_status_t st = check(lcd);
    if (st != LCD_OK) { return st; }
    st = command(lcd, CMD_HOME);
    delay(lcd, 2);                       /* home needs 1.52 ms */
    return st;
}

lcd_status_t lcd_set_cursor(lcd_i2c_t *lcd, uint8_t col, uint8_t row)
{
    lcd_status_t st = check(lcd);
    if (st != LCD_OK) { return st; }
    if (col >= lcd->cfg.cols || row >= lcd->cfg.rows) {
        return LCD_ERR_PARAM;
    }
    return command(lcd, (uint8_t)(CMD_SET_DDRAM | (row_address(lcd, row) + col)));
}

lcd_status_t lcd_putc(lcd_i2c_t *lcd, char c)
{
    lcd_status_t st = check(lcd);
    if (st != LCD_OK) { return st; }
    return write_byte(lcd, (uint8_t)c, PIN_RS);
}

lcd_status_t lcd_puts(lcd_i2c_t *lcd, const char *text)
{
    lcd_status_t st = check(lcd);
    if (st != LCD_OK) { return st; }
    if (text == NULL) {
        return LCD_ERR_PARAM;
    }
    while (*text != '\0' && st == LCD_OK) {
        st = write_byte(lcd, (uint8_t)*text++, PIN_RS);
    }
    return st;
}

lcd_status_t lcd_print_at(lcd_i2c_t *lcd, uint8_t col, uint8_t row, const char *text)
{
    lcd_status_t st;

    if (text == NULL) {
        return LCD_ERR_PARAM;
    }
    st = lcd_set_cursor(lcd, col, row);
    for (uint8_t c = col; st == LCD_OK && c < lcd->cfg.cols && *text != '\0'; c++) {
        st = write_byte(lcd, (uint8_t)*text++, PIN_RS);
    }
    return st;
}

lcd_status_t lcd_write_line(lcd_i2c_t *lcd, uint8_t row, const char *text)
{
    lcd_status_t st;

    if (text == NULL) {
        return LCD_ERR_PARAM;
    }
    st = lcd_set_cursor(lcd, 0, row);
    for (uint8_t c = 0; st == LCD_OK && c < lcd->cfg.cols; c++) {
        char ch = ' ';
        if (*text != '\0') {
            ch = *text++;
        }
        st = write_byte(lcd, (uint8_t)ch, PIN_RS);
    }
    return st;
}

/* ------------------------------------------------------------------ */
/* Control                                                             */
/* ------------------------------------------------------------------ */

lcd_status_t lcd_backlight(lcd_i2c_t *lcd, bool on)
{
    uint8_t value;
    lcd_status_t st = check(lcd);
    if (st != LCD_OK) { return st; }
    lcd->backlight = on ? PIN_BACKLIGHT : 0u;
    value = lcd->backlight;              /* EN low: the display ignores this byte */
    return i2c_write(lcd, &value, 1);
}

static lcd_status_t set_display_bit(lcd_i2c_t *lcd, uint8_t bit, bool on)
{
    lcd_status_t st = check(lcd);
    if (st != LCD_OK) { return st; }
    if (on) {
        lcd->display_ctrl |= bit;
    } else {
        lcd->display_ctrl &= (uint8_t)~bit;
    }
    return command(lcd, (uint8_t)(CMD_DISPLAY_CTRL | lcd->display_ctrl));
}

lcd_status_t lcd_display(lcd_i2c_t *lcd, bool on) { return set_display_bit(lcd, DISPLAY_ON, on); }
lcd_status_t lcd_cursor(lcd_i2c_t *lcd, bool on)  { return set_display_bit(lcd, CURSOR_ON, on); }
lcd_status_t lcd_blink(lcd_i2c_t *lcd, bool on)   { return set_display_bit(lcd, BLINK_ON, on); }

lcd_status_t lcd_create_char(lcd_i2c_t *lcd, uint8_t slot, const uint8_t pattern[8])
{
    lcd_status_t st = check(lcd);
    if (st != LCD_OK) { return st; }
    if (slot > 7u || pattern == NULL) {
        return LCD_ERR_PARAM;
    }
    st = command(lcd, (uint8_t)(CMD_SET_CGRAM | (slot << 3)));
    for (uint8_t i = 0; st == LCD_OK && i < 8u; i++) {
        st = write_byte(lcd, (uint8_t)(pattern[i] & 0x1Fu), PIN_RS);
    }
    if (st == LCD_OK) {
        st = command(lcd, CMD_SET_DDRAM);        /* back to display memory, 0,0 */
    }
    return st;
}
