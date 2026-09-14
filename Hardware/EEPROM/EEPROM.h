/**
  ******************************************************************************
  * @file    EEPROM.h
  * @brief   AT24C32 (32 kbit / 4 KB) I2C EEPROM driver.
  *
  * Why an external EEPROM rather than the STM32's own flash: the F103 flash is
  * rated for about 10 000 erase cycles. A door that logs a few dozen events a
  * day would wear a page out within a year or two. The AT24C32 is rated for
  * 1 000 000 cycles, which is decades at that rate.
  ******************************************************************************
  */

#ifndef __EEPROM_H
#define __EEPROM_H

#include "stm32f10x.h"
#include <stdint.h>

/** @brief Verify the device answers on the bus. MYI2C_OK when present. */
uint8_t EEPROM_Init(void);

/** @return 1 when the device acknowledged during EEPROM_Init(). */
uint8_t EEPROM_IsPresent(void);

/**
  * @brief  Read len bytes starting at a 16-bit word address.
  * @note   A whole-page or whole-device read is a single transaction: the
  *         AT24C32 auto-increments its address pointer and wraps internally.
  */
uint8_t EEPROM_Read(uint16_t addr, uint8_t *buf, uint16_t len);

/**
  * @brief  Write len bytes, splitting at page boundaries.
  * @note   THIS IS THE PART THAT IS EASY TO GET WRONG. An AT24C32 write wraps
  *         around WITHIN a 32-byte page: asking it to write across a boundary
  *         does not continue into the next page, it silently overwrites the
  *         start of the current one. Every transfer is therefore split so that
  *         no single write crosses a page, and each chunk is followed by
  *         acknowledge polling.
  */
uint8_t EEPROM_Write(uint16_t addr, const uint8_t *buf, uint16_t len);

/** @return 1 when the whole device reads back as 0xFF (blank). */
uint8_t EEPROM_IsBlank(void);

/*===========================================================================*/
/*  Self test                                                                */
/*===========================================================================*/

/**
  * @brief  Read/write/verify a scratch area at the end of the device.
  * @param  failAddr  Receives the first address that failed to verify, if any.
  * @return MYI2C_OK on success, non-zero on failure.
  * @note   Uses the last 32 bytes of the device, which the log never reaches,
  *         so a self test is non-destructive to recorded data.
  */
uint8_t EEPROM_SelfTest(uint16_t *failAddr);

#endif /* __EEPROM_H */
