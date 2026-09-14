/**
  ******************************************************************************
  * @file    Key.c
  * @brief   Four panel keys with press / long-press discrimination.
  *
  * PRESS CLASSIFICATION
  * --------------------
  * The key state machine is kept explicit (down / hold / consumed) rather than
  * inferred from the debouncer's event flag, because reusing that flag for two
  * different meanings is how "the emergency key also closed the door" bugs are
  * born.
  *
  *   released -> down      : start the hold timer
  *   down, timer expires   : report a LONG press once, set `consumed`
  *   down -> released      : report a SHORT press only if not `consumed`
  *
  * The `consumed` flag is what guarantees a long press never also produces a
  * short press. Note that the long press is reported while the key is still
  * held - that is intentional for KEY4, where "hold to stop" should take effect
  * immediately rather than on release.
  ******************************************************************************
  */

#include "Key.h"
#include "Debounce.h"
#include "Exti.h"
#include "main.h"

typedef struct
{
    Debounce_t debounce;
    uint8_t    down;          /* current debounced press state           */
    uint16_t   holdMs;        /* time held so far                        */
    uint8_t    consumed;      /* the press already produced a long event */
    uint8_t    shortEvent;    /* one-shot: short press pending           */
    uint8_t    longEvent;     /* one-shot: long press pending            */
} KeyState_t;

static KeyState_t s_keys[KEY_ID_COUNT];

/* Pin table, indexed by KeyId_t, so the enum and the wiring cannot drift apart. */
static GPIO_TypeDef *const s_port[KEY_ID_COUNT] =
{
    KEY1_PORT, KEY2_PORT, KEY3_PORT, KEY4_PORT
};

static const uint16_t s_pin[KEY_ID_COUNT] =
{
    KEY1_PIN, KEY2_PIN, KEY3_PIN, KEY4_PIN
};

/*===========================================================================*/
/*  Initialisation                                                           */
/*===========================================================================*/

void Key_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;
    uint8_t i;

    RCC_APB2PeriphClockCmd(KEY_RCC, ENABLE);

    /* Active low into the internal pull-up: a press pulls the pin to ground. */
    GPIO_InitStructure.GPIO_Pin  = KEY1_PIN | KEY2_PIN | KEY3_PIN | KEY4_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(KEY1_PORT, &GPIO_InitStructure);

    for (i = 0U; i < (uint8_t)KEY_ID_COUNT; i++)
    {
        KeyState_t *k = &s_keys[i];

        Debounce_Init(&k->debounce, s_port[i], s_pin[i], 0U, KEY_DEBOUNCE_MS);
        k->down       = 0U;
        k->holdMs     = 0U;
        k->consumed   = 0U;
        k->shortEvent = 0U;
        k->longEvent  = 0U;

        /* Preemption priority 3: lowest of the three input groups. A human
           pressing a button is the least time-critical event in the system. */
        (void)Exti_ConfigPin(s_port[i], s_pin[i], EXTI_Trigger_Rising_Falling, 3U, 0U);
    }
}

void Key_Reset(void)
{
    uint8_t i;

    for (i = 0U; i < (uint8_t)KEY_ID_COUNT; i++)
    {
        KeyState_t *k = &s_keys[i];

        Debounce_SyncNow(&k->debounce);
        k->down       = 0U;
        k->holdMs     = 0U;
        k->consumed   = 0U;
        k->shortEvent = 0U;
        k->longEvent  = 0U;
    }
}

void Key_IrqHandler(KeyId_t id)
{
    if ((uint8_t)id < (uint8_t)KEY_ID_COUNT)
    {
        Debounce_IrqEdge(&s_keys[id].debounce);
    }
}

void Key_Tick1ms(void)
{
    uint8_t i;

    for (i = 0U; i < (uint8_t)KEY_ID_COUNT; i++)
    {
        KeyState_t *k = &s_keys[i];

        Debounce_Tick1ms(&k->debounce);

        if (Debounce_IsActive(&k->debounce) != 0U)
        {
            if (k->down == 0U)
            {
                /* Falling edge accepted: begin a new press. */
                k->down     = 1U;
                k->holdMs   = 0U;
                k->consumed = 0U;
            }
            else if ((k->consumed == 0U) && (k->holdMs < KEY_LONGPRESS_MS))
            {
                k->holdMs++;

                if (k->holdMs >= KEY_LONGPRESS_MS)
                {
                    /* Report while still held, and mark the press consumed so
                       the eventual release does not also fire a short press. */
                    k->consumed  = 1U;
                    k->longEvent = 1U;
                }
            }
        }
        else if (k->down != 0U)
        {
            /* Rising edge accepted: the key was released. */
            k->down = 0U;

            if (k->consumed == 0U)
            {
                k->shortEvent = 1U;
            }

            k->holdMs   = 0U;
            k->consumed = 0U;
        }
    }
}

/*===========================================================================*/
/*  Queries                                                                  */
/*===========================================================================*/

uint8_t Key_TakeShortPress(KeyId_t id)
{
    uint8_t e;

    if ((uint8_t)id >= (uint8_t)KEY_ID_COUNT)
    {
        return 0U;
    }

    e = s_keys[id].shortEvent;
    s_keys[id].shortEvent = 0U;

    return e;
}

uint8_t Key_TakeLongPress(KeyId_t id)
{
    uint8_t e;

    if ((uint8_t)id >= (uint8_t)KEY_ID_COUNT)
    {
        return 0U;
    }

    e = s_keys[id].longEvent;
    s_keys[id].longEvent = 0U;

    return e;
}

uint8_t Key_IsDown(KeyId_t id)
{
    if ((uint8_t)id >= (uint8_t)KEY_ID_COUNT)
    {
        return 0U;
    }

    return s_keys[id].down;
}
