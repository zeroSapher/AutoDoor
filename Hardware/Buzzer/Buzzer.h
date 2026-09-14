/**
  ******************************************************************************
  * @file    Buzzer.h
  * @brief   Active buzzer patterns for door events.
  *
  * The part on the list is an ACTIVE buzzer (its own oscillator plus a driver
  * transistor), so a plain GPIO level is enough - no PWM and no timer, which is
  * why this costs one pin instead of two. The consequence is that pitch cannot be
  * varied; events are therefore distinguished by RHYTHM, which is the right
  * choice anyway: rhythm is understood regardless of tone, and it is what a user
  * actually learns to recognise.
  *
  * Pattern vocabulary
  * ------------------
  *   open    : two short beeps   "the door opened"
  *   close   : one long beep     "the door is closing" (a warning, so it is the
  *                                longest and least missable)
  *   fault   : three fast beeps  "something is wrong"
  *   estop   : one very long beep "emergency stop"
  *   warning : slow intermittent beeps while the door is moving
  ******************************************************************************
  */

#ifndef __BUZZER_H
#define __BUZZER_H

#include "stm32f10x.h"
#include <stdint.h>

/** Recognisable sound patterns. */
typedef enum
{
    BUZZ_NONE = 0,
    BUZZ_OPEN,          /* door reached the open limit            */
    BUZZ_CLOSE,         /* door is about to close                 */
    BUZZ_FAULT,         /* limit fault / travel timeout           */
    BUZZ_ESTOP,         /* emergency stop                         */
    BUZZ_CLOSED,        /* door reached the closed limit          */
    BUZZ_READY          /* power-on self test passed              */
} BuzzerPattern_t;

/** @brief Configure the buzzer pin, silent. */
void Buzzer_Init(void);

/** @brief Drive the pin directly, cancelling any running pattern. */
void Buzzer_Set(uint8_t on);

/** @brief Start a pattern, replacing whatever is playing. */
void Buzzer_Play(BuzzerPattern_t pattern);

/** @brief Stop immediately and go silent. */
void Buzzer_Stop(void);

/** @return 1 while a pattern is still playing. */
uint8_t Buzzer_IsBusy(void);

/** @brief Advance the pattern; call once per millisecond. */
void Buzzer_Tick1ms(void);

#endif /* __BUZZER_H */
