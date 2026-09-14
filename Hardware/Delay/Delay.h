/**
  ******************************************************************************
  * @file    Delay.h
  * @brief   Millisecond and microsecond blocking delays.
  *
  * Two independent time bases are provided:
  *   - SysTick  : 1 ms periodic interrupt, drives Delay_ms().
  *   - DWT CYCCNT (falling back to TIM4): free-running cycle/us counter, which
  *     drives Delay_us() and Delay_GetUsTick().
  *
  * The two are deliberately separate. Timing a DHT11 response pulse needs a
  * monotonically increasing microsecond counter, while the system tick needs a
  * periodic interrupt; sharing one SysTick reload register between the two is a
  * common source of intermittent sensor failures.
  ******************************************************************************
  */

#ifndef __DELAY_H
#define __DELAY_H

#include "stm32f10x.h"
#include <stdint.h>

/** Initialise SysTick (1 ms tick) and the microsecond time base. */
void Delay_Init(void);

/** Blocking delay of the given number of milliseconds. */
void Delay_ms(uint32_t ms);

/** Blocking delay of the given number of microseconds. */
void Delay_us(uint32_t us);

/**
  * @brief  Monotonic free-running microsecond counter.
  * @note   Wraps every ~71 minutes at 1 us resolution. Compare successive
  *         readings with an unsigned subtraction so a wrap is harmless.
  */
uint32_t Delay_GetUsTick(void);

/** Non-blocking millisecond tick, incremented from the SysTick handler. */
extern volatile uint32_t g_msTick;

#endif /* __DELAY_H */
