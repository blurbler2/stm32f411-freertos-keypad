/*
 * Mock of the STM32 HAL for host (PC) unit tests of the keypad and LCD libraries.
 * The Makefile force-includes this file (-include main.h). It defines the
 * include guard of the real Core/Inc/main.h, so the real one (and with it the
 * STM32 HAL) is skipped when keypad.h includes "main.h".
 */
#ifndef __MAIN_H
#define __MAIN_H

#include <stdint.h>

typedef struct { int id; } GPIO_TypeDef;
typedef enum { GPIO_PIN_RESET = 0, GPIO_PIN_SET } GPIO_PinState;
typedef struct { uint32_t Pin, Mode, Pull, Speed, Alternate; } GPIO_InitTypeDef;

#define GPIO_MODE_INPUT       0x00u
#define GPIO_MODE_OUTPUT_OD   0x11u
#define GPIO_NOPULL           0u
#define GPIO_PULLUP           1u
#define GPIO_SPEED_FREQ_LOW   0u

extern GPIO_TypeDef mock_port_rows, mock_port_cols;
#define GPIOD (&mock_port_rows)
#define GPIOE (&mock_port_cols)

void          HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState state);
GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *port, uint16_t pin);
void          HAL_GPIO_Init(GPIO_TypeDef *port, GPIO_InitTypeDef *init);

/* ---- I2C (used by lcd_i2c and lcd1602) ---- */
typedef enum { HAL_OK = 0, HAL_ERROR, HAL_BUSY, HAL_TIMEOUT } HAL_StatusTypeDef;
typedef struct {
    int   id;
    void *hdmatx;        /* DMA handle linked by CubeMX (lcd1602 checks it) */
} I2C_HandleTypeDef;

HAL_StatusTypeDef HAL_I2C_Master_Transmit(I2C_HandleTypeDef *hi2c, uint16_t dev_address,
                                          uint8_t *data, uint16_t size, uint32_t timeout);
HAL_StatusTypeDef HAL_I2C_IsDeviceReady(I2C_HandleTypeDef *hi2c, uint16_t dev_address,
                                        uint32_t trials, uint32_t timeout);
void              HAL_Delay(uint32_t ms);

/* ---- only used by lcd1602 ---- */
HAL_StatusTypeDef HAL_I2C_Master_Transmit_DMA(I2C_HandleTypeDef *hi2c, uint16_t dev_address,
                                              uint8_t *data, uint16_t size);
HAL_StatusTypeDef HAL_I2C_Init(I2C_HandleTypeDef *hi2c);
HAL_StatusTypeDef HAL_I2C_DeInit(I2C_HandleTypeDef *hi2c);

typedef enum { I2C1_EV_IRQn = 31, I2C1_ER_IRQn = 32 } IRQn_Type;
uint32_t NVIC_GetEnableIRQ(IRQn_Type irq);
uint32_t NVIC_GetPriority(IRQn_Type irq);

#define __WEAK __attribute__((weak))     /* from CMSIS cmsis_compiler.h */

#endif /* __MAIN_H */
