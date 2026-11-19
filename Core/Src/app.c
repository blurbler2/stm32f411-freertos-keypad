/**
 * @file    app.c
 * @brief   Application layer of the FreeRTOS keypad demo.
 *
 *                      +-----------+
 *   4x4 keypad ------> | KeypadTask|  (inside keypad_rtos.c, scans every 10 ms)
 *                      +-----+-----+
 *                            | keypad_event_t        (key queue)
 *                            v
 *                      +-----------+   app_led_msg_t  +---------+
 *                      |  KeyTask  | ---------------> |         |
 *                      +-----------+    (LED queue)   | LedTask | --> LD3..LD6
 *   user button --ISR--> semaphore --> +----------+   |         |
 *                                      |ButtonTask| ->|         |
 *                                      +----------+   +---------+
 *
 *   KeyTask / ButtonTask --App_Display()--> lcd1602 driver: queue --> LCD gatekeeper task
 *                                           --> I2C1 (bus mutex, DMA) --> 20x4 LCD
 *
 *   KeyTask also prints every key event on USART2 (mutex protected).
 *   The LCD gatekeeper task inside lcd1602.c is the only task that talks to the
 *   display; App_Display() only puts the text into its queue and never waits.
 *
 * Key assignment
 *   1..4   toggle green / orange / red / blue LED
 *   A      all LEDs on
 *   B      all LEDs off
 *   D      running light: one step per press, auto-repeat while held
 *   other  only logged on the UART
 *   blue user button (B1): toggle all LEDs
 */
#include "app.h"
#include "main.h"
#include "keypad_rtos.h"
#include "lcd1602.h"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

#define KEYPAD_SCAN_PERIOD_MS   10u
#define KEY_QUEUE_LEN           10u
#define LED_QUEUE_LEN           10u
#define BUTTON_DEBOUNCE_MS      50u

#define PRIO_KEYPAD_TASK        (tskIDLE_PRIORITY + 3)
#define PRIO_KEY_TASK           (tskIDLE_PRIORITY + 2)
#define PRIO_LED_TASK           (tskIDLE_PRIORITY + 2)
#define PRIO_BUTTON_TASK        (tskIDLE_PRIORITY + 2)
/* The LCD gatekeeper task runs at LCD1602_TASK_PRIORITY (idle + 1, lcd1602.h):
 * the display is slow and not urgent */

/* Keypad wiring - the pin names come from the CubeMX labels in main.h */
static const keypad_pin_t kp_rows[4] = {
    { KP_ROW1_GPIO_Port, KP_ROW1_Pin }, { KP_ROW2_GPIO_Port, KP_ROW2_Pin },
    { KP_ROW3_GPIO_Port, KP_ROW3_Pin }, { KP_ROW4_GPIO_Port, KP_ROW4_Pin }
};
static const keypad_pin_t kp_cols[4] = {
    { KP_COL1_GPIO_Port, KP_COL1_Pin }, { KP_COL2_GPIO_Port, KP_COL2_Pin },
    { KP_COL3_GPIO_Port, KP_COL3_Pin }, { KP_COL4_GPIO_Port, KP_COL4_Pin }
};

static const keypad_config_t kp_cfg = {
    .rows           = kp_rows,
    .num_rows       = 4,
    .cols           = kp_cols,
    .num_cols       = 4,
    .keymap         = KEYPAD_KEYMAP_4X4,
    .debounce_ms    = 20,
    .hold_ms        = 800,
    .repeat_ms      = 200,
    .configure_gpio = false,   /* pins are configured by MX_GPIO_Init() */
    .settle_loops   = 50,
    .callback       = NULL,
    .user           = NULL
};

/* ------------------------------------------------------------------ */
/* Private data                                                        */
/* ------------------------------------------------------------------ */

extern UART_HandleTypeDef huart2;        /* created by CubeMX in main.c */

static keypad_rtos_t     keypad;         /* keypad instance + scan task + key queue */
static QueueHandle_t     ledQueue;       /* app_led_msg_t                           */
static SemaphoreHandle_t buttonSem;      /* given from the EXTI interrupt           */
static SemaphoreHandle_t uartMutex;      /* protects USART2 (shared resource)       */

static GPIO_TypeDef *const led_port[APP_LED_COUNT] = {
    LED_GREEN_GPIO_Port, LED_ORANGE_GPIO_Port, LED_RED_GPIO_Port, LED_BLUE_GPIO_Port
};
static const uint16_t led_pin[APP_LED_COUNT] = {
    LED_GREEN_Pin, LED_ORANGE_Pin, LED_RED_Pin, LED_BLUE_Pin
};

static void KeyTask(void *pvParameters);
static void LedTask(void *pvParameters);
static void ButtonTask(void *pvParameters);

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

void App_Log(const char *text)
{
    if (uartMutex == NULL || text == NULL) {
        return;
    }
    /* The mutex keeps the messages of different tasks from being mixed */
    if (xSemaphoreTake(uartMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        HAL_UART_Transmit(&huart2, (uint8_t *)text, (uint16_t)strlen(text), 100);
        xSemaphoreGive(uartMutex);
    }
}

void App_Display(uint8_t row, const char *text)
{
    /* Copied into the queue of the LCD driver. If it is full, the text is
     * dropped - the caller never waits for the display. */
    (void)lcd1602_write_line(row, text);
}

static void send_led(app_sender_t sender, app_led_t led, app_led_action_t action)
{
    app_led_msg_t msg;
    msg.sender = (uint8_t)sender;
    msg.led    = (uint8_t)led;
    msg.action = (uint8_t)action;
    /* The content of msg is copied into the queue. Do not block if it is full. */
    (void)xQueueSend(ledQueue, &msg, 0);
}

static void log_key_event(const keypad_event_t *evt)
{
    static const char *const names[] = { "PRESS", "RELEASE", "HOLD", "REPEAT" };
    char line[32] = "Key ";
    size_t n = strlen(line);

    line[n++] = evt->key;
    line[n++] = ' ';
    line[n]   = '\0';
    strcat(line, names[evt->type]);
    App_Display(1, line);               /* second row of the LCD */
    strcat(line, "\r\n");
    App_Log(line);
}

static void apply_led(GPIO_TypeDef *port, uint16_t pin, uint8_t action)
{
    switch (action) {
    case APP_LED_ON:     HAL_GPIO_WritePin(port, pin, GPIO_PIN_SET);   break;
    case APP_LED_OFF:    HAL_GPIO_WritePin(port, pin, GPIO_PIN_RESET); break;
    case APP_LED_TOGGLE: HAL_GPIO_TogglePin(port, pin);                break;
    default:                                                           break;
    }
}

/* ------------------------------------------------------------------ */
/* Init                                                                */
/* ------------------------------------------------------------------ */

void App_Init(void)
{
    TaskHandle_t handle = NULL;

    uartMutex = xSemaphoreCreateMutex();
    buttonSem = xSemaphoreCreateBinary();
    ledQueue  = xQueueCreate(LED_QUEUE_LEN, sizeof(app_led_msg_t));
    if (uartMutex == NULL || buttonSem == NULL || ledQueue == NULL) {
        Error_Handler();          /* heap too small? -> configTOTAL_HEAP_SIZE */
    }

    /* Keypad library: creates the key queue and the scan task */
    if (keypad_rtos_start(&keypad, &kp_cfg, NULL, KEY_QUEUE_LEN, KEYPAD_SCAN_PERIOD_MS,
                          PRIO_KEYPAD_TASK, 128) != pdPASS) {
        Error_Handler();
    }
    vTaskSetApplicationTaskTag(keypad.task, (TaskHookFunction_t)APP_TAG_KEYPAD_TASK);

    if (xTaskCreate(KeyTask, "KeyTask", 256, NULL, PRIO_KEY_TASK, &handle) != pdPASS) {
        Error_Handler();
    }
    vTaskSetApplicationTaskTag(handle, (TaskHookFunction_t)APP_TAG_KEY_TASK);

    if (xTaskCreate(LedTask, "LedTask", 128, NULL, PRIO_LED_TASK, &handle) != pdPASS) {
        Error_Handler();
    }
    vTaskSetApplicationTaskTag(handle, (TaskHookFunction_t)APP_TAG_LED_TASK);

    if (xTaskCreate(ButtonTask, "ButtonTask", 128, NULL, PRIO_BUTTON_TASK, &handle) != pdPASS) {
        Error_Handler();
    }
    vTaskSetApplicationTaskTag(handle, (TaskHookFunction_t)APP_TAG_BUTTON_TASK);

    /* LCD driver: creates its queue, the I2C bus mutex and the gatekeeper task
     * (not traced: there are only four trace channels). The display itself is
     * initialised by the gatekeeper task once the scheduler runs. */
    if (lcd1602_start(NULL) != LCD1602_OK) {
        Error_Handler();
    }
    App_Display(0, "Keypad demo");
    App_Display(1, "Press a key");
    App_Display(3, lcd1602_dma_active() ? "I2C: DMA" : "I2C: blocking");
}

/* ------------------------------------------------------------------ */
/* Tasks                                                               */
/* ------------------------------------------------------------------ */

/** Receives key events, logs them and turns them into LED messages. */
static void KeyTask(void *pvParameters)
{
    keypad_event_t evt;
    uint8_t chase = 0;                    /* position of the running light */
    (void)pvParameters;

    App_Log("\r\nFreeRTOS keypad demo ready\r\n");

    for (;;) {
        /* Blocks until the keypad task has sent an event - no polling here */
        if (keypad_rtos_wait_event(&keypad, &evt, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        log_key_event(&evt);

        if (evt.type == KEYPAD_EVT_PRESS) {
            switch (evt.key) {
            case '1': send_led(APP_SENDER_KEYPAD, APP_LED_GREEN,  APP_LED_TOGGLE); break;
            case '2': send_led(APP_SENDER_KEYPAD, APP_LED_ORANGE, APP_LED_TOGGLE); break;
            case '3': send_led(APP_SENDER_KEYPAD, APP_LED_RED,    APP_LED_TOGGLE); break;
            case '4': send_led(APP_SENDER_KEYPAD, APP_LED_BLUE,   APP_LED_TOGGLE); break;
            case 'A': send_led(APP_SENDER_KEYPAD, APP_LED_ALL,    APP_LED_ON);     break;
            case 'B': send_led(APP_SENDER_KEYPAD, APP_LED_ALL,    APP_LED_OFF);    break;
            default:                                                               break;
            }
        }

        /* Running light: one step per press and per auto-repeat */
        if (evt.key == 'D' &&
            (evt.type == KEYPAD_EVT_PRESS || evt.type == KEYPAD_EVT_REPEAT)) {
            send_led(APP_SENDER_KEYPAD, APP_LED_ALL, APP_LED_OFF);
            send_led(APP_SENDER_KEYPAD, (app_led_t)chase, APP_LED_ON);
            chase = (uint8_t)((chase + 1u) % APP_LED_COUNT);
        }
    }
}

/** Only this task touches the LEDs. It executes the messages from the queue. */
static void LedTask(void *pvParameters)
{
    app_led_msg_t msg;
    (void)pvParameters;

    for (;;) {
        if (xQueueReceive(ledQueue, &msg, portMAX_DELAY) != pdPASS) {
            continue;
        }
        if (msg.led == APP_LED_ALL) {
            for (uint8_t i = 0; i < APP_LED_COUNT; i++) {
                apply_led(led_port[i], led_pin[i], msg.action);
            }
        } else if (msg.led < APP_LED_COUNT) {
            apply_led(led_port[msg.led], led_pin[msg.led], msg.action);
        }
    }
}

/** Waits for the semaphore given by the button interrupt. */
static void ButtonTask(void *pvParameters)
{
    (void)pvParameters;

    for (;;) {
        if (xSemaphoreTake(buttonSem, portMAX_DELAY) == pdTRUE) {
            send_led(APP_SENDER_BUTTON, APP_LED_ALL, APP_LED_TOGGLE);
            App_Log("Button B1\r\n");
            App_Display(1, "Button B1");

            /* Simple debounce: ignore the bounces of the next 50 ms */
            vTaskDelay(pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS));
            (void)xSemaphoreTake(buttonSem, 0);
        }
    }
}

/**
 * Called by the LCD gatekeeper task when the display starts or stops answering
 * (replaces the empty weak version in lcd1602.c). The application also runs
 * without a display; the driver then tries again every 2 s.
 */
void lcd1602_ready_changed(bool ready)
{
    char line[80];

    if (!ready) {
        App_Log("LCD not found\r\n");
        return;
    }
    if (lcd1602_dma_active()) {
        App_Log("LCD found, I2C with DMA\r\n");
        return;
    }
    /* One App_Log() call, so that no other message ends up in the middle */
    strcpy(line, "LCD found, I2C blocking: ");
    strncat(line, lcd1602_dma_problem(), sizeof(line) - strlen(line) - 3u);
    strcat(line, "\r\n");
    App_Log(line);
}

/* ------------------------------------------------------------------ */
/* Interrupt callback                                                  */
/* ------------------------------------------------------------------ */

/**
 * EXTI callback (called from EXTI0_IRQHandler via the HAL).
 * The interrupt priority must be numerically >= 5
 * (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY), otherwise the FromISR
 * functions must not be used.
 */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    if (GPIO_Pin == BTN_USER_Pin && buttonSem != NULL) {
        xSemaphoreGiveFromISR(buttonSem, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}
