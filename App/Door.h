/**
  ******************************************************************************
  * @file    Door.h
  * @brief   Door state machine: the control core of the system.
  *
  * STATES
  * ------
  *   INIT       power-on self test
  *   IDLE       closed and latched
  *   OPENING    travelling open
  *   OPEN       at the open limit, auto-close countdown running
  *   CLOSING    travelling closed
  *   REVERSING  was closing, someone was detected, now returning to open
  *   STOPPED    emergency stop or fault; motion inhibited
  *
  * WHY THE LIMIT SWITCH IS CHECKED AS A LEVEL, NOT ONLY AS AN EVENT
  * ---------------------------------------------------------------
  * Limit transitions are delivered as one-shot events, but the state machine
  * also polls Limit_IsOpen()/Limit_IsClosed() every iteration. Two reasons:
  *
  *   1. Redundancy. If an edge is lost (a bounce that never settles, a missed
  *      ISR), the level still drives the machine to the correct state.
  *   2. Start-up. On power-up the door may already be sitting on a limit, which
  *      produces no edge at all - only the level tells the truth about where the
  *      door actually is.
  *
  * SAFETY ORDERING
  * ---------------
  * Every update begins with the emergency-stop check and the limit handling,
  * before any "would you like the door to open" logic. Motion commands are
  * therefore always subordinate to the reasons to stop.
  ******************************************************************************
  */

#ifndef __DOOR_H
#define __DOOR_H

#include "stm32f10x.h"
#include <stdint.h>

/** Operating mode. */
typedef enum
{
    DOOR_MODE_AUTO = 0,     /* sensors drive the door      */
    DOOR_MODE_MANUAL = 1    /* only the keys drive the door */
} DoorMode_t;

/** Machine state. Persisted values matter - see Log.c doorState field. */
typedef enum
{
    DOOR_STATE_INIT = 0,
    DOOR_STATE_IDLE,
    DOOR_STATE_OPENING,
    DOOR_STATE_OPEN,
    DOOR_STATE_CLOSING,
    DOOR_STATE_REVERSING,
    DOOR_STATE_STOPPED
} DoorState_t;

/** Fault codes, shown on the display and reported over the console. */
typedef enum
{
    DOOR_FAULT_NONE = 0,
    DOOR_FAULT_OPEN_TIMEOUT,    /* did not reach the open limit in time   */
    DOOR_FAULT_CLOSE_TIMEOUT,   /* did not reach the closed limit in time */
    DOOR_FAULT_REVERSE_TIMEOUT, /* was reversing but never got clear      */
    DOOR_FAULT_LIMIT_CONFLICT,  /* both limits asserted: wiring fault     */
    DOOR_FAULT_ESTOP            /* emergency stop                         */
} DoorFault_t;

/**
  * @brief  Callbacks so the state machine needs no knowledge of the console,
  *         the display or the log format.
  * @note   All are optional; pass NULL for any that are not wanted.
  */
typedef struct
{
    /** An event happened. `event` is a LogEvent_t; `durationMs` is the travel
        time for OPEN_DONE/CLOSE_DONE and 0 otherwise. */
    void (*OnEvent)(uint8_t event, DoorState_t state, DoorMode_t mode, uint16_t durationMs);
    /** The state changed, for display refresh. */
    void (*OnStateChange)(DoorState_t from, DoorState_t to);
    /** A fault was raised (or cleared, with DOOR_FAULT_NONE). */
    void (*OnFault)(DoorFault_t fault);
    /** The buzzer should play this BuzzerPattern_t. */
    void (*OnSound)(uint8_t pattern);
} DoorHooks_t;

/*===========================================================================*/
/*  Lifecycle                                                                */
/*===========================================================================*/

/**
  * @brief  Initialise the state machine from the current hardware state.
  * @param  hooks         Callback table, or NULL.
  * @param  mode          Starting mode, normally read from persistent storage.
  * @param  autoCloseMs   Starting auto-close delay, normally read from storage.
  * @note   Reads the limit levels to decide whether the door starts IDLE or
  *         OPEN, rather than assuming it is closed. Assuming closed would make
  *         the controller immediately try to close an already-closed door.
  *
  *         The persisted settings are passed in rather than read here, so this
  *         module stays a pure state machine: the application layer owns the
  *         EEPROM and decides what the initial configuration is.
  */
void Door_Init(const DoorHooks_t *hooks, DoorMode_t mode, uint16_t autoCloseMs);

/** @brief Run one iteration. Call from the main loop; never blocks. */
void Door_Update(void);

/*===========================================================================*/
/*  Inputs                                                                   */
/*===========================================================================*/

void Door_NotifyOutsideSensor(void);    /* someone arriving  */
void Door_NotifyInsideSensor(void);     /* someone leaving   */

void Door_KeyStartStop(void);
void Door_KeyToggleMode(void);
void Door_KeyManualOpen(void);
void Door_KeyManualClose(void);

/** Emergency stop: stops motion at once and latches STOPPED until reset. */
void Door_EmergencyStop(void);

/** Clear a latched fault and re-run the power-on self test. */
void Door_Reset(void);

/*===========================================================================*/
/*  Configuration                                                            */
/*===========================================================================*/

/** @brief Set the auto-close delay. Out-of-range values are rejected. */
uint8_t Door_SetAutoCloseMs(uint16_t ms);

/** @return The auto-close delay currently in use. */
uint16_t Door_GetAutoCloseMs(void);

/**
  * @brief  Set the duty used for travel, in percent.
  * @return 0 on success, non-zero when the value is outside MOTOR_DUTY_MIN..MAX.
  * @note   Applied to a move already in progress, so it can be tuned while the
  *         door is running. Changing it changes how long the door takes, which is
  *         what DOOR_TRAVEL_MS has to be calibrated against.
  */
uint8_t Door_SetTravelDuty(uint8_t percent);

/** @return The travel duty currently in use, in percent. */
uint8_t Door_GetTravelDuty(void);

/** @brief Switch mode. Logs a MODE_CHANGE event when the mode really changes. */
void Door_SetMode(DoorMode_t mode);

/*===========================================================================*/
/*  Queries                                                                  */
/*===========================================================================*/

DoorState_t Door_GetState(void);
DoorMode_t  Door_GetMode(void);
DoorFault_t Door_GetFault(void);
uint8_t     Door_IsRunning(void);       /* 1 when the system is not INIT/STOPPED */

/** @return Milliseconds left before auto-close, or 0 when not counting. */
uint16_t    Door_GetCloseCountdown(void);

/** @return The travel time of the last completed move, in milliseconds. */
uint16_t    Door_GetLastTravelMs(void);

/** @return A short human-readable state name, for the display and console. */
const char *Door_StateName(DoorState_t state);

/** @return A short human-readable fault name. */
const char *Door_FaultName(DoorFault_t fault);

#endif /* __DOOR_H */
