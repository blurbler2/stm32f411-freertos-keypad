/*
 * Minimal FreeRTOS mock for the host (PC) unit test of lcd1602.
 * Only the types, macros and functions the driver uses. The functions are
 * implemented in test_lcd1602.c. 1 tick = 1 ms.
 */
#ifndef FREERTOS_MOCK_H
#define FREERTOS_MOCK_H

#include <stddef.h>
#include <stdint.h>

typedef uint32_t      TickType_t;
typedef long          BaseType_t;
typedef unsigned long UBaseType_t;
typedef uint32_t      StackType_t;

#define pdFALSE                 ((BaseType_t)0)
#define pdTRUE                  ((BaseType_t)1)
#define pdPASS                  pdTRUE
#define pdMS_TO_TICKS(ms)       ((TickType_t)(ms))
#define tskIDLE_PRIORITY        ((UBaseType_t)0)
#define portYIELD_FROM_ISR(x)   ((void)(x))

#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5

#endif /* FREERTOS_MOCK_H */
