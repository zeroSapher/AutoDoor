/**
  ******************************************************************************
  * @file    startup_stm32f10x_md_gcc.c
  * @brief   Startup and vector table for STM32F10x medium-density, GNU toolchain.
  *
  * WHY A C FILE
  * ------------
  * The ST-supplied Drivers/CMSIS/startup_stm32f10x_md.s uses ARM/Keil assembler
  * directives (AREA, EXPORT, PROC, DCD, ...) and does not assemble with GNU as.
  * PM0214 section 2.3.4 defines the vector table purely in terms of 32-bit
  * words, so building it in C with the array placed in .isr_vector - which the
  * linker script puts at the very start of flash - produces byte-identical
  * results without a second assembly dialect to maintain.
  *
  * Keil projects use the original .s file; CMake and the Makefile use this one.
  * Only one of the two may be linked at a time: both define Reset_Handler.
  ******************************************************************************
  */

#include <stdint.h>

/* Symbols provided by the linker script. */
extern uint32_t _estack;    /* top of SRAM: the initial stack pointer  */
extern uint32_t _sidata;    /* .data load address in flash             */
extern uint32_t _sdata;     /* .data start in RAM                      */
extern uint32_t _edata;     /* .data end in RAM                        */
extern uint32_t _sbss;      /* .bss start in RAM                       */
extern uint32_t _ebss;      /* .bss end in RAM                         */

int main(void);
void SystemInit(void);
void Reset_Handler(void);
void Default_Handler(void);

/* Provided by newlib; runs the .init_array / .fini_array constructors. */
extern void __libc_init_array(void);

/*
 * Every vector is weakly aliased to Default_Handler, so the firmware links
 * without a definition for each unused interrupt, and any handler that IS
 * defined (in Core/stm32f10x_it.c) silently overrides the alias.
 */
#define WEAK_DEFAULT(name) \
    void name(void) __attribute__((weak, alias("Default_Handler")))

WEAK_DEFAULT(NMI_Handler);
WEAK_DEFAULT(HardFault_Handler);
WEAK_DEFAULT(MemManage_Handler);
WEAK_DEFAULT(BusFault_Handler);
WEAK_DEFAULT(UsageFault_Handler);
WEAK_DEFAULT(SVC_Handler);
WEAK_DEFAULT(DebugMon_Handler);
WEAK_DEFAULT(PendSV_Handler);
WEAK_DEFAULT(SysTick_Handler);

/* --- Peripheral interrupts (STM32F103 medium density) --- */
WEAK_DEFAULT(WWDG_IRQHandler);
WEAK_DEFAULT(PVD_IRQHandler);
WEAK_DEFAULT(TAMPER_IRQHandler);
WEAK_DEFAULT(RTC_IRQHandler);
WEAK_DEFAULT(FLASH_IRQHandler);
WEAK_DEFAULT(RCC_IRQHandler);
WEAK_DEFAULT(EXTI0_IRQHandler);
WEAK_DEFAULT(EXTI1_IRQHandler);
WEAK_DEFAULT(EXTI2_IRQHandler);
WEAK_DEFAULT(EXTI3_IRQHandler);
WEAK_DEFAULT(EXTI4_IRQHandler);
WEAK_DEFAULT(DMA1_Channel1_IRQHandler);
WEAK_DEFAULT(DMA1_Channel2_IRQHandler);
WEAK_DEFAULT(DMA1_Channel3_IRQHandler);
WEAK_DEFAULT(DMA1_Channel4_IRQHandler);
WEAK_DEFAULT(DMA1_Channel5_IRQHandler);
WEAK_DEFAULT(DMA1_Channel6_IRQHandler);
WEAK_DEFAULT(DMA1_Channel7_IRQHandler);
WEAK_DEFAULT(ADC1_2_IRQHandler);
WEAK_DEFAULT(USB_HP_CAN1_TX_IRQHandler);
WEAK_DEFAULT(USB_LP_CAN1_RX0_IRQHandler);
WEAK_DEFAULT(CAN1_RX1_IRQHandler);
WEAK_DEFAULT(CAN1_SCE_IRQHandler);
WEAK_DEFAULT(EXTI9_5_IRQHandler);
WEAK_DEFAULT(TIM1_BRK_IRQHandler);
WEAK_DEFAULT(TIM1_UP_IRQHandler);
WEAK_DEFAULT(TIM1_TRG_COM_IRQHandler);
WEAK_DEFAULT(TIM1_CC_IRQHandler);
WEAK_DEFAULT(TIM2_IRQHandler);
WEAK_DEFAULT(TIM3_IRQHandler);
WEAK_DEFAULT(TIM4_IRQHandler);
WEAK_DEFAULT(I2C1_EV_IRQHandler);
WEAK_DEFAULT(I2C1_ER_IRQHandler);
WEAK_DEFAULT(I2C2_EV_IRQHandler);
WEAK_DEFAULT(I2C2_ER_IRQHandler);
WEAK_DEFAULT(SPI1_IRQHandler);
WEAK_DEFAULT(SPI2_IRQHandler);
WEAK_DEFAULT(USART1_IRQHandler);
WEAK_DEFAULT(USART2_IRQHandler);
WEAK_DEFAULT(USART3_IRQHandler);
WEAK_DEFAULT(EXTI15_10_IRQHandler);
WEAK_DEFAULT(RTCAlarm_IRQHandler);
WEAK_DEFAULT(USBWakeUp_IRQHandler);

/**
  * @brief  Vector table at the start of flash.
  * @note   Element 0 is the initial stack pointer, not a handler; the Cortex-M3
  *         core loads it before executing anything.
  */
__attribute__((section(".isr_vector"), used))
void (*const g_vectors[])(void) =
{
    (void (*)(void))(&_estack),     /* 0x00  initial stack pointer       */
    Reset_Handler,                  /* 0x04  reset                       */
    NMI_Handler,                    /* 0x08  non-maskable interrupt      */
    HardFault_Handler,              /* 0x0C  hard fault                  */
    MemManage_Handler,              /* 0x10  memory management fault     */
    BusFault_Handler,               /* 0x14  bus fault                   */
    UsageFault_Handler,             /* 0x18  usage fault                 */
    0, 0, 0, 0,                     /* 0x1C  reserved                    */
    SVC_Handler,                    /* 0x2C  supervisor call             */
    DebugMon_Handler,               /* 0x30  debug monitor               */
    0,                              /* 0x34  reserved                    */
    PendSV_Handler,                 /* 0x38  pendable request            */
    SysTick_Handler,                /* 0x3C  system tick                 */

    /* External interrupts 0..42 */
    WWDG_IRQHandler,                /* 0   window watchdog               */
    PVD_IRQHandler,                 /* 1   PVD through EXTI line 16      */
    TAMPER_IRQHandler,              /* 2   tamper                        */
    RTC_IRQHandler,                 /* 3   RTC global                    */
    FLASH_IRQHandler,               /* 4   flash global                  */
    RCC_IRQHandler,                 /* 5   RCC global                    */
    EXTI0_IRQHandler,               /* 6   EXTI line 0                   */
    EXTI1_IRQHandler,               /* 7   EXTI line 1                   */
    EXTI2_IRQHandler,               /* 8   EXTI line 2                   */
    EXTI3_IRQHandler,               /* 9   EXTI line 3                   */
    EXTI4_IRQHandler,               /* 10  EXTI line 4                   */
    DMA1_Channel1_IRQHandler,       /* 11                               */
    DMA1_Channel2_IRQHandler,       /* 12                               */
    DMA1_Channel3_IRQHandler,       /* 13                               */
    DMA1_Channel4_IRQHandler,       /* 14                               */
    DMA1_Channel5_IRQHandler,       /* 15                               */
    DMA1_Channel6_IRQHandler,       /* 16                               */
    DMA1_Channel7_IRQHandler,       /* 17                               */
    ADC1_2_IRQHandler,              /* 18  ADC1 and ADC2                 */
    USB_HP_CAN1_TX_IRQHandler,      /* 19  USB high prio / CAN TX        */
    USB_LP_CAN1_RX0_IRQHandler,     /* 20  USB low prio / CAN RX0        */
    CAN1_RX1_IRQHandler,            /* 21  CAN RX1                       */
    CAN1_SCE_IRQHandler,            /* 22  CAN SCE                       */
    EXTI9_5_IRQHandler,             /* 23  EXTI lines 5..9               */
    TIM1_BRK_IRQHandler,            /* 24  TIM1 break                    */
    TIM1_UP_IRQHandler,             /* 25  TIM1 update                   */
    TIM1_TRG_COM_IRQHandler,        /* 26  TIM1 trigger / commutation    */
    TIM1_CC_IRQHandler,             /* 27  TIM1 capture compare          */
    TIM2_IRQHandler,                /* 28                               */
    TIM3_IRQHandler,                /* 29                               */
    TIM4_IRQHandler,                /* 30                               */
    I2C1_EV_IRQHandler,             /* 31  I2C1 event                    */
    I2C1_ER_IRQHandler,             /* 32  I2C1 error                    */
    I2C2_EV_IRQHandler,             /* 33  I2C2 event                    */
    I2C2_ER_IRQHandler,             /* 34  I2C2 error                    */
    SPI1_IRQHandler,                /* 35                               */
    SPI2_IRQHandler,                /* 36                               */
    USART1_IRQHandler,              /* 37                               */
    USART2_IRQHandler,              /* 38                               */
    USART3_IRQHandler,              /* 39                               */
    EXTI15_10_IRQHandler,           /* 40  EXTI lines 10..15             */
    RTCAlarm_IRQHandler,            /* 41  RTC alarm through EXTI17      */
    USBWakeUp_IRQHandler            /* 42  USB wakeup through EXTI18     */
};

/*
 * The STM32F103 medium-density vector table is 16 system entries (initial SP,
 * 15 handlers) plus 43 peripheral IRQs = 59 words. Asserting the size at
 * compile time turns a miscounted or misordered table into a build error
 * instead of a hard fault that only shows up on the target.
 */
typedef char vector_table_size_check[
    (sizeof(g_vectors) / sizeof(g_vectors[0]) == 59) ? 1 : -1];

/**
  * @brief  Reset entry point: copy .data, zero .bss, run constructors, main().
  */
void Reset_Handler(void)
{
    uint32_t *src;
    uint32_t *dst;

    /* Copy initialised data from its flash load address into RAM. */
    src = &_sidata;
    dst = &_sdata;
    while (dst < &_edata)
    {
        *dst++ = *src++;
    }

    /* Zero the .bss segment: C guarantees statics start at zero. */
    for (dst = &_sbss; dst < &_ebss; dst++)
    {
        *dst = 0U;
    }

    /* Match the ST startup sequence: main.c configures delay and USART1 from
       SystemCoreClock, so the PLL must be configured before either peripheral
       is initialised.  Without this call the MCU stays at reset's 8 MHz HSI
       while the firmware calculates a 72 MHz UART divisor. */
    SystemInit();

    /* Run .init_array constructors. Nothing here needs them today, but it keeps
       the startup correct if C++ or constructor attributes are added later. */
    __libc_init_array();

    main();

    /* main() must not return on bare metal; trap it rather than running off. */
    for (;;)
    {
    }
}

/** @brief Catch-all for every vector that has no real handler. */
void Default_Handler(void)
{
    for (;;)
    {
    }
}
