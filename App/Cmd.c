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

    UART_Printf("%lu:%02lu:%02lu.%03lu",
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
    UART_Printf("\r\nAutoDoor ready. boot#%u  records=%u  delay=%ums\r\n",
                (unsigned)Log_GetBootId(),
                (unsigned)Log_Count(),
                (unsigned)Log_GetAutoCloseMs());
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
    UART_Printf(" LIM=%u/%u", (unsigned)Limit_IsOpen(), (unsigned)Limit_IsClosed());
    UART_Printf(" I2C=%s", (MyI2C_IsIdle() != 0U) ? "ok" : "STUCK");
    UART_SendLine();
}

/*===========================================================================*/
/*  Command implementations                                                  */
/*===========================================================================*/

static void cmd_log(uint16_t want)
{
    uint16_t count = Log_Count();
    uint16_t i;
    uint16_t start;
    LogEntry_t e;

    if (count == 0U)
    {
        UART_SendString("OK LOG EMPTY\r\n");
        return;
    }

    if (want == 0U || want > count)
    {
        want = count;
    }

    /* Print the newest `want` records in chronological order. */
    start = (uint16_t)(count - want);

    UART_Printf("OK LOG %u of %u\r\n", (unsigned)want, (unsigned)count);

    for (i = 0U; i < want; i++)
    {
        if (Log_Get((uint16_t)(start + i), &e) != 0U)
        {
            UART_SendString("ERR LOG READ\r\n");
            return;
        }

        UART_Printf("REC %u #%u [", (unsigned)(start + i), (unsigned)e.seq);
        print_timestamp(e.timestampMs);
        UART_Printf("] boot#%u %-18s S=%-8s M=%s",
                    (unsigned)e.bootId,
                    Log_EventName(e.event),
                    Door_StateName((DoorState_t)e.doorState),
                    mode_name((DoorMode_t)e.mode));

        if (e.durationMs != 0U)
        {
            UART_Printf(" DUR=%ums", (unsigned)e.durationMs);
        }

        UART_SendLine();
    }

    UART_SendString("OK LOG END\r\n");
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
        if (Log_Clear() == 0U)
        {
            UART_SendString("OK LOG CLEARED\r\n");
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
        if (parse_u32(&line[6], &value) != 0U)
        {
            UART_SendString("ERR BAD_ARG\r\n");
            return;
        }

        if (Door_SetAutoCloseMs((uint16_t)value) != 0U)
        {
            UART_Printf("ERR RANGE %u..%u\r\n",
                        (unsigned)DOOR_AUTO_CLOSE_MIN_MS,
                        (unsigned)DOOR_AUTO_CLOSE_MAX_MS);
            return;
        }

        /* Persist so the setting survives a power cut. */
        (void)Log_SetAutoCloseMs((uint16_t)value);
        Log_Add(LOG_EVT_PARAM_CHANGE, (uint8_t)Door_GetState(),
                (uint8_t)Door_GetMode(), 0U);

        UART_Printf("OK DELAY=%u\r\n", (unsigned)value);
        return;
    }

    /* ---- MODE=AUTO | MODE=MANUAL ---------------------------------------- */
    if (strncmp(line, "MODE=", 5) == 0)
    {
        const char *arg = &line[5];
        DoorMode_t  m;

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
        (void)Log_SetPersistedMode((uint8_t)m);
        Log_Add(LOG_EVT_MODE_CHANGE, (uint8_t)Door_GetState(), (uint8_t)m, 0U);

        UART_Printf("OK MODE=%s\r\n", mode_name(m));
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
        /* The log is NOT cleared here. Losing history because someone asked for
           default settings would be a surprising and irreversible side effect. */
        Door_SetAutoCloseMs(DOOR_AUTO_CLOSE_MS);
        (void)Log_SetAutoCloseMs(DOOR_AUTO_CLOSE_MS);

        Door_SetMode(DOOR_MODE_AUTO);
        (void)Log_SetPersistedMode((uint8_t)DOOR_MODE_AUTO);

        Log_Add(LOG_EVT_PARAM_CHANGE, (uint8_t)Door_GetState(),
                (uint8_t)DOOR_MODE_AUTO, 0U);

        UART_Printf("OK DEFAULTS delay=%ums mode=AUTO (log kept)\r\n",
                    (unsigned)DOOR_AUTO_CLOSE_MS);
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
