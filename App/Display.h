/**
  ******************************************************************************
  * @file    Display.h
  * @brief   Display abstraction: the only interface the application uses.
  *
  * WHY AN ABSTRACTION LAYER
  * ------------------------
  * The real board has an SSD1306 OLED over I2C. The Proteus simulation has no
  * OLED model, so the simulation build uses a 1602 character LCD instead (see
  * docs/设计文档.md section 9.2). Those two devices share nothing at the driver
  * level - one is a 128x64 bitmap over I2C, the other a 16x2 character device on
  * a parallel bus.
  *
  * If the door logic called OLED_* directly, swapping or simulating the display
  * would mean editing every call site. Instead everything above goes through the
  * handful of functions below, which describe WHAT to show rather than HOW to
  * draw it. Door.c, Log.c and Cmd.c never include an OLED header.
  *
  * The vocabulary is deliberately small and semantic (a status view, an event
  * view, a log view) rather than a generic canvas. That keeps a character-only
  * implementation feasible: it can degrade the content instead of having to
  * emulate a bitmap.
  ******************************************************************************
  */

#ifndef __DISPLAY_H
#define __DISPLAY_H

#include "stm32f10x.h"
#include <stdint.h>
#include "Door.h"
#include "Log.h"

/** Which screen is showing. */
typedef enum
{
    DISP_SCREEN_STATUS = 0,     /* live state, mode, countdown, counters */
    DISP_SCREEN_EVENT,          /* the most recent events                */
    DISP_SCREEN_LOG,            /* browse the stored history             */
    DISP_SCREEN_COUNT
} DispScreen_t;

/** @brief Bring up the panel and show the boot screen. */
void Display_Init(void);

/** @return 1 when the panel answered on its bus during Display_Init(). */
uint8_t Display_IsPresent(void);

/**
  * @brief  Redraw the active screen.
  * @note   Call periodically (a few times a second) rather than every loop
  *         iteration: a full OLED flush is over I2C and costs milliseconds.
  */
void Display_Update(void);

/*===========================================================================*/
/*  Content                                                                  */
/*===========================================================================*/

/**
  * @brief  Feed the status view.
  * @param  state        Current door state.
  * @param  mode         Operating mode.
  * @param  fault        Active fault code.
  * @param  running      Whether the system is enabled (KEY1).
  * @param  countdownMs  Milliseconds until auto-close, 0 when not counting.
  * @param  records      Records currently stored.
  */
void Display_SetStatus(DoorState_t state, DoorMode_t mode, DoorFault_t fault,
                       uint8_t running, uint16_t countdownMs, uint16_t records);

/** @brief Push one event onto the event view (newest first). */
void Display_PushEvent(uint8_t event, uint32_t timestampMs);

/**
  * @brief  Show a transient message until the next Display_Update().
  * @note   Used for console command acknowledgements, so a host command is
  *         visible on the panel as well as in the terminal.
  */
void Display_ShowMessage(const char *line1, const char *line2);

/*===========================================================================*/
/*  Navigation                                                               */
/*===========================================================================*/

void Display_SetScreen(DispScreen_t screen);
DispScreen_t Display_GetScreen(void);

/** @brief Switch to the next screen (wraps). */
void Display_NextScreen(void);

/**
  * @brief  Page the log view up (older) or down (newer).
  * @param  dir  1 = older, 0 = newer.
  */
void Display_LogScroll(uint8_t dir);

/**
  * @brief  Handle a key press that may be a screen-navigation action.
  * @return 1 when the key was consumed by the display, 0 when the caller should
  *         handle it normally.
  * @note   Navigation only claims KEY3/KEY4 while the log screen is open AND the
  *         system is in manual mode with the door idle. Otherwise those keys must
  *         reach the door controller - silently swallowing a manual-open command
  *         because a menu is open would be a genuine operational bug.
  */
uint8_t Display_HandleKey(uint8_t isOpenKey, uint8_t isLongPress);

#endif /* __DISPLAY_H */
