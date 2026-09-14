/**
  ******************************************************************************
  * @file    EEPROM_Stub.c
  * @brief   No-EEPROM build variant: keeps the event log working in RAM only.
  *
  * WHY THIS EXISTS
  * ---------------
 * In the Proteus simulation the OLED display uses the shared I2C pins, but the
 * EEPROM is intentionally omitted so the build exercises the
 * no-persistence path. Rather than let the
  * link fail on missing EEPROM_* symbols, or fake a working EEPROM (which would
  * let the simulation claim a persistence feature it never tested), this stub
  * reports "device absent" for every call.
  *
  * That routes the application down the degradation path it already implements:
  * Log_Init() fails, the console prints "Log: UNAVAILABLE - running with
  * defaults", and the door keeps working with defaults and no history. So the
  * simulation verifies the DEGRADATION behaviour, which is real behaviour worth
  * testing, and the console says plainly what is missing.
  *
  * Persistence itself must be verified on the real board - see
  * docs/上电调试步骤.md step 6.
  ******************************************************************************
  */

#include "EEPROM.h"
#include "MyI2C.h"

uint8_t EEPROM_Init(void)
{
    return MYI2C_ERR_NACK;      /* no device on this bus */
}

uint8_t EEPROM_IsPresent(void)
{
    return 0U;
}

uint8_t EEPROM_Read(uint16_t addr, uint8_t *buf, uint16_t len)
{
    /* Zero-fill rather than leaving the buffer untouched: a caller that ignores
       the return code then reads deterministic data instead of stack garbage. */
    uint16_t i;

    (void)addr;

    if (buf != 0)
    {
        for (i = 0U; i < len; i++)
        {
            buf[i] = 0U;
        }
    }

    return MYI2C_ERR_NACK;
}

uint8_t EEPROM_Write(uint16_t addr, const uint8_t *buf, uint16_t len)
{
    (void)addr;
    (void)buf;
    (void)len;

    return MYI2C_ERR_NACK;
}

uint8_t EEPROM_IsBlank(void)
{
    return 0U;      /* nothing readable, so not "blank" in the usable sense */
}

uint8_t EEPROM_SelfTest(uint16_t *failAddr)
{
    if (failAddr != 0)
    {
        *failAddr = 0U;
    }
    return MYI2C_ERR_NACK;
}
