/**
  ******************************************************************************
  * @file    Limit.c
  * @brief   Door position feedback: no switches fitted (timed estimate) by
  *          default, with the optional end-stop variant behind a switch.
  *
  * TWO MODES, CHOSEN AT COMPILE TIME
  * ---------------------------------
  *   AUTODOOR_NO_LIMITS == 1 (DEFAULT - this is what ships) - no end stops are
  *       fitted, so the position is estimated from the motor direction and
  *       elapsed travel. Nothing here touches a pin.
  *
  *   AUTODOOR_NO_LIMITS == 0 (build with -WithLimits) - the optional variant: NO
  *       contact readback on PA0/PA1 with debounce and EXTI edges.
  *
  * WHY A SIMULATION AND NOT A DELETION
  * -----------------------------------
  * Simply disabling the limit reads does not produce a testable door. The state
  * machine settles OPENING/CLOSING by watching the limit LEVEL, so with no
  * switches it would never settle, and every single move would end in
  * FAULT_OPEN_TIMEOUT / FAULT_CLOSE_TIMEOUT after DOOR_TRAVEL_TIMEOUT_MS. The
  * door would work for exactly five seconds and then latch a fault, which tests
  * nothing.
  *
  * So instead of removing the feedback, this mode SUBSTITUTES it: a virtual
  * position that advances while the motor runs and latches at 0 % and 100 %. The
  * whole state machine - both settle paths, the travel watchdog, the auto-close
  * countdown, the reversal net - then runs exactly as it will on the real door,
  * which is the whole reason the feedback is substituted rather than deleted.
  *
  * WHAT IS LOST (say it here, not in a footnote)
  * ---------------------------------------------
  *   - The limit EXTI handlers are never reached, so the software fast-stop layer
  *     is gone. The only thing between a runaway motor and the mechanism is the
  *     NC contact in the +5V -> VM path, and that is HARDWARE - it still works,
  *     but only if it is wired.
  *   - Limit_IsFaulted() always reports healthy, so a shorted limit line cannot
  *     be detected.
  *   - The travel time is a calibration constant, not a measurement. Report
  *     arrival early and the door stops short of its end stop; err long and the
  *     mechanism runs against that end stop for the difference.
  *
  * This is the SHIPPING configuration, so the estimate is what the door runs on:
  * the boot banner and every STATUS? line carry it, because "the position is an
  * estimate" is what tells a reader how much a timeout fault is worth.
  ******************************************************************************
  */

#include "Limit.h"
#include "Debounce.h"
#include "Exti.h"
#include "Motor.h"
#include "main.h"

/*===========================================================================*/
/*  State                                                                    */
/*===========================================================================*/

#if AUTODOOR_NO_LIMITS

/*
 * Virtual door position, 0 = fully closed .. 100 = fully open, in percent.
 * It follows the motor: whatever direction the bridge is driving, the virtual
 * door moves that way at a fixed rate.
 */
static uint8_t  s_simPos    = 0U;
static uint16_t s_simAccum  = 0U;
static uint8_t  s_simOpenEvent  = 0U;
static uint8_t  s_simCloseEvent = 0U;

/* 1 % of travel per this many milliseconds => DOOR_TRAVEL_MS end to end. */
#define SIM_MS_PER_PERCENT      (DOOR_TRAVEL_MS / 100U)

#else

static Debounce_t s_open;
static Debounce_t s_close;

#endif

/*===========================================================================*/
/*  Initialisation                                                           */
/*===========================================================================*/

void Limit_Init(void)
{
#if AUTODOOR_NO_LIMITS
    /*
     * No pin, no clock, no EXTI. The virtual door starts CLOSED, which is what a
     * rig with the motor at rest looks like, and is the same assumption the
     * state machine makes when the real close limit is held down.
     */
    s_simPos        = 0U;
    s_simAccum      = 0U;
    s_simOpenEvent  = 0U;
    s_simCloseEvent = 0U;
#else
    GPIO_InitTypeDef GPIO_InitStructure;

    RCC_APB2PeriphClockCmd(LIMIT_RCC, ENABLE);

    /*
     * WIRING - both contacts of the SPDT switch are used, on two electrically
     * separate circuits that share one mechanical actuation:
     *
     *   NC contact : in series in the +5V -> L9110S VM path. This is the SAFETY
     *                layer and it never touches the MCU - a limit removes motor
     *                power even if the firmware is hung.
     *   NO contact : COM to GND, NO to the MCU pin with a 10k pull-up to +3V3.
     *                This is the READBACK layer.
     *
     * An earlier version of the schematic notes had COM to GND and NC to the MCU,
     * which cannot work: the same NC contact cannot also carry the 5 V motor
     * supply, and having it do so would put 5 V on a pin whose absolute maximum
     * is 3.6 V. Using the NO contact for readback keeps the 5 V and 3.3 V domains
     * apart with no level shifting.
     *
     * Sense is therefore INVERTED relative to a NC readback:
     *
     *   door away from limit : NO open    -> pull-up wins -> pin HIGH
     *   door AT the limit    : NO closed  -> pulled to GND -> pin LOW
     *
     * so activeLevel is 0. The trade-off is that a broken readback wire now reads
     * "not at limit" rather than "at limit" - the state machine will sit until the
     * 5 s travel watchdog fires. The hardware power cut still protects the
     * mechanism, so this costs diagnostics, not safety.
     */
    GPIO_InitStructure.GPIO_Pin  = LIMIT_OPEN_PIN | LIMIT_CLOSE_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(LIMIT_OPEN_PORT, &GPIO_InitStructure);

    /* activeLevel = 0: the pin is pulled LOW when the door reaches the limit. */
    Debounce_Init(&s_open,  LIMIT_OPEN_PORT,  LIMIT_OPEN_PIN,  0U, LIMIT_DEBOUNCE_MS);
    Debounce_Init(&s_close, LIMIT_CLOSE_PORT, LIMIT_CLOSE_PIN, 0U, LIMIT_DEBOUNCE_MS);

    /* Preemption priority 0: above the sensors (2) and the keys (3), because a
       limit event must never wait behind a "someone approached" event. */
    (void)Exti_ConfigPin(LIMIT_OPEN_PORT,  LIMIT_OPEN_PIN,  EXTI_Trigger_Rising_Falling, 0U, 0U);
    (void)Exti_ConfigPin(LIMIT_CLOSE_PORT, LIMIT_CLOSE_PIN, EXTI_Trigger_Rising_Falling, 0U, 0U);
#endif /* AUTODOOR_NO_LIMITS */
}

void Limit_Reset(void)
{
#if AUTODOOR_NO_LIMITS
    /* "Closed" is the assumed rest position; see Limit_Init(). */
    s_simPos        = 0U;
    s_simAccum      = 0U;
    s_simOpenEvent  = 0U;
    s_simCloseEvent = 0U;
#else
    Debounce_SyncNow(&s_open);
    Debounce_SyncNow(&s_close);
#endif
}

uint8_t Limit_IrqHandler(uint8_t openEdge)
{
#if AUTODOOR_NO_LIMITS
    /* No EXTI line is claimed in this mode, so this cannot be reached. It stays
       compiled so that turning the switch off needs no other change. */
    (void)openEdge;
    return 0U;
#else
    GPIO_TypeDef *port = openEdge ? LIMIT_OPEN_PORT  : LIMIT_CLOSE_PORT;
    uint16_t      pin  = openEdge ? LIMIT_OPEN_PIN   : LIMIT_CLOSE_PIN;

    /*
     * The direction in which the motor is driving AWAY from this switch - i.e.
     * the one direction in which this edge must NOT be treated as "stop now".
     *
     *   open limit  : reached by driving OPEN,  so departing means driving CLOSE
     *   close limit : reached by driving CLOSE, so departing means driving OPEN
     */
    MotorDir_t away = openEdge ? MOTOR_DIR_CLOSE : MOTOR_DIR_OPEN;

    if (openEdge != 0U)
    {
        Debounce_IrqEdge(&s_open);
    }
    else
    {
        Debounce_IrqEdge(&s_close);
    }

    /*
     * THE DECISION LIVES HERE, NOT IN THE ISR - and that is a fix, not a
     * preference. It used to sit in stm32f10x_it.c as a bare
     * `Motor_EmergencyStop()` call, and this module's EXTI lines are configured
     * Rising_Falling, so ANY edge cut the motor. That included the RELEASE edge
     * when the door started moving away from a limit - so the door moved a few
     * millimetres off the switch, the motor was cut, nothing restarted it
     * (only begin_open/begin_close call Motor_Run), and five seconds later the
     * travel watchdog latched FAULT_*_TIMEOUT. The default build could not open
     * or close the door at all.
     *
     * The guard that got removed when the ISRs were simplified had been doing two
     * jobs, and only one of them was obvious: it disambiguated PA0 from PB0 (which
     * did become unnecessary once the pins got distinct EXTI lines) AND it
     * restricted the cut to the ASSERT edge. Removing it took the second job with
     * it. Keeping the decision next to the polarity constants it depends on makes
     * that harder to repeat: the ISR now has nothing to get wrong.
     *
     * The level test is on the RAW pin, not the debounced level - the whole point
     * of doing this in the interrupt is to act before the main loop can.
     */
    if ((GPIO_ReadInputDataBit(port, pin) == Bit_RESET) &&      /* asserted */
        (Motor_GetDir() != away))                              /* not departing */
    {
        /*
         * Cut unless the motor is deliberately driving AWAY from this switch.
         *
         * "Not departing" rather than "arriving" on purpose: with the motor
         * stopped (STOP or BRAKE) the cut is a harmless no-op, and during
         * Motor_Run()'s reversal dead time s_dir is momentarily STOP - treating
         * that as "not arriving" would open a window where a limit asserts and
         * nothing cuts. The only case excluded is the one that caused the bug:
         * driving away, where a contact bounce can dip the pin low again and
         * would otherwise re-trigger the cut for the whole departure.
         */
        Motor_EmergencyStop();
        return 1U;
    }

    return 0U;
#endif
}

void Limit_Tick1ms(void)
{
#if AUTODOOR_NO_LIMITS
    MotorDir_t dir = Motor_GetDir();

    /*
     * Track the motor rather than a timer started by a command: this way the
     * virtual door stops moving the moment the motor stops, so the simulated
     * position stays consistent with what the mechanism is actually doing. That
     * matters because the state machine's settle conditions and its travel
     * watchdog are both driven from here.
     */
    if ((dir == MOTOR_DIR_OPEN) || (dir == MOTOR_DIR_CLOSE))
    {
        s_simAccum++;

        if (s_simAccum >= SIM_MS_PER_PERCENT)
        {
            s_simAccum = 0U;

            if (dir == MOTOR_DIR_OPEN)
            {
                if (s_simPos < 100U)
                {
                    s_simPos++;

                    if (s_simPos == 100U)
                    {
                        s_simOpenEvent = 1U;    /* console still shows the edge */
                    }
                }
            }
            else
            {
                if (s_simPos > 0U)
                {
                    s_simPos--;

                    if (s_simPos == 0U)
                    {
                        s_simCloseEvent = 1U;
                    }
                }
            }
        }
    }
    else
    {
        s_simAccum = 0U;
    }
#else
    Debounce_Tick1ms(&s_open);
    Debounce_Tick1ms(&s_close);
#endif
}

/*===========================================================================*/
/*  Queries                                                                  */
/*===========================================================================*/

uint8_t Limit_TakeOpenEvent(void)
{
#if AUTODOOR_NO_LIMITS
    uint8_t evt = s_simOpenEvent;

    s_simOpenEvent = 0U;
    return evt;
#else
    return Debounce_TakeEvent(&s_open);
#endif
}

uint8_t Limit_TakeCloseEvent(void)
{
#if AUTODOOR_NO_LIMITS
    uint8_t evt = s_simCloseEvent;

    s_simCloseEvent = 0U;
    return evt;
#else
    return Debounce_TakeEvent(&s_close);
#endif
}

uint8_t Limit_IsOpen(void)
{
#if AUTODOOR_NO_LIMITS
    return (s_simPos >= 100U) ? 1U : 0U;
#else
    return Debounce_IsActive(&s_open);
#endif
}

uint8_t Limit_IsClosed(void)
{
#if AUTODOOR_NO_LIMITS
    return (s_simPos == 0U) ? 1U : 0U;
#else
    return Debounce_IsActive(&s_close);
#endif
}

uint8_t Limit_IsFaulted(void)
{
#if AUTODOOR_NO_LIMITS
    /*
     * The simulated position is one-dimensional, so it can never report both
     * ends at once. That does not mean the check is unnecessary in this mode - it
     * means the fault it detects is undetectable without real switches, which is
     * one of the costs of running without them.
     */
    return 0U;
#else
    /* Both limits asserted at once cannot happen on a real door: they are at
       opposite ends of travel. It means a shorted wire or a dead switch, and the
       state machine must refuse to drive until it is cleared. */
    return ((Debounce_IsActive(&s_open) != 0U) &&
            (Debounce_IsActive(&s_close) != 0U)) ? 1U : 0U;
#endif
}

uint8_t Limit_IsSimulated(void)
{
#if AUTODOOR_NO_LIMITS
    return 1U;
#else
    return 0U;
#endif
}
