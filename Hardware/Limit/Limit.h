/**
  ******************************************************************************
  * @file    Limit.h
  * @brief   Door limit switches (KW12-3 micro switches, NC contacts).
  *
  * SAFETY ROLE
  * -----------
  * These are the highest-priority inputs in the system. Reaching a limit means
  * "stop now or the mechanism breaks", which must always win over a presence
  * sensor's "I would like the door to open".
  *
  * The interrupt is only the SECOND of three protection layers:
  *
  *   1. Hardware  - the NC contacts are wired in series in the 5 V -> L9110S VM
  *                  motor supply path, so a limit physically removes motor
  *                  power even if the MCU is hung. This is the only layer that
  *                  can claim "the door cannot overrun".
  *   2. Interrupt - this module: fastest possible reaction and event logging.
  *   3. Software  - the state machine's travel timeout in Door.c.
  *
  * Note the contact sense: NC means the switch conducts while the door is away
  * from the limit. Tripping the switch OPENS the contact, so the MCU pin is
  * pulled up by its internal pull-up and reads HIGH. "Triggered" therefore means
  * "pin high", and any broken wire also reads as triggered - which is the whole
  * point of using the NC contact.
  ******************************************************************************
  */

#ifndef __LIMIT_H
#define __LIMIT_H

#include "stm32f10x.h"
#include <stdint.h>

/** @brief Configure both limit pins as EXTI inputs with the top priority. */
void Limit_Init(void);

/** @brief Re-read both pins and discard any pending edge. */
void Limit_Reset(void);

/**
  * @brief  Confirm a pending edge after the debounce window has elapsed.
  * @note   Called from the main loop (or the 1 ms tick) once per millisecond.
  *         A pending edge only becomes a real event if the pin still reads the
  *         same way after LIMIT_DEBOUNCE_MS, which rejects contact bounce
  *         without doing any timing work inside the interrupt.
  */
void Limit_Tick1ms(void);

/**
  * @brief  EXTIM-based edge capture.
  * @param  openEdge  1 when the interrupt was on the "door open" limit line.
  * @note   Called from the EXTI0/EXTI1 handlers in Core/stm32f10x_it.c.
  *         Deliberately does nothing but timestamp the edge - an ISR must be
  *         short, and the pin is left to be re-read after the debounce window.
  */
void Limit_IrqHandler(uint8_t openEdge);

/** @return 1 exactly once when the "door open" limit has newly triggered. */
uint8_t Limit_TakeOpenEvent(void);
/** @return 1 exactly once when the "door closed" limit has newly triggered. */
uint8_t Limit_TakeCloseEvent(void);

/** @return 1 while the door is against the open limit (debounced level). */
uint8_t Limit_IsOpen(void);
/** @return 1 while the door is against the closed limit (debounced level). */
uint8_t Limit_IsClosed(void);

/** @return 1 when both limits read triggered at once, which is impossible for
  *          a real mechanism and therefore indicates a wiring or sensor fault. */
uint8_t Limit_IsFaulted(void);

#endif /* __LIMIT_H */
