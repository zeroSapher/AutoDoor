/**
  ******************************************************************************
  * @file    Door.c
  * @brief   Door state machine. See Door.h for the design rationale.
  *
  * UPDATE ORDER (this is a safety property, not a style choice)
  * -----------------------------------------------------------
  *   1. Limits      - if a limit is reached, the move is over. Always first.
  *   2. Faults      - conflicting limits mean the wiring lies; refuse to move.
  *   3. Sensors     - the only reason to reverse or to (re)open.
  *   4. Commands    - keys, applied only if not overridden above.
  *   5. Timing      - travel watchdog and the auto-close countdown.
  *   6. Motion      - actually drive the motor.
  *
  * Putting limits and faults first is what guarantees that no later branch can
  * restart a motor that a limit just stopped.
  ******************************************************************************
  */

#include "Door.h"
#include "main.h"
#include "Motor.h"
#include "Limit.h"
#include "Sensor.h"
#include "Delay.h"
#include "Log.h"       /* for LogEvent_t / LOG_EVT_* only - no EEPROM access */
#include "Buzzer.h"    /* for BUZZ_* pattern constants only */

/*===========================================================================*/
/*  State                                                                    */
/*===========================================================================*/

static DoorHooks_t s_hooks;
static uint8_t     s_hooked = 0U;

static DoorState_t s_state   = DOOR_STATE_INIT;
static DoorMode_t  s_mode    = DOOR_MODE_AUTO;
static DoorFault_t s_fault   = DOOR_FAULT_NONE;

static uint8_t     s_enabled = 0U;      /* the run/stop latch toggled by KEY1 */

static uint32_t    s_travelStartMs  = 0U;   /* when the current move began     */
static uint32_t    s_openSinceMs    = 0U;   /* when the door reached OPEN      */
static uint16_t    s_lastTravelMs   = 0U;   /* duration of the last completed move */
static uint8_t     s_closeWarned    = 0U;   /* one-shot for the closing warning */

static uint16_t    s_autoCloseMs    = DOOR_AUTO_CLOSE_MS;
/* Travel duty, adjustable at runtime with SPEED=<%>. It changes how long the door
   takes, which is why it is a commissioning value rather than a setting: the
   position is a timed estimate, so a duty that suits the bench rig is wrong for
   the finished door (see the note in Cmd.c). */
static uint8_t     s_travelDuty     = MOTOR_DEFAULT_DUTY;

/* Pending notifications, set by the input paths and consumed by Door_Update.
   Keeping them as flags (rather than acting immediately) ensures every decision
   is taken in one place, in a known order, on the main-loop stack. */
static uint8_t     s_pendingOutside = 0U;
static uint8_t     s_pendingInside  = 0U;
static uint8_t     s_pendingOpenKey = 0U;
static uint8_t     s_pendingCloseKey = 0U;
static uint8_t     s_pendingModeKey = 0U;

/*===========================================================================*/
/*  Notifications                                                            */
/*===========================================================================*/

static void emit_event(LogEvent_t evt, uint16_t durationMs)
{
    if ((s_hooked != 0U) && (s_hooks.OnEvent != 0))
    {
        s_hooks.OnEvent((uint8_t)evt, s_state, s_mode, durationMs);
    }
}

static void emit_sound(uint8_t pattern)
{
    if ((s_hooked != 0U) && (s_hooks.OnSound != 0))
    {
        s_hooks.OnSound(pattern);
    }
}

static void set_state(DoorState_t next)
{
    DoorState_t prev = s_state;

    if (prev == next)
    {
        return;
    }

    s_state = next;

    if ((s_hooked != 0U) && (s_hooks.OnStateChange != 0))
    {
        s_hooks.OnStateChange(prev, next);
    }
}

static void set_fault(DoorFault_t fault)
{
    if (s_fault == fault)
    {
        return;
    }

    s_fault = fault;

    if ((s_hooked != 0U) && (s_hooks.OnFault != 0))
    {
        s_hooks.OnFault(fault);
    }
}

/*===========================================================================*/
/*  Motion primitives                                                        */
/*===========================================================================*/

/**
  * @brief  Begin opening.
  * @note   Records the start time unconditionally, so the travel watchdog always
  *         measures the move in progress rather than an older one.
  */
static void begin_open(void)
{
    s_travelStartMs = g_msTick;

    Motor_Run(MOTOR_DIR_OPEN);
    /* The L9110S has no separate enable pin, so the ramp in Motor.c is doing the
       soft start; the duty is the normal travel speed. */
    Motor_SetDuty(s_travelDuty);

    set_state(DOOR_STATE_OPENING);
    emit_event(LOG_EVT_OPEN_START, 0U);
}

static void begin_close(void)
{
    s_travelStartMs = g_msTick;
    s_closeWarned   = 0U;

    Motor_Run(MOTOR_DIR_CLOSE);
    Motor_SetDuty(s_travelDuty);

    set_state(DOOR_STATE_CLOSING);

    /*
     * The closing BEEP is deliberately not emitted here. It is played from
     * Door_Update() once the door has actually been travelling for a moment:
     *
     *   - Motor_Run() blocks for the reversal dead time, so at this point the
     *     bridge may not even be energised yet. Beeping now would warn about
     *     motion that has not started.
     *   - Emitting in both places produced two beeps for every close, which is
     *     ambiguous - it sounds like a fault rather than a warning.
     */
    emit_event(LOG_EVT_CLOSE_START, 0U);
}

/** Stop the motor and go to the closed-and-latched state. */
static void settle_closed(void)
{
    Motor_Stop();
    s_lastTravelMs = (uint16_t)(g_msTick - s_travelStartMs);

    set_state(DOOR_STATE_IDLE);
    emit_sound(BUZZ_CLOSED);
    emit_event(LOG_EVT_CLOSE_DONE, s_lastTravelMs);
}

/** Stop the motor and start the auto-close countdown. */
static void settle_open(void)
{
    Motor_Stop();
    s_lastTravelMs = (uint16_t)(g_msTick - s_travelStartMs);

    s_openSinceMs = g_msTick;

    set_state(DOOR_STATE_OPEN);
    emit_sound(BUZZ_OPEN);
    emit_event(LOG_EVT_OPEN_DONE, s_lastTravelMs);
}

/** Latch stopped, with the reason recorded. */
static void halt_with_fault(DoorFault_t fault, LogEvent_t evt)
{
    Motor_EmergencyStop();
    set_fault(fault);
    set_state(DOOR_STATE_STOPPED);
    emit_sound(BUZZ_FAULT);
    emit_event(evt, 0U);
}

/*===========================================================================*/
/*  Lifecycle                                                                */
/*===========================================================================*/

void Door_Init(const DoorHooks_t *hooks, DoorMode_t mode, uint16_t autoCloseMs)
{
    if (hooks != 0)
    {
        s_hooks  = *hooks;
        s_hooked = 1U;
    }
    else
    {
        s_hooked = 0U;
    }

    s_enabled      = 0U;
    s_fault        = DOOR_FAULT_NONE;
    s_lastTravelMs = 0U;
    s_closeWarned  = 0U;

    s_pendingOutside  = 0U;
    s_pendingInside   = 0U;
    s_pendingOpenKey  = 0U;
    s_pendingCloseKey = 0U;
    s_pendingModeKey  = 0U;

    /* Trust the caller's persisted values only within the legal range. */
    if ((autoCloseMs < DOOR_AUTO_CLOSE_MIN_MS) || (autoCloseMs > DOOR_AUTO_CLOSE_MAX_MS))
    {
        autoCloseMs = DOOR_AUTO_CLOSE_MS;
    }
    s_autoCloseMs = autoCloseMs;

    s_mode = (mode == DOOR_MODE_MANUAL) ? DOOR_MODE_MANUAL : DOOR_MODE_AUTO;

    /* Where is the door actually? Assuming "closed" would make the controller
       immediately try to close a door that is already closed, or worse, drive a
       door that is already at the open limit. The limit levels are the truth. */
    if (Limit_IsFaulted() != 0U)
    {
        set_state(DOOR_STATE_STOPPED);
        set_fault(DOOR_FAULT_LIMIT_CONFLICT);
        return;
    }

    if (Limit_IsOpen() != 0U)
    {
        s_openSinceMs = g_msTick;
        set_state(DOOR_STATE_OPEN);
    }
    else if (Limit_IsClosed() != 0U)
    {
        set_state(DOOR_STATE_IDLE);
    }
    else
    {
        /* Between the limits: position unknown, but that is not a fault - the
           first sensor event or key press will resolve it. Treat it as stopped
           so nothing moves until commanded. */
        set_state(DOOR_STATE_STOPPED);
    }
}

void Door_Update(void)
{
    uint8_t someonePresent;

    /*========================================================================*/
    /* 1. Limits - if we have arrived, the move is over. Checked before         */
    /*    anything else so no later branch can restart a stopped motor.         */
    /*========================================================================*/
    if (s_state == DOOR_STATE_OPENING || s_state == DOOR_STATE_REVERSING)
    {
        if (Limit_IsOpen() != 0U)
        {
            settle_open();
        }
    }
    else if (s_state == DOOR_STATE_CLOSING)
    {
        if (Limit_IsClosed() != 0U)
        {
            settle_closed();
        }
    }

    /* Limit transitions are deliberately NOT consumed here. The level checks
       above already drive the state machine (and they also cover power-up, where
       no edge occurs at all), so the edge events are left for the application
       layer to report. Consuming them in both places would silently swallow one
       of the two uses. */

    /*========================================================================*/
    /* 2. Faults - conflicting limits mean the wiring is lying.                 */
    /*========================================================================*/
    if ((s_state != DOOR_STATE_STOPPED) && (Limit_IsFaulted() != 0U))
    {
        halt_with_fault(DOOR_FAULT_LIMIT_CONFLICT, LOG_EVT_LIMIT_FAULT);
        return;
    }

    /*========================================================================*/
    /* 3. Sensors - the only reason to reverse or to (re)open.                 */
    /*========================================================================*/
    someonePresent = ((Sensor_IsOutsideActive() != 0U) ||
                      (Sensor_IsInsideActive() != 0U)) ? 1U : 0U;

    if (s_pendingOutside != 0U || s_pendingInside != 0U)
    {
        LogEvent_t evt = (s_pendingOutside != 0U) ? LOG_EVT_ENTER : LOG_EVT_EXIT;
        s_pendingOutside = 0U;
        s_pendingInside  = 0U;

        if (s_enabled != 0U)
        {
            emit_event(evt, 0U);

            /* A closing door must always yield to a person. This is checked as a
               level as well as via the event, so a detection that arrives while
               the door is still travelling is never ignored. */
            if (s_state == DOOR_STATE_CLOSING)
            {
                emit_event(LOG_EVT_REVERSE, 0U);
                emit_sound(BUZZ_FAULT);
                begin_open();
            }
            else if ((s_state == DOOR_STATE_OPEN) && (s_mode == DOOR_MODE_AUTO))
            {
                /* Someone is still coming through: restart the countdown. */
                s_openSinceMs = g_msTick;
            }
            else if ((s_state == DOOR_STATE_IDLE) && (s_mode == DOOR_MODE_AUTO))
            {
                begin_open();
            }
        }
    }

    /*
     * LEVEL-BASED REVERSAL - the safety net under the edge-triggered path above.
     *
     * The block above only fires when an edge event was delivered. If that edge
     * is ever lost - an electrically noisy sensor, a glitch, a sensor whose
     * output is already asserted when the door starts closing - the door would
     * happily continue closing on somebody standing in it. That is the one
     * failure this system must not have.
     *
     * So the sensors are ALSO consulted as levels, which cannot be missed
     * because they are sampled every millisecond rather than latched. This is
     * the same "level is the truth, edges are an optimisation" principle the
     * limit handling uses.
     */
    if ((s_state == DOOR_STATE_CLOSING) &&
        (s_enabled != 0U) &&
        (someonePresent != 0U))
    {
        emit_event(LOG_EVT_REVERSE, 0U);
        emit_sound(BUZZ_FAULT);
        begin_open();
    }

    /*========================================================================*/
    /* 4. Commands - keys, applied only if the branches above did not act.      */
    /*========================================================================*/
    if (s_pendingModeKey != 0U)
    {
        s_pendingModeKey = 0U;

        if (s_enabled != 0U)
        {
            Door_SetMode((s_mode == DOOR_MODE_AUTO) ? DOOR_MODE_MANUAL : DOOR_MODE_AUTO);
            emit_event(LOG_EVT_MODE_CHANGE, 0U);
        }
    }

    if (s_enabled != 0U)
    {
        if (s_pendingOpenKey != 0U)
        {
            s_pendingOpenKey = 0U;
            if (s_state != DOOR_STATE_OPENING && s_state != DOOR_STATE_OPEN)
            {
                begin_open();
            }
        }

        if (s_pendingCloseKey != 0U)
        {
            s_pendingCloseKey = 0U;

            /*
             * Refuse to START a close while someone is detected. Without this the
             * command would begin travelling and the level-based guard above
             * would reverse it on the very next iteration - a pointless
             * start-stop cycle that wears the mechanism and looks like a fault to
             * anyone watching. Refusing up front is both kinder to the hardware
             * and clearer in the log, which shows no CLOSE_START at all.
             */
            if ((s_state != DOOR_STATE_CLOSING) &&
                (s_state != DOOR_STATE_IDLE) &&
                (someonePresent == 0U))
            {
                begin_close();
            }
        }
    }
    else
    {
        s_pendingOpenKey  = 0U;
        s_pendingCloseKey = 0U;
    }

    /*========================================================================*/
    /* 5. Timing                                                                */
    /*========================================================================*/

    /* Travel watchdog. A jammed mechanism or a dead limit switch would otherwise
       stall the motor against a hard stop indefinitely, cooking the winding. */
    if ((s_state == DOOR_STATE_OPENING) ||
        (s_state == DOOR_STATE_CLOSING) ||
        (s_state == DOOR_STATE_REVERSING))
    {
        if ((g_msTick - s_travelStartMs) > DOOR_TRAVEL_TIMEOUT_MS)
        {
            /*
             * Report the direction that actually failed. A reversal is heading
             * for the open limit, but if it never gets there the cause is
             * different from a normal open that stalled - most often something
             * is still physically in the doorway. Reporting OPEN_TIMEOUT here
             * would send a service technician looking at the wrong end of the
             * door.
             */
            if (s_state == DOOR_STATE_CLOSING)
            {
                halt_with_fault(DOOR_FAULT_CLOSE_TIMEOUT, LOG_EVT_FAULT_CLOSE);
            }
            else if (s_state == DOOR_STATE_REVERSING)
            {
                halt_with_fault(DOOR_FAULT_REVERSE_TIMEOUT, LOG_EVT_FAULT_OPEN);
            }
            else
            {
                halt_with_fault(DOOR_FAULT_OPEN_TIMEOUT, LOG_EVT_FAULT_OPEN);
            }
            return;
        }
    }

    /* Auto-close countdown. Restarted whenever proximity is seen, so the door
       stays open as long as people keep arriving. */
    if ((s_state == DOOR_STATE_OPEN) &&
        (s_mode == DOOR_MODE_AUTO) &&
        (s_enabled != 0U))
    {
        if (someonePresent != 0U)
        {
            s_openSinceMs = g_msTick;
        }
        else if ((g_msTick - s_openSinceMs) >= s_autoCloseMs)
        {
            begin_close();
        }
    }

    /* Closing warning: a short intermittent beep while the door moves, so
       someone standing in the doorway is warned before contact. */
    if ((s_state == DOOR_STATE_CLOSING) && (s_closeWarned == 0U) &&
        ((g_msTick - s_travelStartMs) > 300U))
    {
        s_closeWarned = 1U;
        emit_sound(BUZZ_CLOSE);
    }
}

/*===========================================================================*/
/*  Input notifications                                                      */
/*===========================================================================*/

void Door_NotifyOutsideSensor(void)
{
    s_pendingOutside = 1U;
}

void Door_NotifyInsideSensor(void)
{
    s_pendingInside = 1U;
}

void Door_KeyStartStop(void)
{
    if (s_enabled == 0U)
    {
        /* Start: re-run the self test so a cleared fault is verified first. */
        s_enabled = 1U;
        s_fault   = DOOR_FAULT_NONE;

        if (Limit_IsFaulted() != 0U)
        {
            halt_with_fault(DOOR_FAULT_LIMIT_CONFLICT, LOG_EVT_LIMIT_FAULT);
            s_enabled = 0U;
            return;
        }

        if (Limit_IsOpen() != 0U)
        {
            s_openSinceMs = g_msTick;
            set_state(DOOR_STATE_OPEN);
        }
        else
        {
            set_state(DOOR_STATE_IDLE);
        }

        emit_event(LOG_EVT_SYSTEM_START, 0U);
    }
    else
    {
        /* Stop: motion off, latch disabled. */
        s_enabled = 0U;
        Motor_Stop();
        set_state(DOOR_STATE_STOPPED);
        emit_event(LOG_EVT_SYSTEM_STOP, 0U);
    }
}

void Door_KeyToggleMode(void)
{
    s_pendingModeKey = 1U;
}

void Door_KeyManualOpen(void)
{
    s_pendingOpenKey = 1U;
}

void Door_KeyManualClose(void)
{
    s_pendingCloseKey = 1U;
}

void Door_EmergencyStop(void)
{
    /* Immediate, not deferred to Door_Update: this comes from a long key press
       and must not wait for the loop to come round. */
    s_enabled = 0U;
    Motor_EmergencyStop();
    halt_with_fault(DOOR_FAULT_ESTOP, LOG_EVT_ESTOP);
}

void Door_Reset(void)
{
    Motor_Stop();

    s_fault        = DOOR_FAULT_NONE;
    s_enabled      = 0U;
    s_closeWarned  = 0U;
    s_lastTravelMs = 0U;

    if (Limit_IsFaulted() != 0U)
    {
        halt_with_fault(DOOR_FAULT_LIMIT_CONFLICT, LOG_EVT_LIMIT_FAULT);
        return;
    }

    if (Limit_IsOpen() != 0U)
    {
        s_openSinceMs = g_msTick;
        set_state(DOOR_STATE_OPEN);
    }
    else
    {
        set_state(DOOR_STATE_IDLE);
    }
}

/*===========================================================================*/
/*  Configuration                                                            */
/*===========================================================================*/

uint8_t Door_SetAutoCloseMs(uint16_t ms)
{
    if ((ms < DOOR_AUTO_CLOSE_MIN_MS) || (ms > DOOR_AUTO_CLOSE_MAX_MS))
    {
        return 1U;
    }

    s_autoCloseMs = ms;

    /* Restart the countdown so a shortened delay takes effect immediately
       instead of only after the next opening. */
    if (s_state == DOOR_STATE_OPEN)
    {
        s_openSinceMs = g_msTick;
    }

    return 0U;
}

uint16_t Door_GetAutoCloseMs(void)
{
    return s_autoCloseMs;
}

uint8_t Door_SetTravelDuty(uint8_t percent)
{
    if ((percent < MOTOR_DUTY_MIN) || (percent > MOTOR_DUTY_MAX))
    {
        return 1U;
    }

    s_travelDuty = percent;

    /* Push it to the motor if a move is already in progress. Motor_SetDuty() only
       moves the ramp TARGET, so the change is applied smoothly by the existing
       ramp instead of jumping - which matters when this is being tuned while
       watching the door. */
    if ((s_state == DOOR_STATE_OPENING) || (s_state == DOOR_STATE_CLOSING) ||
        (s_state == DOOR_STATE_REVERSING))
    {
        Motor_SetDuty(percent);
    }

    return 0U;
}

uint8_t Door_GetTravelDuty(void)
{
    return s_travelDuty;
}

void Door_SetMode(DoorMode_t mode)
{
    if (s_mode != mode)
    {
        s_mode = mode;
    }
}

/*===========================================================================*/
/*  Queries                                                                  */
/*===========================================================================*/

DoorState_t Door_GetState(void)
{
    return s_state;
}

DoorMode_t Door_GetMode(void)
{
    return s_mode;
}

DoorFault_t Door_GetFault(void)
{
    return s_fault;
}

uint8_t Door_IsRunning(void)
{
    return s_enabled;
}

uint16_t Door_GetCloseCountdown(void)
{
    uint32_t elapsed;

    if ((s_state != DOOR_STATE_OPEN) || (s_enabled == 0U) ||
        (s_mode != DOOR_MODE_AUTO))
    {
        return 0U;
    }

    elapsed = g_msTick - s_openSinceMs;

    if (elapsed >= s_autoCloseMs)
    {
        return 0U;
    }

    return (uint16_t)(s_autoCloseMs - elapsed);
}

uint16_t Door_GetLastTravelMs(void)
{
    return s_lastTravelMs;
}

/*===========================================================================*/
/*  Names                                                                    */
/*===========================================================================*/

const char *Door_StateName(DoorState_t state)
{
    switch (state)
    {
        case DOOR_STATE_INIT:      return "INIT";
        case DOOR_STATE_IDLE:      return "IDLE";
        case DOOR_STATE_OPENING:   return "OPENING";
        case DOOR_STATE_OPEN:      return "OPEN";
        case DOOR_STATE_CLOSING:   return "CLOSING";
        case DOOR_STATE_REVERSING: return "REVERSING";
        case DOOR_STATE_STOPPED:   return "STOPPED";
        default:                   return "?";
    }
}

const char *Door_FaultName(DoorFault_t fault)
{
    switch (fault)
    {
        case DOOR_FAULT_NONE:            return "NONE";
        case DOOR_FAULT_OPEN_TIMEOUT:    return "OPEN_TIMEOUT";
        case DOOR_FAULT_CLOSE_TIMEOUT:   return "CLOSE_TIMEOUT";
        case DOOR_FAULT_REVERSE_TIMEOUT: return "REVERSE_TIMEOUT";
        case DOOR_FAULT_LIMIT_CONFLICT:  return "LIMIT_CONFLICT";
        case DOOR_FAULT_ESTOP:           return "ESTOP";
        default:                         return "UNKNOWN";
    }
}
