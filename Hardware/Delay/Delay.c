/**
  ******************************************************************************
  * @file    Delay.c
  * @brief   Millisecond and microsecond blocking delays - see Delay.h.
  ******************************************************************************
  */

#include "Delay.h"

/*===========================================================================*/
/*  Public state                                                             */
/*===========================================================================*/

volatile uint32_t g_msTick = 0;

/*===========================================================================*/
/*  Microsecond time base                                                    */
/*===========================================================================*/

#define DELAY_TIM_US_BASE       TIM4
#define DELAY_TIM_US_RCC        RCC_APB1Periph_TIM4

/*
 * Prefer the Cortex-M3 DWT cycle counter: it is 32-bit and runs at the core
 * clock, so a microsecond is exactly (SystemCoreClock / 1000000) cycles with no
 * overflow to reason about. TIM4 is used only when no debug unit is available
 * (for example when the chip is locked down), where one tick is 1 us.
 *
 * This vintage of core_cm3.h (CMSIS 1.x) declares CoreDebug and ITM but not
 * DWT, so the two registers used here are addressed directly. Both live in the
 * private peripheral bus at fixed architectural addresses.
 */
#define DWT_CTRL                (*(volatile uint32_t *)0xE0001000UL)
#define DWT_CYCCNT              (*(volatile uint32_t *)0xE0001004UL)
#define DWT_CTRL_CYCCNTENA      (1UL << 0)

static uint8_t  s_useDwt   = 0;
static uint32_t s_cyclesPerUs = 0;

/** TIM4 free-running microseconds, used when DWT is unavailable. */
static uint32_t tim_us_count(void)
{
    return (uint32_t)TIM4->CNT + ((uint32_t)TIM4->SR & TIM_FLAG_Update ? 65536UL : 0UL);
}

/** Free-running microseconds, from whichever time base is active. */
static uint32_t us_count(void)
{
    if (s_useDwt)
    {
        /*
         * Read CNT before CTRL so that an overflow arriving between the two
         * reads is attributed to the older (lower) count rather than being
         * missed, which would make the counter appear to jump backwards.
         */
        uint32_t ticks = DWT_CYCCNT;
        uint32_t top   = DWT_CTRL & DWT_CTRL_CYCCNTENA;

        if (top == 0U && ticks != 0U)
        {
            return 0xFFFFFFFFU / s_cyclesPerUs;   /* counter just wrapped */
        }
        return ticks / s_cyclesPerUs;
    }

    return tim_us_count();
}

/** Start TIM4 as a 1 MHz free-running counter (one tick == 1 us). */
static void tim_us_init(void)
{
    TIM_TimeBaseInitTypeDef TIM_TimeBaseStructure;

    RCC_APB1PeriphClockCmd(DELAY_TIM_US_RCC, ENABLE);

    /* Prescaler divides the 72 MHz APB1 timer clock down to 1 MHz. */
    TIM_TimeBaseStructure.TIM_Prescaler         = (uint16_t)(SystemCoreClock / 1000000U) - 1U;
    TIM_TimeBaseStructure.TIM_CounterMode       = TIM_CounterMode_Up;
    TIM_TimeBaseStructure.TIM_Period            = 0xFFFFU;
    TIM_TimeBaseStructure.TIM_ClockDivision     = TIM_CKD_DIV1;
    TIM_TimeBaseStructure.TIM_RepetitionCounter = 0;
    TIM_TimeBaseInit(DELAY_TIM_US_BASE, &TIM_TimeBaseStructure);

    TIM_ClearFlag(DELAY_TIM_US_BASE, TIM_FLAG_Update);
    TIM_Cmd(DELAY_TIM_US_BASE, ENABLE);
}

/*===========================================================================*/
/*  Public API                                                               */
/*===========================================================================*/

void Delay_Init(void)
{
    /*
     * Derive the real core clock from the RCC registers instead of trusting the
     * compile-time default, so delays stay accurate when the board runs on the
     * 8 MHz internal oscillator (missing or failed HSE crystal).
     */
    SystemCoreClockUpdate();

    /* ---- SysTick: 1 ms periodic interrupt ---- */
    if (SysTick_Config(SystemCoreClock / 1000U) != 0U)
    {
        /* Reload value out of range - should not happen at 72 MHz. */
        while (1)
        {
        }
    }

    /* ---- Microsecond time base ---- */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;      /* power up the trace unit */
    DWT_CYCCNT = 0U;
    DWT_CTRL  |= DWT_CTRL_CYCCNTENA;

    if ((DWT_CTRL & DWT_CTRL_CYCCNTENA) != 0U)
    {
        s_cyclesPerUs = SystemCoreClock / 1000000U;
        if (s_cyclesPerUs == 0U)
        {
            s_cyclesPerUs = 1U;
        }
        s_useDwt = 1;
    }
    else
    {
        s_useDwt = 0;
        tim_us_init();
    }
}

uint32_t Delay_GetUsTick(void)
{
    return us_count();
}

void Delay_us(uint32_t us)
{
    uint32_t start;

    if (us == 0U)
    {
        return;
    }

    start = us_count();
    while ((us_count() - start) < us)
    {
        /* busy wait */
    }
}

void Delay_ms(uint32_t ms)
{
    while (ms-- != 0U)
    {
        Delay_us(1000U);
    }
}
