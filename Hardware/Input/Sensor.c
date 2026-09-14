/**
  ******************************************************************************
  * @file    Sensor.c
  * @brief   Presence detection at the door (currently button-simulated).
  ******************************************************************************
  */

#include "Sensor.h"
#include "Debounce.h"
#include "Exti.h"
#include "main.h"

static Debounce_t s_outside;
static Debounce_t s_inside;

void Sensor_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;

    RCC_APB2PeriphClockCmd(SENSOR_RCC, ENABLE);

    /* Active low into a pull-up: pressing the button (or a future optical
       sensor pulling its open-collector output down) asserts the input. */
    GPIO_InitStructure.GPIO_Pin  = SENSOR_OUT_PIN | SENSOR_IN_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(SENSOR_OUT_PORT, &GPIO_InitStructure);

    Debounce_Init(&s_outside, SENSOR_OUT_PORT, SENSOR_OUT_PIN, 0U, SENSOR_DEBOUNCE_MS);
    Debounce_Init(&s_inside,  SENSOR_IN_PORT,  SENSOR_IN_PIN,  0U, SENSOR_DEBOUNCE_MS);

    /*
     * Preemption priority 2: below the limits (0), above the keys (3). A person
     * arriving must be noticed promptly but must never delay a limit event.
     *
     * These are EXTI12/EXTI13, NOT EXTI0/EXTI1. They used to be PB0/PB1, which
     * silently stole the limit switches' lines - see the pin-choice note in
     * Core/main.h. Both sensors share the EXTI15_10 vector, which is why they use
     * the same priority: a vector has one priority, and passing different values
     * would just mean the last call wins.
     *
     * The return value is discarded deliberately: a bad pin mask or a line
     * conflict is a static configuration error, and Exti_ConfigPin() counts
     * conflicts for main() to report once the console exists.
     */
    (void)Exti_ConfigPin(SENSOR_OUT_PORT, SENSOR_OUT_PIN, EXTI_Trigger_Falling, 2U, 0U);
    (void)Exti_ConfigPin(SENSOR_IN_PORT,  SENSOR_IN_PIN,  EXTI_Trigger_Falling, 2U, 0U);
}

void Sensor_Reset(void)
{
    Debounce_SyncNow(&s_outside);
    Debounce_SyncNow(&s_inside);
}

void Sensor_IrqHandler(uint8_t outsideEdge)
{
    if (outsideEdge != 0U)
    {
        Debounce_IrqEdge(&s_outside);
    }
    else
    {
        Debounce_IrqEdge(&s_inside);
    }
}

void Sensor_Tick1ms(void)
{
    Debounce_Tick1ms(&s_outside);
    Debounce_Tick1ms(&s_inside);
}

uint8_t Sensor_TakeOutsideEvent(void)
{
    return Debounce_TakeEvent(&s_outside);
}

uint8_t Sensor_TakeInsideEvent(void)
{
    return Debounce_TakeEvent(&s_inside);
}

uint8_t Sensor_IsOutsideActive(void)
{
    return Debounce_IsActive(&s_outside);
}

uint8_t Sensor_IsInsideActive(void)
{
    return Debounce_IsActive(&s_inside);
}
