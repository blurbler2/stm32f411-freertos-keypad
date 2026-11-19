# Host unit tests for the libraries (mock HAL, no hardware needed)
CC ?= gcc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Wconversion -include main.h -I../../Core/Inc
SRC = ../../Core/Src
INC = ../../Core/Inc

test: test_keypad test_lcd
	./test_keypad
	./test_lcd

test_keypad: test_keypad.c $(SRC)/keypad.c $(INC)/keypad.h main.h
	$(CC) $(CFLAGS) test_keypad.c $(SRC)/keypad.c -o $@

test_lcd: test_lcd.c $(SRC)/lcd_i2c.c $(INC)/lcd_i2c.h main.h
	$(CC) $(CFLAGS) test_lcd.c $(SRC)/lcd_i2c.c -o $@

clean:
	rm -f test_keypad test_lcd

.PHONY: test clean
