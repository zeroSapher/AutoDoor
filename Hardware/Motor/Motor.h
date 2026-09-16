/**
  ******************************************************************************
  * @file    Motor.h
  * @brief   SG90 servo actuator: position commanded over hardware PWM, slewed.
  ******************************************************************************
  */

#ifndef __MOTOR_H
#define __MOTOR_H

#include "stm32f10x.h"
#include <stdint.h>

/** Direction of travel. The sign convention is fixed by the wiring, so if the
  *  door moves the wrong way swap the two leads (or swap these two macros). */
typedef enum
{
    MOTOR_DIR_STOP = 0,     /* coast: both low-side drivers off            */
    MOTOR_DIR_OPEN,         /* drive the door open                         */
    MOTOR_DIR_CLOSE,        /* drive the door closed                       */
    MOTOR_DIR_BRAKE         /* both low-side drivers on: active braking    */
} MotorDir_t;

/** Configure the IA/IB pins. Safe to call before Delay_Init(). */
void Motor_Init(void);

/**
  * @brief  Latch the motor off immediately.
  * @note   Intended for the limit-switch and emergency-stop paths: it writes
  *         IA=IB=0 with no ramp, so it is safe to call from an interrupt.
  *         It does not touch the GPIO clock, so it cannot fault.
  */
void Motor_EmergencyStop(void);

/** @brief Stop with the normal deceleration ramp (use for planned stops). */
void Motor_Stop(void);

/** @brief Apply active braking (IA=IB=1) - fast stop, shorted windings. */
void Motor_Brake(void);

/**
  * @brief  Start travelling in the given direction at the default duty.
  * @note   If the direction differs from the current one the call blocks for
  *         MOTOR_DEADTIME_MS with the bridge released. Skipping that guard is
  *         how an H-bridge gets destroyed, so it is enforced here rather than
  *         left to the caller.
  */
void Motor_Run(MotorDir_t dir);

/** @brief Set the target duty (percent, 0..100). Ramped, never stepped. */
void Motor_SetDuty(uint8_t percent);

/** @return The duty currently being applied (mid-ramp), in percent. */
uint8_t Motor_GetDuty(void);

/**
  * @brief  Set how long one full door move should take, in milliseconds.
  * @param  ms  MOTOR_TRAVEL_MIN_MS .. MOTOR_TRAVEL_MAX_MS.
  * @return 1 when applied, 0 when out of range - in which case nothing changes.
  * @note   On the servo build this IS the smoothness control: the slew advances
  *         (pulse span / travel time) per millisecond, so a longer travel time is
  *         a slower, gentler door. Safe to call mid-move: the pulse target is
  *         kept and only the rate changes. The door's travel watchdog follows
  *         this value, so a deliberately slowed move is not read as a stall.
  */
uint8_t Motor_SetTravelMs(uint16_t ms);

/** @return The travel time currently in use, in milliseconds. */
uint16_t Motor_GetTravelMs(void);

/** @return The direction currently commanded. */
MotorDir_t Motor_GetDir(void);

/** @return 1 while a ramp is still running. */
uint8_t Motor_IsRamping(void);

/** @return 1 when the motor is stopped or braking. */
uint8_t Motor_IsIdle(void);

/**
  * @brief  Advance the software PWM and the speed ramp.
  * @note   Must be called from the 1 ms SysTick handler. This is the "timer
  *         interrupt drives motor PWM" requirement: the carrier is synthesised
  *         by accumulating phase once per millisecond.
  */
void Motor_Tick1ms(void);

#endif /* __MOTOR_H */
