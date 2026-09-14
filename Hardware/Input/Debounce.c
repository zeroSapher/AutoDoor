/**
  ******************************************************************************
  * @file    Debounce.c
  * @brief   Shared edge debouncer for mechanical inputs. See Debounce.h.
  ******************************************************************************
  */

#include "Debounce.h"

/*===========================================================================*/
/*  Helpers                                                                  */
/*===========================================================================*/

/** Read the raw pin and reduce it to "asserted / not asserted". */
static uint8_t read_asserted(const Debounce_t *d)
{
    uint8_t level = (GPIO_ReadInputDataBit(d->port, d->pin) != Bit_RESET) ? 1U : 0U;

    return (level == d->activeLevel) ? 1U : 0U;
}

/*===========================================================================*/
/*  Public API                                                               */
/*===========================================================================*/

void Debounce_Init(Debounce_t *d, GPIO_TypeDef *port, uint16_t pin,
                   uint8_t activeLevel, uint8_t debounceMs)
{
    if (d == 0)
    {
        return;
    }

    d->port        = port;
    d->pin         = pin;
    d->activeLevel = (activeLevel != 0U) ? 1U : 0U;
    d->debounceMs  = debounceMs;

    d->pending  = 0U;
    d->timerMs  = 0U;
    d->event    = 0U;
    d->raw      = read_asserted(d);
    d->stable   = d->raw;
}

void Debounce_SyncNow(Debounce_t *d)
{
    if (d == 0)
    {
        return;
    }

    d->pending = 0U;
    d->timerMs = 0U;
    d->event   = 0U;
    d->raw     = read_asserted(d);
    d->stable  = d->raw;
}

void Debounce_IrqEdge(Debounce_t *d)
{
    if (d == 0)
    {
        return;
    }

    d->pending = 1U;

    /* Restart the window on every bounce. The interrupt only fires on a
       transition, so a chattering contact keeps pushing the deadline out until
       the line finally settles - exactly the behaviour we want, and it costs
       nothing but a store. */
    d->timerMs = d->debounceMs;
}

void Debounce_Tick1ms(Debounce_t *d)
{
    uint8_t now;

    if (d == 0)
    {
        return;
    }

    /*
     * SAMPLE THE PIN EVERY MILLISECOND
     *
     * The interrupt is only an accelerator. If this function relied on
     * `pending`, a single missed edge would freeze `stable` forever - the input
     * would appear stuck at its old level and no amount of real-world wiggling
     * would recover it. That is unacceptable for a limit switch, and it is also
     * what makes the code work in environments where edge interrupts are
     * unreliable (Proteus among them).
     *
     * So the tick compares the pin against the last raw reading and starts the
     * settle window itself. The ISR's contribution is to start that window
     * sooner, which matters for latency but not for correctness.
     */
    now = read_asserted(d);

    if (now != d->raw)
    {
        /* Level is moving: (re)start the settle window. A bouncing contact keeps
           pushing the deadline out until the line stops changing. */
        d->raw     = now;
        d->pending = 1U;
        d->timerMs = d->debounceMs;
        return;
    }

    /* Nothing changed and nothing pending: nothing to do. */
    if (d->pending == 0U)
    {
        return;
    }

    if (d->timerMs != 0U)
    {
        d->timerMs--;
        return;
    }

    /* The level has been steady for the whole window, so accept it. */
    d->pending = 0U;

    if (d->raw != d->stable)
    {
        d->stable = d->raw;

        if (d->stable != 0U)
        {
            d->event = 1U;      /* asserted transition - this is the "press" */
        }
    }
}

uint8_t Debounce_IsActive(const Debounce_t *d)
{
    return (d != 0) ? d->stable : 0U;
}

uint8_t Debounce_TakeEvent(Debounce_t *d)
{
    uint8_t e;

    if (d == 0)
    {
        return 0U;
    }

    e = d->event;
    d->event = 0U;

    return e;
}

uint8_t Debounce_IsPending(const Debounce_t *d)
{
    return (d != 0) ? d->pending : 0U;
}
