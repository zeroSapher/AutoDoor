/**
  ******************************************************************************
  * @file    Motor.c
  * @brief   TB6612FNG driver: direction, speed ramp, software PWM, standby.
  *
  * PIN ARRANGEMENT
  * ---------------
  * The TB6612 takes the direction on two static inputs and the speed on a
  * separate PWM input, so unlike the L9110S this firmware used to drive, the
  * carrier never touches a direction pin. Reversing therefore cannot chatter the
  * direction inputs, and the dead time below only has to let the current decay.
  *
  * SOFTWARE PWM
  * ------------
  * A 20 kHz carrier - the frequency that would keep the motor electrically quiet -
  * is not reachable in software: at 72 MHz an interrupt every 20 us leaves only
  * 1440 cycles, far too little once the door state machine shares the CPU.
  *
  * Instead the carrier runs slowly and cheaply: the 1 ms SysTick handler advances
  * a phase accumulator and drives PWMA from it, giving a 100 Hz carrier with 1 %
  * duty resolution (see the PWM geometry note below). One interrupt per
  * millisecond costs a few dozen cycles, and 100 Hz is orders of magnitude faster
  * than any mechanical time constant of a model door, so the motion is smooth.
  * There is a faint audible hum from the motor; if that ever becomes
  * objectionable the fix is to move PWMA onto TIM3_CH3 (it is already on PB0 for
  * exactly that reason) without changing this API.
  *
  * DEAD TIME
  * ---------
  * Reversing an H-bridge while current is still flowing is the classic way to
  * destroy it. Motor_Run() therefore always inserts MOTOR_DEADTIME_MS with the
  * outputs released before applying a new direction.
  ******************************************************************************
  */

#include "Motor.h"
#include "main.h"
#include "Delay.h"

/*===========================================================================*/
/*  Pin macros                                                               */
/*===========================================================================*/

#define PWM_HIGH()      GPIO_SetBits(MOTOR_PWM_PORT, MOTOR_PWM_PIN)
#define PWM_LOW()       GPIO_ResetBits(MOTOR_PWM_PORT, MOTOR_PWM_PIN)
#define STBY_HIGH()     GPIO_SetBits(MOTOR_STBY_PORT, MOTOR_STBY_PIN)
#define STBY_LOW()      GPIO_ResetBits(MOTOR_STBY_PORT, MOTOR_STBY_PIN)

/**
  * @brief  Write both direction inputs in ONE store.
  *
  * They live on the same port, so BSRR can change both atomically. Writing them
  * one after the other is what let a pin check catch the intermediate
  * combination: turning from CLOSE (AIN1=0, AIN2=1) to OPEN sets AIN1 first, so
  * for the few nanoseconds between the two stores the bridge sees 1/1 - the brake
  * state. Brake is legal and the glitch is far too short to matter mechanically,
  * but the whole point of this part is that the direction inputs are STATIC for a
  * move, and one store is what makes that actually true.
  */
static void write_direction(uint8_t ain1High, uint8_t ain2High)
{
    uint32_t bsrr;

    bsrr  = (ain1High != 0U) ? (uint32_t)MOTOR_AIN1_PIN
                             : ((uint32_t)MOTOR_AIN1_PIN << 16U);
    bsrr |= (ain2High != 0U) ? (uint32_t)MOTOR_AIN2_PIN
                             : ((uint32_t)MOTOR_AIN2_PIN << 16U);

    MOTOR_AIN1_PORT->BSRR = bsrr;
}

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
    /*
     * The direction pins are written on every call, including during the low half
     * of the carrier cycle. They are static for the whole move, so this costs
     * nothing and leaves them defined instead of depending on what the previous
     * call happened to leave behind.
     */
    switch (dir)
    {
        case MOTOR_DIR_OPEN:
            write_direction(1U, 0U);
            break;

        case MOTOR_DIR_CLOSE:
            write_direction(0U, 1U);
            break;

        case MOTOR_DIR_BRAKE:
            write_direction(1U, 1U);
            break;

        case MOTOR_DIR_STOP:
        default:
            write_direction(0U, 0U);
            break;
    }

    /* The carrier: high = the direction above is driven, low = not driven. */
    if (output != 0U)
    {
        PWM_HIGH();
    }
    else
    {
        PWM_LOW();
    }
}

/** Release the bridge: direction inputs low and the carrier low (no drive). */
static void outputs_release(void)
{
    write_direction(0U, 0U);
    PWM_LOW();
}

/*===========================================================================*/
/*  Public API                                                               */
/*===========================================================================*/

void Motor_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;

    RCC_APB2PeriphClockCmd(MOTOR_RCC, ENABLE);

    /* Drive every pin to its harmless level BEFORE switching it to an output, and
       leave the driver in standby, so power-up cannot kick the motor. STBY low is
       the TB6612's own disable: the outputs stay off whatever the inputs say. */
    write_direction(0U, 0U);
    PWM_LOW();
    STBY_LOW();

    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;

    GPIO_InitStructure.GPIO_Pin = MOTOR_AIN1_PIN | MOTOR_AIN2_PIN;
    GPIO_Init(MOTOR_AIN1_PORT, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin = MOTOR_PWM_PIN | MOTOR_STBY_PIN;
    GPIO_Init(MOTOR_PWM_PORT, &GPIO_InitStructure);

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

    /* ...and drop STBY as well. This is the one place a torn write could not
       undo: with the driver in standby the outputs are off no matter what the
       direction pins were left holding. */
    STBY_LOW();

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

    /* Put the direction on the pins with the carrier still low, and only THEN
       enable the driver. Raising STBY first would let the chip drive whatever the
       direction pins happened to hold from the previous move. */
    apply_output(dir, 0U);
    STBY_HIGH();
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
    if (s_dir == MOTOR_DIR_BRAKE)
    {
        /* Braking is a DRIVEN state (both low sides on), so it is re-asserted
           rather than released. */
        apply_output(MOTOR_DIR_BRAKE, 1U);
        s_phase = 0U;
        return;
    }

    if ((s_dir != MOTOR_DIR_OPEN) && (s_dir != MOTOR_DIR_CLOSE))
    {
        /*
         * Idle: re-release the bridge on every tick instead of assuming the pins
         * are already low.
         *
         * apply_output() and outputs_release() each write IA and IB with two
         * separate stores, and this tick runs in the SysTick interrupt, so it can
         * land BETWEEN those two writes when the main loop is stopping the motor.
         * The half-written result - one side driven - would then stay on the
         * bridge forever: once s_dir is STOP, nothing else ever writes these
         * pins. A motor left energised after an emergency stop would simply keep
         * running, and the travel watchdog cannot catch it because the state is
         * no longer "travelling". That was reproduced on hardware: the bridge was
         * driven by hand, s_dir was set to STOP, and a full second of ticks left
         * the outputs exactly as they were.
         *
         * Two GPIO writes per millisecond make the stopped state self-healing.
         */
        outputs_release();
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
