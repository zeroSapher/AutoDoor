/**
  ******************************************************************************
  * @file    Display_Oled.c
  * @brief   SSD1306 backend for the display abstraction - the real board.
  *
  * Thin adapter: the OLED driver already owns a framebuffer and does the I2C
  * work, so this file only translates the backend contract into OLED_* calls.
  * Keeping it thin is the point - all the screen logic lives in Display.c and is
  * therefore shared with the simulation backend.
  ******************************************************************************
  */

#include "Display_Backend.h"
#include "OLED.h"

void DispBk_Init(void)
{
    OLED_Init();
}

uint8_t DispBk_IsPresent(void)
{
    return OLED_IsConnected();
}

void DispBk_Clear(void)
{
    OLED_Clear();
}

void DispBk_Flush(void)
{
    OLED_Update();
}

void DispBk_Text(int16_t x, int16_t y, const char *text, uint8_t font)
{
    if (text == 0)
    {
        return;
    }

    /* Display.c uses DISPBK_FONT_LARGE/SMALL, which map onto the two font sizes
       the OLED driver provides. */
    OLED_ShowString(x, y, text, (font == DISPBK_FONT_LARGE) ? OLED_8X16 : OLED_6X8);
}

void DispBk_HLine(int16_t x0, int16_t x1, int16_t y)
{
    OLED_DrawLine(x0, y, x1, y);
}

int16_t DispBk_Width(void)
{
    return 128;
}

int16_t DispBk_Height(void)
{
    return 64;
}
