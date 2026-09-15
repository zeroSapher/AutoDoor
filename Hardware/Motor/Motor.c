/**
  ******************************************************************************
  * @file    Motor.c
  * @brief   SG90 servo actuator: position commanded over hardware PWM.
  *
  * WHY THIS IS NOT A DRIVER SWAP
  * -----------------------------
  * The L9110S/TB6612 builds drive a CONTINUOUS-ROTATION motor: they command a
  * direction plus a duty and keep driving until told to stop, which is why that
  * firmware needs a travel estimate and a story about end stops.
  *
  * An SG90 is a POSITION actuator. It takes a 50 Hz pulse train, moves to the
  * angle the pulse width encodes, and then HOLDS it against load. Open and closed
  * are two pulse widths, there is no duty to modulate, and a servo that is not
  * being moved is already holding the door - the behaviour the DC version could
  * only approximate by calling Motor_Brake() (which it never did).
  *
  *   pulse SERVO_CLOSED_US -> door closed
  *   pulse SERVO_OPEN_US   -> door open
  *
  * THE SLEW, AND WHY IT IS THE INTERESTING PART
  * -------------------------------------------
  * An SG90 covers that span in roughly 150 ms, which would slam the door. Instead
  * Motor_Tick1ms() walks the emitted pulse toward the target one step per
  * millisecond, so the servo FOLLOWS the commanded pulse and the door moves
  * gently. The step is derived from DOOR_TRAVEL_MS, which makes the two agree by
  * construction:
  *
  *   step = (SERVO_OPEN_US - SERVO_CLOSED_US) / DOOR_TRAVEL_MS
  *
  * so the firmware's timed position estimate and the servo's real travel are the
  * same number. On the DC-motor builds that estimate was a guess that had to be
  * calibrated against the mechanism; here it is exact, because this driver decides
  * how fast the pulse travels.
  *
  * Freezing is real: Motor_Stop() points the target at the current pulse, so a
  * move can be halted mid-travel (and Motor_EmergencyStop() freezes instantly)
  * while the servo keeps holding that position. There is no coast state to fall
  * back on - see the note on Motor_EmergencyStop().
  *
  * TIMING: TIM3 is clocked at 72 MHz (APB1 timer clock is doubled), so PSC=71
  * gives a 1 us tick and ARR=20000-1 a 20 ms period. CCR1 is then literally the
  * pulse width in microseconds. A different HSE would break that arithmetic in
  * the same way it would break the SysTick 1 ms setup.
  ******************************************************************************
  */

#include "Motor.h"
#include "main.h"
#include "Delay.h"
#include <stddef.h>

/*===========================================================================*/
/*  Geometry                                                                 */
/*===========================================================================*/

/* One step of the slew, in microseconds per millisecond. Derived so the pulse
   reaches the far end exactly when the state machine's timed estimate says it
   has; the compile-time check below rejects a DOOR_TRAVEL_MS so long that the
   division would round the step down to nothing. */
#define SLEW_STEP_US        ((SERVO_OPEN_US - SERVO_CLOSED_US) / DOOR_TRAVEL_MS)

_Static_assert((SERVO_OPEN_US > SERVO_CLOSED_US),
               "servo open pulse must be longer than the closed pulse");
_Static_assert(((SERVO_OPEN_US - SERVO_CLOSED_US) >= DOOR_TRAVEL_MS),
               "DOOR_TRAVEL_MS is longer than the pulse span in microseconds, so "
               "the slew step would be 0 and the servo would never move");

/*===========================================================================*/
/*  Internal state                                                           */
/*===========================================================================*/

static volatile MotorDir_t s_dir          = MOTOR_DIR_STOP;
static volatile uint16_t   s_pulseNow     = SERVO_CLOSED_US;
static volatile uint16_t   s_pulseTarget  = SERVO_CLOSED_US;
static uint8_t             s_initialised  = 0U;

/*===========================================================================*/
/*  Helpers                                                                  */
/*===========================================================================*/

/** Emit a pulse width. Written straight to CCR1: the timer does the rest. */
static void emit_pulse(uint16_t us)
{
    TIM_SetCompare1(SERVO_TIM, (uint16_t)us);
}

/** @return The duty equivalent of a pulse, for the status reporting path. */
static uint8_t pulse_to_percent(uint16_t us)
{
    uint32_t span = (uint32_t)(SERVO_OPEN_US - SERVO_CLOSED_US);
    uint32_t off  = (us > SERVO_CLOSED_US) ? (uint32_t)(us - SERVO_CLOSED_US) : 0U;

    if (off >= span)
    {
        return 100U;
    }

    return (uint8_t)((off * 100U) / span);
}

/*===========================================================================*/
/*  Public API                                                               */
/*===========================================================================*/

void Motor_Init(void)
{
    GPIO_InitTypeDef        GPIO_InitStructure;
    TIM_TimeBaseInitTypeDef TIM_TimeBaseStructure;
    TIM_OCInitTypeDef       TIM_OCInitStructure;

    RCC_APB1PeriphClockCmd(SERVO_TIM_RCC, ENABLE);
    RCC_APB2PeriphClockCmd(SERVO_RCC, ENABLE);

    /* PA6 as TIM3_CH1, alternate function push-pull. */
    GPIO_InitStructure.GPIO_Pin   = SERVO_PIN;
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_AF_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(SERVO_PORT, &GPIO_InitStructure);

    /* 1 us tick, 20 ms period => CCR1 is a pulse width in microseconds. */
    TIM_TimeBaseStructure.TIM_Prescaler         = (uint16_t)((SERVO_TIMER_HZ / 1000000U) - 1U);
    TIM_TimeBaseStructure.TIM_Period            = (uint16_t)(SERVO_PERIOD_US - 1U);
    TIM_TimeBaseStructure.TIM_ClockDivision     = TIM_CKD_DIV1;
    TIM_TimeBaseStructure.TIM_CounterMode       = TIM_CounterMode_Up;
    TIM_TimeBaseInit(SERVO_TIM, &TIM_TimeBaseStructure);

    TIM_OCInitStructure.TIM_OCMode      = TIM_OCMode_PWM1;
    TIM_OCInitStructure.TIM_OutputState = TIM_OutputState_Enable;
    TIM_OCInitStructure.TIM_Pulse       = SERVO_CLOSED_US;
    TIM_OCInitStructure.TIM_OCPolarity  = TIM_OCPolarity_High;
    TIM_OC1Init(SERVO_TIM, &TIM_OCInitStructure);
    TIM_OC1PreloadConfig(SERVO_TIM, TIM_OCPreload_Enable);

    TIM_ARRPreloadConfig(SERVO_TIM, ENABLE);
    TIM_Cmd(SERVO_TIM, ENABLE);

    /* Power up pointing at "closed", holding it, so the door cannot drift while
       the rest of the firmware is still initialising. */
    s_dir         = MOTOR_DIR_STOP;
    s_pulseNow    = SERVO_CLOSED_US;
    s_pulseTarget = SERVO_CLOSED_US;
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
        s_pulseTarget = SERVO_OPEN_US;
    }
    else if (dir == MOTOR_DIR_CLOSE)
    {
        s_dir         = MOTOR_DIR_CLOSE;
        s_pulseTarget = SERVO_CLOSED_US;
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
    /* Identical to Stop here, and that is not a shortcut: a servo holds its
       position whenever it is powered, so there is no separate braking state to
       enter. The API keeps the call for the builds that do have one. */
    Motor_Stop();
    s_dir = MOTOR_DIR_BRAKE;
}

void Motor_EmergencyStop(void)
{
    /* Freeze at the pulse that is on the wire right now, not at the target: an
       emergency stop must not let the door finish a move it has started.
       Unlike the H-bridge builds there is NO coast state to fall back on - a servo
       with no pulses goes limp, and a door that falls open or shut by itself is
       not a safer failure. Holding is the safe state here, and the position is
       only as wrong as it already was.
       (If a pushable door is wanted instead, disabling the channel - TIM_Cmd(
       SERVO_TIM, DISABLE) - detaches the servo. That is a deliberate behaviour
       choice, not a tuning knob, so it is not the default.) */
    s_pulseTarget = s_pulseNow;
    s_dir         = MOTOR_DIR_STOP;
    emit_pulse(s_pulseNow);
}

void Motor_SetDuty(uint8_t percent)
{
    /* Accepted and ignored: an SG90's speed is fixed by the servo, not by a duty
       cycle, and this driver's only speed control is the slew step. The callers
       keep calling it because they are shared with the motor builds; SPEED=<%> is
       answered with "n/a" on this branch so nobody is told a number was applied. */
    (void)percent;
}

uint8_t Motor_GetDuty(void)
{
    /* Report travel as a percentage so the status path stays meaningful. */
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
        uint16_t next = (uint16_t)(s_pulseNow + SLEW_STEP_US);

        s_pulseNow = (next > s_pulseTarget) ? s_pulseTarget : next;
    }
    else
    {
        uint16_t next = (uint16_t)(s_pulseNow - SLEW_STEP_US);

        s_pulseNow = (next < s_pulseTarget) ? s_pulseTarget : next;
    }

    emit_pulse(s_pulseNow);
}
