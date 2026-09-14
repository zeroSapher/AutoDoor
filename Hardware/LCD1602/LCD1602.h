/**
  ******************************************************************************
  * @file    LCD1602.h
  * @brief   HD44780 1602 character LCD, 4-bit parallel interface.
  *
  * PURPOSE
  * -------
  * Simulation only. Proteus has no SSD1306 model, so the simulation build shows
  * status on a 1602 instead. App/Display.c picks the backend at compile time, so
  * nothing above the display layer knows the difference.
  *
  * The 4-bit interface is used rather than 8-bit because it needs 6 pins instead
  * of 11 - and on a part where every GPIO is already committed, pin count is the
  * binding constraint. Each byte goes out as two nibbles, high first.
  *
  * Timing comes from Delay_us(), so this driver depends on the microsecond time
  * base being initialised before LCD1602_Init() is called.
  ******************************************************************************
  */

#ifndef __LCD1602_H
#define __LCD1602_H

#include "stm32f10x.h"
#include <stdint.h>

/** @brief Configure the GPIOs and run the HD44780 initialisation sequence. */
void LCD1602_Init(void);

/** @brief Clear the display and return the cursor to home. */
void LCD1602_Clear(void);

/**
  * @brief  Write a NUL-terminated string at a row/column.
  * @param  row  0 or 1.
  * @param  col  0..15.
  * @note   Text longer than the remaining row is truncated, not wrapped: a 1602
  *         has no line-wrap concept that would not corrupt the other row.
  */
void LCD1602_WriteString(uint8_t row, uint8_t col, const char *text);

#endif /* __LCD1602_H */
