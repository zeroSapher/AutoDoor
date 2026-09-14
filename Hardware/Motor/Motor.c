/**
  ******************************************************************************
  * @file    Motor.c
  * @brief   L9110S dual H-bridge driver: direction, speed ramp, software PWM.
  *
  * SOFTWARE PWM
  * ------------
  * The L9110S has no enable pin, so speed has to be modulated on IA/IB. A
  * 20 kHz carrier - the frequency that would keep the motor electrically quiet -
  * is not reachable in software: at 72 MHz an interrupt every 20 us leaves only
  * 1440 cycles, far too little once the door state machine shares the CPU.
  *
  * Instead the carrier runs slowly and cheaply: the 1 ms SysTick handler
  * advances a phase accumulator and drives IA/IB from it, giving a 100 Hz
  * carrier with 1 % duty resolution (see the PWM geometry note below). One
  * interrupt per millisecond costs a few dozen cycles, and 100 Hz is orders of
  * magnitude faster than any mechanical time constant of a model door, so the
  * motion is smooth. There is a faint audible hum from the motor; if that ever
  * becomes objectionable the fix is to move PWM onto TIM3 (see main.h) without
  * changing this API.
  *
  * DEAD TIME
  * ---------
  * Reversing an H-bridge while current is still flowing is the classic way to
  * destroy it. Motor_Run() therefore always inserts MOTOR_DEADTIME_MS with
  * IA=IB=0 before applying a new direction.
  ******************************************************************************
  */

#include "Motor.h"
#include "main.h"
#include "Delay.h"

/*===========================================================================*/
/*  Pin macros                                                               */
/*===========================================================================*/

#define IA_HIGH()       GPIO_SetBits(MOTOR_IA_PORT, MOTOR_IA_PIN)
#define IA_LOW()        GPIO_ResetBits(MOTOR_IA_PORT, MOTOR_IA_PIN)
#define IB_HIGH()       GPIO_SetBits(MOTOR_IB_PORT, MOTOR_IB_PIN)
#define IB_LOW()        GPIO_ResetBits(MOTOR_IB_PORT, MOTOR_IB_PIN)

/*
 * Software PWM geometry.
 *
 * One carrier cycle spans PWM_CYCLE_TICKS SysTick ticks (1 tick = 1 ms). The
 * phase accumulator advances PWM_STEP_PER_TICK per tick and wraps at
 * PWM_PHASE_MAX, so a cycle completes every
 *
 *     PWM_CYCLE_TICKS = PWM_PHASE_MAX / PWM_STEP_PER_TICK = 100 / 10 = 10 ms
 *
 * i.e. a 100 Hz carrier, and the output is high for `duty` of the 100 phase
 * steps => 1 % duty resolution, controllable to a 1 ms granularity.
 *
 * 100 Hz is low for a motor carrier - it is audible and it puts a little torque
 * ripple on the shaft - but it is comfortably above the mechanical time constant
 * of a model door, and it costs one interrupt per millisecond. That trade is
 * deliberate: hardware PWM at 20 kHz would need TIM3 and a pin remap, and the
 * objective is a quiet, working mechanism rather than a silent one.
 */
#define PWM_PHASE_MAX           100U    /* duty resolution == 100 percent   */
#define PWM_STEP_PER_TICK       10U     /* => 10 ms per carrier cycle       */

/*
 * Ramp rate, in duty percent per millisecond. 100 % over MOTOR_RAMP_UP_MS
 * gives 100/400 = 0.25 %/ms, which is below 1 so the ramp accumulates
 * fractional progress in "permille of a percent" instead of stalling.
 * RAMP_STEP_INTERVAL is how many ms between single-percent increments.
 */
#define RAMP_UP_INTERVAL_MS     (MOTOR_RAMP_UP_MS / 100U)     /* 4 ms  */
#define RAMP_DOWN_INTERVAL_MS   (MOTOR_RAMP_DOWN_MS / 100U)   /* 2 ms  */

/*===========================================================================*/
/*  Internal state                                                           */
/*===========================================================================*/

static volatile MotorDir_t s_dir       = MOTOR_DIR_STOP;
static volatile uint8_t    s_dutyNow   = 0U;   /* applied duty, percent   */
static volatile uint8_t    s_dutyTarget = 0U;  /* requested duty, percent */
static volatile uint8_t    s_phase     = 0U;   /* software PWM phase      */
static uint8_t             s_rampAccum = 0U;   /* ms accumulator for ramp */
static uint8_t             s_initialised = 0U;

/*===========================================================================*/
/*  Helpers                                                                  */
/*===========================================================================*/

/**
  * @brief  Drive IA/IB for a direction, applying the current duty.
  * @param  dir    Direction to encode.
  * @param  output 1 to energise, 0 to release both outputs.
  * @note   Only called with a direction that is not STOP/BRAKE when output=1.
  */
static void apply_output(MotorDir_t dir, uint8_t output)
{
    if (output == 0U)
    {
        IA_LOW();
        IB_LOW();
        return;
    }

    switch (dir)
    {
        case MOTOR_DIR_OPEN:
            IA_LOW();
            IB_HIGH();
            break;

        case MOTOR_DIR_CLOSE:
            IA_HIGH();
            IB_LOW();
            break;

        case MOTOR_DIR_BRAKE:
            IA_HIGH();
            IB_HIGH();
            break;

        case MOTOR_DIR_STOP:
        default:
            IA_LOW();
            IB_LOW();
            break;
    }
}

/** Release both bridge inputs (coast). */
static void outputs_release(void)
{
    IA_LOW();
    IB_LOW();
}

/*===========================================================================*/
/*  Public API                                                               */
/*===========================================================================*/

void Motor_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;

    RCC_APB2PeriphClockCmd(MOTOR_RCC, ENABLE);

    /* Release the bridge before enabling the outputs so power-up cannot kick
       the motor. */
    IA_LOW();
    IB_LOW();

    GPIO_InitStructure.GPIO_Pin   = MOTOR_IA_PIN | MOTOR_IB_PIN;
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(MOTOR_IA_PORT, &GPIO_InitStructure);

    s_dir         = MOTOR_DIR_STOP;
    s_dutyNow     = 0U;
    s_dutyTarget  = 0U;
    s_phase       = 0U;
    s_rampAccum   = 0U;
    s_initialised = 1U;
}

void Motor_EmergencyStop(void)
{
    /* No ramp, no delay block: this runs from the limit/estop interrupt. */
    outputs_release();

    s_dir        = MOTOR_DIR_STOP;
    s_dutyNow    = 0U;
    s_dutyTarget = 0U;
    s_phase      = 0U;
    s_rampAccum  = 0U;
}

void Motor_Brake(void)
{
    s_dir        = MOTOR_DIR_BRAKE;
    s_dutyTarget = 0U;          /* no modulation: both low sides stay on */
    s_dutyNow    = 0U;
    apply_output(MOTOR_DIR_BRAKE, 1U);
}

void Motor_Stop(void)
{
    /* Ramp down rather than cutting, unless already slow enough to just stop.
       Blocking here is acceptable because every caller is in the main loop. */
    if (s_dutyNow >= 5U)
    {
        s_dutyTarget = 0U;
        while (s_dutyNow != 0U)
        {
            Delay_ms(1U);       /* lets Motor_Tick1ms() run the ramp down */
        }
    }

    outputs_release();
    s_dir       = MOTOR_DIR_STOP;
    s_dutyNow   = 0U;
    s_dutyTarget = 0U;
    s_phase     = 0U;
}

void Motor_Run(MotorDir_t dir)
{
    if ((dir != MOTOR_DIR_OPEN) && (dir != MOTOR_DIR_CLOSE))
    {
        Motor_Stop();
        return;
    }

    /* Dead time on any direction change - including from a stop, which is
       cheap and keeps the rule unconditional and easy to audit. */
    if (dir != s_dir)
    {
        outputs_release();
        s_dir       = MOTOR_DIR_STOP;
        s_dutyNow   = 0U;
        s_dutyTarget = 0U;
        Delay_ms(MOTOR_DEADTIME_MS);
    }

    s_dir = dir;

    if (s_dutyTarget < MOTOR_MIN_DUTY)
    {
        s_dutyTarget = MOTOR_MIN_DUTY;
    }

    /* Start from zero so the ramp always runs; this is what limits inrush on a
       stalled 130 motor, whose locked-rotor current is several times rated. */
    s_dutyNow   = 0U;
    s_rampAccum = 0U;
}

void Motor_SetDuty(uint8_t percent)
{
    if (percent > 100U)
    {
        percent = 100U;
    }
    s_dutyTarget = percent;
}

uint8_t Motor_GetDuty(void)
{
    return s_dutyNow;
}

MotorDir_t Motor_GetDir(void)
{
    return s_dir;
}

uint8_t Motor_IsRamping(void)
{
    return (s_dutyNow != s_dutyTarget) ? 1U : 0U;
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

    /* ---- 1. Speed ramp ---------------------------------------------------- */
    if (s_dutyNow != s_dutyTarget)
    {
        s_rampAccum++;

        if (s_dutyNow < s_dutyTarget)
        {
            if (s_rampAccum >= RAMP_UP_INTERVAL_MS)
            {
                s_rampAccum = 0U;
                s_dutyNow++;
            }
        }
        else
        {
            if (s_rampAccum >= RAMP_DOWN_INTERVAL_MS)
            {
                s_rampAccum = 0U;
                s_dutyNow--;
            }
        }
    }
    else
    {
        s_rampAccum = 0U;
    }

    /* ---- 2. Software PWM carrier ----------------------------------------- */
    if ((s_dir != MOTOR_DIR_OPEN) && (s_dir != MOTOR_DIR_CLOSE))
    {
        /* Idle or braking: nothing to modulate. */
        s_phase = 0U;
        return;
    }

    s_phase = (uint8_t)(s_phase + PWM_STEP_PER_TICK);
    if (s_phase >= PWM_PHASE_MAX)
    {
        s_phase = 0U;
    }

    apply_output(s_dir, (s_phase < s_dutyNow) ? 1U : 0U);
}
