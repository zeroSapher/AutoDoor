/**
  ******************************************************************************
  * @file    Cmd.c
  * @brief   Line-based console protocol: reporting and host commands.
  *
  * PARSING STYLE
  * -------------
  * Matching is done with explicit string comparisons rather than sscanf. Reasons:
  *
  *   - sscanf with %s into a fixed buffer cannot be bounds-checked portably, and
  *     a malformed host command should never be able to smash the stack.
  *   - The command set is small and fixed; a table of comparisons is easier to
  *     audit than a format string, and it reports exactly why it refused input.
  *
  * Replies are always OK/ERR prefixed so a host script can parse them without
  * knowing the payload format.
  ******************************************************************************
  */

#include "Cmd.h"
#include "main.h"
#include "UART.h"
#include "Log.h"
#include "MyI2C.h"
#include "Limit.h"
#include "Sensor.h"
#include "Motor.h"
#include "Delay.h"
#include <string.h>

/* Long enough for "LOG?CLEAR" plus slack; UART_ReadLine() drops overflow. */
#define CMD_LINE_MAX    48U

/*===========================================================================*/
/*  Formatting helpers                                                       */
/*===========================================================================*/

/** Print a relative timestamp as HH:MM:SS.mmm for readability. */
static void print_timestamp(uint32_t ms)
{
    uint32_t hours   = ms / 3600000UL;
    uint32_t minutes = (ms / 60000UL) % 60UL;
    uint32_t seconds = (ms / 1000UL) % 60UL;
    uint32_t millis  = ms % 1000UL;

    UART_Printf("%02lu:%02lu:%02lu.%03lu",
                (unsigned long)hours,
                (unsigned long)minutes,
                (unsigned long)seconds,
                (unsigned long)millis);
}

static const char *mode_name(DoorMode_t m)
{
    return (m == DOOR_MODE_MANUAL) ? "MANUAL" : "AUTO";
}

/*===========================================================================*/
/*  Reporting                                                                */
/*===========================================================================*/

void Cmd_Init(void)
{
    /* The EFFECTIVE delay, not the persisted one. Log_GetAutoCloseMs() returns
       what was read out of the EEPROM, which is 0 when no EEPROM answered - and
       this banner then claimed "delay=0ms" while STATUS? reported DELAY=5000ms
       for the very same running system. main.c has already applied the fallback
       by this point, so the live value is the only one that is not a lie. */
    UART_Printf("\r\nAutoDoor ready. boot#%u  records=%u  delay=%ums\r\n",
                (unsigned)Log_GetBootId(),
                (unsigned)Log_Count(),
                (unsigned)Door_GetAutoCloseMs());
    UART_SendString("type HELP for commands\r\n");
}

void Cmd_ReportEvent(uint8_t event, DoorState_t state, DoorMode_t mode, uint16_t durationMs)
{
    UART_SendString("EVT [");
    print_timestamp(g_msTick);
    UART_Printf("] boot#%u %-18s S=%-8s M=%s",
                (unsigned)Log_GetBootId(),
                Log_EventName(event),
                Door_StateName(state),
                mode_name(mode));

    if (durationMs != 0U)
    {
        UART_Printf(" DUR=%ums", (unsigned)durationMs);
    }

    UART_SendLine();
}

void Cmd_ReportState(DoorState_t from, DoorState_t to)
{
    UART_Printf("    state: %s -> %s\r\n", Door_StateName(from), Door_StateName(to));
}

void Cmd_ReportFault(DoorFault_t fault)
{
    if (fault == DOOR_FAULT_NONE)
    {
        UART_SendString("    fault cleared\r\n");
    }
    else
    {
        UART_Printf("    FAULT: %s\r\n", Door_FaultName(fault));
    }
}

void Cmd_ReportError(const char *text)
{
    UART_Printf("ERR %s\r\n", (text != 0) ? text : "UNKNOWN");
}

void Cmd_ReportStatus(void)
{
    UART_SendString("OK STATUS");
    UART_Printf(" RUN=%u", (unsigned)Door_IsRunning());
    UART_Printf(" MODE=%s", mode_name(Door_GetMode()));
    UART_Printf(" DOOR=%s", Door_StateName(Door_GetState()));
    UART_Printf(" FAULT=%s", Door_FaultName(Door_GetFault()));
    UART_Printf(" DELAY=%ums", (unsigned)Door_GetAutoCloseMs());
    UART_Printf(" CNT=%u", (unsigned)Log_Count());
    UART_Printf(" TOTAL=%lu", (unsigned long)Log_TotalEvents());

    /* Only shown when non-zero: a healthy log should not clutter the line, but a
       silently failing one must not be invisible either. */
    if (Log_DroppedCount() != 0U)
    {
        UART_Printf(" DROPPED=%u", (unsigned)Log_DroppedCount());
    }

    /* Same reasoning: the position source is a property of the build that an
       unattended log has to carry, because a status line may be the only thing
       anyone looks at and "where did the door's position come from" changes what
       a fault means. */
    if (Limit_IsSimulated() != 0U)
    {
        UART_SendString(" LIMITS=TIMED");
    }
    UART_Printf(" LIM=%u/%u", (unsigned)Limit_IsOpen(), (unsigned)Limit_IsClosed());
    UART_Printf(" TRAVEL=%ums", (unsigned)Motor_GetTravelMs());
    UART_Printf(" I2C=%s", (MyI2C_IsIdle() != 0U) ? "ok" : "STUCK");
    UART_SendLine();
}

/*===========================================================================*/
/*  Command implementations                                                  */
/*===========================================================================*/

/**
  * @brief  State of an in-progress log listing.
  * @note   A listing is emitted a little at a time, from the main loop, instead of
  *         inside the command handler.
  *
  *         WHY: a full ring is 252 records at ~92 characters each, which is
  *         ~23 KB. The UART transmits that at 115200 baud, so the transfer takes
  *         ~2 seconds - and it used to happen inside cmd_log(), i.e. inside the
  *         main loop. For those two seconds Door_Update() never ran (no
  *         level-triggered reversal safety net), poll_inputs() never ran (no
  *         keypad emergency stop) and UART input was not parsed either, so a STOP
  *         typed during a dump was ignored. Combined with the limit's EXTI having
  *         been broken until this round, the only thing left that could stop the
  *         motor during `LOG?ALL` was the hardware NC contact.
  *
  *         The UART link is the bottleneck and cannot be made faster, but the main
  *         loop no longer waits for it: each iteration queues at most one record,
  *         and only when the TX ring has room for it.
  */
static uint8_t  s_dumpActive = 0U;
static uint16_t s_dumpNext   = 0U;   /* next index to emit      */
static uint16_t s_dumpEnd    = 0U;   /* one past the last index */

/* Room needed before one more record is queued. A record line is ~92 bytes worst
   case; the slack leaves the ring usable for the spontaneous EVT and status lines
   that share it. */
#define LOG_DUMP_HEADROOM       128U

static void print_record(uint16_t index, const LogEntry_t *e)
{
    UART_Printf("REC %u #%u [", (unsigned)index, (unsigned)e->seq);
    print_timestamp(e->timestampMs);
    UART_Printf("] boot#%u %-18s S=%-8s M=%s",
                (unsigned)e->bootId,
                Log_EventName(e->event),
                Door_StateName((DoorState_t)e->doorState),
                mode_name((DoorMode_t)e->mode));

    if (e->durationMs != 0U)
    {
        UART_Printf(" DUR=%ums", (unsigned)e->durationMs);
    }

    UART_SendLine();
}

/**
  * @brief  Emit the next slice of a pending log listing.
  * @note   Call once per main-loop iteration. Emits NOTHING when the TX ring is
  *         too full, which is what makes it non-blocking: the ring drains in the
  *         interrupt while the loop goes on running the door.
  */
void Cmd_ProcessLogDump(void)
{
    LogEntry_t e;

    if (s_dumpActive == 0U)
    {
        return;
    }

    if (s_dumpNext >= s_dumpEnd)
    {
        UART_SendString("OK LOG END\r\n");
        s_dumpActive = 0U;
        return;
    }

    if (UART_TxFree() < LOG_DUMP_HEADROOM)
    {
        return;         /* let the interrupt catch up; try again next iteration */
    }

    if (Log_Get(s_dumpNext, &e) != 0U)
    {
        UART_SendString("ERR LOG READ\r\n");
        s_dumpActive = 0U;
        return;
    }

    print_record(s_dumpNext, &e);
    s_dumpNext++;
}

uint8_t Cmd_LogDumpActive(void)
{
    return s_dumpActive;
}

static void cmd_log(uint16_t want)
{
    uint16_t count;

    /*
     * Persist before reporting. Without this the most recent events - up to
     * LOG_FLUSH_INTERVAL_MS old - are still in the RAM queue and invisible here,
     * so a user who watched the door move and immediately asked for the log would
     * be told it never happened. Log.h has documented "flush before reporting
     * counts" from the start; this is where that promise is actually kept.
     *
     * A failure is reported rather than papered over: showing a list that is
     * silently missing the newest records is worse than saying the EEPROM is
     * unavailable, because the list looks complete.
     */
    if (Log_Flush() != 0U)
    {
        UART_SendString("ERR EEPROM (log not fully persisted)\r\n");
        return;
    }

    count = Log_Count();

    if (count == 0U)
    {
        UART_SendString("OK LOG EMPTY\r\n");
        return;
    }

    if (want == 0U || want > count)
    {
        want = count;
    }

    /* Print the newest `want` records in chronological order, starting at the
       oldest of those. */
    UART_Printf("OK LOG %u of %u\r\n", (unsigned)want, (unsigned)count);

    s_dumpNext   = (uint16_t)(count - want);
    s_dumpEnd    = count;
    s_dumpActive = 1U;

    /* The records themselves are emitted by Cmd_ProcessLogDump() from the main
       loop, one per iteration. */
}

/** Parse a decimal string strictly: only digits, no sign, no overflow. */
static uint8_t parse_u32(const char *s, uint32_t *out)
{
    uint32_t v = 0U;
    uint8_t  digits = 0U;

    if ((s == 0) || (*s == '\0'))
    {
        return 1U;
    }

    while (*s != '\0')
    {
        if ((*s < '0') || (*s > '9'))
        {
            return 2U;
        }

        /* Reject anything beyond 7 digits outright rather than wrapping. */
        if (digits >= 7U)
        {
            return 3U;
        }

        v = (v * 10U) + (uint32_t)(*s - '0');
        digits++;
        s++;
    }

    *out = v;
    return 0U;
}

void Cmd_Process(void)
{
    char line[CMD_LINE_MAX];
    uint16_t len;
    uint32_t value;

    len = UART_ReadLine(line, sizeof(line));
    if (len == 0U)
    {
        return;
    }

    /* ---- STATUS? -------------------------------------------------------- */
    if (strcmp(line, "STATUS?") == 0)
    {
        Cmd_ReportStatus();
        return;
    }

    /* ---- LOG? ----------------------------------------------------------- */
    if (strcmp(line, "LOG?") == 0)
    {
        cmd_log(10U);           /* default: the last ten */
        return;
    }
    if (strcmp(line, "LOG?ALL") == 0)
    {
        cmd_log(0U);            /* 0 means "everything retained" */
        return;
    }
    if (strcmp(line, "LOG?CLEAR") == 0)
    {
        uint8_t rc = Log_Clear();

        if (rc == 0U)
        {
            UART_SendString("OK LOG CLEARED\r\n");
        }
        else if (rc == 2U)
        {
            /* The rare generation-wrap erase failed part way. The log has been
               reloaded from what physically survives, so it is usable but NOT
               empty - saying "ERR EEPROM" alone would leave the caller guessing
               whether anything was deleted. */
            UART_Printf("ERR EEPROM (clear incomplete, %u records remain)\r\n",
                        (unsigned)Log_Count());
        }
        else
        {
            UART_SendString("ERR EEPROM\r\n");
        }
        return;
    }
    if (strncmp(line, "LOG?", 4) == 0)
    {
        if (parse_u32(&line[4], &value) != 0U)
        {
            UART_SendString("ERR BAD_ARG\r\n");
            return;
        }
        cmd_log((uint16_t)value);
        return;
    }

    /* ---- DELAY=<ms> ----------------------------------------------------- */
    if (strncmp(line, "DELAY=", 6) == 0)
    {
        uint8_t persistOk;

        if (parse_u32(&line[6], &value) != 0U)
        {
            UART_SendString("ERR BAD_ARG\r\n");
            return;
        }

        /*
         * Validate BEFORE narrowing to uint16_t. Passing (uint16_t)value into the
         * range check truncated the argument first, so DELAY=70000 became 4464,
         * passed the range test, and was accepted - the caller asked for 70 s and
         * silently got 4.4 s. Anything above the uint16 range is out of range by
         * definition, so it is rejected here rather than wrapped.
         */
        if (value > 0xFFFFUL)
        {
            UART_Printf("ERR RANGE %u..%u\r\n",
                        (unsigned)DOOR_AUTO_CLOSE_MIN_MS,
                        (unsigned)DOOR_AUTO_CLOSE_MAX_MS);
            return;
        }

        if (Door_SetAutoCloseMs((uint16_t)value) != 0U)
        {
            UART_Printf("ERR RANGE %u..%u\r\n",
                        (unsigned)DOOR_AUTO_CLOSE_MIN_MS,
                        (unsigned)DOOR_AUTO_CLOSE_MAX_MS);
            return;
        }

        /* Persist so the setting survives a power cut. The result is REPORTED,
           not discarded: if the EEPROM write failed the setting is live but will
           revert on the next boot, and saying a bare "OK" would hide that. The
           command still counts as applied, because it did change the running
           configuration. */
        persistOk = (Log_SetAutoCloseMs((uint16_t)value) == MYI2C_OK) ? 1U : 0U;

        Log_Add(LOG_EVT_PARAM_CHANGE, (uint8_t)Door_GetState(),
                (uint8_t)Door_GetMode(), 0U);

        if (persistOk != 0U)
        {
            UART_Printf("OK DELAY=%u\r\n", (unsigned)value);
        }
        else
        {
            UART_Printf("OK DELAY=%u (NOT PERSISTED - eeprom write failed)\r\n",
                        (unsigned)value);
        }
        return;
    }

    /* ---- TRAVEL=<ms> ---------------------------------------------------- */
    if (strncmp(line, "TRAVEL=", 7) == 0)
    {
        uint8_t persistOk;

        if (parse_u32(&line[7], &value) != 0U)
        {
            UART_SendString("ERR BAD_ARG\r\n");
            return;
        }

        /* Validate the wide value before narrowing, exactly as DELAY does:
           casting first would turn 200000 into 3392 and accept it. */
        if ((value < MOTOR_TRAVEL_MIN_MS) || (value > MOTOR_TRAVEL_MAX_MS))
        {
            UART_Printf("ERR RANGE %u..%u\r\n",
                        (unsigned)MOTOR_TRAVEL_MIN_MS, (unsigned)MOTOR_TRAVEL_MAX_MS);
            return;
        }

        (void)Motor_SetTravelMs((uint16_t)value);

        /* Persisted, unlike SPEED. It is tuned on the bench, and a value that
           reverted on the next power-up would have to be re-typed every session.
           Door.c's travel watchdog follows the live value too, so a silent revert
           would also change how long a move is allowed to take. */
        persistOk = (Log_SetPersistedTravelMs((uint16_t)value) == MYI2C_OK) ? 1U : 0U;

        Log_Add(LOG_EVT_PARAM_CHANGE, (uint8_t)Door_GetState(),
                (uint8_t)Door_GetMode(), 0U);

        /*
         * Echo the number that actually decides the feel of the door: how far the
         * pulse moves between two servo frames. That, not the millisecond figure,
         * is what the SG90 reacts to - below ~10 us per frame it ignores the change
         * for several frames and the door visibly steps.
         *
         * A failed EEPROM write is REPORTED, never swallowed: answering a bare OK
         * would claim the setting survives a power cut when it does not.
         */
        if (persistOk != 0U)
        {
            UART_Printf("OK TRAVEL=%ums (%u us of pulse per 20 ms frame)\r\n",
                        (unsigned)value,
                        (unsigned)(((uint32_t)(SERVO_OPEN_US - SERVO_CLOSED_US) * 20U) / value));
        }
        else
        {
            UART_Printf("OK TRAVEL=%ums (%u us of pulse per 20 ms frame) "
                        "(NOT PERSISTED - eeprom write failed)\r\n",
                        (unsigned)value,
                        (unsigned)(((uint32_t)(SERVO_OPEN_US - SERVO_CLOSED_US) * 20U) / value));
        }
        return;
    }

    /* ---- SPEED=<%> ------------------------------------------------------ */
    if (strncmp(line, "SPEED=", 6) == 0)
    {
        /* An SG90 has no duty to set: its speed is fixed by the servo, and the only
           thing shaping motion on this branch is the travel time (the slew step is
           derived from it). Saying so beats accepting a number and ignoring it. */
        UART_SendString("ERR N/A - SG90 speed is fixed (use TRAVEL=<ms>)\r\n");
        return;
        if (parse_u32(&line[6], &value) != 0U)
        {
            UART_SendString("ERR BAD_ARG\r\n");
            return;
        }

        /* Validate before narrowing, exactly as DELAY does: truncating to uint8
           first would turn 300 into 44 and accept it. */
        if (value > 100UL)
        {
            UART_Printf("ERR RANGE %u..%u\r\n",
                        (unsigned)MOTOR_DUTY_MIN, (unsigned)MOTOR_DUTY_MAX);
            return;
        }

        if (Door_SetTravelDuty((uint8_t)value) != 0U)
        {
            UART_Printf("ERR RANGE %u..%u\r\n",
                        (unsigned)MOTOR_DUTY_MIN, (unsigned)MOTOR_DUTY_MAX);
            return;
        }

        Log_Add(LOG_EVT_PARAM_CHANGE, (uint8_t)Door_GetState(),
                (uint8_t)Door_GetMode(), 0U);

        /*
         * Deliberately NOT persisted, unlike DELAY and MODE. Those are settings
         * the end user owns; this is a commissioning value, and it is coupled to
         * the calibrated travel time: the duty sets how fast the door moves, so a
         * number that is right for a rig on the bench can be wrong for the
         * finished door, and the value that ships belongs in MOTOR_DEFAULT_DUTY
         * where it is visible in the source and in the BOM notes. Saying so is
         * better than a silent "OK" that implies it will survive a power cut.
         */
        UART_Printf("OK SPEED=%u%% (not saved - put it in MOTOR_DEFAULT_DUTY)\r\n",
                    (unsigned)value);
        return;
    }

    /* ---- MODE=AUTO | MODE=MANUAL ---------------------------------------- */
    if (strncmp(line, "MODE=", 5) == 0)
    {
        const char *arg = &line[5];
        DoorMode_t  m;
        uint8_t     persistOk;

        if (strcmp(arg, "AUTO") == 0)
        {
            m = DOOR_MODE_AUTO;
        }
        else if (strcmp(arg, "MANUAL") == 0)
        {
            m = DOOR_MODE_MANUAL;
        }
        else
        {
            UART_SendString("ERR BAD_ARG (AUTO|MANUAL)\r\n");
            return;
        }

        Door_SetMode(m);

        /* Report the persistence result rather than discarding it - see the note
           in the DELAY handler. */
        persistOk = (Log_SetPersistedMode((uint8_t)m) == MYI2C_OK) ? 1U : 0U;

        Log_Add(LOG_EVT_MODE_CHANGE, (uint8_t)Door_GetState(), (uint8_t)m, 0U);

        if (persistOk != 0U)
        {
            UART_Printf("OK MODE=%s\r\n", mode_name(m));
        }
        else
        {
            UART_Printf("OK MODE=%s (NOT PERSISTED - eeprom write failed)\r\n",
                        mode_name(m));
        }
        return;
    }

    /* ---- DOOR=OPEN | DOOR=CLOSE ----------------------------------------- */
    if (strncmp(line, "DOOR=", 5) == 0)
    {
        const char *arg = &line[5];
        DoorState_t before = Door_GetState();
        DoorState_t after;

        if (strcmp(arg, "OPEN") == 0)
        {
            Door_KeyManualOpen();
            (void)Door_Update();          /* let the command take effect now */
            after = Door_GetState();

            if (after == DOOR_STATE_OPENING && before != DOOR_STATE_OPENING)
            {
                UART_SendString("OK DOOR OPENING\r\n");
            }
            else
            {
                /* Either disabled, or already open/opening. Say so rather than
                   claim success - a host script polling STATUS? would otherwise
                   see no state change and retry forever. */
                UART_Printf("ERR BUSY (state=%s run=%u)\r\n",
                            Door_StateName(after), (unsigned)Door_IsRunning());
            }
            return;
        }

        if (strcmp(arg, "CLOSE") == 0)
        {
            Door_KeyManualClose();
            (void)Door_Update();
            after = Door_GetState();

            if (after == DOOR_STATE_CLOSING && before != DOOR_STATE_CLOSING)
            {
                UART_SendString("OK DOOR CLOSING\r\n");
            }
            else if ((Sensor_IsOutsideActive() != 0U) ||
                     (Sensor_IsInsideActive() != 0U))
            {
                /* The controller refuses to start a close onto a person; report
                   the real reason instead of a generic failure. */
                UART_SendString("ERR OCCUPIED (person detected in doorway)\r\n");
            }
            else
            {
                UART_Printf("ERR BUSY (state=%s run=%u)\r\n",
                            Door_StateName(after), (unsigned)Door_IsRunning());
            }
            return;
        }

        UART_SendString("ERR BAD_ARG (OPEN|CLOSE)\r\n");
        return;
    }

    /* ---- STOP ----------------------------------------------------------- */
    if (strcmp(line, "STOP") == 0)
    {
        Door_EmergencyStop();
        UART_SendString("OK EMERGENCY STOP\r\n");
        return;
    }

    /* ---- RESET ---------------------------------------------------------- */
    /* Clears the fault latch ONLY. Deliberately not a factory reset: a host
       script sending RESET to recover from an e-stop must not silently wipe the
       configured delay. Factory defaults are a separate, explicitly named
       command. */
    if (strcmp(line, "RESET") == 0)
    {
        Door_Reset();
        UART_SendString("OK RESET (fault latch cleared)\r\n");
        return;
    }

    /* ---- DEFAULTS ------------------------------------------------------- */
    if (strcmp(line, "DEFAULTS") == 0)
    {
        uint8_t persistOk;

        /* The log is NOT cleared here. Losing history because someone asked for
           default settings would be a surprising and irreversible side effect. */
        Door_SetAutoCloseMs(DOOR_AUTO_CLOSE_MS);
        Door_SetMode(DOOR_MODE_AUTO);
        /* The travel duty belongs to the build, not to the settings, but DEFAULTS
           claims to restore defaults and leaving the door at a tuned speed would
           make that a half-truth. On the servo build the equivalent setting is the
           travel time, and unlike the duty it IS persisted - so it is reset here
           and written back with the other two. */
        (void)Door_SetTravelDuty(MOTOR_DEFAULT_DUTY);
        (void)Motor_SetTravelMs(DOOR_TRAVEL_MS);

        /* All three writes are reported, not discarded. */
        persistOk = ((Log_SetAutoCloseMs(DOOR_AUTO_CLOSE_MS) == MYI2C_OK) &&
                     (Log_SetPersistedMode((uint8_t)DOOR_MODE_AUTO) == MYI2C_OK) &&
                     (Log_SetPersistedTravelMs(DOOR_TRAVEL_MS) == MYI2C_OK))
                    ? 1U : 0U;

        Log_Add(LOG_EVT_PARAM_CHANGE, (uint8_t)Door_GetState(),
                (uint8_t)DOOR_MODE_AUTO, 0U);

        if (persistOk != 0U)
        {
            UART_Printf("OK DEFAULTS delay=%ums mode=AUTO travel=%ums (log kept)\r\n",
                        (unsigned)DOOR_AUTO_CLOSE_MS,
                        (unsigned)DOOR_TRAVEL_MS);
        }
        else
        {
            UART_Printf("OK DEFAULTS delay=%ums mode=AUTO travel=%ums "
                        "(NOT PERSISTED - eeprom write failed)\r\n",
                        (unsigned)DOOR_AUTO_CLOSE_MS,
                        (unsigned)DOOR_TRAVEL_MS);
        }
        return;
    }

    /* ---- HELP ----------------------------------------------------------- */
    if (strcmp(line, "HELP") == 0)
    {
        UART_SendString("OK HELP\r\n");
        UART_SendString("  STATUS?           report state, mode, delay, counts\r\n");
        UART_SendString("  LOG?              list the most recent 10 records\r\n");
        UART_SendString("  LOG?<n>           list the most recent <n> records\r\n");
        UART_SendString("  LOG?ALL           list every retained record\r\n");
        UART_SendString("  LOG?CLEAR         erase all records\r\n");
        UART_SendString("  DELAY=<ms>        auto-close delay, 1000..30000\r\n");
        UART_SendString("  TRAVEL=<ms>       travel time, 600..4000 (saved; default 1500)\r\n");
        UART_SendString("                    bigger = slower and smoother; 1000..2000 is the\r\n");
        UART_SendString("                    window where an SG90 sweeps instead of stepping\r\n");
        UART_SendString("  SPEED=<%>         n/a on the SG90 build - use TRAVEL=<ms>\r\n");
        UART_SendString("  MODE=AUTO|MANUAL  set the operating mode\r\n");
        UART_SendString("  DOOR=OPEN|CLOSE   manual travel command\r\n");
        UART_SendString("  STOP              emergency stop (latches)\r\n");
        UART_SendString("  RESET             clear the fault latch\r\n");
        UART_SendString("  DEFAULTS          delay/mode to defaults (keeps log)\r\n");
        UART_SendString("  HELP              this list\r\n");
        return;
    }

    UART_SendString("ERR UNKNOWN_CMD (try HELP)\r\n");
}
