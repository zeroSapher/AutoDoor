/**
  ******************************************************************************
  * @file    Limit.c
  * @brief   Door limit switches. See Limit.h for the safety rationale.
  ******************************************************************************
  */

#include "Limit.h"
#include "Debounce.h"
#include "Exti.h"
#include "main.h"

/*===========================================================================*/
/*  State                                                                    */
/*===========================================================================*/

static Debounce_t s_open;
static Debounce_t s_close;

/*===========================================================================*/
/*  Initialisation                                                           */
/*===========================================================================*/

void Limit_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;

    RCC_APB2PeriphClockCmd(LIMIT_RCC, ENABLE);

    /* NC contact: closed (door away from the limit) pulls the pin to ground, so
       with the internal pull-up enabled the pin reads low while idle and goes
       high when the switch trips. A broken wire also reads high - fail-safe. */
    GPIO_InitStructure.GPIO_Pin  = LIMIT_OPEN_PIN | LIMIT_CLOSE_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(LIMIT_OPEN_PORT, &GPIO_InitStructure);

    Debounce_Init(&s_open,  LIMIT_OPEN_PORT,  LIMIT_OPEN_PIN,  1U, LIMIT_DEBOUNCE_MS);
    Debounce_Init(&s_close, LIMIT_CLOSE_PORT, LIMIT_CLOSE_PIN, 1U, LIMIT_DEBOUNCE_MS);

    /* Preemption priority 0: above the sensors (2) and the keys (3), because a
       limit event must never wait behind a "someone approached" event. */
    (void)Exti_ConfigPin(LIMIT_OPEN_PORT,  LIMIT_OPEN_PIN,  EXTI_Trigger_Rising_Falling, 0U, 0U);
    (void)Exti_ConfigPin(LIMIT_CLOSE_PORT, LIMIT_CLOSE_PIN, EXTI_Trigger_Rising_Falling, 0U, 0U);
}

void Limit_Reset(void)
{
    Debounce_SyncNow(&s_open);
    Debounce_SyncNow(&s_close);
}

void Limit_IrqHandler(uint8_t openEdge)
{
    if (openEdge != 0U)
    {
        Debounce_IrqEdge(&s_open);
    }
    else
    {
        Debounce_IrqEdge(&s_close);
    }
}

void Limit_Tick1ms(void)
{
    Debounce_Tick1ms(&s_open);
    Debounce_Tick1ms(&s_close);
}

/*===========================================================================*/
/*  Queries                                                                  */
/*===========================================================================*/

uint8_t Limit_TakeOpenEvent(void)
{
    return Debounce_TakeEvent(&s_open);
}

uint8_t Limit_TakeCloseEvent(void)
{
    return Debounce_TakeEvent(&s_close);
}

uint8_t Limit_IsOpen(void)
{
    return Debounce_IsActive(&s_open);
}

uint8_t Limit_IsClosed(void)
{
    return Debounce_IsActive(&s_close);
}

uint8_t Limit_IsFaulted(void)
{
    /* Both limits asserted at once cannot happen on a real door: they are at
       opposite ends of travel. It means a shorted wire or a dead switch, and the
       state machine must refuse to drive until it is cleared. */
    return ((Debounce_IsActive(&s_open) != 0U) &&
            (Debounce_IsActive(&s_close) != 0U)) ? 1U : 0U;
}
