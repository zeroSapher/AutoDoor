/**
  ******************************************************************************
  * @file    Debounce.h
  * @brief   Shared edge debouncer for mechanical inputs (limits, keys, sensors).
  *
  * WHY THIS EXISTS
  * ---------------
  * A mechanical contact does not close cleanly: for 5-20 ms it chatters, and a
  * naive edge interrupt fires dozens of times for one press. Three input modules
  * in this project (limit switches, keys, presence sensors) need exactly the same
  * treatment, and debounce logic that is written three times is debounce logic
  * that is subtly wrong in two of them.
  *
  * THE PATTERN
  * -----------
  * Work is split so that the interrupt stays as short as possible:
  *
  *   ISR   : record that an edge happened and start the timer. No timing loops,
  *           no GPIO re-reads, nothing that can take microseconds.
  *   Tick  : while the timer runs, keep restarting it if the line is still
  *           bouncing; when it finally expires, re-read the pin. The edge counts
  *           as real only if the pin still reads the expected level.
  *
  * The ISR therefore costs a handful of cycles, and the expensive part happens
  * in the 1 ms tick where a few microseconds do not matter.
  ******************************************************************************
  */

#ifndef __DEBOUNCE_H
#define __DEBOUNCE_H

#include "stm32f10x.h"
#include <stdint.h>

/** State of one debounced input. Treat as opaque; use the accessors below. */
typedef struct
{
    GPIO_TypeDef *port;
    uint16_t      pin;
    uint8_t       activeLevel;    /* level that means "asserted" (1 or 0)      */
    uint8_t       debounceMs;     /* stable time required before accepting     */

    volatile uint8_t pending;     /* settle window running                      */
    volatile uint16_t timerMs;    /* countdown while the line is unsettled      */

    uint8_t       raw;            /* last raw pin reading                       */
    uint8_t       stable;         /* debounced level                            */
    uint8_t       event;          /* set on a confirmed transition, one-shot     */
} Debounce_t;

/** @brief Bind a debouncer to a pin. Does not touch the GPIO configuration. */
void Debounce_Init(Debounce_t *d, GPIO_TypeDef *port, uint16_t pin,
                   uint8_t activeLevel, uint8_t debounceMs);

/**
  * @brief  Record an edge. Call from the EXTI interrupt.
  * @note   Kept minimal on purpose: flag plus timer, nothing else.
  */
void Debounce_IrqEdge(Debounce_t *d);

/** @brief Advance the debounce timer; call once per millisecond. */
void Debounce_Tick1ms(Debounce_t *d);

/** @brief Sample the pin immediately, bypassing debounce (power-on state). */
void Debounce_SyncNow(Debounce_t *d);

/** @return The debounced level: 1 when asserted, 0 otherwise. */
uint8_t Debounce_IsActive(const Debounce_t *d);

/** @return 1 exactly once after an asserted transition, then clears. */
uint8_t Debounce_TakeEvent(Debounce_t *d);

/** @return 1 while an edge is waiting to be confirmed. */
uint8_t Debounce_IsPending(const Debounce_t *d);

#endif /* __DEBOUNCE_H */
