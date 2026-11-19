/**
 * @file    mytrace.c
 * @brief   Task-switch trace on GPIO pins. See mytrace.h.
 */
#include "mytrace.h"
#include "main.h"

/* Called from the scheduler (inside PendSV): must be short and must not call
 * any FreeRTOS API. Writing BSRR is atomic, so no locking is needed. */

static GPIO_TypeDef *const trace_port[TRACE_NUM_CHANNELS] = {
    TRACE_1_GPIO_Port, TRACE_2_GPIO_Port, TRACE_3_GPIO_Port, TRACE_4_GPIO_Port
};
static const uint16_t trace_pin[TRACE_NUM_CHANNELS] = {
    TRACE_1_Pin, TRACE_2_Pin, TRACE_3_Pin, TRACE_4_Pin
};

void Trace_TaskSwitchedIn(int tag)
{
    if (tag >= 1 && tag <= TRACE_NUM_CHANNELS) {
        trace_port[tag - 1]->BSRR = trace_pin[tag - 1];                    /* set   */
    }
}

void Trace_TaskSwitchedOut(int tag)
{
    if (tag >= 1 && tag <= TRACE_NUM_CHANNELS) {
        trace_port[tag - 1]->BSRR = (uint32_t)trace_pin[tag - 1] << 16u;   /* reset */
    }
}
