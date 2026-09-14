/**
  ******************************************************************************
  * @file    StatusLed.h
  * @brief   Single status LED showing door state at a glance.
  *
  * WHY THE PATTERN IS STATE-DEPENDENT
  * ----------------------------------
  * The console and the OLED both carry far more detail, but neither is visible
  * from across a room. The LED is the only indicator that tells a passer-by
  * whether the door is safe to approach, so its pattern is chosen to answer
  * exactly that question:
  *
  *   IDLE      off               - safe, door latched
  *   OPENING   solid on          - in motion, do not obstruct
  *   OPEN      slow blink        - open, about to close
  *   CLOSING   fast blink        - in motion the other way, keep clear
  *   STOPPED   double flash      - fault or emergency stop, needs attention
  *
  * A slow blink therefore means "open and counting down" while a fast blink
  * means "moving" - the two states a person in the doorway most needs to tell
  * apart at a glance.
  ******************************************************************************
  */

#ifndef __STATUS_LED_H
#define __STATUS_LED_H

#include "stm32f10x.h"
#include <stdint.h>
#include "Door.h"

/** @brief Configure the LED pin, initially off. */
void StatusLed_Init(void);

/**
  * @brief  Tell the LED what to display.
  * @param  state     Current door state.
  * @param  faulted   1 when a fault is latched (changes the STOPPED pattern).
  * @note   Safe to call every loop iteration: the pattern only recomputes when
  *         the state or fault flag actually changes.
  */
void StatusLed_Set(DoorState_t state, uint8_t faulted);

/** @brief Advance the blink phase; call once per millisecond. */
void StatusLed_Tick1ms(void);

#endif /* __STATUS_LED_H */
