/**
 * @file    app.h
 * @brief   Application layer: tasks, queues and semaphores of the demo.
 *
 * All application code lives in app.c so that STM32CubeMX can regenerate
 * main.c at any time. main.c only calls App_Init() once, inside the
 * "USER CODE BEGIN RTOS_THREADS" section.
 */
#ifndef APP_H
#define APP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* ---- Message sent to the LED task (the struct from the whiteboard) ---- */

/** Who sent the message. */
typedef enum {
    APP_SENDER_KEYPAD = 1,
    APP_SENDER_BUTTON = 2
} app_sender_t;

/** Which LED the message refers to. */
typedef enum {
    APP_LED_GREEN = 0,   /* LD4, PD12 */
    APP_LED_ORANGE,      /* LD3, PD13 */
    APP_LED_RED,         /* LD5, PD14 */
    APP_LED_BLUE,        /* LD6, PD15 */
    APP_LED_COUNT,
    APP_LED_ALL = 0xFF
} app_led_t;

typedef enum {
    APP_LED_OFF = 0,
    APP_LED_ON,
    APP_LED_TOGGLE
} app_led_action_t;

typedef struct {
    uint8_t sender;      /* app_sender_t      */
    uint8_t led;         /* app_led_t         */
    uint8_t action;      /* app_led_action_t  */
} app_led_msg_t;

/* ---- Message sent to the display task ---- */
#define APP_LCD_COLS   16
#define APP_LCD_ROWS   2

typedef struct {
    uint8_t row;                      /* 0 .. APP_LCD_ROWS-1          */
    char    text[APP_LCD_COLS + 1];   /* zero-terminated              */
} app_display_msg_t;

/* ---- Trace tags (logic analyser channel = tag, see mytrace.h) ---- */
#define APP_TAG_KEYPAD_TASK   1
#define APP_TAG_KEY_TASK      2
#define APP_TAG_LED_TASK      3
#define APP_TAG_BUTTON_TASK   4

/**
 * Create all queues, semaphores, mutexes and tasks of the application.
 * Call once after osKernelInitialize() and before osKernelStart().
 */
void App_Init(void);

/** Thread-safe text output on USART2 (mutex protected). Not for ISRs. */
void App_Log(const char *text);

/**
 * Show a text in one row of the LCD (0 = top row). The text is copied into
 * the display queue and written by the DisplayTask, so this can be called
 * from any task and never waits for the display. Not for ISRs.
 * Longer texts are clipped to the width of the display.
 */
void App_Display(uint8_t row, const char *text);

#ifdef __cplusplus
}
#endif

#endif /* APP_H */
