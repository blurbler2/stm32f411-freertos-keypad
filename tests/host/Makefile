# Host unit tests for the libraries (mock HAL, no hardware needed)
CC ?= gcc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Wconversion -include main.h -I../../Core/Inc
SRC = ../../Core/Src
INC = ../../Core/Inc

test: test_keypad test_lcd test_lcd1602
	./test_keypad
	./test_lcd
	./test_lcd1602

test_keypad: test_keypad.c $(SRC)/keypad.c $(INC)/keypad.h main.h
	$(CC) $(CFLAGS) test_keypad.c $(SRC)/keypad.c -o $@

test_lcd: test_lcd.c $(SRC)/lcd_i2c.c $(INC)/lcd_i2c.h main.h
	$(CC) $(CFLAGS) test_lcd.c $(SRC)/lcd_i2c.c -o $@

# lcd1602.c is included by the test (white-box); rtos_mock/ replaces FreeRTOS
test_lcd1602: test_lcd1602.c test_lcd1602_hook.c $(SRC)/lcd1602.c $(INC)/lcd1602.h main.h $(wildcard rtos_mock/*.h)
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=200809L -Irtos_mock test_lcd1602.c test_lcd1602_hook.c -o $@

clean:
	rm -f test_keypad test_lcd test_lcd1602

.PHONY: test clean
