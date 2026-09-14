/**
  ******************************************************************************
  * @file    Exti.c
  * @brief   Shared EXTI line configuration.
  ******************************************************************************
  */

#include "Exti.h"

/* Map a GPIO port pointer to the 4-bit port source code used by AFIO_EXTICR. */
static uint8_t port_source(GPIO_TypeDef *port)
{
    if (port == GPIOA) { return 0U; }
    if (port == GPIOB) { return 1U; }
    if (port == GPIOC) { return 2U; }
    if (port == GPIOD) { return 3U; }
    if (port == GPIOE) { return 4U; }
    return 0xFFU;
}

/*
 * EXTI lines 0-4 each have a dedicated interrupt vector; lines 5-9 share one and
 * lines 10-15 share another. This is a hardware property, not a choice.
 */
static uint8_t irqn_from_line(uint8_t line)
{
    switch (line)
    {
        case 0U:  return (uint8_t)EXTI0_IRQn;
        case 1U:  return (uint8_t)EXTI1_IRQn;
        case 2U:  return (uint8_t)EXTI2_IRQn;
        case 3U:  return (uint8_t)EXTI3_IRQn;
        case 4U:  return (uint8_t)EXTI4_IRQn;
        case 5U:
        case 6U:
        case 7U:
        case 8U:
        case 9U:  return (uint8_t)EXTI9_5_IRQn;
        default:  return (uint8_t)EXTI15_10_IRQn;
    }
}

uint8_t Exti_ConfigPin(GPIO_TypeDef *port, uint16_t pin, EXTITrigger_TypeDef trigger,
                       uint8_t preemptPrio, uint8_t subPrio)
{
    uint8_t  line  = 0xFFU;
    uint8_t  src   = port_source(port);
    uint8_t  bit;
    uint8_t  reg;
    uint8_t  shift;
    uint32_t value;
    EXTI_InitTypeDef EXTI_InitStructure;
    NVIC_InitTypeDef NVIC_InitStructure;

    if (src == 0xFFU)
    {
        return 1U;
    }

    /* Recover the line number from the pin mask; exactly one bit must be set. */
    for (bit = 0U; bit < 16U; bit++)
    {
        if ((pin & (uint16_t)(1U << bit)) != 0U)
        {
            if (line != 0xFFU)
            {
                return 2U;      /* more than one bit: ambiguous line */
            }
            line = bit;
        }
    }
    if (line == 0xFFU)
    {
        return 3U;
    }

    /* AFIO is needed for both the EXTI line mapping and the SWJ remap, so it is
       enabled here rather than by each caller. */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_AFIO, ENABLE);

    /* AFIO_EXTICR[x] holds four 4-bit port selectors, one per EXTI line. */
    reg   = (uint8_t)(line / 4U);
    shift = (uint8_t)((line % 4U) * 4U);
    value = AFIO->EXTICR[reg];
    value &= ~(0x0FUL << shift);
    value |= ((uint32_t)src << shift);
    AFIO->EXTICR[reg] = value;

    EXTI_InitStructure.EXTI_Line    = (uint32_t)(1UL << line);
    EXTI_InitStructure.EXTI_Mode    = EXTI_Mode_Interrupt;
    EXTI_InitStructure.EXTI_Trigger = trigger;
    EXTI_InitStructure.EXTI_LineCmd = ENABLE;
    EXTI_Init(&EXTI_InitStructure);

    /* Clear anything latched before enabling, so start-up does not fire a
       spurious event for an edge that happened while configuring. */
    EXTI_ClearITPendingBit((uint32_t)(1UL << line));

    NVIC_InitStructure.NVIC_IRQChannel                   = irqn_from_line(line);
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = preemptPrio;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority        = subPrio;
    NVIC_InitStructure.NVIC_IRQChannelCmd                = ENABLE;
    NVIC_Init(&NVIC_InitStructure);

    return 0U;
}
