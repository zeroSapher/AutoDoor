/**
  ******************************************************************************
  * @file    stm32f10x_it.h
  * @brief   Cortex-M3 exception and peripheral interrupt handler prototypes.
  ******************************************************************************
  */

#ifndef __STM32F10x_IT_H
#define __STM32F10x_IT_H

#ifdef __cplusplus
extern "C" {
#endif

/* --- Cortex-M3 core exceptions --- */
void NMI_Handler(void);
void HardFault_Handler(void);
void MemManage_Handler(void);
void BusFault_Handler(void);
void UsageFault_Handler(void);
void SVC_Handler(void);
void DebugMon_Handler(void);
void PendSV_Handler(void);
void SysTick_Handler(void);

/* --- Peripheral interrupts used by AutoDoor --- */
void USART1_IRQHandler(void);           /* PC console                     */

/* --- EXTI: one vector per line 0-4, then grouped --- */
void EXTI0_IRQHandler(void);            /* PA0  open limit  / PB0 sensor-out */
void EXTI1_IRQHandler(void);            /* PA1  close limit / PB1 sensor-in  */
void EXTI9_5_IRQHandler(void);          /* PB5..PB8 keys                     */

#ifdef __cplusplus
}
#endif

#endif /* __STM32F10x_IT_H */
