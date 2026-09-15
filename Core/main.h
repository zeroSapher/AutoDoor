/**
  ******************************************************************************
  * @file    main.h
  * @brief   AutoDoor - project-wide configuration: clock, pin map, parameters.
  *
  * This is the single place where the hardware wiring is described. Every
  * peripheral pin is a macro here; the drivers in Hardware/ contain no literal
  * pin numbers, so retargeting the board means editing this file only.
  *
  * EXTI CONSTRAINT (why the pin map is frozen before layout)
  * --------------------------------------------------------
  * On STM32F1 the external interrupt lines are shared per pin NUMBER: PA0,
  * PB0 and PC0 all map to EXTI0, and only one of them can be an interrupt
  * source at a time. This system needs eight independent edge interrupts
  * (2 limit switches + 2 simulated presence sensors + 4 keys), so those eight
  * pins MUST sit on eight different numbers. Changing one number can force a
  * cascade of rewiring - fix the map first, wire second.
  ******************************************************************************
  */

#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32f10x.h"
#include <stdint.h>

/*===========================================================================*/
/*  Clock                                                                    */
/*===========================================================================*/
/*
 * STM32F103C8T6: 64 KB flash, 20 KB SRAM, Cortex-M3 up to 72 MHz.
 * system_stm32f10x.c calls SetSysClockTo72() from SystemInit(), so SYSCLK is
 * 72 MHz from the 8 MHz crystal. Delay_Init() re-derives SystemCoreClock at
 * runtime, so timing stays correct if the crystal is absent (8 MHz HSI).
 */
#define SYSCLK_FREQ_HZ          72000000U

/*===========================================================================*/
/*  On-board LED (Blue Pill style, active low on PC13)                        */
/*===========================================================================*/
#define LED_RCC                 RCC_APB2Periph_GPIOC
#define LED_PORT                GPIOC
#define LED_PIN                 GPIO_Pin_13

/*===========================================================================*/
/*  USART                                                                     */
/*===========================================================================*/
/*
 * USART1 on PA9 (TX) / PA10 (RX) - the standard programming/debug header, so
 * the wiring matches the on-board USB-serial adapter of most F103 boards.
 *
 * USART1 itself is fixed: Core/stm32f10x_it.c defines USART1_IRQHandler and
 * UART.c drives the USART1 registers and the APB2 USART1 clock.
 */
#define UART_BAUDRATE           115200U
#define UART_RX_BUFFER_SIZE     128U    /* must be a power of two */
/*
 * Working buffer for one UART_Printf() call. It must fit the longest formatted
 * line the firmware prints, terminator included.
 *
 * This used to be 64, which is smaller than several lines the firmware actually
 * emits: the boot line that reported the position used to format to 69
 * characters ("Limits : *** SIMULATED *** (open=0 closed=1, 2000ms travel)"; its
 * replacement, "Position : timed estimate, ...", is shorter, but the buffer must
 * fit whatever the longest line happens to be). vsnprintf() truncated it to 63,
 * which cut the "\r\n" too - so the next line was appended to it and the boot log
 * showed a door-state event glued onto the middle of that report. Nothing was lost
 * on the wire and no ISR was involved; the whole artefact was this buffer being
 * too small. The truncation handler in UART_Printf() now makes a recurrence
 * obvious instead of silent, and the longest line the firmware can print (the
 * "BOTH ASSERTED" wiring-fault warning, ~71 characters) fits with room to spare.
 */
#define UART_PRINTF_BUFFER_SIZE 128U
/*
 * Assembled-line buffer owned by UART.c, NOT by the caller of UART_ReadLine().
 *
 * A command line does not arrive atomically: the USB-serial bridge hands the
 * bytes to the USART one interrupt at a time, and the main loop is fast enough
 * that it calls UART_ReadLine() several times in the middle of a nine-byte
 * burst. The partial line therefore has to survive from one call to the next.
 * It is deliberately larger than CMD_LINE_MAX so that UART.c stays independent
 * of the application layer: an over-long line is saturated here and reported to
 * the caller truncated, which the command dispatcher answers with UNKNOWN_CMD
 * instead of staying silent forever.
 */
#define UART_LINE_BUFFER_SIZE   64U
/*
 * Transmit is buffered too, and this size is the headroom that keeps the door
 * state machine responsive during a long output burst.
 *
 * UART_SendByte() used to busy-wait on TXE with no buffer at all, which is fine
 * for a 60-byte status line and catastrophic for a log dump: 252 records is
 * ~23 KB, i.e. ~2 s of blocked main loop, during which Door_Update() and
 * poll_inputs() never run. With a TX ring plus the chunked dump in Cmd.c the
 * producer normally never waits, and the 1 ms tick keeps running throughout.
 */
#define UART_TX_BUFFER_SIZE     256U    /* must be a power of two */

/*
 * Naming: UART_RCC / UART_PORT, not UART_GPIO_*. Every other peripheral in this
 * file uses "<SIGNAL>_PORT" beside "<SIGNAL>_PIN" (LIMIT_OPEN_PORT beside
 * LIMIT_OPEN_PIN), so the stem of the port and the pin agree and the pairing is
 * obvious. tools/gen-hardware.py relies on that agreement to match a pin macro
 * to its port, and the "_GPIO_" form broke it - the tool reported UART_TX/RX as
 * unusable rather than silently guessing. Keeping the convention is cheaper than
 * teaching every tool about an exception.
 */
#define UART_RCC                RCC_APB2Periph_GPIOA
#define UART_PORT               GPIOA
#define UART_TX_PIN             GPIO_Pin_9
#define UART_RX_PIN             GPIO_Pin_10

/*===========================================================================*/
/*  Shared software I2C bus - SSD1306 OLED + AT24C32 EEPROM                  */
/*===========================================================================*/
/*
 * Deliberately NOT PB6/PB7 (the hardware I2C1 pins) as in the earlier DHT11
 * project: those numbers are needed for keys, whose EXTI channels would
 * otherwise collide with other peripherals. PB10/PB11 are free, adjacent, and
 * still usable as hardware I2C2 later if the software bus ever becomes a
 * bottleneck.
 */
#define MYI2C_GPIO_RCC          RCC_APB2Periph_GPIOB
#define MYI2C_SCL_PORT          GPIOB
#define MYI2C_SCL_PIN           GPIO_Pin_10
#define MYI2C_SDA_PORT          GPIOB
#define MYI2C_SDA_PIN           GPIO_Pin_11
#define MYI2C_DELAY_US          5U      /* ~50 kHz SCL; see MyI2C.h          */

/*===========================================================================*/
/*  OLED (SSD1306, 0.96" 128x64, I2C)                                        */
/*===========================================================================*/
#define OLED_I2C_ADDRESS        0x78U   /* 7-bit 0x3C shifted left */

/*===========================================================================*/
/*  AT24C32 EEPROM (I2C)                                                     */
/*===========================================================================*/
#define EEPROM_I2C_ADDRESS      0xA0U   /* 7-bit 0x50 shifted left */
#define EEPROM_PAGE_SIZE        32U     /* bytes per write page (page boundary!) */
#define EEPROM_WRITE_TIMEOUT_MS 20U     /* internal write is ~5 ms; generous */
#define EEPROM_SIZE_BYTES       4096U   /* AT24C32 = 32 kbit */

/*===========================================================================*/
/*  Limit switches - SAFETY CRITICAL                                         */
/*===========================================================================*/
/*
 * KW12-3 micro switches, NC (normally closed) contacts, wired in series in the
 * 5 V -> L9110S VM motor supply path. A closed door and an open door each hold
 * one switch actuated, so "input high" means "the switch has tripped".
 *
 * The NC contact is what makes the hardware layer fail-safe: a broken wire, a
 * loose connector or a dead switch all read as "tripped", so the failure mode
 * is "motor stops", never "motor keeps driving into the end stop".
 *
 * These two are EXTI0/EXTI1 and get the highest interrupt priority: a limit
 * event means "stop now or the mechanism breaks", which must beat the presence
 * sensors' "I would like the door to open".
 */
#define LIMIT_RCC               RCC_APB2Periph_GPIOA
#define LIMIT_OPEN_PORT         GPIOA
#define LIMIT_OPEN_PIN          GPIO_Pin_0    /* EXTI0 - door at 90 degrees  */
#define LIMIT_CLOSE_PORT        GPIOA
#define LIMIT_CLOSE_PIN         GPIO_Pin_1    /* EXTI1 - door fully closed   */

/* Debounce window for a mechanical micro switch, in milliseconds. Contacts
   bounce for 5-20 ms; 25 ms is comfortably past the worst case while still
   feeling instant to a user. */
#define LIMIT_DEBOUNCE_MS       25U

/*===========================================================================*/
/*  POSITION FEEDBACK: no limit switches fitted (timed estimate)                            */
/*===========================================================================*/
/*
 * LIMIT SWITCHES ARE NOT FITTED. This is the shipping configuration, not a bench
 * workaround: the design was changed to derive the door position from a
 * calibrated travel time instead of end stops, so PA0/PA1 are left unconnected
 * and unconfigured. Build with -WithLimits to get the optional variant back.
 *
 * Hardware/Limit.c therefore touches no pin; it estimates the position from the
 * motor direction and elapsed travel, reaching 100 % after DOOR_TRAVEL_MS.
 * Everything above it - the state machine, both settle paths, the travel
 * watchdog, the auto-close countdown, the reversal net - runs unchanged, because
 * the point of SUBSTITUTING the feedback rather than deleting it is that the rest
 * of the firmware cannot tell the difference.
 *
 * DELETING THE FEEDBACK WAS THE OBVIOUS READING AND IT DOES NOT WORK: if the
 * limit reads simply reported "not at limit", the state machine would never
 * settle and every move would end in FAULT_OPEN_TIMEOUT / FAULT_CLOSE_TIMEOUT
 * after DOOR_TRAVEL_TIMEOUT_MS - the door would work for a few seconds and then
 * latch a fault. An estimate is what makes a door with no end stops usable.
 *
 * WHAT IS GIVEN UP BY NOT FITTING SWITCHES - say it here, not in a footnote:
 *   - the limit EXTI handlers are unreachable, so there is no software fast-stop
 *     layer, and A MECHANICAL JAM IS NOT DETECTED AT ALL. The estimate advances
 *     whenever the motor is commanded, whatever the door is actually doing, so a
 *     blocked door reaches 100 % on schedule and the firmware declares it open.
 *     The motor is still stopped at the calibrated time - it cannot stall for
 *     ever - but the reported state is then a lie and nothing raises a fault;
 *   - the travel watchdog does NOT cover that case. It only fires while the state
 *     is still travelling, which is what happens when the DRIVE stops: then
 *     Motor_GetDir() no longer advances the estimate and the move never
 *     completes. It is a "the move never finished" detector, not a stall detector,
 *     and DOOR_TRAVEL_MARGIN_MS is only the tolerance for the estimate overrunning
 *     its calibration;
 *   - a broken or shorted limit line cannot be detected (Limit_IsFaulted() is
 *     forced healthy);
 *   - there is no absolute position reference and no correction. DOOR_TRAVEL_MS
 *     must be calibrated to the real door and must err LONG, because reporting
 *     arrival early stops the door short of its end stop; erring long means the
 *     mechanism runs against that end stop for the difference;
 *   - a door powered off mid-travel comes back believing whatever the last
 *     commanded move implied.
 */
#ifndef AUTODOOR_NO_LIMITS
#define AUTODOOR_NO_LIMITS      1
#endif

/* Full travel, in milliseconds. On this branch it is NOT a guess to be calibrated:
   Hardware/Motor/Motor.c derives the servo slew step from it, so the door takes
   this long by construction and the position estimate is exact. */
#define DOOR_TRAVEL_MS          1000U

/*===========================================================================*/
/*  Presence sensors simulated by keys on this branch                       */
/*===========================================================================*/
/*
 * This branch does not use a real presence sensor. Two panel switches on PB12
 * and PB13 simulate the outside and inside detection events.
 *
 * The application still consumes semantic sensor events, so the simulation
 * input can later be replaced by an active-low optical sensor without changing
 * the door state machine.
 *
 * PIN CHOICE - this is an EXTI decision, not a convenience one.
 *
 * These were PB0/PB1, which put them on EXTI0/EXTI1 - the same two lines the
 * limit switches use, because on STM32F1 a line belongs to a pin NUMBER, not to
 * a physical pin. AFIO can route only one port to a line, so the two modules
 * fought over it and the one that initialised last won: Sensor_Init() runs after
 * Limit_Init(), so the sensors took EXTI0/EXTI1 at priority 2 and the limit
 * switches were left with NO interrupt at all - silently, since every caller
 * discards Exti_ConfigPin()'s result.
 *
 * PB12/PB13 are on lines 12 and 13, which nothing else uses, so both limits and
 * both sensors now have their own line. They share the EXTI15_10 vector, which
 * is why both are configured with the same priority - one vector has one
 * priority, and giving them different values would just mean the last call wins.
 *
 * Rejected alternatives, recorded so this is not re-litigated:
 *   PA2/PA3  - lines 2/3 are free, but those pins are USART2, reserved for the
 *              HC-05. Taking them would silently cost that option later.
 *   PB2      - line 2 is free, but PB2 is BOOT1 and carries a board pull-down
 *              that fights the internal pull-up.
 *   PB9      - line 9 shares the EXTI9_5 vector with the four keys, which sit at
 *              priority 3; one vector cannot hold both priorities.
 */
#define SENSOR_RCC              RCC_APB2Periph_GPIOB
#define SENSOR_OUT_PORT         GPIOB
#define SENSOR_OUT_PIN          GPIO_Pin_12   /* KEY-simulated outside detection */
#define SENSOR_IN_PORT          GPIOB
#define SENSOR_IN_PIN           GPIO_Pin_13   /* KEY-simulated inside detection  */
#define SENSOR_DEBOUNCE_MS      20U

/*===========================================================================*/
/*  Keys                                                                     */
/*===========================================================================*/
#define KEY_RCC                 RCC_APB2Periph_GPIOB
#define KEY1_PORT               GPIOB          /* start / stop the system      */
#define KEY1_PIN                GPIO_Pin_5     /* EXTI5                        */
#define KEY2_PORT               GPIOB          /* toggle AUTO / MANUAL         */
#define KEY2_PIN                GPIO_Pin_6     /* EXTI6                        */
#define KEY3_PORT               GPIOB          /* manual OPEN                  */
#define KEY3_PIN                GPIO_Pin_7     /* EXTI7                        */
#define KEY4_PORT               GPIOB          /* manual CLOSE, long = E-STOP  */
#define KEY4_PIN                GPIO_Pin_8     /* EXTI8                        */
#define KEY_DEBOUNCE_MS         20U

/* A key must be held this long to count as a long press (emergency stop). */
#define KEY_LONGPRESS_MS        1500U

/*===========================================================================*/
/*  EXTI LINE UNIQUENESS - checked, not merely documented                    */
/*===========================================================================*/
/*
 * The rule at the top of this file ("those eight pins MUST sit on eight
 * different numbers") was written down and then violated for several rounds:
 * PA0/PB0 and PA1/PB1 both claimed lines 0 and 1, and because AFIO routes
 * exactly one port per line, Sensor_Init() - running after Limit_Init() - took
 * both lines away from the limit switches. The limit ISR became unreachable and
 * nothing anywhere said so.
 *
 * A rule that only exists in a comment has already failed once. This is the
 * check: the sum of the pins equals their bitwise OR if and only if no two of
 * them share a bit position. A collision makes the typedef's array size
 * negative, which is a compile error naming the problem.
 *
 * Every input that calls Exti_ConfigPin() must appear in both expressions below.
 * Adding a ninth EXTI input without adding it here will not be caught - but a
 * grep for "Exti_ConfigPin" finds every call site, and that is the audit.
 */
#define EXTI_PIN_SUM  ((uint32_t)LIMIT_OPEN_PIN  + (uint32_t)LIMIT_CLOSE_PIN + \
                       (uint32_t)SENSOR_OUT_PIN  + (uint32_t)SENSOR_IN_PIN   + \
                       (uint32_t)KEY1_PIN + (uint32_t)KEY2_PIN + \
                       (uint32_t)KEY3_PIN + (uint32_t)KEY4_PIN)
#define EXTI_PIN_OR   ((uint32_t)LIMIT_OPEN_PIN  | (uint32_t)LIMIT_CLOSE_PIN | \
                       (uint32_t)SENSOR_OUT_PIN  | (uint32_t)SENSOR_IN_PIN   | \
                       (uint32_t)KEY1_PIN | (uint32_t)KEY2_PIN | \
                       (uint32_t)KEY3_PIN | (uint32_t)KEY4_PIN)

typedef char exti_inputs_must_occupy_distinct_line_numbers[
    (EXTI_PIN_SUM == EXTI_PIN_OR) ? 1 : -1];

/*===========================================================================*/
/*  Buzzer and status LED                                                    */
/*===========================================================================*/
/*
 * The parts list has an ACTIVE buzzer (built-in oscillator, driven by a
 * transistor), so a plain GPIO high/low is all that is needed - no PWM. That
 * costs one pin instead of two and removes a timer requirement.
 *
 * PB3/PB4 are JTAG pins on reset (JTDO / NJTRST), so main() must disable JTAG
 * - but NOT SWD - before configuring them. See APP_JtagDisable().
 */
#define BUZZER_RCC              RCC_APB2Periph_GPIOB
#define BUZZER_PORT             GPIOB
#define BUZZER_PIN              GPIO_Pin_3
#define BUZZER_ACTIVE_HIGH      1U

#define STATUS_LED_RCC          RCC_APB2Periph_GPIOB
#define STATUS_LED_PORT         GPIOB
#define STATUS_LED_PIN          GPIO_Pin_4

/*===========================================================================*/
/*  Motor - SG90 servo on TIM3_CH1 (hardware PWM)                            */
/*===========================================================================*/
/*
 * SG90 micro servo. It is a POSITION actuator, not a motor: a 50 Hz pulse train
 * encodes an angle, the servo drives to it and then holds it against load. There
 * is no direction pair, no duty cycle and no coast - see Hardware/Motor/Motor.c
 * for how that maps onto the existing Motor_* API, and why the slew is derived
 * from DOOR_TRAVEL_MS rather than being tuned by hand.
 *
 *   pulse SERVO_CLOSED_US -> door closed
 *   pulse SERVO_OPEN_US   -> door open
 *
 * PA6 is TIM3_CH1. TIM3 is clocked at 72 MHz, so PSC gives a 1 us tick and
 * ARR = SERVO_PERIOD_US - 1 a 20 ms period, which makes CCR1 the pulse width in
 * microseconds. Do not move this pin without also moving the timer channel.
 *
 * POWER: an SG90 stalls at several hundred milliamps and MUST NOT be fed from the
 * MCU's 3V3 rail or from the ST-Link's 5 V pin - give it its own 5 V supply and a
 * common ground, or the board browns out and resets mid-move.
 */
#define SERVO_RCC               RCC_APB2Periph_GPIOA
#define SERVO_TIM_RCC           RCC_APB1Periph_TIM3
#define SERVO_PORT              GPIOA
#define SERVO_PIN               GPIO_Pin_6
#define SERVO_TIM               TIM3
#define SERVO_TIMER_HZ          72000000U   /* APB1 timers run at HCLK here */
#define SERVO_PERIOD_US         20000U      /* 50 Hz */

/* Pulse widths. 1000..2000 us is a 90 degree swing, which is all a door needs and
   stays clear of the servo's mechanical stops; pushing past ~2400 us makes an
   SG90 buzz against its end stop instead of moving further. */
#define SERVO_CLOSED_US         1000U
#define SERVO_OPEN_US           2000U

/* The DC-motor builds' carrier macro, kept because the shared code and the docs
   reference it; on this branch it documents the servo frame rate, not a carrier.
   MOTOR_DEFAULT_DUTY / MOTOR_MIN_DUTY / MOTOR_DUTY_MIN / MOTOR_DUTY_MAX are
   defined further down and are unused here - Motor_SetDuty() ignores them. */
#define MOTOR_PWM_FREQ_HZ       50U
/* Dead time and ramps belong to the H-bridge builds: a servo input is a logic
   signal with no shoot-through risk, and its soft start IS the slew. */
#define MOTOR_DEADTIME_MS       0U
#define MOTOR_DEFAULT_DUTY      70U     /* percent for normal travel */
#define MOTOR_MIN_DUTY          25U     /* below this a 130 motor will not turn */
/*
 * Range accepted by the SPEED=<%> console command. The floor is MOTOR_MIN_DUTY
 * rather than something lower on purpose: Motor_Run() raises any duty target
 * below MOTOR_MIN_DUTY back up to it, so accepting a smaller number here would
 * mean the command reports a duty the motor never actually gets.
 */
#define MOTOR_DUTY_MIN          MOTOR_MIN_DUTY
#define MOTOR_DUTY_MAX          100U

/*===========================================================================*/
/*  Door behaviour                                                           */
/*===========================================================================*/
#define DOOR_AUTO_CLOSE_MS      5000U   /* default hold time, persisted in EEPROM */
#define DOOR_AUTO_CLOSE_MIN_MS  1000U
#define DOOR_AUTO_CLOSE_MAX_MS  30000U
/* Travel watchdog: if the door has not arrived in this time the mechanism has
   jammed or the feedback has failed - stop and report rather than stall the
   motor against a hard stop indefinitely. */
#if AUTODOOR_NO_LIMITS
/* The timed estimate IS the arrival detector here, so the watchdog is derived
   from it and only adds a margin: how much longer than DOOR_TRAVEL_MS a move may
   still count as "travelling" before the firmware gives up and latches a fault.
   It is NOT a stall allowance - a mechanically jammed door still reaches 100 % on
   schedule and is declared arrived, so read the note above before assuming this
   number protects the mechanism. */
#define DOOR_TRAVEL_MARGIN_MS   1200U
#define DOOR_TRAVEL_TIMEOUT_MS  (DOOR_TRAVEL_MS + DOOR_TRAVEL_MARGIN_MS)
#else
/* With switches fitted the switch is what detects arrival, so this is a generous
   backstop that only has to catch a jam or a switch that never closes. */
#define DOOR_TRAVEL_TIMEOUT_MS  5000U
#endif
/* How long the door keeps re-opening while people are still detected. */
#define DOOR_REOPEN_EXTEND_MS   2000U

/*===========================================================================*/
/*  Event log (RAM queue backed by EEPROM)                                   */
/*===========================================================================*/
#define LOG_SLOT_COUNT          252U    /* (4096 - 32) / 16 */
#define LOG_HEADER_SIZE         32U
#define LOG_ENTRY_SIZE          16U
/* Writing to EEPROM blocks the main loop for a few ms per page, so events are
   queued in RAM and flushed in batches. A power cut can lose at most the last
   few hundred milliseconds of entries, which is an accepted trade for never
   blocking the limit-switch path. */
#define LOG_FLUSH_BATCH         8U
#define LOG_FLUSH_INTERVAL_MS   500U

#ifdef __cplusplus
}
#endif

/*===========================================================================*/
/*  Small system helpers                                                     */
/*===========================================================================*/

/**
  * @brief  Release PB3/PB4 (and PA15) from the JTAG controller.
  * @note   These pins come up as JTDO / NJTRST / JTDI, so the buzzer and status
  *         LED cannot be driven until JTAG is disabled. Must run BEFORE any GPIO
  *         is configured, and it disables JTAG only, never SWD - calling
  *         GPIO_Remap_SWJ_Disable here would make the target undownloadable.
  */
void App_JtagDisable(void);

/** @brief Latch a fatal condition: motor off, interrupts as they are, spin. */
void App_Panic(void);

#endif /* __MAIN_H */
