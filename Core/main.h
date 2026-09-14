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
 * WIRING / PIN CONFLICT - READ THIS
 * ---------------------------------
 * A 4-bit HD44780 needs 6 pins, and this design has none free: every GPIO is
 * already committed to limits, sensors, keys, motor, console and the I2C bus.
 * The simulation build therefore CLAIMS PB10..PB15, which on the real board
 * carry the OLED/EEPROM I2C bus and the limit switches - three things the
 * simulation does not have:
 *
 *     PB10..PB13  ->  LCD D4..D7
 *     PB14        ->  LCD RS
 *     PB15        ->  LCD EN
 *
 * Consequence: the simulation build cannot exercise the I2C bus or the
 * hardware limit inputs. Those are real-hardware-only features; see
 * docs/Proteus仿真方案.md for what the simulation can and cannot show.
 */
#define LCD1602_RCC             RCC_APB2Periph_GPIOB
#define LCD1602_PORT            GPIOB
#define LCD1602_RS_PIN          GPIO_Pin_14
#define LCD1602_EN_PIN          GPIO_Pin_15
#define LCD1602_D4_PIN          GPIO_Pin_10
#define LCD1602_D5_PIN          GPIO_Pin_11
#define LCD1602_D6_PIN          GPIO_Pin_12
#define LCD1602_D7_PIN          GPIO_Pin_13
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
/*  Simulated presence sensors                                              */
/*===========================================================================*/
/*
 * The parts list has no sensor that can detect a person (the TCRT5000 in it is
 * a ~1 cm reflective line sensor, useless at door range), so presence is
 * simulated with two push buttons for now.
 *
 * This is a clean substitution rather than a throwaway hack: a button and a
 * real IR/optical sensor are both "active-low edge into an EXTI line plus
 * debounce", so swapping in an E18-D80NK later changes only these macros.
 */
#define SENSOR_RCC              RCC_APB2Periph_GPIOB
#define SENSOR_OUT_PORT         GPIOB
#define SENSOR_OUT_PIN          GPIO_Pin_0    /* EXTI0 - outside, someone entering */
#define SENSOR_IN_PORT          GPIOB
#define SENSOR_IN_PIN           GPIO_Pin_1    /* EXTI1 - inside, someone leaving   */
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
