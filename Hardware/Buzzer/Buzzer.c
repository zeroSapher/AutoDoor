/**
  ******************************************************************************
  * @file    Buzzer.c
  * @brief   Active buzzer patterns, driven from the 1 ms tick.
  *
  * A pattern is a short list of (level, duration) steps walked once. Encoding it
  * as data rather than as a chain of delays matters: the door state machine must
  * keep running while the buzzer sounds, so nothing here may block.
  *
  * An unfinished pattern is abandoned when a new one starts, so the most recent
  * event always owns the buzzer. That is the right priority for a door: the user
  * needs to hear what just happened, not what happened three seconds ago.
  ******************************************************************************
  */

#include "Buzzer.h"
#include "main.h"

/** One step of a pattern. */
typedef struct
{
    uint8_t  on;        /* 1 = sound, 0 = silence */
    uint16_t ms;
} BuzzStep_t;

/* A finished pattern always leaves the pin silent (see Buzzer_Tick1ms). */
static const BuzzStep_t P_OPEN[]   = { {1, 80},  {0, 80},  {1, 80} };
static const BuzzStep_t P_CLOSED[] = { {1, 60} };
static const BuzzStep_t P_CLOSE[]  = { {1, 400} };
static const BuzzStep_t P_FAULT[]  = { {1, 60},  {0, 60},  {1, 60}, {0, 60}, {1, 60} };
static const BuzzStep_t P_ESTOP[]  = { {1, 1200} };
static const BuzzStep_t P_READY[]  = { {1, 40},  {0, 60},  {1, 40} };

typedef struct
{
    const BuzzStep_t *steps;
    uint8_t           count;
} PatternRef_t;

/* Indexed by BuzzerPattern_t: BUZZ_NONE must stay first, and the enum and this
   table must be edited together. */
static const PatternRef_t s_patterns[] =
{
    { 0, 0 },                                                        /* NONE   */
    { P_OPEN,   (uint8_t)(sizeof(P_OPEN)   / sizeof(BuzzStep_t)) },   /* OPEN   */
    { P_CLOSE,  (uint8_t)(sizeof(P_CLOSE)  / sizeof(BuzzStep_t)) },   /* CLOSE  */
    { P_FAULT,  (uint8_t)(sizeof(P_FAULT)  / sizeof(BuzzStep_t)) },   /* FAULT  */
    { P_ESTOP,  (uint8_t)(sizeof(P_ESTOP)  / sizeof(BuzzStep_t)) },   /* ESTOP  */
    { P_CLOSED, (uint8_t)(sizeof(P_CLOSED) / sizeof(BuzzStep_t)) },   /* CLOSED */
    { P_READY,  (uint8_t)(sizeof(P_READY)  / sizeof(BuzzStep_t)) }    /* READY  */
};

#define PATTERN_COUNT   ((uint8_t)(sizeof(s_patterns) / sizeof(s_patterns[0])))

/*===========================================================================*/
/*  State                                                                    */
/*===========================================================================*/

static const BuzzStep_t *s_steps    = 0;
static uint8_t           s_count    = 0U;
static uint8_t           s_index    = 0U;
static uint16_t          s_remainMs = 0U;

/*===========================================================================*/
/*  Pin control                                                              */
/*===========================================================================*/

static void drive(uint8_t on)
{
#if BUZZER_ACTIVE_HIGH
    if (on != 0U)
    {
        GPIO_SetBits(BUZZER_PORT, BUZZER_PIN);
    }
    else
    {
        GPIO_ResetBits(BUZZER_PORT, BUZZER_PIN);
    }
#else
    if (on != 0U)
    {
        GPIO_ResetBits(BUZZER_PORT, BUZZER_PIN);
    }
    else
    {
        GPIO_SetBits(BUZZER_PORT, BUZZER_PIN);
    }
#endif
}

/*===========================================================================*/
/*  Public API                                                               */
/*===========================================================================*/

void Buzzer_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;

    RCC_APB2PeriphClockCmd(BUZZER_RCC, ENABLE);

    /* Silent before the pin becomes an output, so power-up cannot chirp. */
    drive(0U);

    GPIO_InitStructure.GPIO_Pin   = BUZZER_PIN;
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_2MHz;
    GPIO_Init(BUZZER_PORT, &GPIO_InitStructure);

    drive(0U);

    s_steps    = 0;
    s_count    = 0U;
    s_index    = 0U;
    s_remainMs = 0U;
}

void Buzzer_Set(uint8_t on)
{
    /* Direct control cancels any running pattern. */
    s_steps    = 0;
    s_count    = 0U;
    s_index    = 0U;
    s_remainMs = 0U;

    drive((on != 0U) ? 1U : 0U);
}

void Buzzer_Play(BuzzerPattern_t pattern)
{
    uint8_t idx = (uint8_t)pattern;

    if (idx >= PATTERN_COUNT)
    {
        return;
    }

    s_steps = s_patterns[idx].steps;
    s_count = s_patterns[idx].count;

    if ((s_steps == 0) || (s_count == 0U))
    {
        Buzzer_Stop();
        return;
    }

    s_index    = 0U;
    s_remainMs = s_steps[0].ms;

    drive(s_steps[0].on);
}

void Buzzer_Stop(void)
{
    s_steps    = 0;
    s_count    = 0U;
    s_index    = 0U;
    s_remainMs = 0U;

    drive(0U);
}

uint8_t Buzzer_IsBusy(void)
{
    return (s_count != 0U) ? 1U : 0U;
}

void Buzzer_Tick1ms(void)
{
    if (s_count == 0U)
    {
        return;
    }

    if (s_remainMs != 0U)
    {
        s_remainMs--;
        return;
    }

    /* Current step elapsed: advance, or finish silent. */
    s_index++;

    if (s_index >= s_count)
    {
        Buzzer_Stop();
        return;
    }

    s_remainMs = s_steps[s_index].ms;
    drive(s_steps[s_index].on);
}
