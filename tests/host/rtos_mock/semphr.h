/* FreeRTOS semaphore mock for the lcd1602 host test (see FreeRTOS.h) */
#ifndef SEMPHR_MOCK_H
#define SEMPHR_MOCK_H

#include "FreeRTOS.h"

typedef struct { int count; } StaticSemaphore_t;   /* 1 = available */
typedef StaticSemaphore_t *SemaphoreHandle_t;

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *buffer);
SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *buffer);
BaseType_t        xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks);
BaseType_t        xSemaphoreGive(SemaphoreHandle_t sem);
BaseType_t        xSemaphoreGiveFromISR(SemaphoreHandle_t sem, BaseType_t *woken);

#endif /* SEMPHR_MOCK_H */
