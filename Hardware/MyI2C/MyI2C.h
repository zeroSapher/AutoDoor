/**
  ******************************************************************************
  * @file    MyI2C.h
  * @brief   Robust software (bit-banged) I2C master on two open-drain GPIOs.
  *
  * The bus is shared by the SSD1306 OLED and the AT24C32 EEPROM, on a board that
  * also carries a brushed DC motor, so this driver has to survive electrical
  * noise rather than merely work on a bench. Compared with a minimal bit-bang
  * driver it adds three things:
  *
  *   1. Deterministic exit state - every code path leaves BOTH lines released
  *      high, which is the I2C idle state, so the bus is never left half-driven
  *      by this master and MyI2C_IsIdle() means what it says. (It used to leave
  *      SCL driven low, copied from multi-master bus parking - see the note in
  *      MyI2C.c. Among other things that made MyI2C_IsIdle() permanently false,
  *      which silently killed the console's I2C diagnostic and made
  *      MyI2C_BusRecover() unable to ever report success.)
  *   2. Bus recovery - if a slave holds a line low (the classic symptom of a
  *      slave reset mid-byte, common next to motor noise), MyI2C_BusRecover()
  *      clocks up to 9 pulses plus a STOP to force every slave back to a known
  *      state. Without this the only cure is a power cycle.
  *   3. Acknowledge polling - MyI2C_WriteWithPoll() waits for the device to
  *      answer again instead of sleeping a guessed delay. This is both faster
  *      and more reliable than a fixed wait.
  *
  * Timing: SCL is held low for MYI2C_DELAY_US at each phase, so the bus runs at
  * roughly 1 / (4 * MYI2C_DELAY_US) - about 50 kHz at the default 5 us. Slow and
  * tolerant of long breadboard wiring, which matters far more here than raw
  * speed (the largest transfer is a 1 KB OLED frame).
  ******************************************************************************
  */

#ifndef __MYI2C_H
#define __MYI2C_H

#include "stm32f10x.h"
#include <stdint.h>

/*===========================================================================*/
/*  Result codes                                                             */
/*===========================================================================*/
/* Failures are positive so that "if (rc != MYI2C_OK)" reads naturally and a
   stray non-zero value is still treated as an error. */
#define MYI2C_OK            0
#define MYI2C_ERR_NACK      1   /* a device did not acknowledge */
#define MYI2C_ERR_TIMEOUT   2   /* SCL held low too long, or bus stuck low */
#define MYI2C_ERR_PARAM     3   /* bad argument */

/*===========================================================================*/
/*  Lifecycle                                                                */
/*===========================================================================*/

/**
  * @brief  Configure SCL/SDA as open-drain outputs, release the bus and clear
  *         any lines a slave was already holding low.
  */
void MyI2C_Init(void);

/**
  * @brief  Free a bus that a slave is holding low.
  * @return MYI2C_OK if the bus ends up idle, MYI2C_ERR_TIMEOUT otherwise.
  * @note   Called automatically by MyI2C_Init() and by bus_start(), so an
  *         application rarely needs it - but it is exposed for a manual retry
  *         after a reported failure.
  */
uint8_t MyI2C_BusRecover(void);

/** @return 1 when both lines are released (bus idle), 0 when a line is held
  *          low by something. */
uint8_t MyI2C_IsIdle(void);

/*===========================================================================*/
/*  Transactions                                                             */
/*===========================================================================*/

/** Probe an address (8-bit, write direction). MYI2C_OK = device present. */
uint8_t MyI2C_Probe(uint8_t devAddr);

/** Write len bytes to a device. */
uint8_t MyI2C_Write(uint8_t devAddr, const uint8_t *buf, uint16_t len);

/** Read len bytes from a device (START, address|1, read, STOP). */
uint8_t MyI2C_Read(uint8_t devAddr, uint8_t *buf, uint16_t len);

/**
  * @brief  Write, then poll until the device acknowledges again.
  * @param  timeoutMs  Give up after this long.
  * @return MYI2C_OK, or MYI2C_ERR_TIMEOUT if the device never came back.
  * @note   The correct way to write EEPROM: the part ignores the bus for about
  *         5 ms while its internal write completes, and a fixed delay either
  *         wastes time or corrupts data. Polling detects real completion.
  */
uint8_t MyI2C_WriteWithPoll(uint8_t devAddr, const uint8_t *buf, uint16_t len,
                            uint32_t timeoutMs);

/**
  * @brief  Write a pointer/register, then read back in one transaction using a
  *         repeated START (no STOP in between).
  * @note   Required by pointer-addressed devices such as the AT24C32: the STOP
  *         of a separate write would reset the internal address pointer.
  */
uint8_t MyI2C_WriteRead(uint8_t devAddr, const uint8_t *wbuf, uint16_t wlen,
                        uint8_t *rbuf, uint16_t rlen);

/** Emit a STOP and return the bus to rest, whatever state it was in. */
void MyI2C_ForceStop(void);

#endif /* __MYI2C_H */
