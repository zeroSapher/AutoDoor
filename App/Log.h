/**
  ******************************************************************************
  * @file    Log.h
  * @brief   Persistent event log: RAM queue backed by the AT24C32 EEPROM.
  *
  * STORAGE LAYOUT (4096 bytes total)
  * ---------------------------------
  *   0x0000  header, 32 bytes
  *   0x0020  record area: 252 slots x 16 bytes = 4032 bytes, ring buffer
  *
  * The record area is a ring: once full the oldest entry is overwritten. A door
  * generates a handful of events a day, so 252 slots is weeks of history, and a
  * bounded ring is far more robust than trying to manage "delete oldest" through
  * a filesystem on 4 KB of EEPROM.
  *
  * WHY WRITES ARE BATCHED
  * ----------------------
  * An EEPROM write blocks for about 5 ms (page write plus acknowledge polling).
  * Doing that inside an event handler would stall the main loop and, worse, could
  * delay the limit-switch response. Events are therefore queued in RAM and
  * flushed in batches, and a power cut can lose at most the last few hundred
  * milliseconds of entries. That is the right trade for a door: never delay the
  * safety path in order to guarantee a log line.
  ******************************************************************************
  */

#ifndef __LOG_H
#define __LOG_H

#include "stm32f10x.h"
#include <stdint.h>

/*===========================================================================*/
/*  Event codes - these are persisted, so values must never be renumbered.    */
/*===========================================================================*/
typedef enum
{
    LOG_EVT_NONE = 0x00,
    LOG_EVT_ENTER = 0x01,            /* outside sensor: someone entering      */
    LOG_EVT_EXIT = 0x02,             /* inside sensor: someone leaving        */
    LOG_EVT_OPEN_START = 0x03,       /* started opening                       */
    LOG_EVT_CLOSE_START = 0x04,      /* started closing                       */
    LOG_EVT_OPEN_DONE = 0x05,        /* reached the open limit                */
    LOG_EVT_REVERSE = 0x06,          /* reversed while closing (person seen)  */
    LOG_EVT_CLOSE_DONE = 0x07,       /* reached the closed limit              */
    LOG_EVT_ESTOP = 0x08,            /* emergency stop                        */
    LOG_EVT_FAULT_OPEN = 0x09,       /* travel timeout while opening          */
    LOG_EVT_FAULT_CLOSE = 0x0A,      /* travel timeout while closing          */
    LOG_EVT_MODE_CHANGE = 0x0B,      /* AUTO <-> MANUAL                       */
    LOG_EVT_PARAM_CHANGE = 0x0C,     /* a setting was changed                 */
    LOG_EVT_LIMIT_FAULT = 0x0D,      /* both limits asserted at once          */
    LOG_EVT_SYSTEM_START = 0x0E,     /* power-on self test passed             */
    LOG_EVT_SYSTEM_STOP = 0x0F       /* system stopped by the user            */
} LogEvent_t;

/*===========================================================================*/
/*  Record layout - 16 bytes, little endian                                  */
/*===========================================================================*/
typedef struct
{
    uint16_t seq;           /* monotonic record number, for ordering         */
    uint32_t timestampMs;   /* milliseconds since power-on                   */
    uint16_t bootId;        /* which power cycle this belongs to             */
    uint8_t  event;         /* LogEvent_t                                    */
    uint8_t  doorState;     /* DoorState_t after the event                   */
    uint8_t  mode;          /* 0 = AUTO, 1 = MANUAL                          */
    uint16_t durationMs;    /* travel time for OPEN_DONE / CLOSE_DONE        */
    uint8_t  reserved[3];
} LogEntry_t;

/*===========================================================================*/
/*  Lifecycle                                                                */
/*===========================================================================*/

/**
  * @brief  Load the header, or initialise a fresh ring if the EEPROM is blank.
  * @return 0 on success, non-zero when the EEPROM is absent or unusable.
  * @note   This scans the record area to find the newest sequence number, which
  *         is how the ring position is recovered after a power cut - a stored
  *         index alone would be ambiguous if the power failed mid-write.
  */
uint8_t Log_Init(void);

/** @brief Advance the flush timer and write pending records; call per ms. */
void Log_Tick1ms(void);

/** @brief Force a flush now (used before reporting counts and on shutdown). */
void Log_Flush(void);

/*===========================================================================*/
/*  Recording                                                                */
/*===========================================================================*/

/**
  * @brief  Queue one event.
  * @param  event      LogEvent_t value.
  * @param  doorState  DoorState_t in effect after the event.
  * @param  mode       0 = AUTO, 1 = MANUAL.
  * @param  durationMs Travel time, or 0 when not applicable.
  * @note   Never blocks and never touches the EEPROM, so it is safe to call from
  *         anywhere including shortly after an interrupt.
  */
void Log_Add(LogEvent_t event, uint8_t doorState, uint8_t mode, uint16_t durationMs);

/** @return Number of records currently stored (0..LOG_SLOT_COUNT). */
uint16_t Log_Count(void);

/** @return Total events ever recorded on this unit (survives ring wrap). */
uint64_t Log_TotalEvents(void);

/**
  * @brief  Read one record by position, where 0 is the oldest retained.
  * @return 0 on success, non-zero when index is out of range.
  */
uint8_t Log_Get(uint16_t index, LogEntry_t *out);

/** @brief Erase every record (writes a fresh header, keeps the sequence). */
uint8_t Log_Clear(void);

/*===========================================================================*/
/*  Persisted settings                                                       */
/*===========================================================================*/
/* Settings live in the same header, so they survive a power cut and are written
   with the same page-safe writer as the records. */

uint16_t Log_GetAutoCloseMs(void);
uint8_t  Log_SetAutoCloseMs(uint16_t ms);

uint8_t  Log_SetPersistedMode(uint8_t mode);
uint8_t  Log_GetPersistedMode(void);

/** @return The boot counter, incremented once per power-up. */
uint16_t Log_GetBootId(void);

/** @brief Human-readable name for an event code (for the console). */
const char *Log_EventName(uint8_t event);

#endif /* __LOG_H */
