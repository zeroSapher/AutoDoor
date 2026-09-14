/**
  ******************************************************************************
  * @file    EEPROM.c
  * @brief   AT24C32 (32 kbit / 4 KB) I2C EEPROM driver.
  *
  * ADDRESSING
  * ----------
  * The AT24C32 needs a 2-byte word address (it has 4096 locations) sent as two
  * bytes immediately after the device address. A read is therefore a write of
  * the 2-byte address followed by a repeated START and the read - which is
  * exactly what MyI2C_WriteRead() provides, and why the STOP must not appear
  * between them (it would reset the internal address pointer).
  *
  * PAGES
  * -----
  * Writes are buffered in an internal 32-byte page. If a write crosses a page
  * boundary the address counter wraps to the start of THAT page instead of
  * continuing, so the overflow silently clobbers the beginning of the page
  * instead of landing where the caller expects. The driver hides this by
  * splitting every write on page boundaries.
  ******************************************************************************
  */

#include "EEPROM.h"
#include "MyI2C.h"
#include "main.h"
#include "Delay.h"

/* Scratch area for the self test: the last 32 bytes, which the event log never
   reaches (see LOG_SLOT_COUNT in main.h). */
#define EEPROM_SCRATCH_ADDR     (EEPROM_SIZE_BYTES - EEPROM_PAGE_SIZE)

static uint8_t s_present = 0U;

/*===========================================================================*/
/*  Public API                                                               */
/*===========================================================================*/

uint8_t EEPROM_Init(void)
{
    uint8_t rc;

    rc = MyI2C_Probe(EEPROM_I2C_ADDRESS);
    s_present = (rc == MYI2C_OK) ? 1U : 0U;

    return rc;
}

uint8_t EEPROM_IsPresent(void)
{
    return s_present;
}

uint8_t EEPROM_Read(uint16_t addr, uint8_t *buf, uint16_t len)
{
    uint8_t wbuf[2];

    if ((buf == 0) || (len == 0U))
    {
        return MYI2C_ERR_PARAM;
    }
    if (((uint32_t)addr + len) > EEPROM_SIZE_BYTES)
    {
        return MYI2C_ERR_PARAM;
    }

    wbuf[0] = (uint8_t)(addr >> 8);
    wbuf[1] = (uint8_t)(addr & 0xFFU);

    return MyI2C_WriteRead(EEPROM_I2C_ADDRESS, wbuf, 2U, buf, len);
}

uint8_t EEPROM_Write(uint16_t addr, const uint8_t *buf, uint16_t len)
{
    uint8_t wbuf[2 + EEPROM_PAGE_SIZE];
    uint8_t rc;

    if ((buf == 0) || (len == 0U))
    {
        return MYI2C_ERR_PARAM;
    }
    if (((uint32_t)addr + len) > EEPROM_SIZE_BYTES)
    {
        return MYI2C_ERR_PARAM;
    }

    while (len != 0U)
    {
        /* Bytes that still fit before the next page boundary. */
        uint16_t pageRoom = (uint16_t)(EEPROM_PAGE_SIZE - (addr % EEPROM_PAGE_SIZE));
        uint16_t n = (len < pageRoom) ? len : pageRoom;
        uint16_t i;

        wbuf[0] = (uint8_t)(addr >> 8);
        wbuf[1] = (uint8_t)(addr & 0xFFU);
        for (i = 0U; i < n; i++)
        {
            wbuf[2U + i] = buf[i];
        }

        /* WriteWithPoll returns only once the device has finished its internal
           write cycle (~5 ms), so the next chunk can start immediately. */
        rc = MyI2C_WriteWithPoll(EEPROM_I2C_ADDRESS, wbuf, (uint16_t)(n + 2U),
                                 EEPROM_WRITE_TIMEOUT_MS);
        if (rc != MYI2C_OK)
        {
            return rc;
        }

        addr = (uint16_t)(addr + n);
        buf += n;
        len = (uint16_t)(len - n);
    }

    return MYI2C_OK;
}

uint8_t EEPROM_IsBlank(void)
{
    uint8_t buf[EEPROM_PAGE_SIZE];
    uint16_t i;

    if (EEPROM_Read(0U, buf, EEPROM_PAGE_SIZE) != MYI2C_OK)
    {
        return 0U;
    }

    for (i = 0U; i < EEPROM_PAGE_SIZE; i++)
    {
        if (buf[i] != 0xFFU)
        {
            return 0U;
        }
    }

    return 1U;
}

uint8_t EEPROM_SelfTest(uint16_t *failAddr)
{
    /* Deliberately not 0x00/0xFF: a stuck-at bus reads back as 0xFF, and a
       write that never lands reads back as whatever was there before, so a
       pattern with both 0 and 1 bits catches both faults. */
    static const uint8_t pattern[4] = { 0xA5U, 0x5AU, 0x3CU, 0xC3U };
    uint8_t  verify[4];
    uint16_t offset;

    if (failAddr != 0)
    {
        *failAddr = 0U;
    }

    if (EEPROM_Init() != MYI2C_OK)
    {
        return MYI2C_ERR_NACK;
    }

    for (offset = 0U; offset < EEPROM_PAGE_SIZE; offset += 4U)
    {
        uint16_t a = (uint16_t)(EEPROM_SCRATCH_ADDR + offset);

        if (EEPROM_Write(a, pattern, 4U) != MYI2C_OK)
        {
            if (failAddr != 0) { *failAddr = a; }
            return MYI2C_ERR_TIMEOUT;
        }
        if (EEPROM_Read(a, verify, 4U) != MYI2C_OK)
        {
            if (failAddr != 0) { *failAddr = a; }
            return MYI2C_ERR_TIMEOUT;
        }

        {
            uint8_t k;
            for (k = 0U; k < 4U; k++)
            {
                if (verify[k] != pattern[k])
                {
                    if (failAddr != 0) { *failAddr = (uint16_t)(a + k); }
                    return MYI2C_ERR_NACK;
                }
            }
        }
    }

    return MYI2C_OK;
}
