/* FreeRTOS task mock for the lcd1602 host test (see FreeRTOS.h) */
#ifndef TASK_MOCK_H
#define TASK_MOCK_H

#include "FreeRTOS.h"

typedef struct { int unused; } StaticTask_t;
typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);

TaskHandle_t xTaskCreateStatic(TaskFunction_t code, const char *name, uint32_t stack_depth,
                               void *param, UBaseType_t priority, StackType_t *stack,
                               StaticTask_t *tcb);
void         vTaskDelay(TickType_t ticks);
TickType_t   xTaskGetTickCount(void);

#endif /* TASK_MOCK_H */
