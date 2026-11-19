/* FreeRTOS queue mock for the lcd1602 host test (see FreeRTOS.h) */
#ifndef QUEUE_MOCK_H
#define QUEUE_MOCK_H

#include "FreeRTOS.h"

typedef struct {
    uint8_t    *storage;
    UBaseType_t item_size, length, head, count;
} StaticQueue_t;
typedef StaticQueue_t *QueueHandle_t;

QueueHandle_t xQueueCreateStatic(UBaseType_t length, UBaseType_t item_size,
                                 uint8_t *storage, StaticQueue_t *buffer);
BaseType_t    xQueueSend(QueueHandle_t queue, const void *item, TickType_t ticks);
BaseType_t    xQueueReceive(QueueHandle_t queue, void *item, TickType_t ticks);

#endif /* QUEUE_MOCK_H */
