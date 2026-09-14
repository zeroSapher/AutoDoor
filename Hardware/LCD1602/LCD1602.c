/**
  ******************************************************************************
  * @file    LCD1602.c
  * @brief   HD44780 1602 character LCD, 4-bit parallel interface.
  *
  * WHY THE DELAYS ARE NOT NEGOTIABLE
  * ---------------------------------
  * The HD44780 is a slow, old part: `Clear display` and `Return home` take about
  * 1.5 ms, and an ordinary command takes ~40 us. The busy flag can be polled
  * instead, but that needs the data bus to be readable, which the 4-bit wiring
  * here does not provide (RW is tied to ground). Fixed delays are therefore used,
  * sized to the datasheet maximum. Skipping them produces a display that works on
  * a fast simulator and fails on real silicon - a classic and very confusing bug.
  *
  * All delays come from Delay_us()/Delay_ms(), so Delay_Init() must have run.
  ******************************************************************************
  */

#include "LCD1602.h"
#include "main.h"
#include "Delay.h"

/*===========================================================================*/
/*  Timing (datasheet maxima, with margin)                                   */
/*===========================================================================*/

#define LCD_EN_PULSE_US     2U      /* EN high time, min 450 ns        */
#define LCD_CMD_DELAY_US    50U     /* ordinary command, max 37 us     */
#define LCD_LONG_DELAY_MS   2U      /* clear / home, max 1.52 ms       */

/*===========================================================================*/
/*  Pin macros                                                               */
/*===========================================================================*/

#define LCD_RS_HIGH()   GPIO_SetBits(LCD1602_PORT, LCD1602_RS_PIN)
#define LCD_RS_LOW()    GPIO_ResetBits(LCD1602_PORT, LCD1602_RS_PIN)
#define LCD_EN_HIGH()   GPIO_SetBits(LCD1602_PORT, LCD1602_EN_PIN)
#define LCD_EN_LOW()    GPIO_ResetBits(LCD1602_PORT, LCD1602_EN_PIN)

/*===========================================================================*/
/*  Low level                                                                */
/*===========================================================================*/

/** Drive the four data lines from the low nibble of `nibble`. */
static void lcd_put_nibble(uint8_t nibble)
{
    if ((nibble & 0x01U) != 0U) { GPIO_SetBits(LCD1602_PORT, LCD1602_D4_PIN); }
    else                        { GPIO_ResetBits(LCD1602_PORT, LCD1602_D4_PIN); }

    if ((nibble & 0x02U) != 0U) { GPIO_SetBits(LCD1602_PORT, LCD1602_D5_PIN); }
    else                        { GPIO_ResetBits(LCD1602_PORT, LCD1602_D5_PIN); }

    if ((nibble & 0x04U) != 0U) { GPIO_SetBits(LCD1602_PORT, LCD1602_D6_PIN); }
    else                        { GPIO_ResetBits(LCD1602_PORT, LCD1602_D6_PIN); }

    if ((nibble & 0x08U) != 0U) { GPIO_SetBits(LCD1602_PORT, LCD1602_D7_PIN); }
    else                        { GPIO_ResetBits(LCD1602_PORT, LCD1602_D7_PIN); }
}

/**
  * @brief  Clock one nibble into the controller.
  * @note   The HD44780 latches on the FALLING edge of EN, so the data must be
  *         stable while EN is high and held briefly after it falls.
  */
static void lcd_pulse_en(void)
{
    LCD_EN_HIGH();
    Delay_us(LCD_EN_PULSE_US);
    LCD_EN_LOW();
    Delay_us(LCD_EN_PULSE_US);
}

/** Send one byte as two nibbles, high first. */
static void lcd_write_byte(uint8_t value, uint8_t isData)
{
    if (isData != 0U)
    {
        LCD_RS_HIGH();
    }
    else
    {
        LCD_RS_LOW();
    }

    lcd_put_nibble((uint8_t)((value >> 4) & 0x0FU));
    lcd_pulse_en();

    lcd_put_nibble((uint8_t)(value & 0x0FU));
    lcd_pulse_en();

    Delay_us(LCD_CMD_DELAY_US);
}

static void lcd_command(uint8_t cmd)
{
    lcd_write_byte(cmd, 0U);
}

static void lcd_data(uint8_t data)
{
    lcd_write_byte(data, 1U);
}

/*===========================================================================*/
/*  Public API                                                               */
/*===========================================================================*/

void LCD1602_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;

    RCC_APB2PeriphClockCmd(LCD1602_RCC, ENABLE);

    GPIO_InitStructure.GPIO_Pin = LCD1602_RS_PIN | LCD1602_EN_PIN |
                                  LCD1602_D4_PIN | LCD1602_D5_PIN |
                                  LCD1602_D6_PIN | LCD1602_D7_PIN;
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(LCD1602_PORT, &GPIO_InitStructure);

    LCD_RS_LOW();
    LCD_EN_LOW();

    /* Power-on settle: the controller needs >15 ms after VCC rises before it
       accepts commands, and >40 ms in total before the first one. */
    Delay_ms(50U);

    /*
     * The documented 4-bit wake-up sequence. It deliberately sends 0x03 three
     * times while still in 8-bit mode: this is what forces a controller that may
     * already be in 4-bit mode (after a warm reset) back to a known state. Any
     * attempt to "optimise" this by sending 0x02 first produces an intermittent
     * blank display.
     */
    lcd_put_nibble(0x03U); lcd_pulse_en(); Delay_ms(5U);
    lcd_put_nibble(0x03U); lcd_pulse_en(); Delay_us(150U);
    lcd_put_nibble(0x03U); lcd_pulse_en(); Delay_us(150U);

    /* Switch to 4-bit operation. */
    lcd_put_nibble(0x02U); lcd_pulse_en(); Delay_us(150U);

    /* Function set: 4-bit bus, 2 lines, 5x8 font. */
    lcd_command(0x28U);

    /* Display off, then on with no cursor: a blinking cursor on a status panel
       is just noise. */
    lcd_command(0x08U);
    lcd_command(0x0CU);

    /* Entry mode: increment address, no display shift. */
    lcd_command(0x06U);

    LCD1602_Clear();
}

void LCD1602_Clear(void)
{
    lcd_command(0x01U);
    Delay_ms(LCD_LONG_DELAY_MS);
}

void LCD1602_WriteString(uint8_t row, uint8_t col, const char *text)
{
    /* Row addresses are not contiguous in HD44780 RAM: row 0 starts at 0x00 and
       row 1 at 0x40. Writing row 1 as 0x10 would wrap into the tail of row 0. */
    uint8_t addr = (uint8_t)(((row == 0U) ? 0x00U : 0x40U) + col);

    if ((text == 0) || (row >= LCD1602_ROWS) || (col >= LCD1602_COLS))
    {
        return;
    }

    lcd_command((uint8_t)(0x80U | addr));

    while ((*text != '\0') && (col < LCD1602_COLS))
    {
        lcd_data((uint8_t)(*text));
        text++;
        col++;
    }
}
