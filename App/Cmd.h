/**
  ******************************************************************************
  * @file    Cmd.h
  * @brief   Line-based console protocol: reporting and host commands.
  *
  * The protocol is deliberately plain ASCII terminated by CR/LF, so any serial
  * terminal or generic Bluetooth terminal can drive it with no custom host
  * software. That matters here because it makes the whole system debuggable on
  * day one, and because the same protocol will later serve the Bluetooth link
  * unchanged.
  *
  * COMMANDS (host -> device)
  *   STATUS?          current state, mode, delay, record count
  *   LOG?             list the most recent records
  *   LOG?<n>          list the most recent <n> records
  *   LOG?ALL          list every retained record
  *   LOG?CLEAR        erase all records
  *   DELAY=<ms>       set the auto-close delay (1000..30000)
  *   MODE=AUTO        automatic mode
  *   MODE=MANUAL      manual mode
  *   DOOR=OPEN        manual open
  *   DOOR=CLOSE       manual close
  *   STOP             emergency stop
  *   RESET            clear the fault latch
  *   HELP             list the commands
  *
  * REPLIES (device -> host)
  *   OK <text>        success
  *   ERR <reason>     failure
  *   EVT ...          an event as it happens
  ******************************************************************************
  */

#ifndef __CMD_H
#define __CMD_H

#include "stm32f10x.h"
#include <stdint.h>
#include "Door.h"

/** @brief Print the banner and the self-test summary. */
void Cmd_Init(void);

/**
  * @brief  Consume and act on any complete received line.
  * @note   Call from the main loop; non-blocking when no line is ready.
  */
void Cmd_Process(void);

/*===========================================================================*/
/*  Reporting hooks - called from the application's Door callbacks            */
/*===========================================================================*/

/**
  * @brief  Report an event line.
  * @param  event       LogEvent_t value.
  * @param  durationMs  Travel time, or 0.
  */
void Cmd_ReportEvent(uint8_t event, DoorState_t state, DoorMode_t mode, uint16_t durationMs);

/** @brief Report a state change. */
void Cmd_ReportState(DoorState_t from, DoorState_t to);

/** @brief Report a fault being raised (or cleared). */
void Cmd_ReportFault(DoorFault_t fault);

/** @brief Report a periodic one-line status summary. */
void Cmd_ReportStatus(void);

/** @brief Report a free-form error string. */
void Cmd_ReportError(const char *text);

#endif /* __CMD_H */
