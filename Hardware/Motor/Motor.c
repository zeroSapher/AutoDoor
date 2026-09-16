/**
  ******************************************************************************
  * @file    Motor.c
  * @brief   SG90 servo actuator: position commanded over hardware PWM.
  *
  * WHY THIS IS NOT A DRIVER SWAP
  * -----------------------------
  * The L9110S/TB6612 builds drive a CONTINUOUS-ROTATION motor: direction plus
  * duty, held until told to stop, which is why that firmware needs a travel
  * estimate and a story about end stops. An SG90 is a POSITION actuator: a 50 Hz
  * pulse train encodes an angle, the servo goes there and HOLDS it against load.
  * Open and closed are two pulse widths; there is no duty and no coast.
  *
  *   pulse SERVO_CLOSED_US -> door closed
  *   pulse SERVO_OPEN_US   -> door open
  *
  * THE SLEW IS THE SPEED CONTROL
  * -----------------------------
  * An SG90 crosses that span in ~150 ms, which slams a door. Motor_Tick1ms()
  * instead walks the emitted pulse toward the target one step per millisecond, so
  * the servo FOLLOWS the pulse and the door moves gently. The step is derived from
  * DOOR_TRAVEL_MS:
  *
  *     step = (span in tenths of a microsecond) / DOOR_TRAVEL_MS
  *
  * which makes the firmware's timed position estimate and the servo's real travel
  * the same number by construction. Stretching DOOR_TRAVEL_MS is therefore the way
  * to slow the door down, and it costs nothing in accuracy.
  *
  * WHY THE PULSE IS TRACKED IN TENTHS OF A MICROSECOND
  * ---------------------------------------------------
  * The timer only resolves 1 us, but the slew step has to be able to be finer than
  * 1 us per ms: with integer microseconds a travel time longer than the pulse span
  * in milliseconds rounds the step to zero and the servo never moves at all. The
  * extra digit is what allows a slow demo door (DOOR_TRAVEL_MS = 4000 here, i.e.
  * 0.25 us/ms) instead of making it a compile error.
  *
  * Freezing is real: Motor_Stop() points the target at the current pulse, so a move
  * can be halted mid-travel while the servo keeps holding that position. There is
  * no coast to fall back on - see the note on Motor_EmergencyStop().
  *
  * TIMING: TIM3 is clocked at 72 MHz (APB1 timer clock is doubled), so PSC=71
  * gives a 1 us tick and ARR=20000-1 a 20 ms period. CCR1 is then the pulse width
  * in microseconds. A different HSE would break that arithmetic in the same way it
  * would break the SysTick 1 ms setup.
  ******************************************************************************
  */

#include "Motor.h"
#include "main.h"
#include "Delay.h"
#include <stddef.h>

/*===========================================================================*/
/*  Geometry                                                                 */
/*===========================================================================*/

/* Hundredths of a microsecond, not tenths: the slew step is an integer, so with
   tenths a 4000 ms travel gave 10000/4000 = 2 instead of 2.5 and the pulse only
   covered 80 % of its span before the state machine declared arrival. Hundredths
   make 100000/4000 = 25 exact. (A remainder accumulator would be exact for every
   value; this is the cheap version of the same fix.) */
#define PULSE_UNITS_PER_US  100U
#define PULSE_CLOSED        ((uint32_t)SERVO_CLOSED_US * PULSE_UNITS_PER_US)
#define PULSE_OPEN          ((uint32_t)SERVO_OPEN_US * PULSE_UNITS_PER_US)
#define SLEW_STEP           ((PULSE_OPEN - PULSE_CLOSED) / DOOR_TRAVEL_MS)

_Static_assert((SERVO_OPEN_US > SERVO_CLOSED_US),
               "servo open pulse must be longer than the closed pulse");
_Static_assert(((PULSE_OPEN - PULSE_CLOSED) >= DOOR_TRAVEL_MS),
               "DOOR_TRAVEL_MS is longer than the pulse span in tenths of a "
               "microsecond, so the slew step would be 0 and the servo would "
               "never move");

/*===========================================================================*/
/*  Internal state                                                           */
/*===========================================================================*/

static volatile MotorDir_t s_dir         = MOTOR_DIR_STOP;
static volatile uint32_t   s_pulseNow    = PULSE_CLOSED;   /* tenths of a us */
static volatile uint32_t   s_pulseTarget = PULSE_CLOSED;
static uint8_t             s_initialised = 0U;

/*===========================================================================*/
/*  Helpers                                                                  */
/*===========================================================================*/

/** Emit a pulse. Rounds to the timer's 1 us granularity rather than truncating,
    so the middle of a travel lands where it should. */
static void emit_pulse(uint32_t tenths)
{
    TIM_SetCompare1(SERVO_TIM,
                    (uint16_t)((tenths + (PULSE_UNITS_PER_US / 2U)) / PULSE_UNITS_PER_US));
}

/** @return Travel as a percentage, so the status path stays meaningful. */
static uint8_t pulse_to_percent(uint32_t tenths)
{
    uint32_t span = (uint32_t)(PULSE_OPEN - PULSE_CLOSED);
    uint32_t off  = (tenths > PULSE_CLOSED) ? (uint32_t)(tenths - PULSE_CLOSED) : 0U;

    if (off >= span)
    {
        return 100U;
    }

    return (uint8_t)((off * 100U) / span);
}

/*===========================================================================*/
/*  Public API                                                               */
/*===========================================================================*/

/** The APB1 timer clock, read from the clock registers at init time.
 *
 * The servo's entire contract - a 50 Hz frame, and a pulse width counted in
 * microseconds - rests on the prescaler turning the timer clock into a 1 us tick.
 * Hardcoding that clock is a silent trap. This board came up on the 8 MHz HSI
 * (RCC_CFGR = 0: HSE and PLL were never switched on), a 72000000 constant was
 * assumed, and PSC=71 therefore produced a 180 ms frame carrying 9 ms pulses.
 * That is far outside a servo's 0.5-2.5 ms range, so the servo ignored the signal
 * and never moved - while SysTick, the UART baud rate and Delay all kept working,
 * because those already derive from SystemCoreClock. The servo was the only part
 * of the firmware that trusted a number instead of measuring one.
 *
 * An APB1 timer is clocked at PCLK1 when the APB1 prescaler is 1, and at twice
 * PCLK1 when it divides (RM0008, "timer clock frequencies"). Both cases are
 * handled here, so PSC lands on a 1 us tick at 8 MHz (PSC=7) or 72 MHz (PSC=71).
 */
static uint32_t servo_timer_hz(void)
{
    RCC_ClocksTypeDef clocks;

    RCC_GetClocksFreq(&clocks);

    return (clocks.PCLK1_Frequency == clocks.HCLK_Frequency)
               ? clocks.PCLK1_Frequency
               : (clocks.PCLK1_Frequency * 2U);
}

void Motor_Init(void)
{
    GPIO_InitTypeDef        GPIO_InitStructure;
    TIM_TimeBaseInitTypeDef TIM_TimeBaseStructure;
    TIM_OCInitTypeDef       TIM_OCInitStructure;
    uint32_t                timHz;

    RCC_APB1PeriphClockCmd(SERVO_TIM_RCC, ENABLE);
    RCC_APB2PeriphClockCmd(SERVO_RCC, ENABLE);

    GPIO_InitStructure.GPIO_Pin   = SERVO_PIN;
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_AF_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(SERVO_PORT, &GPIO_InitStructure);

    /* 1 us tick, 20 ms period => CCR1 is a pulse width in microseconds. */
    timHz = servo_timer_hz();
    TIM_TimeBaseStructure.TIM_Prescaler     = (uint16_t)(((timHz / 1000000U) > 0U) ? ((timHz / 1000000U) - 1U) : 0U);
    TIM_TimeBaseStructure.TIM_Period        = (uint16_t)(SERVO_PERIOD_US - 1U);
    TIM_TimeBaseStructure.TIM_ClockDivision = TIM_CKD_DIV1;
    TIM_TimeBaseStructure.TIM_CounterMode   = TIM_CounterMode_Up;
    TIM_TimeBaseInit(SERVO_TIM, &TIM_TimeBaseStructure);

    TIM_OCInitStructure.TIM_OCMode      = TIM_OCMode_PWM1;
    TIM_OCInitStructure.TIM_OutputState = TIM_OutputState_Enable;
    TIM_OCInitStructure.TIM_Pulse       = SERVO_CLOSED_US;
    TIM_OCInitStructure.TIM_OCPolarity  = TIM_OCPolarity_High;
    TIM_OC1Init(SERVO_TIM, &TIM_OCInitStructure);
    TIM_OC1PreloadConfig(SERVO_TIM, TIM_OCPreload_Enable);

    TIM_ARRPreloadConfig(SERVO_TIM, ENABLE);
    TIM_Cmd(SERVO_TIM, ENABLE);

    /* Power up pointing at "closed" and holding it, so the door cannot drift while
       the rest of the firmware is still initialising. */
    s_dir         = MOTOR_DIR_STOP;
    s_pulseNow    = PULSE_CLOSED;
    s_pulseTarget = PULSE_CLOSED;
    emit_pulse(s_pulseNow);
    s_initialised = 1U;
}

void Motor_Run(MotorDir_t dir)
{
    if (s_initialised == 0U)
    {
        return;
    }

    if (dir == MOTOR_DIR_OPEN)
    {
        s_dir         = MOTOR_DIR_OPEN;
        s_pulseTarget = PULSE_OPEN;
    }
    else if (dir == MOTOR_DIR_CLOSE)
    {
        s_dir         = MOTOR_DIR_CLOSE;
        s_pulseTarget = PULSE_CLOSED;
    }
    else
    {
        /* Anything else is not a move: treat it as Stop so a bad call cannot leave
           the door running to a target nobody asked for. */
        Motor_Stop();
    }
}

void Motor_Stop(void)
{
    /* Freeze where we are. The servo keeps holding this pulse, so the door stays
       put - for a servo "stopped" and "locked" are the same thing. */
    s_pulseTarget = s_pulseNow;
    s_dir         = MOTOR_DIR_STOP;
}

void Motor_Brake(void)
{
    /* Identical to Stop here, and that is not a shortcut: a servo holds position
       whenever it is powered, so there is no separate braking state to enter. */
    Motor_Stop();
    s_dir = MOTOR_DIR_BRAKE;
}

void Motor_EmergencyStop(void)
{
    /* Freeze at the pulse that is on the wire right now, not at the target: an
       emergency stop must not let the door finish a move it has started.
       Unlike the H-bridge builds there is NO coast state to fall back on - a servo
       with no pulses goes limp, and a door that falls open or shut by itself is not
       a safer failure. Holding is the safe state, and the position is only as wrong
       as it already was.
       (For a pushable door instead, disable the channel - TIM_Cmd(SERVO_TIM,
       DISABLE) - which detaches the servo. That is a deliberate behaviour choice,
       not a tuning knob, so it is not the default.) */
    s_pulseTarget = s_pulseNow;
    s_dir         = MOTOR_DIR_STOP;
    emit_pulse(s_pulseNow);
}

void Motor_SetDuty(uint8_t percent)
{
    /* Accepted and ignored: an SG90's speed is fixed by the servo, and this
       driver's only speed control is the slew, which follows DOOR_TRAVEL_MS. The
       callers keep calling it because they are shared with the motor builds;
       SPEED=<%> answers "n/a" on this branch so nobody is told a number applied. */
    (void)percent;
}

uint8_t Motor_GetDuty(void)
{
    return pulse_to_percent(s_pulseNow);
}

MotorDir_t Motor_GetDir(void)
{
    return s_dir;
}

uint8_t Motor_IsRamping(void)
{
    return (s_pulseNow != s_pulseTarget) ? 1U : 0U;
}

uint8_t Motor_IsIdle(void)
{
    return ((s_dir == MOTOR_DIR_STOP) || (s_dir == MOTOR_DIR_BRAKE)) ? 1U : 0U;
}

void Motor_Tick1ms(void)
{
    if (s_initialised == 0U)
    {
        return;
    }

    if (s_pulseNow == s_pulseTarget)
    {
        return;
    }

    if (s_pulseNow < s_pulseTarget)
    {
        uint32_t next = s_pulseNow + SLEW_STEP;

        s_pulseNow = (next > s_pulseTarget) ? s_pulseTarget : next;
    }
    else
    {
        uint32_t next = s_pulseNow - SLEW_STEP;

        s_pulseNow = (next < s_pulseTarget) ? s_pulseTarget : next;
    }

    emit_pulse(s_pulseNow);
}
