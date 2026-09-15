/**
  ******************************************************************************
  * @file    MyI2C.c
  * @brief   Robust software (bit-banged) I2C master. See MyI2C.h for the design
  *          rationale; this file documents the implementation details.
  *
  * INVARIANTS MAINTAINED BY EVERY FUNCTION
  * ---------------------------------------
  *   I1. On entry to any public function, the bus is either idle (SDA and SCL
  *       released) or held by this master mid-transaction.
  *   I2. On return from any public function, the bus is left IDLE: BOTH SCL and
  *       SDA released high. That is what I2C defines as idle, and it is what
  *       every slave expects between transactions.
  *
  *       This used to say "SCL is driven LOW and SDA is RELEASED", on the
  *       reasoning that holding SCL low stops a slave from starting a transfer
  *       while the master is not looking. That reasoning is a MULTI-MASTER bus
  *       parking trick, and this system has exactly one master - there is nobody
  *       to block. What it actually cost:
  *
  *         - MyI2C_IsIdle() could never be true, so the console line
  *           "I2C idle : ..." printed NO (bus held low) on EVERY boot no matter
  *           how the board was wired. The one diagnostic meant to distinguish
  *           "missing pull-ups" from "device not answering" was dead.
  *         - MyI2C_Init() ends by calling MyI2C_BusRecover(), which ends in
  *           bus_rest() - so SCL was pulled low BEFORE main.c sampled the bus
  *           for that very diagnostic. Measuring a state the firmware creates
  *           and then blaming the hardware for it.
  *         - MyI2C_BusRecover() could never report success: it ends with
  *           bus_rest() and then returns (MyI2C_IsIdle() != 0), which was
  *           therefore always a timeout.
  *         - bus_start() checks "is a line low while I am not busy" to decide
  *           whether to run a recovery. With SCL permanently low it ran three
  *           full recovery rounds - about 30 clock edges plus delays - before
  *           EVERY transaction, forever.
  *
  *       None of that is visible by reading the I2C-facing code, which is why it
  *       survived: every transaction still "worked", and the single line that
  *       would have revealed it was itself the casualty.
  *   I3. s_sclLevel always reflects the level the MASTER is driving on SCL.
  *       It is never inferred by reading the pin, because a slave stretching
  *       the clock makes the pin read low even while we drive high - and
  *       recovery logic must not be fooled by that.
  ******************************************************************************
  */

#include "MyI2C.h"
#include "main.h"
#include "Delay.h"

/*===========================================================================*/
/*  Pin access                                                               */
/*===========================================================================*/

/*
 * Both lines are open-drain outputs and never change mode. Writing 1 releases
 * the line, writing 0 pulls it down, and GPIO_ReadInputDataBit still reports the
 * real level - which is what makes clock stretching and ACK sampling work.
 */

#define SCL_HIGH()      GPIO_SetBits(MYI2C_SCL_PORT, MYI2C_SCL_PIN)
#define SCL_LOW()       GPIO_ResetBits(MYI2C_SCL_PORT, MYI2C_SCL_PIN)
#define SDA_HIGH()      GPIO_SetBits(MYI2C_SDA_PORT, MYI2C_SDA_PIN)
#define SDA_LOW()       GPIO_ResetBits(MYI2C_SDA_PORT, MYI2C_SDA_PIN)

#define SCL_PIN_READ()  GPIO_ReadInputDataBit(MYI2C_SCL_PORT, MYI2C_SCL_PIN)
#define SDA_PIN_READ()  GPIO_ReadInputDataBit(MYI2C_SDA_PORT, MYI2C_SDA_PIN)

#define QDELAY()        Delay_us(MYI2C_DELAY_US)

/* Guard interval for a slave holding SCL low (clock stretching) or SDA low
   (a stuck/sleeping slave). Generous, because a legitimate stretch on a slow
   part can reach tens of microseconds. */
#define STRETCH_GUARD_US    10U
#define STRETCH_TRIES       (MYI2C_DELAY_US * 20U)

/* I2C standard allows removing the bus by clocking 9 bits; 9 covers every
   possible phase of an interrupted byte. A couple of extra rounds are cheap
   insurance for a slave that needs more edges to resynchronise. */
#define RECOVER_CLOCKS      9U
#define RECOVER_ROUNDS      3U

/*===========================================================================*/
/*  Internal state                                                           */
/*===========================================================================*/

/** Level the MASTER drives on SCL (invariant I3). */
static uint8_t s_sclLevel = 1U;

/** True while this master holds the bus between START and STOP. */
static uint8_t s_busBusy = 0U;

/*===========================================================================*/
/*  Low-level helpers                                                        */
/*===========================================================================*/

/** Drive SCL, keeping the shadow level in step (invariant I3). */
static void scl_drive(uint8_t level)
{
    if (level != 0U)
    {
        SCL_HIGH();
        s_sclLevel = 1U;
    }
    else
    {
        SCL_LOW();
        s_sclLevel = 0U;
    }
}

/**
  * @brief  Release SCL and wait for it to actually reach high.
  * @return MYI2C_OK once SCL is high, MYI2C_ERR_TIMEOUT if a slave held it low.
  * @note   A slave may legitimately stretch the clock, so this waits. The
  *         shadow level is set even on the timeout path: we did drive it high,
  *         and a later recovery sequence needs to know that.
  */
static uint8_t scl_release(void)
{
    uint32_t guard = STRETCH_TRIES;

    SCL_HIGH();
    s_sclLevel = 1U;

    while (SCL_PIN_READ() == Bit_RESET)
    {
        if (guard-- == 0U)
        {
            /* Slave is holding the clock. Leave SCL as the master drives it
               (high/released) so the bus is not doubly driven. */
            return MYI2C_ERR_TIMEOUT;
        }
        Delay_us(STRETCH_GUARD_US);
    }

    return MYI2C_OK;
}

/** Return the bus to the I2C idle state: BOTH lines released high. */
static void bus_rest(void)
{
    SDA_HIGH();
    scl_drive(1U);
    s_busBusy = 0U;
}

/** Generate a START condition and hold the bus. */
static uint8_t bus_start(void)
{
    /* Recover automatically if a previous transaction left a slave mid-byte.
       This is what keeps a single glitch from bricking the bus until power
       cycle - important on a board that also carries a brushed motor. */
    if (s_busBusy == 0U && (SCL_PIN_READ() == Bit_RESET || SDA_PIN_READ() == Bit_RESET))
    {
        (void)MyI2C_BusRecover();
    }

    SDA_HIGH();
    if (scl_release() != MYI2C_OK)
    {
        bus_rest();
        return MYI2C_ERR_TIMEOUT;
    }
    QDELAY();

    SDA_LOW();              /* SDA falls while SCL is high: START */
    QDELAY();

    scl_drive(0U);
    QDELAY();

    s_busBusy = 1U;
    return MYI2C_OK;
}

/** Generate a STOP condition and release the bus. */
static void bus_stop(void)
{
    SDA_LOW();
    QDELAY();

    /* Best effort: if a slave stretches SCL here there is nothing useful left
       to do, and the timeout is ignored on purpose. */
    (void)scl_release();
    QDELAY();

    SDA_HIGH();             /* SDA rises while SCL is high: STOP */
    QDELAY();

    bus_rest();
}

/** Shift out one byte, then sample the ACK bit. */
static uint8_t bus_write_byte(uint8_t byte)
{
    uint8_t i;
    uint8_t ack;

    for (i = 0U; i < 8U; i++)
    {
        if ((byte & 0x80U) != 0U)
        {
            SDA_HIGH();
        }
        else
        {
            SDA_LOW();
        }
        QDELAY();

        if (scl_release() != MYI2C_OK)
        {
            return MYI2C_ERR_TIMEOUT;
        }
        QDELAY();

        scl_drive(0U);
        QDELAY();

        byte = (uint8_t)(byte << 1);
    }

    /* ACK phase: release SDA and let the slave drive it. */
    SDA_HIGH();
    QDELAY();

    if (scl_release() != MYI2C_OK)
    {
        return MYI2C_ERR_TIMEOUT;
    }
    QDELAY();

    ack = (SDA_PIN_READ() == Bit_RESET) ? MYI2C_OK : MYI2C_ERR_NACK;

    scl_drive(0U);
    QDELAY();

    return ack;
}

/** Shift in one byte, releasing SDA for the whole byte. */
static uint8_t bus_read_byte(uint8_t *out)
{
    uint8_t i;
    uint8_t byte = 0U;

    SDA_HIGH();

    for (i = 0U; i < 8U; i++)
    {
        byte = (uint8_t)(byte << 1);

        if (scl_release() != MYI2C_OK)
        {
            return MYI2C_ERR_TIMEOUT;
        }
        QDELAY();

        if (SDA_PIN_READ() != Bit_RESET)
        {
            byte |= 0x01U;
        }

        scl_drive(0U);
        QDELAY();
    }

    *out = byte;
    return MYI2C_OK;
}

/** Drive the master ACK bit for the byte just received (0 = ACK, 1 = NACK). */
static uint8_t bus_send_ack(uint8_t nack)
{
    if (nack != 0U)
    {
        SDA_HIGH();
    }
    else
    {
        SDA_LOW();
    }
    QDELAY();

    if (scl_release() != MYI2C_OK)
    {
        return MYI2C_ERR_TIMEOUT;
    }
    QDELAY();

    scl_drive(0U);
    QDELAY();

    return MYI2C_OK;
}

/*===========================================================================*/
/*  Lifecycle                                                                */
/*===========================================================================*/

void MyI2C_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;

    RCC_APB2PeriphClockCmd(MYI2C_GPIO_RCC, ENABLE);

    /* Pre-load the output register high so enabling the outputs releases the
       bus instead of briefly pulling it low. */
    SCL_HIGH();
    SDA_HIGH();
    s_sclLevel = 1U;
    s_busBusy  = 0U;

    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_Out_OD;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_Pin   = MYI2C_SCL_PIN | MYI2C_SDA_PIN;
    GPIO_Init(MYI2C_SCL_PORT, &GPIO_InitStructure);

    /* A slave reset mid-transaction may already be holding a line low from
       before the MCU started. Clear that before anything else touches the bus. */
    (void)MyI2C_BusRecover();
}

uint8_t MyI2C_IsIdle(void)
{
    return ((SCL_PIN_READ() != Bit_RESET) && (SDA_PIN_READ() != Bit_RESET)) ? 1U : 0U;
}

uint8_t MyI2C_BusRecover(void)
{
    uint8_t round;

    /* The bus is ours now regardless of what happened before. */
    s_busBusy = 0U;

    /* If the master was left having driven SDA low, release it first - we
       cannot generate edges while fighting the slave. */
    SDA_HIGH();
    QDELAY();

    for (round = 0U; round < RECOVER_ROUNDS; round++)
    {
        uint8_t i;

        if (MyI2C_IsIdle() != 0U)
        {
            bus_rest();
            return MYI2C_OK;
        }

        /* Clock the slave through whatever bit it was stuck on. scl_drive() is
           used rather than scl_release() because the point here is to emit
           edges even if the slave is holding the line - a recovery sequence
           that waits for the slave is not a recovery sequence. */
        for (i = 0U; i < RECOVER_CLOCKS; i++)
        {
            scl_drive(0U);
            QDELAY();
            scl_drive(1U);
            QDELAY();
        }

        /* STOP: SDA low -> high while SCL is high. */
        SDA_LOW();
        QDELAY();
        scl_drive(1U);
        QDELAY();
        SDA_HIGH();
        QDELAY();
        scl_drive(0U);
        QDELAY();
    }

    bus_rest();

    return (MyI2C_IsIdle() != 0U) ? MYI2C_OK : MYI2C_ERR_TIMEOUT;
}

void MyI2C_ForceStop(void)
{
    bus_stop();
}

/*===========================================================================*/
/*  Primitive transactions                                                   */
/*===========================================================================*/

uint8_t MyI2C_Probe(uint8_t devAddr)
{
    uint8_t rc;

    rc = bus_start();
    if (rc != MYI2C_OK)
    {
        return rc;
    }

    rc = bus_write_byte(devAddr);
    bus_stop();

    return rc;
}

uint8_t MyI2C_Write(uint8_t devAddr, const uint8_t *buf, uint16_t len)
{
    uint16_t i;
    uint8_t  rc;

    if ((buf == 0) || (len == 0U))
    {
        return MYI2C_ERR_PARAM;
    }

    rc = bus_start();
    if (rc != MYI2C_OK)
    {
        return rc;
    }

    rc = bus_write_byte(devAddr);
    if (rc != MYI2C_OK)
    {
        bus_stop();
        return rc;
    }

    for (i = 0U; i < len; i++)
    {
        rc = bus_write_byte(buf[i]);
        if (rc != MYI2C_OK)
        {
            bus_stop();
            return rc;
        }
    }

    bus_stop();
    return MYI2C_OK;
}

uint8_t MyI2C_Read(uint8_t devAddr, uint8_t *buf, uint16_t len)
{
    uint16_t i;
    uint8_t  rc;

    if ((buf == 0) || (len == 0U))
    {
        return MYI2C_ERR_PARAM;
    }

    rc = bus_start();
    if (rc != MYI2C_OK)
    {
        return rc;
    }

    rc = bus_write_byte((uint8_t)(devAddr | 0x01U));   /* read direction */
    if (rc != MYI2C_OK)
    {
        bus_stop();
        return rc;
    }

    for (i = 0U; i < len; i++)
    {
        rc = bus_read_byte(&buf[i]);
        if (rc != MYI2C_OK)
        {
            bus_stop();
            return rc;
        }

        /* ACK everything except the final byte, which is NACKed to end the
           read (the slave releases SDA after a NACK). */
        rc = bus_send_ack((i == (uint16_t)(len - 1U)) ? 1U : 0U);
        if (rc != MYI2C_OK)
        {
            bus_stop();
            return rc;
        }
    }

    bus_stop();
    return MYI2C_OK;
}

uint8_t MyI2C_WriteRead(uint8_t devAddr, const uint8_t *wbuf, uint16_t wlen,
                        uint8_t *rbuf, uint16_t rlen)
{
    uint16_t i;
    uint8_t  rc;

    if ((wbuf == 0) || (wlen == 0U) || (rbuf == 0) || (rlen == 0U))
    {
        return MYI2C_ERR_PARAM;
    }

    rc = bus_start();
    if (rc != MYI2C_OK)
    {
        return rc;
    }

    rc = bus_write_byte(devAddr);
    if (rc != MYI2C_OK)
    {
        bus_stop();
        return rc;
    }

    for (i = 0U; i < wlen; i++)
    {
        rc = bus_write_byte(wbuf[i]);
        if (rc != MYI2C_OK)
        {
            bus_stop();
            return rc;
        }
    }

    /* Repeated START: keeps the bus and, for pointer-addressed devices like the
       AT24C32, keeps the internal address pointer we just set. */
    SDA_HIGH();
    QDELAY();
    if (scl_release() != MYI2C_OK)
    {
        bus_stop();
        return MYI2C_ERR_TIMEOUT;
    }
    QDELAY();
    SDA_LOW();
    QDELAY();
    scl_drive(0U);
    QDELAY();

    rc = bus_write_byte((uint8_t)(devAddr | 0x01U));
    if (rc != MYI2C_OK)
    {
        bus_stop();
        return rc;
    }

    for (i = 0U; i < rlen; i++)
    {
        rc = bus_read_byte(&rbuf[i]);
        if (rc != MYI2C_OK)
        {
            bus_stop();
            return rc;
        }

        rc = bus_send_ack((i == (uint16_t)(rlen - 1U)) ? 1U : 0U);
        if (rc != MYI2C_OK)
        {
            bus_stop();
            return rc;
        }
    }

    bus_stop();
    return MYI2C_OK;
}

uint8_t MyI2C_WriteWithPoll(uint8_t devAddr, const uint8_t *buf, uint16_t len,
                            uint32_t timeoutMs)
{
    uint32_t waitedUs = 0U;
    uint32_t limitUs  = timeoutMs * 1000U;
    uint8_t  rc;

    rc = MyI2C_Write(devAddr, buf, len);
    if (rc != MYI2C_OK)
    {
        return rc;
    }

    /* While the device performs its internal write it does not acknowledge.
       Poll until it answers, then the caller may proceed immediately instead
       of sleeping a worst-case delay. */
    for (;;)
    {
        rc = MyI2C_Probe(devAddr);
        if (rc == MYI2C_OK)
        {
            return MYI2C_OK;
        }
        if (rc == MYI2C_ERR_TIMEOUT)
        {
            /* Genuine bus problem, not merely a busy device. */
            return rc;
        }

        if (waitedUs >= limitUs)
        {
            return MYI2C_ERR_TIMEOUT;
        }

        Delay_us(200U);
        waitedUs += 200U;
    }
}
