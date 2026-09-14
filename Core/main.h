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
/*  HD44780 1602 character LCD - SIMULATION BUILD ONLY                       */
/*===========================================================================*/
/*
 * Proteus has no SSD1306 model, so the simulation build (AUTODOOR_SIM_BUILD)
 * drives a 1602 character LCD instead. App/Display.c selects its backend at
 * compile time and everything above it is unchanged, which is exactly what the
 * display abstraction in Display.h exists for.
 *
 * WIRING - THE SIMULATION MOVES, THE BOARD DOES NOT
 * -------------------------------------------------
 * A 4-bit HD44780 needs 6 pins, and the real board has none to spare. The 1602
 * used to claim PB10..PB15, which forced two awkward declarations: an I2C/LCD
 * overlap special case in tools/gen-hardware.py, and a note in the bring-up
 * documentation about pins that meant different things per build.
 *
 * That was the wrong side of the trade. The 1602 exists only inside a Proteus
 * simulation - it has no physical constraint at all - whereas PB12/PB13 are the
 * only free EXTI lines suitable for the presence sensors. So the simulation's
 * display is what moved, onto the six port A pins that are free in BOTH builds:
 *
 *     PA2, PA3, PA6, PA7, PA8, PA15
 *
 * The overlap special case is gone with it, and PB10/PB11 (I2C) and PB12/PB13
 * (sensors) now mean the same thing in both builds.
 *
 * Consequence: the simulation still cannot exercise the I2C bus or the EEPROM.
 * Those are real-hardware-only features; see docs/Proteus仿真方案.md.
 */
#define LCD1602_RCC             RCC_APB2Periph_GPIOA
#define LCD1602_PORT            GPIOA
#define LCD1602_RS_PIN          GPIO_Pin_2
#define LCD1602_EN_PIN          GPIO_Pin_3
#define LCD1602_D4_PIN          GPIO_Pin_6
#define LCD1602_D5_PIN          GPIO_Pin_7
#define LCD1602_D6_PIN          GPIO_Pin_8
#define LCD1602_D7_PIN          GPIO_Pin_15
#define LCD1602_COLS            16U
#define LCD1602_ROWS            2U

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
/*  BENCH MODE: run without limit switches fitted                            */
/*===========================================================================*/
/*
 * Default 0 = real limit switches. Override from the command line:
 *
 *     .\tools\build.ps1 -NoLimits -Flash
 *
 * Set to 1, Hardware/Limit.c stops touching PA0/PA1 and instead SIMULATES the
 * door position from the motor direction and elapsed travel, finishing in
 * DOOR_SIM_TRAVEL_MS. Everything above it - the state machine, both settle paths,
 * the travel watchdog, the auto-close countdown, the reversal net - runs
 * unchanged, which is the only reason a bench test is worth anything.
 *
 * DELETING THE LIMIT CODE WAS THE OBVIOUS READING AND IT DOES NOT WORK: if the
 * limit reads simply report "not at limit", the state machine never settles, and
 * every move ends in FAULT_OPEN_TIMEOUT / FAULT_CLOSE_TIMEOUT after
 * DOOR_TRAVEL_TIMEOUT_MS. The door works for five seconds and then latches a
 * fault, which tests nothing. Substituting the feedback is what makes a rig
 * without switches testable at all.
 *
 * WHAT THIS MODE REMOVES - it is not a smaller version of the real thing:
 *   - the limit EXTI handlers are unreachable, so the software fast-stop layer
 *     does not exist. The ONLY thing between a runaway motor and the mechanism
 *     is the NC contact in the +5V -> VM path, and that is hardware: it still
 *     works, but only if it is actually wired;
 *   - a shorted or broken limit line cannot be detected (Limit_IsFaulted() is
 *     forced healthy);
 *   - the simulated travel time is a guess, so "arrived" and the watchdog margins
 *     are not the real ones.
 *
 * Never ship a build with this set. The boot banner, STATUS?, and
 * docs/上电调试步骤.md all say so.
 */
#ifndef AUTODOOR_NO_LIMITS
#define AUTODOOR_NO_LIMITS      0
#endif

/* End-to-end simulated travel, in milliseconds. Must stay comfortably below
   DOOR_TRAVEL_TIMEOUT_MS or the watchdog wins the race and faults every move. */
#define DOOR_SIM_TRAVEL_MS      2000U

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
/*  Motor - L9110S dual H-bridge                                             */
/*===========================================================================*/
/*
 * The L9110S has no enable pin: direction and speed are both encoded on the two
 * input pins, so PWM has to be applied to IA/IB themselves.
 *
 *   IA  IB   result
 *    0   0   stop    - both low-side drivers off, motor coasts
 *    0   1   forward
 *    1   0   reverse
 *    1   1   brake   - both low-side drivers on, motor is shorted
 *
 * Because PWM drives the direction pins, a direction change MUST be separated by
 * a dead time with IA=IB=0. Otherwise the bridge can be asked to reverse while
 * current is still flowing, and the resulting transient is the classic way to
 * destroy an H-bridge.
 *
 * Software PWM is used (see App/timer notes) at 1 kHz: a 20 kHz software PWM
 * would need an interrupt every 20 us, which is 1440 CPU cycles at 72 MHz -
 * not enough headroom at 72 MHz once the state machine is also running. 1 kHz
 * is far above any mechanical time constant of a model door.
 */
#define MOTOR_RCC               RCC_APB2Periph_GPIOA
#define MOTOR_IA_PORT           GPIOA
#define MOTOR_IA_PIN            GPIO_Pin_4
#define MOTOR_IB_PORT           GPIOA
#define MOTOR_IB_PIN            GPIO_Pin_5

#define MOTOR_PWM_FREQ_HZ       1000U   /* software PWM carrier */
#define MOTOR_PWM_LEVELS        100U    /* duty resolution: 1 % steps */
#define MOTOR_DEADTIME_MS       20U     /* IA=IB=0 guard on every reversal */
#define MOTOR_RAMP_UP_MS        400U    /* 0 -> target duty, avoids inrush */
#define MOTOR_RAMP_DOWN_MS      200U
#define MOTOR_DEFAULT_DUTY      70U     /* percent for normal travel */
#define MOTOR_MIN_DUTY          25U     /* below this a 130 motor will not turn */

/*===========================================================================*/
/*  Door behaviour                                                           */
/*===========================================================================*/
#define DOOR_AUTO_CLOSE_MS      5000U   /* default hold time, persisted in EEPROM */
#define DOOR_AUTO_CLOSE_MIN_MS  1000U
#define DOOR_AUTO_CLOSE_MAX_MS  30000U
/* Travel watchdog: if a limit is not reached in this time the mechanism has
   jammed or a limit switch has failed - stop and report rather than stall the
   motor against a hard stop indefinitely. */
#define DOOR_TRAVEL_TIMEOUT_MS  5000U
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
