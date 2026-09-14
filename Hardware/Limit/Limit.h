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
  *   1. Hardware  - the switch's NC contact is wired in series in the 5 V ->
  *                  L9110S VM motor supply path, so a limit physically removes
  *                  motor power even if the MCU is hung. This is the only layer
  *                  that can claim "the door cannot overrun".
  *   2. Interrupt - this module: fastest possible reaction and event logging.
  *   3. Software  - the state machine's travel timeout in Door.c.
  *
  * SENSE (see Limit_Init for the full wiring note)
  * -----------------------------------------------
  * The switch is SPDT and BOTH contacts are used, on two electrically separate
  * circuits: NC carries the 5 V motor supply, NO carries the readback. Readback
  * is therefore active LOW - the NO contact closes onto GND at the limit:
  *
  *   door away from limit : pin HIGH (10k pull-up to 3V3)
  *   door AT the limit    : pin LOW  (NO contact to GND)
  *
  * activeLevel is configured as 0 to match. A broken readback wire now reads
  * "not at limit" rather than "at limit", so the state machine waits for the 5 s
  * travel watchdog instead of stopping immediately - the hardware power cut still
  * protects the mechanism, so the cost is diagnostics rather than safety.
  ******************************************************************************
  */

#ifndef __LIMIT_H
#define __LIMIT_H

#include "stm32f10x.h"
#include <stdint.h>

/*
 * BENCH MODE
 * ----------
 * With AUTODOOR_NO_LIMITS set (see Core/main.h), every function below keeps its
 * contract but reads a simulated door position instead of the pins. The API is
 * deliberately unchanged, so Door.c needs no conditional compilation at all: the
 * state machine cannot tell the difference, which is what makes the bench test
 * meaningful.
 *
 * What changes in that mode: no pin is configured, no EXTI line is claimed,
 * Limit_IrqHandler() is unreachable, and Limit_IsFaulted() always reports healthy.
 * The hardware NC layer is unaffected - it does not involve the MCU - but it only
 * protects the mechanism if it is actually wired.
 */

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
  * @brief  Limit edge captured by EXTI - and the only place the motor is cut.
  * @param  openEdge  1 when the interrupt was on the "door open" limit line.
  * @return 1 when this edge cut the motor, 0 otherwise.
  * @note   Called from the EXTI0/EXTI1 handlers in Core/stm32f10x_it.c, which do
  *         nothing else: the decision to stop belongs here, beside the polarity
  *         it depends on.
  * @note   A limit line is configured Rising_Falling, so this is entered on BOTH
  *         edges. Only the ASSERT edge (pin low) may cut the motor, and only when
  *         the motor is not already driving away from that switch. Cutting on the
  *         release edge stranded the door a few millimetres off the switch, with
  *         nothing to restart the motor - the whole reason this logic is here
  *         rather than inline in the handler.
  */
uint8_t Limit_IrqHandler(uint8_t openEdge);

/** @return 1 exactly once when the "door open" limit has newly triggered. */
uint8_t Limit_TakeOpenEvent(void);
/** @return 1 exactly once when the "door closed" limit has newly triggered. */
uint8_t Limit_TakeCloseEvent(void);

/** @return 1 while the door is against the open limit (debounced level). */
uint8_t Limit_IsOpen(void);
/** @return 1 while the door is against the closed limit (debounced level). */
uint8_t Limit_IsClosed(void);

/** @return 1 when both limits read triggered at once, which is impossible for
  *          a real mechanism and therefore indicates a wiring or sensor fault.
  *          Always 0 in bench mode - see the note above. */
uint8_t Limit_IsFaulted(void);

/** @return 1 when the position came from the bench-mode simulation rather than
  *          from real switches. Used by the boot banner and STATUS? so an
  *          unsafe build cannot be mistaken for a normal one. */
uint8_t Limit_IsSimulated(void);

#endif /* __LIMIT_H */
