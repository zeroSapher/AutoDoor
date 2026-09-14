/**
  ******************************************************************************
  * @file    Sensor.h
  * @brief   Presence detection at the door - outside (entering) and inside
  *          (leaving).
  *
  * CURRENT IMPLEMENTATION
  * ----------------------
  * Presence is simulated with two push buttons, because the available parts list
  * contains no sensor that can actually detect a person: the TCRT5000 in it is a
  * ~1 cm reflective line sensor and is useless at door range.
  *
  * This is an interface-compatible substitution, not a throwaway stub. A real
  * E18-D80NK optical sensor and a button are both "active-low signal into an EXTI
  * line, then debounced", so swapping one in later means editing the pin macros
  * in main.h and nothing else. The state machine, logging and reporting code all
  * see the same events either way.
  *
  * Sensor polarity note for a future optical sensor: the E18-D80NK has an
  * open-collector NPN output, which pulls low when it detects something - so it
  * is active low against a pull-up, exactly like the buttons here.
  ******************************************************************************
  */

#ifndef __SENSOR_H
#define __SENSOR_H

#include "stm32f10x.h"
#include <stdint.h>

/** @brief Configure both presence inputs as EXTI lines at sensor priority. */
void Sensor_Init(void);

/** @brief Discard any pending edge and latch the current levels. */
void Sensor_Reset(void);

/** @brief Confirm pending edges; call once per millisecond. */
void Sensor_Tick1ms(void);

/**
  * @brief  Record an edge from the EXTI9_5/EXTI0/EXTI1 handlers.
  * @param  outsideEdge 1 for the outside sensor, 0 for the inside one.
  */
void Sensor_IrqHandler(uint8_t outsideEdge);

/** @return 1 exactly once when someone was detected outside (an arrival). */
uint8_t Sensor_TakeOutsideEvent(void);
/** @return 1 exactly once when someone was detected inside (a departure). */
uint8_t Sensor_TakeInsideEvent(void);

/** @return 1 while something is present outside. */
uint8_t Sensor_IsOutsideActive(void);
/** @return 1 while something is present inside. */
uint8_t Sensor_IsInsideActive(void);

#endif /* __SENSOR_H */
