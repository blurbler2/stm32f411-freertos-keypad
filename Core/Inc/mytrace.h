/**
 * @file    mytrace.h
 * @brief   Task-switch trace on GPIO pins for a logic analyser.
 *
 * Every traced task gets a tag 1..4 (vTaskSetApplicationTaskTag). While that
 * task is running, the matching TRACE pin is HIGH:
 *
 *      tag 1 -> TRACE_1 (PE11)      tag 3 -> TRACE_3 (PE13)
 *      tag 2 -> TRACE_2 (PE12)      tag 4 -> TRACE_4 (PE14)
 *
 * The hooks are called by the FreeRTOS kernel through traceTASK_SWITCHED_IN /
 * traceTASK_SWITCHED_OUT (see the USER CODE section in FreeRTOSConfig.h).
 * Untagged tasks (idle, timer, defaultTask) have tag 0 and are ignored.
 *
 * This header is included from FreeRTOSConfig.h, so keep it free of any
 * FreeRTOS or HAL includes.
 */
#ifndef INC_MYTRACE_H_
#define INC_MYTRACE_H_

#ifdef __cplusplus
extern "C" {
#endif

#define TRACE_NUM_CHANNELS  4

void Trace_TaskSwitchedIn(int tag);
void Trace_TaskSwitchedOut(int tag);

#ifdef __cplusplus
}
#endif

#endif /* INC_MYTRACE_H_ */
