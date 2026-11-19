/**
 * @file    keypad_rtos.h
 * @brief   Optional FreeRTOS wrapper for keypad.h
 *
 * Starts a scan task that polls the keypad periodically and copies every
 * event (keypad_event_t) into a FreeRTOS queue. Any other task can then block
 * on xQueueReceive() and wait for key presses - no polling in the application.
 *
 *    [keypad task] --keypad_event_t--> [queue] ---> [your task(s)]
 */
#ifndef KEYPAD_RTOS_H
#define KEYPAD_RTOS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "keypad.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

typedef struct {
    keypad_t      keypad;       /* the actual keypad instance                  */
    QueueHandle_t queue;        /* destination queue for keypad_event_t        */
    TickType_t    period;       /* scan period in ticks                        */
    TaskHandle_t  task;         /* handle of the scan task                     */
    uint32_t      queue_full;   /* events dropped because the queue was full   */
} keypad_rtos_t;

/**
 * Initialise the keypad and start the scan task.
 *
 * @param h           handle (must be static/global, NOT on the stack!)
 * @param cfg         keypad configuration (must stay valid)
 * @param queue       destination queue; NULL = create one here with queue_len
 * @param queue_len   queue length if it is created here (e.g. 10)
 * @param period_ms   scan period, 10 ms recommended
 * @param priority    task priority (e.g. tskIDLE_PRIORITY + 3)
 * @param stack_words stack size in words (128 is normally enough)
 * @return pdPASS on success
 */
BaseType_t keypad_rtos_start(keypad_rtos_t *h,
                             const keypad_config_t *cfg,
                             QueueHandle_t queue,
                             UBaseType_t queue_len,
                             uint32_t period_ms,
                             UBaseType_t priority,
                             configSTACK_DEPTH_TYPE stack_words);

/** Queue the events are read from. */
static inline QueueHandle_t keypad_rtos_queue(const keypad_rtos_t *h) { return h->queue; }

/**
 * Wait for the next event.
 * @param timeout ticks (portMAX_DELAY = wait forever)
 * @return pdTRUE if an event was received
 */
BaseType_t keypad_rtos_wait_event(keypad_rtos_t *h, keypad_event_t *evt, TickType_t timeout);

/** Wait for the next PRESS and return its character (KEYPAD_NO_KEY on timeout). */
char keypad_rtos_wait_key(keypad_rtos_t *h, TickType_t timeout);

#ifdef __cplusplus
}
#endif

#endif /* KEYPAD_RTOS_H */
