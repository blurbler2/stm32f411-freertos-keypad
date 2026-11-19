/**
 * @file    keypad_rtos.c
 * @brief   FreeRTOS wrapper for the key-matrix driver. See keypad_rtos.h.
 */
#include "keypad_rtos.h"

static void keypad_rtos_task(void *pvParameters)
{
    keypad_rtos_t *h = (keypad_rtos_t *)pvParameters;
    TickType_t last_wake = xTaskGetTickCount();
    keypad_event_t evt;

    for (;;) {
        keypad_scan(&h->keypad, (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS));

        /* copy all new events into the queue (never block) */
        while (keypad_get_event(&h->keypad, &evt)) {
            if (xQueueSend(h->queue, &evt, 0) != pdPASS) {
                h->queue_full++;
            }
        }

        /* strictly periodic, independent of how long the scan took */
        vTaskDelayUntil(&last_wake, h->period);
    }
}

BaseType_t keypad_rtos_start(keypad_rtos_t *h,
                             const keypad_config_t *cfg,
                             QueueHandle_t queue,
                             UBaseType_t queue_len,
                             uint32_t period_ms,
                             UBaseType_t priority,
                             configSTACK_DEPTH_TYPE stack_words)
{
    if (h == NULL || cfg == NULL) {
        return pdFAIL;
    }
    if (keypad_init(&h->keypad, cfg) != KEYPAD_OK) {
        return pdFAIL;
    }

    if (queue == NULL) {
        queue = xQueueCreate(queue_len > 0u ? queue_len : 10u, sizeof(keypad_event_t));
        if (queue == NULL) {
            return pdFAIL;   /* heap too small? check configTOTAL_HEAP_SIZE */
        }
    }
    h->queue      = queue;
    h->period     = pdMS_TO_TICKS(period_ms) > 0 ? pdMS_TO_TICKS(period_ms) : 1;
    h->queue_full = 0;

    return xTaskCreate(keypad_rtos_task, "Keypad", stack_words, h, priority, &h->task);
}

BaseType_t keypad_rtos_wait_event(keypad_rtos_t *h, keypad_event_t *evt, TickType_t timeout)
{
    return xQueueReceive(h->queue, evt, timeout);
}

char keypad_rtos_wait_key(keypad_rtos_t *h, TickType_t timeout)
{
    keypad_event_t evt;
    TickType_t start = xTaskGetTickCount();

    for (;;) {
        TickType_t elapsed = xTaskGetTickCount() - start;
        TickType_t remaining = (timeout == portMAX_DELAY) ? portMAX_DELAY
                             : (elapsed >= timeout ? 0 : timeout - elapsed);

        if (xQueueReceive(h->queue, &evt, remaining) != pdPASS) {
            return KEYPAD_NO_KEY;
        }
        if (evt.type == KEYPAD_EVT_PRESS) {
            return evt.key;
        }
    }
}
