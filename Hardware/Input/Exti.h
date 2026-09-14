/**
  ******************************************************************************
  * @file    Exti.h
  * @brief   Shared EXTI line configuration.
  *
  * All three input modules (limit switches, presence sensors, keys) need the same
  * setup - enable AFIO, map a pin to its EXTI line, pick the trigger edge, set the
  * NVIC priority. Doing it in one place keeps the priority table visible instead
  * of scattered across modules, and the priority ordering is a safety property:
  * it must be reviewable at a glance.
  ******************************************************************************
  */

#ifndef __EXTI_H
#define __EXTI_H

#include "stm32f10x.h"
#include <stdint.h>

/**
  * @brief  Claim an EXTI line for a GPIO pin and enable its interrupt.
  * @param  port        GPIO port the pin belongs to.
  * @param  pin         Pin number (GPIO_Pin_x); its low nibble selects EXTIx.
  * @param  trigger     One of EXTI_Trigger_Rising / _Falling / _Rising_Falling.
  * @param  preemptPrio NVIC preemption priority (lower number = higher urgency).
  * @param  subPrio     NVIC sub priority.
  * @return 0 on success, 2 when the pin mask is ambiguous, 3 when it is empty,
  *         4 when the line already belongs to a DIFFERENT port.
  * @note   Return 4 is the important one. On STM32F1 an EXTI line belongs to a pin
  *         NUMBER, and AFIO can route only one port to it, so two modules asking
  *         for the same line do not share it - the later caller takes it and the
  *         earlier one silently loses its interrupt. That is exactly how the limit
  *         switches ended up with no EXTI at all (PA0/PB0 and PA1/PB1 both
  *         claimed lines 0 and 1). The claim is now recorded and a conflict is
  *         counted; main() reports Exti_ConflictCount() once the console is up.
  *         Core/main.h also has a compile-time check that the pin numbers are
  *         distinct, which is the cheaper place to catch it.
  * @note   The AFIO clock is enabled on the first call and left on; it is needed
  *         for the whole runtime.
  */
uint8_t Exti_ConfigPin(GPIO_TypeDef *port, uint16_t pin, EXTITrigger_TypeDef trigger,
                       uint8_t preemptPrio, uint8_t subPrio);

/**
  * @brief  How many EXTI claims were rejected because the line was taken.
  * @note   Non-zero means the pin map has two inputs on one line number and one
  *         of them has no interrupt. It is a wiring/configuration bug, not a
  *         runtime condition.
  */
uint8_t Exti_ConflictCount(void);

#endif /* __EXTI_H */
