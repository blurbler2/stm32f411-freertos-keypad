/**
 * @file    lcd1602.h
 * @brief   HD44780 character LCD (TC2004A-01, 20x4) behind a PCF8574T I2C module,
 *          for FreeRTOS: I2C bus mutex, DMA transfer, gatekeeper task, frame buffer.
 *
 * Based on the original driver LCD1602.c/.h (Golinskiy / Paulis) and the lecture
 * "LCD-Display" (MCP21, 17.11.2026), slides 21-30.
 *
 * ---------------------------------------------------------------------------
 *  Structure (slide 31 "Portierbarkeit")
 * ---------------------------------------------------------------------------
 *  lcd1602.h  section 1  HARDWARE CONFIGURATION  <- the only part that changes
 *                        for another board / display / I2C peripheral
 *             section 2  public API (hardware independent)
 *  lcd1602.c  section 1  PORT: the few functions that call the ST HAL
 *             section 2  HD44780 protocol over PCF8574 (timing diagram)
 *             section 3  bus access: mutex + DMA + semaphore (slide 28)
 *             section 4  gatekeeper task with frame buffer (slide 29)
 *
 * ---------------------------------------------------------------------------
 *  How it works
 * ---------------------------------------------------------------------------
 *   any task --lcd1602_print()--> [queue] --> LCD gatekeeper task
 *                                               |  frame[]  = what should be shown
 *                                               |  shown[]  = what is on the glass
 *                                               |  every LCD1602_REFRESH_MS:
 *                                               |  send only the changed characters,
 *                                               |  one I2C transfer per row
 *                                               v
 *                       take bus mutex -> start DMA -> wait on semaphore (CPU free)
 *                       DMA/I2C IRQ "done" -> give semaphore -> release mutex
 *
 *  - lcd1602_print() and friends never block (queue timeout 0). The calling task
 *    continues with its next line of code at once.
 *  - Only the gatekeeper task talks to the display, so text from different tasks
 *    can never be mixed up ("Buchstabensalat").
 *  - The I2C bus is protected by a mutex that other I2C drivers can share
 *    (lcd1602_bus_mutex(), or pass your own mutex to lcd1602_start()).
 *  - Without a display the application keeps running; the gatekeeper tries
 *    again every LCD1602_RETRY_MS and shows the latest texts as soon as the
 *    display answers.
 *
 * ---------------------------------------------------------------------------
 *  Usage
 * ---------------------------------------------------------------------------
 *   // before vTaskStartScheduler() / osKernelStart():
 *   lcd1602_start(NULL);                     // NULL = create a new bus mutex
 *
 *   // in any task:
 *   lcd1602_write_line(0, "Temperature");    // whole row, rest filled with spaces
 *   lcd1602_print(1, 4, "21.5 C");           // row 1, column 4
 *
 * ---------------------------------------------------------------------------
 *  CubeMX settings for DMA
 * ---------------------------------------------------------------------------
 *   I2C1 > DMA Settings > Add: I2C1_TX, DMA1 Stream 6, Memory To Peripheral,
 *          Normal, Byte/Byte, memory increment on
 *   I2C1 > NVIC Settings: enable "I2C1 event interrupt" and
 *          "I2C1 error interrupt" (the HAL needs them for START/address and
 *          for the "transfer complete" callback, even with DMA)
 *   System Core > NVIC: preemption priority of DMA1 stream6, I2C1 event and
 *          I2C1 error >= 5 (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY),
 *          because the callback calls xSemaphoreGiveFromISR()
 *   Check in main.c that MX_DMA_Init() is called BEFORE MX_I2C1_Init().
 *
 *  lcd1602_start() checks these settings. If one is missing, the driver uses the
 *  blocking HAL_I2C_Master_Transmit() instead (only the gatekeeper task waits,
 *  the other tasks still never block) and lcd1602_dma_problem() says why.
 */
#ifndef LCD1602_H
#define LCD1602_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

/* ========================================================================= */
/*  SECTION 1: HARDWARE CONFIGURATION (the hardware-dependent part)          */
/* ========================================================================= */

#include "main.h"          /* ST HAL: I2C_HandleTypeDef, HAL_I2C_..., IRQn */
#include "FreeRTOS.h"
#include "semphr.h"

/* I2C peripheral (handle created by CubeMX in main.c) and its interrupts */
extern I2C_HandleTypeDef hi2c1;
#define LCD1602_I2C_HANDLE      (&hi2c1)
#define LCD1602_I2C_EV_IRQn     I2C1_EV_IRQn
#define LCD1602_I2C_ER_IRQn     I2C1_ER_IRQn

/* 7-bit address of the PCF8574 module, NOT shifted.
 * PCF8574T: 0x27 (A0..A2 open), PCF8574AT: 0x3F (slide 25) */
#define LCD1602_I2C_ADDR        0x27u

/* 1 = transfer with DMA when it is set up in CubeMX (see above), otherwise
 *     fall back to the blocking transfer
 * 0 = always the blocking HAL_I2C_Master_Transmit() */
#define LCD1602_USE_DMA         1

/* 1 = this driver defines HAL_I2C_MasterTxCpltCallback() and HAL_I2C_ErrorCallback().
 * 0 = you define them yourself and call lcd1602_i2c_tx_done_isr() /
 *     lcd1602_i2c_error_isr() from there (needed when other I2C drivers also
 *     use these HAL callbacks). */
#define LCD1602_DEFINE_HAL_CALLBACKS  1

/* Display geometry (TC2004A-01: 20 x 4) and DDRAM start address of every row
 * (slide 6 / 12: rows 1 and 3, 2 and 4 belong together internally) */
#define LCD1602_COLS            20u
#define LCD1602_ROWS            4u
#define LCD1602_ROW_OFFSETS     { 0x00u, 0x40u, 0x14u, 0x54u }

/* PCF8574 pin mapping (slide 24): P7..P4 = D7..D4, P3 = backlight, P2 = E,
 * P1 = R/W, P0 = RS */
#define LCD1602_PIN_RS          0x01u
#define LCD1602_PIN_RW          0x02u
#define LCD1602_PIN_E           0x04u
#define LCD1602_PIN_BACKLIGHT   0x08u

/* Gatekeeper task and timing */
#define LCD1602_TASK_PRIORITY   (tskIDLE_PRIORITY + 1)  /* low: display is not time-critical */
#define LCD1602_TASK_STACK      256u                    /* words */
#define LCD1602_QUEUE_LENGTH    8u
#define LCD1602_REFRESH_MS      200u   /* compare frame/shown and send changes (slide 29) */
#define LCD1602_BUS_TIMEOUT_MS  50u    /* max. wait for the bus mutex and for one transfer */
#define LCD1602_RETRY_MS        2000u  /* retry interval if the display does not answer */

/* ========================================================================= */
/*  SECTION 2: PUBLIC API (hardware independent)                             */
/* ========================================================================= */

typedef enum {
    LCD1602_OK = 0,
    LCD1602_ERR_PARAM,      /**< invalid row/column/pointer               */
    LCD1602_ERR_QUEUE_FULL, /**< gatekeeper busy, message dropped          */
    LCD1602_ERR_BUSY,       /**< bus mutex not available in time           */
    LCD1602_ERR_I2C,        /**< no ACK / bus error                        */
    LCD1602_ERR_TIMEOUT,    /**< transfer did not finish in time           */
    LCD1602_ERR_RTOS        /**< driver not started                        */
} lcd1602_status_t;

/**
 * Create the queue, the semaphores and the gatekeeper task, and decide between
 * DMA and blocking transfer. Call once, after MX_I2C1_Init() and before the
 * scheduler starts. The display itself is initialised by the gatekeeper task,
 * because the init sequence needs vTaskDelay() (slide 27).
 *
 * @param bus_mutex  mutex that protects the I2C bus and is shared with other
 *                   I2C drivers, or NULL to let the driver create one
 */
lcd1602_status_t lcd1602_start(SemaphoreHandle_t bus_mutex);

/* All of the following functions only put a message into the queue and return
 * at once (timeout 0, slide 29). They can be called from any task, not from
 * interrupts. */

/** Text at row/column (0-based). Clipped at the end of the row, no wrap. */
lcd1602_status_t lcd1602_print(uint8_t row, uint8_t col, const char *text);

/** Whole row: text from column 0, rest of the row filled with spaces. */
lcd1602_status_t lcd1602_write_line(uint8_t row, const char *text);

/** Clear the whole display (sent as spaces, no 2 ms "Clear Display" command). */
lcd1602_status_t lcd1602_clear(void);

/** Backlight on/off. */
lcd1602_status_t lcd1602_backlight(bool on);

/** true when the display answered and is initialised. */
bool lcd1602_is_ready(void);

/** true when the transfers use DMA (decided in lcd1602_start()). */
bool lcd1602_dma_active(void);

/** NULL when DMA is used, otherwise a short text why not (for a log message). */
const char *lcd1602_dma_problem(void);

/** The bus mutex, so that other I2C drivers can take it before using the bus. */
SemaphoreHandle_t lcd1602_bus_mutex(void);

/**
 * Called by the gatekeeper task when the display starts or stops answering
 * (the first result after the start is always reported). The driver contains
 * an empty weak version; define this function in the application to react,
 * e.g. to write a log message. Runs in the gatekeeper task: keep it short.
 */
void lcd1602_ready_changed(bool ready);

/** To be called from HAL_I2C_MasterTxCpltCallback() / HAL_I2C_ErrorCallback()
 *  when LCD1602_DEFINE_HAL_CALLBACKS is 0. */
void lcd1602_i2c_tx_done_isr(I2C_HandleTypeDef *hi2c);
void lcd1602_i2c_error_isr(I2C_HandleTypeDef *hi2c);

#ifdef __cplusplus
}
#endif

#endif /* LCD1602_H */
