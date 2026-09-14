/**
  ******************************************************************************
  * @file    Display.c
  * @brief   SSD1306 implementation of the display abstraction.
  *
  * RENDERING STRATEGY
  * ------------------
  * The panel is driven from a RAM framebuffer by the underlying OLED driver, so
  * redrawing a screen is cheap in CPU terms but not over the wire: a full flush
  * is 1024 bytes of I2C. Two consequences shape this file:
  *
  *   1. Display_Update() is called a few times a second, never per loop
  *      iteration, because each full redraw costs milliseconds of bus time.
  *   2. The status screen only repaints the rows that change. The title is
  *      static, so repainting it every frame would be wasted bus time on a bus
  *      shared with the EEPROM.
  *
  * Layout is fixed rather than computed: a 128x64 panel has room for a title row
  * plus three 8x16 rows, and hard-coding that is clearer (and cheaper) than a
  * layout engine for three screens.
  ******************************************************************************
  */

#include "Display.h"
#include "Display_Backend.h"
#include "main.h"
#include <string.h>
#include "Fmt.h"

/*===========================================================================*/
/*  Layout                                                                   */
/*===========================================================================*/
/* 128x64, using the 8x16 font for figures and the 6x8 font for footnotes. */

#define ROW_TITLE_Y     0U      /* 8x16 */
#define ROW_STATE_Y     18U     /* 8x16 */
#define ROW_MODE_Y      36U     /* 8x16 */
#define ROW_FOOT_Y      54U     /* 6x8  */

/* The event screen uses the small font throughout so several lines fit. */
#define EVT_LINE_H      10U
#define EVT_FIRST_Y     12U
#define EVT_ROWS        5U

/*===========================================================================*/
/*  State                                                                    */
/*===========================================================================*/

#define EVENT_HISTORY   8U

typedef struct
{
    uint8_t  event;
    uint32_t timestampMs;
} EventRecord_t;

static uint8_t  s_present = 0U;
static uint8_t  s_dirty   = 1U;
static uint8_t  s_staticDrawn = 0U;

static DispScreen_t s_screen = DISP_SCREEN_STATUS;

/* Status view snapshot. */
static DoorState_t s_state     = DOOR_STATE_INIT;
static DoorMode_t  s_mode      = DOOR_MODE_AUTO;
static DoorFault_t s_fault     = DOOR_FAULT_NONE;
static uint8_t     s_running   = 0U;
static uint16_t    s_countdown = 0U;
static uint16_t    s_records   = 0U;

/* Event view ring, newest at index 0. */
static EventRecord_t s_events[EVENT_HISTORY];
static uint8_t       s_eventCount = 0U;

/* Log browsing cursor: how many records back from the newest is the top line. */
static uint16_t s_logOffset = 0U;

/* Transient message overlay. */
static uint8_t s_msgActive = 0U;
static char    s_msg1[22];
static char    s_msg2[22];
static uint8_t s_msgFrames = 0U;    /* frames to keep showing it */

/*===========================================================================*/
/*  Small formatting helpers                                                 */
/*===========================================================================*/

/** Print a relative timestamp as H:MM:SS into a caller-supplied buffer. */
static void fmt_timestamp(uint32_t ms, char *out, uint8_t outLen)
{
    uint32_t totalSec = ms / 1000UL;
    uint32_t hours    = totalSec / 3600UL;
    uint32_t minutes  = (totalSec / 60UL) % 60UL;
    uint32_t seconds  = totalSec % 60UL;

    (void)Fmt_Format(out, outLen, "%lu:%02lu:%02lu",
                   (unsigned long)hours,
                   (unsigned long)minutes,
                   (unsigned long)seconds);
}

/*===========================================================================*/
/*  Screen: status                                                           */
/*===========================================================================*/

/**
  * @brief  Should the status row show a countdown?
  * @note   Only while the door is open in auto mode with the system enabled -
  *         exactly the conditions under which the controller is actually
  *         counting, so the panel never shows a timer that is not running.
  */
static uint8_t s_showCountdownNeeded(void)
{
    return ((s_state == DOOR_STATE_OPEN) &&
            (s_mode == DOOR_MODE_AUTO) &&
            (s_running != 0U) &&
            (s_countdown != 0U)) ? 1U : 0U;
}

static void draw_status(void)
{
    char buf[24];
    const char *faultName;

    /* Static title row, drawn once. */
    if (s_staticDrawn == 0U)
    {
        DispBk_Text(0, ROW_TITLE_Y, "AutoDoor", DISPBK_FONT_LARGE);
        s_staticDrawn = 1U;
    }

    /* ---- Row 2: state, plus the countdown when one is running ------------- */
    if (s_showCountdownNeeded())
    {
        /* Right-aligned countdown so the seconds do not jitter the layout. */
        (void)Fmt_Format(buf, sizeof(buf), "%-9s %2us",
                       Door_StateName(s_state),
                       (unsigned)((s_countdown + 999U) / 1000U));
    }
    else
    {
        (void)Fmt_Format(buf, sizeof(buf), "%-12s", Door_StateName(s_state));
    }
    DispBk_Text(0, ROW_STATE_Y, buf, DISPBK_FONT_LARGE);

    /* ---- Row 3: run flag, mode, fault ------------------------------------ */
    faultName = (s_fault == DOOR_FAULT_NONE) ? "" : Door_FaultName(s_fault);
    (void)Fmt_Format(buf, sizeof(buf), "%s %-6s %s",
                   (s_running != 0U) ? "RUN " : "STOP",
                   (s_mode == DOOR_MODE_AUTO) ? "AUTO" : "MAN",
                   faultName);
    DispBk_Text(0, ROW_MODE_Y, buf, DISPBK_FONT_LARGE);

    /* ---- Footer: record count and the most recent event ------------------ */
    if (s_eventCount != 0U)
    {
        (void)Fmt_Format(buf, sizeof(buf), "REC %-3u %s",
                       (unsigned)s_records,
                       Log_EventName(s_events[0].event));
    }
    else
    {
        (void)Fmt_Format(buf, sizeof(buf), "REC %-3u no events", (unsigned)s_records);
    }
    DispBk_Text(0, ROW_FOOT_Y, buf, DISPBK_FONT_SMALL);
}

/*===========================================================================*/
/*  Screen: recent events                                                    */
/*===========================================================================*/

static void draw_events(void)
{
    uint8_t i;
    char    ts[12];

    DispBk_Text(0, 0, "Recent events", DISPBK_FONT_SMALL);

    if (s_eventCount == 0U)
    {
        DispBk_Text(0, EVT_FIRST_Y, "(none yet)", DISPBK_FONT_SMALL);
        return;
    }

    for (i = 0U; i < EVT_ROWS; i++)
    {
        char line[24];

        if (i >= s_eventCount)
        {
            break;
        }

        fmt_timestamp(s_events[i].timestampMs, ts, sizeof(ts));

        (void)Fmt_Format(line, sizeof(line), "%-8s %s",
                       ts, Log_EventName(s_events[i].event));
        DispBk_Text(0, (int16_t)(EVT_FIRST_Y + (i * EVT_LINE_H)), line, DISPBK_FONT_SMALL);
    }
}

/*===========================================================================*/
/*  Screen: stored log                                                       */
/*===========================================================================*/

static void draw_log(void)
{
    uint16_t count = Log_Count();
    uint16_t i;
    char     header[24];
    uint16_t shownFrom;

    /* Show which slice of the history is on screen, counted from newest (1).
       This is what tells the user how far they have paged back. */
    shownFrom = (count > s_logOffset) ? (uint16_t)(count - s_logOffset) : 0U;
    (void)Fmt_Format(header, sizeof(header), "Log %u/%u",
                   (unsigned)shownFrom, (unsigned)count);
    DispBk_Text(0, 0, header, DISPBK_FONT_SMALL);

    if (count == 0U)
    {
        DispBk_Text(0, EVT_FIRST_Y, "(no records)", DISPBK_FONT_SMALL);
        return;
    }

    for (i = 0U; i < EVT_ROWS; i++)
    {
        LogEntry_t e;
        char       line[24];
        uint16_t   index;

        /* Newest first: the cursor counts back from the end of the log. */
        if ((uint32_t)s_logOffset + i >= count)
        {
            break;
        }
        index = (uint16_t)(count - 1U - s_logOffset - i);

        if (Log_Get(index, &e) != 0U)
        {
            break;
        }

        (void)Fmt_Format(line, sizeof(line), "%u %s",
                       (unsigned)e.seq, Log_EventName(e.event));
        DispBk_Text(0, (int16_t)(EVT_FIRST_Y + (i * EVT_LINE_H)), line, DISPBK_FONT_SMALL);
    }
}

/*===========================================================================*/
/*  Message overlay                                                          */
/*===========================================================================*/

static void draw_message(void)
{
    DispBk_Clear();
    DispBk_Text(0, 12, s_msg1, DISPBK_FONT_LARGE);
    DispBk_Text(0, 36, s_msg2, DISPBK_FONT_SMALL);
}

/*===========================================================================*/
/*  Public API                                                               */
/*===========================================================================*/

void Display_Init(void)
{
    DispBk_Init();

    s_present     = DispBk_IsPresent();
    s_eventCount  = 0U;
    s_logOffset   = 0U;
    s_msgActive   = 0U;
    s_msgFrames   = 0U;
    s_staticDrawn = 0U;
    s_dirty       = 1U;
    s_screen      = DISP_SCREEN_STATUS;

    if (s_present != 0U)
    {
        DispBk_Clear();
        DispBk_Text(0, 8, "AutoDoor", DISPBK_FONT_LARGE);
        DispBk_Text(0, 32, "self test...", DISPBK_FONT_SMALL);
        DispBk_Flush();
    }
}

uint8_t Display_IsPresent(void)
{
    return s_present;
}

void Display_SetStatus(DoorState_t state, DoorMode_t mode, DoorFault_t fault,
                       uint8_t running, uint16_t countdownMs, uint16_t records)
{
    /* Mark dirty only on a real change, so a steady door does not repaint the
       panel and steal bus time from the EEPROM. */
    if ((state != s_state) || (mode != s_mode) || (fault != s_fault) ||
        (running != s_running) || (records != s_records) ||
        /* The countdown changes every second, which is the intended refresh. */
        ((countdownMs / 1000U) != (s_countdown / 1000U)))
    {
        s_dirty = 1U;
    }

    /* The title comes and goes with the fault banner, so force a full redraw
       whenever the screen is not the status screen either. */
    if (s_screen != DISP_SCREEN_STATUS)
    {
        s_dirty = 1U;
    }

    s_state     = state;
    s_mode      = mode;
    s_fault     = fault;
    s_running   = running;
    s_countdown = countdownMs;
    s_records   = records;
}

void Display_PushEvent(uint8_t event, uint32_t timestampMs)
{
    uint8_t i;

    /* Shift the ring so index 0 is always the newest. Eight entries is small
       enough that a memmove-style shift is cheaper than index arithmetic. */
    for (i = EVENT_HISTORY - 1U; i > 0U; i--)
    {
        s_events[i] = s_events[i - 1U];
    }

    s_events[0].event       = event;
    s_events[0].timestampMs = timestampMs;

    if (s_eventCount < EVENT_HISTORY)
    {
        s_eventCount++;
    }

    s_dirty = 1U;
}

void Display_ShowMessage(const char *line1, const char *line2)
{
    if (line1 == 0)
    {
        return;
    }

    (void)Fmt_Format(s_msg1, sizeof(s_msg1), "%s", line1);
    (void)Fmt_Format(s_msg2, sizeof(s_msg2), "%s", (line2 != 0) ? line2 : "");

    s_msgActive = 1U;
    s_msgFrames = 3U;       /* roughly a second at the caller's refresh rate */
    s_dirty     = 1U;
}

void Display_Update(void)
{
    if (s_present == 0U)
    {
        return;
    }

    if (s_msgActive != 0U)
    {
        draw_message();

        if (s_msgFrames != 0U)
        {
            s_msgFrames--;
        }
        else
        {
            /* Message expired: fall back to the active screen and force a full
               repaint, because the overlay cleared the framebuffer. */
            s_msgActive   = 0U;
            s_staticDrawn = 0U;
        }

        DispBk_Flush();
        return;
    }

    if (s_dirty == 0U)
    {
        return;
    }

    s_dirty = 0U;

    /* Every screen except the status one repaints in full; only the status
       screen has a static row worth preserving. */
    if (s_screen != DISP_SCREEN_STATUS)
    {
        DispBk_Clear();
    }

    switch (s_screen)
    {
        case DISP_SCREEN_EVENT:
            draw_events();
            break;

        case DISP_SCREEN_LOG:
            draw_log();
            break;

        case DISP_SCREEN_STATUS:
        default:
            draw_status();
            break;
    }

    DispBk_Flush();
}

/*===========================================================================*/
/*  Navigation                                                               */
/*===========================================================================*/

void Display_SetScreen(DispScreen_t screen)
{
    if (screen >= DISP_SCREEN_COUNT)
    {
        return;
    }

    if (screen != s_screen)
    {
        s_screen      = screen;
        s_staticDrawn = 0U;     /* the status title must be redrawn */
        s_dirty       = 1U;

        if (screen == DISP_SCREEN_LOG)
        {
            s_logOffset = 0U;   /* always open the log at the newest entry */
        }
    }
}

DispScreen_t Display_GetScreen(void)
{
    return s_screen;
}

void Display_NextScreen(void)
{
    DispScreen_t next = (DispScreen_t)(((uint8_t)s_screen + 1U) % (uint8_t)DISP_SCREEN_COUNT);

    Display_SetScreen(next);
}

void Display_LogScroll(uint8_t dir)
{
    uint16_t count = Log_Count();

    if (s_screen != DISP_SCREEN_LOG)
    {
        return;
    }

    if (dir != 0U)
    {
        /* Older: stop when the top line would run past the oldest record. */
        if ((uint32_t)s_logOffset + EVT_ROWS < count)
        {
            s_logOffset++;
            s_dirty = 1U;
        }
    }
    else
    {
        if (s_logOffset != 0U)
        {
            s_logOffset--;
            s_dirty = 1U;
        }
    }
}

uint8_t Display_HandleKey(uint8_t isOpenKey, uint8_t isLongPress)
{
    /*
     * Navigation must never steal a door command. KEY3/KEY4 are the manual
     * open/close keys, so they are only treated as browsing controls when there
     * is nothing to command: manual mode, door not moving, system disabled.
     * In auto mode, or while the door is travelling, they go straight through to
     * the door controller.
     */
    uint8_t browsingAllowed;

    browsingAllowed = ((s_mode == DOOR_MODE_MANUAL) &&
                       (s_running == 0U) &&
                       (s_screen == DISP_SCREEN_LOG)) ? 1U : 0U;

    if (browsingAllowed == 0U)
    {
        return 0U;
    }

    if (isLongPress != 0U)
    {
        /* A long press on either key jumps back to the live status screen, which
           is the natural "get me out of here" gesture. */
        Display_SetScreen(DISP_SCREEN_STATUS);
        return 1U;
    }

    /* Short press: KEY3 pages to older records, KEY4 to newer. */
    Display_LogScroll(isOpenKey);
    return 1U;
}
