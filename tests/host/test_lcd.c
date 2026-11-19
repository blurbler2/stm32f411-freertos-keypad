/*
 * Host unit test for the LCD library (no hardware needed).
 * The mock I2C functions feed every byte into a small HD44780 simulator that
 * sits behind a simulated PCF8574, so the test checks what would really end
 * up in the display memory.
 *
 * Build and run:  make -C tests/host
 */
#include "lcd_i2c.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ---------------- HD44780 + PCF8574 simulator ---------------- */

#define SIM_ADDR 0x27u

static struct {
    uint8_t  port;            /* last byte on the PCF8574 outputs */
    int      four_bit;        /* bus mode */
    int      have_high;       /* waiting for the low nibble */
    uint8_t  high;
    int      two_line, display_on, cursor_on, blink_on, increment;
    int      cgram_mode;
    uint8_t  addr;            /* DDRAM or CGRAM address */
    uint8_t  ddram[0x80];
    uint8_t  cgram[64];
    int      func_8bit_count; /* number of 0x3 nibbles seen during init */
} sim;

static int      i2c_fail;         /* 1 = every transfer fails */
static uint32_t delay_total;      /* sum of all requested delays */
static uint32_t delay_before_first_write;
static int      writes;

static void sim_reset(void)
{
    memset(&sim, 0, sizeof(sim));
    memset(sim.ddram, ' ', sizeof(sim.ddram));
    i2c_fail = 0; delay_total = 0; delay_before_first_write = 0; writes = 0;
}

static void sim_execute(uint8_t value, int rs)
{
    if (rs) {                                   /* data write */
        if (sim.cgram_mode) {
            sim.cgram[sim.addr & 0x3Fu] = value;
            sim.addr = (uint8_t)((sim.addr + 1u) & 0x3Fu);
        } else {
            sim.ddram[sim.addr & 0x7Fu] = value;
            sim.addr = (uint8_t)((sim.addr + 1u) & 0x7Fu);
        }
        return;
    }
    if (value & 0x80u)      { sim.cgram_mode = 0; sim.addr = value & 0x7Fu; }
    else if (value & 0x40u) { sim.cgram_mode = 1; sim.addr = value & 0x3Fu; }
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
    else if (value & 0x02u) { sim.cgram_mode = 0; sim.addr = 0; }
    else if (value & 0x01u) { memset(sim.ddram, ' ', sizeof(sim.ddram)); sim.cgram_mode = 0; sim.addr = 0; }
}

/* A new byte appears on the PCF8574 outputs; the LCD latches on EN falling */
static void sim_port_write(uint8_t value)
{
    int falling = (sim.port & 0x04u) && !(value & 0x04u);
    uint8_t latched = sim.port;                 /* data valid while EN was high */
    sim.port = value;

    if (!falling) { return; }
    assert((latched & 0x02u) == 0);             /* RW must stay low (write only) */

    int rs = latched & 0x01u;
    uint8_t nibble = latched & 0xF0u;

    if (!sim.four_bit) {
        int was_8bit = !sim.four_bit;
        sim_execute(nibble, rs);                /* D3..D0 are not connected */
        if (was_8bit && sim.four_bit) { sim.have_high = 0; }
    } else if (!sim.have_high) {
        sim.high = nibble; sim.have_high = 1;
    } else {
        sim.have_high = 0;
        sim_execute((uint8_t)(sim.high | (nibble >> 4)), rs);
    }
}

/* ---------------- HAL mocks ---------------- */

HAL_StatusTypeDef HAL_I2C_Master_Transmit(I2C_HandleTypeDef *hi2c, uint16_t dev_address,
                                          uint8_t *data, uint16_t size, uint32_t timeout)
{
    (void)hi2c;
    assert(timeout > 0);
    if (i2c_fail || dev_address != (SIM_ADDR << 1)) { return HAL_ERROR; }
    if (writes++ == 0) { delay_before_first_write = delay_total; }
    for (uint16_t i = 0; i < size; i++) { sim_port_write(data[i]); }
    return HAL_OK;
}

HAL_StatusTypeDef HAL_I2C_IsDeviceReady(I2C_HandleTypeDef *hi2c, uint16_t dev_address,
                                        uint32_t trials, uint32_t timeout)
{
    (void)hi2c; (void)trials; (void)timeout;
    return (!i2c_fail && dev_address == (SIM_ADDR << 1)) ? HAL_OK : HAL_ERROR;
}

void HAL_Delay(uint32_t ms) { delay_total += ms; }

static int custom_delay_calls;
static void custom_delay(uint32_t ms) { custom_delay_calls++; delay_total += ms; }

/* Compare a row of the simulated display with the expected text */
static void expect_row(uint8_t row, const char *expected)
{
    const uint8_t *p = &sim.ddram[row == 0 ? 0x00 : 0x40];
    if (memcmp(p, expected, strlen(expected)) != 0) {
        printf("row %u is \"%.16s\", expected \"%s\"\n", row, (const char *)p, expected);
        assert(0);
    }
}

int main(void)
{
    I2C_HandleTypeDef hi2c = { 1 };
    lcd_i2c_t lcd;
    lcd_i2c_config_t cfg = { .hi2c = &hi2c, .address = SIM_ADDR, .cols = 16, .rows = 2 };

    /* 1) address search */
    sim_reset();
    assert(lcd_i2c_find(&hi2c) == SIM_ADDR);
    i2c_fail = 1;
    assert(lcd_i2c_find(&hi2c) == 0);

    /* 2) calls before init and parameter checks */
    memset(&lcd, 0, sizeof(lcd));
    assert(lcd_puts(&lcd, "x") == LCD_ERR_NOT_INIT);
    assert(lcd_init(NULL, &cfg) == LCD_ERR_PARAM);
    lcd_i2c_config_t bad = cfg; bad.rows = 5;
    assert(lcd_init(&lcd, &bad) == LCD_ERR_PARAM);

    /* 3) no display on the bus -> error, not initialised */
    sim_reset(); i2c_fail = 1;
    assert(lcd_init(&lcd, &cfg) == LCD_ERR_I2C);
    assert(lcd_clear(&lcd) == LCD_ERR_NOT_INIT);

    /* 4) init sequence */
    sim_reset();
    assert(lcd_init(&lcd, &cfg) == LCD_OK);
    assert(delay_before_first_write >= 40);      /* power-up wait */
    assert(sim.func_8bit_count == 3);            /* three times 0x3, then 4-bit */
    assert(sim.four_bit && sim.two_line && sim.display_on && sim.increment);
    assert(!sim.cursor_on && !sim.blink_on);
    assert(sim.port & 0x08u);                    /* backlight on */
    expect_row(0, "                ");

    /* 5) text at the cursor */
    assert(lcd_puts(&lcd, "Hello") == LCD_OK);
    assert(lcd_putc(&lcd, '!') == LCD_OK);
    expect_row(0, "Hello!          ");

    /* 6) positioning, second row starts at DDRAM 0x40 */
    assert(lcd_print_at(&lcd, 3, 1, "abc") == LCD_OK);
    expect_row(1, "   abc          ");
    assert(lcd_set_cursor(&lcd, 16, 0) == LCD_ERR_PARAM);
    assert(lcd_set_cursor(&lcd, 0, 2) == LCD_ERR_PARAM);

    /* 7) print_at clips at the end of the row and does not spill over */
    assert(lcd_print_at(&lcd, 13, 0, "WXYZ12") == LCD_OK);
    expect_row(0, "Hello!       WXY");
    assert(sim.ddram[16] == ' ');

    /* 8) write_line pads with spaces and clips long text */
    assert(lcd_write_line(&lcd, 1, "Task 1: 00042") == LCD_OK);
    expect_row(1, "Task 1: 00042   ");
    assert(lcd_write_line(&lcd, 0, "0123456789ABCDEFGHIJ") == LCD_OK);
    expect_row(0, "0123456789ABCDEF");
    assert(sim.ddram[16] == ' ');
    assert(lcd_write_line(&lcd, 0, "") == LCD_OK);
    expect_row(0, "                ");

    /* 9) clear and home */
    assert(lcd_clear(&lcd) == LCD_OK);
    expect_row(1, "                ");
    assert(lcd_set_cursor(&lcd, 5, 1) == LCD_OK && sim.addr == 0x45);
    assert(lcd_home(&lcd) == LCD_OK && sim.addr == 0);

    /* 10) display control, backlight */
    assert(lcd_cursor(&lcd, true) == LCD_OK && sim.cursor_on && sim.display_on);
    assert(lcd_blink(&lcd, true) == LCD_OK && sim.blink_on && sim.cursor_on);
    assert(lcd_cursor(&lcd, false) == LCD_OK && !sim.cursor_on && sim.blink_on);
    assert(lcd_display(&lcd, false) == LCD_OK && !sim.display_on);
    assert(lcd_display(&lcd, true) == LCD_OK && sim.display_on);
    assert(lcd_backlight(&lcd, false) == LCD_OK && !(sim.port & 0x08u));
    assert(lcd_puts(&lcd, "dark") == LCD_OK && !(sim.port & 0x08u));   /* stays off */
    expect_row(0, "dark");
    assert(lcd_backlight(&lcd, true) == LCD_OK && (sim.port & 0x08u));

    /* 11) custom character goes to CGRAM, display memory untouched */
    static const uint8_t heart[8] = { 0x00, 0x0A, 0x1F, 0x1F, 0x0E, 0x04, 0x00, 0xFF };
    assert(lcd_create_char(&lcd, 2, heart) == LCD_OK);
    assert(memcmp(&sim.cgram[16], heart, 7) == 0 && sim.cgram[23] == 0x1F);
    assert(!sim.cgram_mode && sim.addr == 0);
    expect_row(0, "dark");
    assert(lcd_create_char(&lcd, 8, heart) == LCD_ERR_PARAM);
    assert(lcd_putc(&lcd, 2) == LCD_OK && sim.ddram[0] == 2);

    /* 12) I2C error during operation is reported */
    i2c_fail = 1;
    assert(lcd_puts(&lcd, "x") == LCD_ERR_I2C);
    i2c_fail = 0;

    /* 13) custom delay function is used instead of HAL_Delay; 20x4 row addresses */
    sim_reset();
    cfg.delay_ms = custom_delay; cfg.cols = 20; cfg.rows = 4;
    assert(lcd_init(&lcd, &cfg) == LCD_OK && custom_delay_calls > 0);
    assert(lcd_set_cursor(&lcd, 0, 2) == LCD_OK && sim.addr == 0x14);
    assert(lcd_set_cursor(&lcd, 19, 3) == LCD_OK && sim.addr == 0x67);

    puts("lcd_i2c: all tests passed");
    return 0;
}
