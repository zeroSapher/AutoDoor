/**
  ******************************************************************************
  * @file    main.c
  * @brief   AutoDoor - STM32F103C8T6 simple automatic door controller.
  *
  * Bring-up order matters and is deliberate:
  *
  *   1. App_JtagDisable()  - PB3/PB4 are JTAG pins until this runs; the buzzer
  *                           cannot be driven before it.
  *   2. NVIC grouping      - fixed before any interrupt is enabled, so the
  *                           priority numbers below mean what they say.
  *   3. Delay_Init()       - SysTick starts here, so every *_Tick1ms() begins
  *                           being called from this point on.
  *   4. Motor_Init()       - release the bridge before anything energises it.
  *   5. inputs             - latch the true pin state before arming interrupts,
  *                           otherwise power-on can look like a limit event.
  *   6. I2C + EEPROM + Log - persistent configuration is read here.
  *   7. Door_Init()        - last, because it reads the limit levels and the
  *                           persisted mode/delay to decide where the door is.
  *
  * WIRING (see Core/main.h for the macros)
  *   OLED SCL/SDA  PB10/PB11      EEPROM shares the same bus
  *   open limit    PA0   (EXTI0)  close limit  PA1   (EXTI1)
  *   sensor out    PB12  (EXTI12) sensor in    PB13  (EXTI13)
  *   keys          PB5..PB8 (EXTI5..8)
  *   buzzer        PB3            motor IA/IB  PA4/PA5 -> L9110S
  *   console       PA9/PA10 (USART1, 115200 8N1)
  *
  * Those eight EXTI inputs sit on eight DISTINCT pin numbers, which Core/main.h
  * enforces at compile time and Exti_ConfigPin() at run time. It is not a style
  * preference: AFIO routes one port per line, so two inputs on one number means
  * the later initialiser takes the line and the earlier one silently has no
  * interrupt. The sensors were on PB0/PB1 until round 11, and the limit switches
  * had no interrupt at all because of it.
  ******************************************************************************
  */

#include "main.h"
#include "Delay.h"
#include "UART.h"
#include "MyI2C.h"
#include "Display.h"
#include "EEPROM.h"
#include "Motor.h"
#include "Limit.h"
#include "Sensor.h"
#include "Key.h"
#include "Exti.h"       /* Exti_ConflictCount() - the boot-time EXTI warning */
#include "Buzzer.h"
#include "StatusLed.h"
#include "Door.h"
#include "Log.h"
#include "Cmd.h"

/*===========================================================================*/
/*  System helpers                                                           */
/*===========================================================================*/

void App_JtagDisable(void)
{
    /* Only JTAG, never SWD: GPIO_Remap_SWJ_Disable would also kill SWDIO/SWCLK
       and the board could not be programmed or debugged again. */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_AFIO, ENABLE);
    GPIO_PinRemapConfig(GPIO_Remap_SWJ_JTAGDisable, ENABLE);
}

void App_Panic(void)
{
    Motor_EmergencyStop();

    for (;;)
    {
    }
}

/*===========================================================================*/
/*  Door callbacks - the only place where the state machine, the buzzer, the
  *  console and the log are wired to each other. Keeping this in one spot means
  *  Door.c stays free of any dependency on reporting or storage.                */
/*===========================================================================*/

static void on_door_event(uint8_t event, DoorState_t state, DoorMode_t mode,
                          uint16_t durationMs)
{
    Log_Add((LogEvent_t)event, (uint8_t)state, (uint8_t)mode, durationMs);
    Cmd_ReportEvent(event, state, mode, durationMs);
    Display_PushEvent(event, g_msTick);
}

static void on_door_state(DoorState_t from, DoorState_t to)
{
    Cmd_ReportState(from, to);
}

static void on_door_fault(DoorFault_t fault)
{
    Cmd_ReportFault(fault);
}

static void on_door_sound(uint8_t pattern)
{
    Buzzer_Play((BuzzerPattern_t)pattern);
}

/*===========================================================================*/
/*  Periodic tasks                                                           */
/*===========================================================================*/

/**
  * @brief  Copy debounced input edges into the state machine.
  * @note   The state machine is driven by explicit notifications rather than
  *         reaching into the input modules itself, which keeps the causal chain
  *         readable: an input module detects, this function translates, Door.c
  *         decides.
  */
static void poll_inputs(void)
{
    /*
     * Raw limit edges are reported but not logged here. The state machine reacts
     * to the limit LEVEL, which is what also handles powering up already sitting
     * on a limit - and it emits the meaningful OPEN_DONE / CLOSE_DONE events, so
     * logging the raw edge as well would just duplicate them.
     *
     * They are still printed, because a limit edge with no accompanying arrival
     * is exactly the signature of a bouncing or intermittent switch, and that is
     * worth being able to see on the console.
     */
    if (Limit_TakeOpenEvent() != 0U)
    {
        UART_Printf("[%lu] LIMIT open edge (state=%s)\r\n",
                    (unsigned long)g_msTick, Door_StateName(Door_GetState()));
    }

    if (Limit_TakeCloseEvent() != 0U)
    {
        UART_Printf("[%lu] LIMIT close edge (state=%s)\r\n",
                    (unsigned long)g_msTick, Door_StateName(Door_GetState()));
    }

    if (Sensor_TakeOutsideEvent() != 0U)
    {
        Door_NotifyOutsideSensor();
    }

    if (Sensor_TakeInsideEvent() != 0U)
    {
        Door_NotifyInsideSensor();
    }

    if (Key_TakeShortPress(KEY_ID_START) != 0U)
    {
        Door_KeyStartStop();
    }

    if (Key_TakeShortPress(KEY_ID_MODE) != 0U)
    {
        Door_KeyToggleMode();
    }

    /*
     * Screen navigation is offered on the same keys the door uses, so
     * Display_HandleKey() decides whether a press is a navigation action or a
     * door command. It only claims KEY3/KEY4 when the log screen is open in
     * manual mode with the door disabled - see the note in Display.h. A manual
     * open command must never be swallowed because a menu happens to be open.
     */
    if (Key_TakeShortPress(KEY_ID_OPEN) != 0U)
    {
        if (Display_HandleKey(1U, 0U) == 0U)
        {
            Door_KeyManualOpen();
        }
    }

    if (Key_TakeShortPress(KEY_ID_CLOSE_ESTOP) != 0U)
    {
        if (Display_HandleKey(0U, 0U) == 0U)
        {
            Door_KeyManualClose();
        }
    }

    /* Long press of KEY2 cycles screens; long press of KEY3/KEY4 leaves the log
       view. Neither conflicts with the door, which uses only short presses on
       these keys. */
    if (Key_TakeLongPress(KEY_ID_MODE) != 0U)
    {
        Display_NextScreen();
    }

    if (Key_TakeLongPress(KEY_ID_OPEN) != 0U)
    {
        (void)Display_HandleKey(1U, 1U);
    }

    /* Long press of KEY4 is the emergency stop, and it acts immediately rather
       than being deferred to Door_Update(). Checked last so the gesture can
       never be intercepted by navigation. */
    if (Key_TakeLongPress(KEY_ID_CLOSE_ESTOP) != 0U)
    {
        Door_EmergencyStop();
    }
}

/*===========================================================================*/
/*  Entry point                                                              */
/*===========================================================================*/

int main(void)
{
    DoorHooks_t hooks;
    uint16_t    savedDelay;
    DoorMode_t  savedMode;
    uint8_t     eepromOk;
    uint8_t     logOk;
    uint16_t    eepromFailAddr = 0U;
    uint32_t    nextStatusMs;
    uint32_t    nextDisplayMs;

    /* ---- 1. Release PB3 from JTAG before any GPIO is configured --------- */
    App_JtagDisable();

    /*
     * All four preemption levels are used (0 = limits, 1 = system/console,
     * 2 = sensors, 3 = keys), so the grouping must give the full 4 bits to
     * preemption. Set explicitly: relying on the reset default would mean a
     * later priority tweak could be silently reinterpreted as a sub-priority
     * and quietly destroy the safety ordering.
     */
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4);

    /* ---- 2. Time bases: SysTick 1 ms + microsecond counter --------------- */
    Delay_Init();

    /* ---- 3. Motor off, bridge released ---------------------------------- */
    Motor_Init();

    /* ---- 4. Inputs: latch real levels, then arm the interrupts ---------- */
    Limit_Init();
    Limit_Reset();
    Sensor_Init();
    Sensor_Reset();
    Key_Init();
    Key_Reset();

    /* ---- 5. Indicators -------------------------------------------------- */
    Buzzer_Init();
    StatusLed_Init();

    /* ---- 6. Console ----------------------------------------------------- */
    UART_Init();

    UART_SendLine();
    UART_SendString("========================================\r\n");
    UART_SendString(" AutoDoor  STM32F103C8T6  v1.0\r\n");
    UART_SendString("========================================\r\n");

    /*
     * Report EXTI line conflicts. The input modules are initialised before the
     * console exists, so Exti_ConfigPin() cannot print; it counts instead. A
     * non-zero count means two inputs share a pin NUMBER, which on STM32F1 means
     * the later one took the line and the earlier one has NO interrupt - silently,
     * because AFIO gives no error. That is exactly how the limit switches lost
     * their interrupt. This line is the only runtime warning of it; the cheaper
     * guard is the compile-time check in Core/main.h, so seeing this message means
     * the pin map was changed without that check being updated too.
     */
    if (Exti_ConflictCount() != 0U)
    {
        UART_Printf("EXTI          : %u LINE CONFLICT(S) - an input has no interrupt!\r\n",
                    (unsigned)Exti_ConflictCount());
        UART_SendString("                (two inputs share a pin number; see Core/main.h)\r\n");
    }

    /* ---- 7. I2C bus and devices ----------------------------------------- */
    MyI2C_Init();
    UART_Printf("I2C idle      : %s\r\n",
                (MyI2C_IsIdle() != 0U) ? "yes" : "NO (bus held low)");

    Display_Init();
    UART_Printf("Display       : %s\r\n",
                (Display_IsPresent() != 0U) ? "OK" : "not found");

    eepromOk = (EEPROM_SelfTest(&eepromFailAddr) == MYI2C_OK) ? 1U : 0U;
    if (eepromOk != 0U)
    {
        UART_Printf("EEPROM @0x%02X  : OK (%u bytes)\r\n",
                    (unsigned)EEPROM_I2C_ADDRESS, (unsigned)EEPROM_SIZE_BYTES);
    }
    else
    {
        UART_Printf("EEPROM @0x%02X  : FAILED at 0x%04X\r\n",
                    (unsigned)EEPROM_I2C_ADDRESS, (unsigned)eepromFailAddr);
    }

    /* ---- 8. Persistent configuration ------------------------------------ */
    logOk = (Log_Init() == 0U) ? 1U : 0U;

    if (logOk != 0U)
    {
        savedDelay = Log_GetAutoCloseMs();
        savedMode  = (Log_GetPersistedMode() == 0U) ? DOOR_MODE_AUTO : DOOR_MODE_MANUAL;

        UART_Printf("Log           : boot#%u, %u/%u records, delay=%ums, mode=%s\r\n",
                    (unsigned)Log_GetBootId(),
                    (unsigned)Log_Count(), (unsigned)LOG_SLOT_COUNT,
                    (unsigned)savedDelay,
                    (savedMode == DOOR_MODE_AUTO) ? "AUTO" : "MANUAL");
    }
    else
    {
        /* No persistent storage: the door still works, it just forgets. Falling
           back to defaults is far better than refusing to run. */
        savedDelay = DOOR_AUTO_CLOSE_MS;
        savedMode  = DOOR_MODE_AUTO;
        UART_SendString("Log           : UNAVAILABLE - running with defaults\r\n");
    }

    /* ---- 9. Limit levels as latched at start-up ------------------------- */
    UART_Printf("Limits        : open=%u closed=%u%s\r\n",
                (unsigned)Limit_IsOpen(), (unsigned)Limit_IsClosed(),
                (Limit_IsFaulted() != 0U) ? "  <-- BOTH ASSERTED (wiring fault)" : "");

    /* ---- 10. State machine ---------------------------------------------- */
    hooks.OnEvent       = on_door_event;
    hooks.OnStateChange = on_door_state;
    hooks.OnFault       = on_door_fault;
    hooks.OnSound       = on_door_sound;

    Door_Init(&hooks, savedMode, savedDelay);

    UART_Printf("Door          : %s (press KEY1 to enable)\r\n",
                Door_StateName(Door_GetState()));

    if (Limit_IsFaulted() != 0U)
    {
        Buzzer_Play(BUZZ_FAULT);
    }
    else
    {
        Buzzer_Play(BUZZ_READY);
    }

    Cmd_Init();

    if (logOk != 0U)
    {
        Log_Add(LOG_EVT_SYSTEM_START, (uint8_t)Door_GetState(), (uint8_t)savedMode, 0U);
    }

    nextStatusMs = g_msTick;
    nextDisplayMs = g_msTick;

    /* ---- 11. Main loop -------------------------------------------------- */
    for (;;)
    {
        poll_inputs();
        Door_Update();
        Cmd_Process();

        /*
         * Emit the next slice of a log listing. This must be its own call rather
         * than part of Cmd_Process(): a dump is served over many iterations, and
         * the whole point is that the loop keeps running between slices. See the
         * note in Cmd.h.
         */
        Cmd_ProcessLogDump();

        /*
         * Service the event log. All EEPROM traffic lives here and not in the
         * tick handler: a flush is tens of milliseconds of blocking I2C, and
         * doing it inside SysTick stalled the UART and could interleave a second
         * transaction with one already in flight in this loop. The 1 ms tick only
         * sets a request flag. It runs after Cmd_Process() so a LOG? command's
         * own flush is not repeated here, and before the panel refresh so the
         * record count shown is the one that was just persisted.
         *
         * Suppressed while a listing is being emitted: Log_Get() addresses records
         * by relative index, and a flush advances wrIndex, which would shift every
         * index the listing is walking. Events keep queueing in RAM meanwhile and
         * are written as soon as the listing finishes.
         */
        if (Cmd_LogDumpActive() == 0U)
        {
            Log_Process();
        }

        /*
         * Refresh the panel a few times a second, not every iteration. A full
         * OLED flush is ~1 KB of I2C, and the bus is shared with the EEPROM, so
         * repainting at loop speed would both waste time and add bus contention.
         * 250 ms is fast enough for a countdown and for state changes to feel
         * immediate.
         */
        if ((g_msTick - nextDisplayMs) >= 250U)
        {
            nextDisplayMs = g_msTick;

            Display_SetStatus(Door_GetState(), Door_GetMode(), Door_GetFault(),
                              Door_IsRunning(), Door_GetCloseCountdown(),
                              Log_Count());
            Display_Update();

            /* The LED tells the same story as the panel, but is readable from
               across the room. "Attention" covers both a latched fault and the
               system being deliberately disabled by KEY1, because in either case
               the door will not respond and that is what a person approaching it
               needs to know. Cheap to call: it only recomputes when the state or
               the attention flag actually changes. */
            StatusLed_Set(Door_GetState(),
                          ((Door_GetFault() != DOOR_FAULT_NONE) ||
                           (Door_IsRunning() == 0U)) ? 1U : 0U);
        }

        /* Periodic status line, so an unattended console log shows liveness and
           the current door state. */
        if ((g_msTick - nextStatusMs) >= 10000U)
        {
            nextStatusMs = g_msTick;
            Cmd_ReportStatus();
        }
    }
}
