/**
  ******************************************************************************
  * @file    StatusLed.c
  * @brief   Single status LED showing door state at a glance.
  *
  * IMPLEMENTATION
  * --------------
  * One table maps each state to an (on, off) period pair in milliseconds, and a
  * single phase counter walks through it. A period of 0 means "steady": either
  * always on or always off. Expressing every pattern as a period pair keeps the
  * tick function trivial and makes the table the only thing to review when
  * changing behaviour.
  ******************************************************************************
  */

#include "StatusLed.h"
#include "main.h"

/** Blink description: on-time and off-time in milliseconds (0 = steady). */
typedef struct
{
    uint16_t onMs;
    uint16_t offMs;
    uint8_t  doubleFlash;   /* 1 to emit two quick pulses per cycle */
} LedPattern_t;

/*
 * Indexed by DoorState_t, so the enum and this table must be edited together.
 * Order: INIT, IDLE, OPENING, OPEN, CLOSING, REVERSING, STOPPED.
 */
static const LedPattern_t s_patterns[] =
{
    { 60U,   60U,  1U },    /* INIT      : quick double flash while booting   */
    { 0U,    0U,   0U },    /* IDLE      : off - nothing to report            */
    { 0xFFFF, 0U,  0U },    /* OPENING   : steady on - in motion              */
    { 500U,  500U, 0U },    /* OPEN      : slow blink - open, counting down   */
    { 125U,  125U, 0U },    /* CLOSING   : fast blink - closing, keep clear   */
    { 125U,  125U, 0U },    /* REVERSING : fast blink - reversing             */
    { 80U,   80U,  1U }     /* STOPPED   : double flash - fault / e-stop      */
};

#define PATTERN_COUNT   ((uint8_t)(sizeof(s_patterns) / sizeof(s_patterns[0])))

/* Time the LED stays lit during one double-flash pulse. */
#define DOUBLE_PULSE_MS     70U
#define DOUBLE_GAP_MS       70U

/*===========================================================================*/
/*  State                                                                    */
/*===========================================================================*/

static const LedPattern_t *s_pat    = 0;
static uint8_t  s_phase  = 0U;      /* 0 = on, 1 = off, 2/3 = double-flash stages */
static uint16_t s_remainMs = 0U;
static uint8_t  s_faulted  = 0U;
static uint8_t  s_initialised = 0U;

/*===========================================================================*/
/*  Pin control                                                              */
/*===========================================================================*/

static void drive(uint8_t on)
{
    if (on != 0U)
    {
        GPIO_SetBits(STATUS_LED_PORT, STATUS_LED_PIN);
    }
    else
    {
        GPIO_ResetBits(STATUS_LED_PORT, STATUS_LED_PIN);
    }
}

/*===========================================================================*/
/*  Pattern handling                                                         */
/*===========================================================================*/

/** Load the first stage of a pattern and drive the pin to match. */
static void start_pattern(const LedPattern_t *pat)
{
    s_pat = pat;

    if (pat == 0)
    {
        s_phase    = 1U;
        s_remainMs = 0U;
        drive(0U);
        return;
    }

    /* onMs == 0 means "always off"; 0xFFFF means "always on". */
    if (pat->onMs == 0U)
    {
        s_phase    = 1U;            /* steady off: phase 1 never expires */
        s_remainMs = 0U;
        drive(0U);
        return;
    }

    s_phase    = 0U;
    s_remainMs = (pat->doubleFlash != 0U) ? DOUBLE_PULSE_MS : pat->onMs;
    drive(1U);
}

/*===========================================================================*/
/*  Public API                                                               */
/*===========================================================================*/

void StatusLed_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;

    RCC_APB2PeriphClockCmd(STATUS_LED_RCC, ENABLE);

    /* Off before the pin becomes an output, so power-up cannot flash. */
    drive(0U);

    GPIO_InitStructure.GPIO_Pin   = STATUS_LED_PIN;
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_2MHz;
    GPIO_Init(STATUS_LED_PORT, &GPIO_InitStructure);

    drive(0U);

    s_phase       = 1U;
    s_remainMs    = 0U;
    s_faulted     = 0U;
    s_pat         = 0;
    s_initialised = 1U;
}

void StatusLed_Set(DoorState_t state, uint8_t faulted)
{
    const LedPattern_t *want;
    uint8_t idx = (uint8_t)state;
    uint8_t changed;

    if (s_initialised == 0U)
    {
        return;
    }

    if (idx >= PATTERN_COUNT)
    {
        idx = (uint8_t)DOOR_STATE_STOPPED;
    }

    want = &s_patterns[idx];

    /*
     * A fault makes STOPPED blink faster than a plain stop, so "it is waiting
     * for you to press start" is distinguishable from "something is wrong".
     * Only STOPPED is affected; the other states already say what they mean.
     */
    changed = ((want != s_pat) || (faulted != s_faulted)) ? 1U : 0U;

    s_faulted = faulted;

    if (changed == 0U)
    {
        return;     /* nothing to do; the tick keeps the current pattern going */
    }

    start_pattern(want);

    if ((faulted != 0U) && (idx == (uint8_t)DOOR_STATE_STOPPED))
    {
        /* Faster double flash for a latched fault: 50 ms pulses, 50 ms gaps. */
        s_phase    = 0U;
        s_remainMs = 50U;
        drive(1U);
    }
}

void StatusLed_Tick1ms(void)
{
    if ((s_initialised == 0U) || (s_pat == 0))
    {
        return;
    }

    /* Steady patterns: the phase never advances. */
    if ((s_pat->onMs == 0U) || (s_pat->offMs == 0U && s_pat->doubleFlash == 0U))
    {
        if (s_pat->onMs != 0U && s_pat->offMs == 0U)
        {
            drive(1U);      /* steady on */
        }
        return;
    }

    if (s_remainMs != 0U)
    {
        s_remainMs--;
        return;
    }

    if (s_pat->doubleFlash == 0U)
    {
        /* Plain blink: two phases, on and off. */
        s_phase = (uint8_t)(s_phase ^ 1U);

        if (s_phase == 0U)
        {
            s_remainMs = s_pat->onMs;
            drive(1U);
        }
        else
        {
            s_remainMs = s_pat->offMs;
            drive(0U);
        }
        return;
    }

    /* Double flash: on, gap, on, longer gap. */
    s_phase = (uint8_t)((s_phase + 1U) % 4U);

    switch (s_phase)
    {
        case 0U:
            s_remainMs = DOUBLE_PULSE_MS;
            drive(1U);
            break;
        case 1U:
            s_remainMs = DOUBLE_GAP_MS;
            drive(0U);
            break;
        case 2U:
            s_remainMs = DOUBLE_PULSE_MS;
            drive(1U);
            break;
        default:
            /* Long pause so the pair reads as a distinct "double" rather than
               a slightly irregular fast blink. */
            s_remainMs = (uint16_t)(s_pat->offMs * 4U);
            drive(0U);
            break;
    }
}
