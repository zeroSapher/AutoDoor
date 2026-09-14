/**
  ******************************************************************************
  * @file    stm32f10x_conf.h
  * @brief   Standard Peripheral Library configuration for this project.
  *
  * Only the modules actually used by the project are enabled. Enabling fewer
  * modules keeps build times and code size down; every enabled module is
  * compiled from Drivers/StdPeriph.
  ******************************************************************************
  */

#ifndef __STM32F10x_CONF_H
#define __STM32F10x_CONF_H

/* Includes ------------------------------------------------------------------*/
#include "stm32f10x.h"

/* Uncomment the line below to enable peripheral header file inclusion */
#include "stm32f10x_rcc.h"     /* Reset & clock control - used by every driver */
#include "stm32f10x_gpio.h"    /* GPIO - I2C, motor, limits, keys, buzzer      */
#include "stm32f10x_exti.h"    /* EXTI - limit switches, sensors, keys         */
#include "stm32f10x_usart.h"   /* USART1 - console                             */
#include "stm32f10x_tim.h"     /* TIM - microsecond timing fallback            */
#include "misc.h"              /* NVIC priority grouping - interrupt priorities */

/*===========================================================================*/
/*  Assert configuration                                                      */
/*===========================================================================*/

/**
  * The StdPeriph library normally expands assert_param() into
  *   ((void)0, (void)((EXPR) ? 1 : 0))
  * when assertions are disabled. The trailing cast expression produces a
  * "-Wunused-value" warning on GCC, so it is replaced with a plain no-op.
  * Define USE_FULL_ASSERT below to re-enable the runtime checks.
  */
//#define USE_FULL_ASSERT    1

#ifdef  USE_FULL_ASSERT
  void assert_failed(uint8_t* file, uint32_t line);
  #define assert_param(expr) ((expr) ? (void)0 : assert_failed((uint8_t *)__FILE__, __LINE__))
#else
  #define assert_param(expr) ((void)0)
#endif /* USE_FULL_ASSERT */

#endif /* __STM32F10x_CONF_H */
