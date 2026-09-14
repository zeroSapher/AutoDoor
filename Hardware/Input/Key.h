/**
  ******************************************************************************
  * @file    Key.h
  * @brief   Four panel keys: start/stop, mode, manual open, manual close/e-stop.
  *
  * Short press and long press are distinguished because KEY4 doubles as the
  * emergency stop: a short press closes the door, holding it kills motion
  * immediately. Requiring a hold for the destructive action is standard practice -
  * it stops an accidental tap from halting the door mid-travel.
  ******************************************************************************
  */

#ifndef __KEY_H
#define __KEY_H

#include "stm32f10x.h"
#include <stdint.h>

/** Logical key identifiers, in panel order. */
typedef enum
{
    KEY_ID_START = 0,   /* start / stop the system                */
    KEY_ID_MODE,        /* toggle AUTO / MANUAL                   */
    KEY_ID_OPEN,        /* manual open                            */
    KEY_ID_CLOSE_ESTOP, /* short: close   long: emergency stop    */
    KEY_ID_COUNT
} KeyId_t;

/** @brief Configure the four key inputs as EXTI lines at key priority. */
void Key_Init(void);

/** @brief Discard pending edges and latch current levels. */
void Key_Reset(void);

/** @brief Confirm edges and run the long-press timer; call once per millisecond. */
void Key_Tick1ms(void);

/** @brief Record an edge from the EXTI9_5 handler. */
void Key_IrqHandler(KeyId_t id);

/** @return 1 exactly once when the key was short-pressed. */
uint8_t Key_TakeShortPress(KeyId_t id);

/** @return 1 exactly once when the key was held past KEY_LONGPRESS_MS. */
uint8_t Key_TakeLongPress(KeyId_t id);

/** @return 1 while the key is physically held (debounced). */
uint8_t Key_IsDown(KeyId_t id);

#endif /* __KEY_H */
