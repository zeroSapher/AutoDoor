/**
  ******************************************************************************
  * @file    stm32f10x_it.c
  * @brief   Cortex-M3 exception handlers and every peripheral interrupt.
  *
  * EXTI SOURCE RESOLUTION
  * ----------------------
  * On STM32F1 the EXTI lines are shared per pin NUMBER, so this project
  * deliberately places one input per number and lets two different ports share a
  * vector:
  *
  *   EXTI0  ->  PA0 (open limit)        and  PB0 (outside sensor)
  *   EXTI1  ->  PA1 (close limit)       and  PB1 (inside sensor)
  *
  * Both appear as the same vector, so the handler has to work out which one
  * fired. Each input is debounced by re-reading its own pin after a settling
  * window (see Debounce.c), which means a mis-attributed edge would be discarded
  * on its own anyway - the extra `if` below is belt and braces, and it costs
  * nothing because the pin read is what the debouncer does regardless.
  *
  * The handlers are intentionally minimal: flag an edge, and let the 1 ms tick
  * do everything else. Nothing time-consuming belongs in an ISR here, because
  * the limit lines sit at preemption priority 0 and delaying them delays the
  * only layer that can stop the door in software.
  ******************************************************************************
  */

#include "stm32f10x_it.h"
#include "main.h"
#include "UART.h"
#include "Delay.h"
#include "Motor.h"
#include "Limit.h"
#include "Sensor.h"
#include "Key.h"
#include "Buzzer.h"
#include "StatusLed.h"

/*===========================================================================*/
/*  Cortex-M3 core exceptions                                                */
/*===========================================================================*/

void NMI_Handler(void)
{
}

void HardFault_Handler(void)
{
    /* A hard fault means the code is already broken. Latch the motor off first
       so the door cannot keep driving while the target sits here, then spin so a
       debugger can read the stacked PC/LR. */
    Motor_EmergencyStop();

    while (1)
    {
    }
}

void MemManage_Handler(void)
{
    Motor_EmergencyStop();
    while (1)
    {
    }
}

void BusFault_Handler(void)
{
    Motor_EmergencyStop();
    while (1)
    {
    }
}

void UsageFault_Handler(void)
{
    Motor_EmergencyStop();
    while (1)
    {
    }
}

void SVC_Handler(void)
{
}

void DebugMon_Handler(void)
{
}

void PendSV_Handler(void)
{
}

/**
  * @brief  SysTick - the 1 ms heartbeat of the whole system.
  * @note   This is where the requirement "timer interrupt drives the motor PWM"
  *         is actually met: Motor_Tick1ms() synthesises the PWM carrier and runs
  *         the speed ramp. Every other periodic task hangs off the same tick.
  */
void SysTick_Handler(void)
{
    g_msTick++;

    Motor_Tick1ms();

    Limit_Tick1ms();
    Sensor_Tick1ms();
    Key_Tick1ms();
    Buzzer_Tick1ms();
    StatusLed_Tick1ms();
}

/*===========================================================================*/
/*  Peripheral interrupts                                                    */
/*===========================================================================*/

/** USART1 - receive only; the buffering policy lives in UART.c. */
void USART1_IRQHandler(void)
{
    if (USART_GetITStatus(USART1, USART_IT_RXNE) != RESET)
    {
        UART_IrqHandler();
    }
}

/**
  * @brief  EXTI line 0 - open limit (PA0) or outside sensor (PB0).
  */
void EXTI0_IRQHandler(void)
{
    if (EXTI_GetITStatus(EXTI_Line0) != RESET)
    {
        /*
         * Identify the source by reading the pin, and mind the polarity: both
         * inputs are active LOW, so a LOW pin means the limit has tripped.
         *
         *   limit active  -> PA0 low
         *   sensor active -> PB0 low
         *
         * If both were low at once the sensor branch wins, which simply means a
         * missed edge event; the debouncer re-reads the pin either way, and the
         * limit LEVEL is what actually stops the door (see Door_Update), so a
         * wrong guess here is self-correcting rather than dangerous.
         */
        if (GPIO_ReadInputDataBit(LIMIT_OPEN_PORT, LIMIT_OPEN_PIN) == Bit_RESET)
        {
            Limit_IrqHandler(1U);

            /*
             * Cut the motor HERE, not in the main loop.
             *
             * The state machine could be blocked when this fires - Motor_Stop()
             * and Motor_Run() both ramp the duty down with Delay_ms(), and a log
             * flush can spend milliseconds in the EEPROM. Waiting for the loop
             * would mean the motor keeps driving into the end stop for the whole
             * of that time. Motor_EmergencyStop() is two GPIO writes with no
             * delay, so it is safe from interrupt context.
             *
             * The hardware NC contact already removes motor power as the outer
             * layer; this is the fast electronic layer inside it.
             */
            Motor_EmergencyStop();
        }
        else
        {
            Sensor_IrqHandler(1U);
        }

        EXTI_ClearITPendingBit(EXTI_Line0);
    }
}

/**
  * @brief  EXTI line 1 - close limit (PA1) or inside sensor (PB1).
  */
void EXTI1_IRQHandler(void)
{
    if (EXTI_GetITStatus(EXTI_Line1) != RESET)
    {
        /* Active LOW, as on line 0 - see the note in EXTI0_IRQHandler. */
        if (GPIO_ReadInputDataBit(LIMIT_CLOSE_PORT, LIMIT_CLOSE_PIN) == Bit_RESET)
        {
            Limit_IrqHandler(0U);
            Motor_EmergencyStop();      /* see the note in EXTI0_IRQHandler */
        }
        else
        {
            Sensor_IrqHandler(0U);
        }

        EXTI_ClearITPendingBit(EXTI_Line1);
    }
}

/**
  * @brief  EXTI lines 5-9 - the four panel keys (PB5..PB8).
  * @note   Several lines share this vector, so every line is tested in turn.
  */
void EXTI9_5_IRQHandler(void)
{
    if (EXTI_GetITStatus(EXTI_Line5) != RESET)
    {
        Key_IrqHandler(KEY_ID_START);
        EXTI_ClearITPendingBit(EXTI_Line5);
    }
    if (EXTI_GetITStatus(EXTI_Line6) != RESET)
    {
        Key_IrqHandler(KEY_ID_MODE);
        EXTI_ClearITPendingBit(EXTI_Line6);
    }
    if (EXTI_GetITStatus(EXTI_Line7) != RESET)
    {
        Key_IrqHandler(KEY_ID_OPEN);
        EXTI_ClearITPendingBit(EXTI_Line7);
    }
    if (EXTI_GetITStatus(EXTI_Line8) != RESET)
    {
        Key_IrqHandler(KEY_ID_CLOSE_ESTOP);
        EXTI_ClearITPendingBit(EXTI_Line8);
    }
}
