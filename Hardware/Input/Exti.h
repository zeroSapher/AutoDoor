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
  * @return 0 on success, non-zero when the pin number is out of range.
  * @note   The AFIO clock is enabled on the first call and left on; it is needed
  *         for the whole runtime.
  */
uint8_t Exti_ConfigPin(GPIO_TypeDef *port, uint16_t pin, EXTITrigger_TypeDef trigger,
                       uint8_t preemptPrio, uint8_t subPrio);

#endif /* __EXTI_H */
