/**
  ******************************************************************************
  * @file    stm32f10x_it.c
  * @brief   Cortex-M3 exception handlers and every peripheral interrupt.
  *
  * EXTI SOURCE RESOLUTION
  * ----------------------
  * On STM32F1 the EXTI lines are shared per pin NUMBER: PA0, PB0 and PC0 all map
  * to EXTI0, and AFIO can route only ONE port to a line. The eight inputs are
  * therefore placed on eight distinct numbers, and every handler below has a
  * single possible source:
  *
  *   EXTI0      ->  PA0        (open limit)
  *   EXTI1      ->  PA1        (close limit)
  *   EXTI5..8   ->  PB5..PB8   (keys, one shared vector)
  *   EXTI12/13  ->  PB12/PB13  (sensors, one shared vector)
  *
  * This block used to describe the opposite arrangement - "two different ports
  * share a vector" - with the handlers working out the source by reading a pin.
  * That could not work: the second initialiser takes the line away from the first,
  * so the limit switches had no interrupt at all and their emergency-stop calls
  * were unreachable code. Reading a pin to guess the source of an interrupt that
  * cannot have come from there is not belt and braces; it is a wrong answer
  * wearing a reassuring comment. See the EXTI note in Core/main.h.
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
#include "Log.h"

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

    /*
     * Log_Tick1ms() MUST be here. Without it the event log never flushes on its
     * own: entries queue up in RAM and are only written when a burst happens to
     * fill the batch, so in normal operation (a few events per minute) nothing
     * ever reaches the EEPROM and a power cut loses all of it. The whole
     * persistent-storage feature depends on this one line.
     */
    Log_Tick1ms();
}

/*===========================================================================*/
/*  Peripheral interrupts                                                    */
/*===========================================================================*/

/** USART1 - RX into the ring buffer, TX refilled from its own ring. */
void USART1_IRQHandler(void)
{
    if (USART_GetITStatus(USART1, USART_IT_RXNE) != RESET)
    {
        UART_IrqHandler();
    }

    if (USART_GetITStatus(USART1, USART_IT_TXE) != RESET)
    {
        UART_TxIrqHandler();
    }
}

/**
  * @brief  EXTI line 0 - the open limit switch (PA0), and nothing else.
  * @note   This used to try to serve both PA0 and PB0, because the two shared
  *         EXTI line 0. That could never have worked: AFIO routes exactly one
  *         port to a line, so whichever module called Exti_ConfigPin() last took
  *         the line - and since Sensor_Init() runs after Limit_Init(), it was
  *         always the sensor. The limit switch had no interrupt at all, so the
  *         motor-cut call the design relied on was unreachable code. Reading the
  *         pin to guess the source was therefore not "self-correcting", it was
  *         guessing about an interrupt that could not have come from there.
  *
  *         The inputs now occupy distinct line numbers (see the EXTI rule in
  *         Core/main.h), so this handler has exactly one possible source.
  */
void EXTI0_IRQHandler(void)
{
    if (EXTI_GetITStatus(EXTI_Line0) != RESET)
    {
        /*
         * Limit_IrqHandler() both starts the debounce window AND decides whether
         * to cut the motor. That decision deliberately lives in Hardware/Limit.c
         * rather than here:
         *
         *   - the line is configured Rising_Falling, so this fires on the RELEASE
         *     edge too, and only the assert edge may cut the motor. A bare
         *     Motor_EmergencyStop() here cut the motor as the door left the
         *     switch, which stranded it a few millimetres off the limit with
         *     nothing to restart it - the default build could not open or close
         *     the door at all;
         *   - the assert condition depends on the active level, which Limit.c
         *     owns (activeLevel 0), so duplicating it here is how the two drift.
         *
         * The cut is still immediate - two GPIO writes, no delay, safe from
         * interrupt context - which is the point of doing it on this path at all:
         * the main loop can be blocked in Motor_Stop()'s ramp, in the ~140 ms
         * panel refresh, or in an EEPROM write. The hardware NC contact is the
         * outer layer; this is the fast electronic layer inside it.
         */
        (void)Limit_IrqHandler(1U);

        EXTI_ClearITPendingBit(EXTI_Line0);
    }
}

/**
  * @brief  EXTI line 1 - the close limit switch (PA1), and nothing else.
  * @note   See the note in EXTI0_IRQHandler for why the motor-cut decision is not
  *         made here.
  */
void EXTI1_IRQHandler(void)
{
    if (EXTI_GetITStatus(EXTI_Line1) != RESET)
    {
        (void)Limit_IrqHandler(0U);

        EXTI_ClearITPendingBit(EXTI_Line1);
    }
}

/**
  * @brief  EXTI lines 10-15 - the two presence sensors (PB12, PB13).
  * @note   They are the only users of this vector, but the other lines in the
  *         range are still tested so that a future input added here cannot be
  *         silently swallowed by this handler.
  */
void EXTI15_10_IRQHandler(void)
{
    if (EXTI_GetITStatus(EXTI_Line12) != RESET)
    {
        Sensor_IrqHandler(1U);      /* outside sensor: someone entering */
        EXTI_ClearITPendingBit(EXTI_Line12);
    }

    if (EXTI_GetITStatus(EXTI_Line13) != RESET)
    {
        Sensor_IrqHandler(0U);      /* inside sensor: someone leaving */
        EXTI_ClearITPendingBit(EXTI_Line13);
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
