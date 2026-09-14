/**
  ******************************************************************************
  * @file    Display_Lcd.c
  * @brief   HD44780 1602 backend for the display abstraction - simulation build.
  *
  * DEGRADATION STRATEGY
  * --------------------
  * The screen logic in Display.c was designed for a 128x64 bitmap. A 1602 gives
  * 16 characters by 2 lines - roughly 8x less room - so this backend cannot
  * simply forward the primitive calls and hope. It does two things instead:
  *
  *   1. QUANTISES. The layout constants in Display.c place text at y = 0, 18, 36
  *      and 54, and at x = 0. Those fall neatly into two character rows
  *      (y < 32 -> row 0, else row 1) and start at column 0, so nothing is
  *      clipped or needs sub-character positioning.
  *   2. SHADOWS EACH ROW. Because a 1602 has no framebuffer, writing a shorter
  *      string over a longer one leaves the tail of the old text behind. Each
  *      row keeps a copy of what is currently displayed and pads with spaces, so
  *      the panel never shows stale characters.
  *
  * The application sees the same Display_SetStatus()/PushEvent()/ShowMessage()
  * calls either way. What differs is how much of it fits - which is exactly the
  * trade accepted by choosing a character panel for the simulation.
  ******************************************************************************
  */

#include "Display_Backend.h"
#include "LCD1602.h"
#include "main.h"
#include <string.h>

/* One shadow copy per row, plus a terminator. */
static char s_shadow[LCD1602_ROWS][LCD1602_COLS + 1U];
static uint8_t s_present = 0U;

/** Map a pixel y from the OLED layout onto a 1602 row. */
static uint8_t row_of(int16_t y)
{
    if (y < 32)
    {
        return 0U;
    }
    return (LCD1602_ROWS > 1U) ? 1U : 0U;
}

void DispBk_Init(void)
{
    LCD1602_Init();

    /* A 1602 on a 4-bit bus has no readable status, so presence cannot be
       probed. It is treated as present: the worst case is a few writes to
       nothing, which is harmless, whereas reporting "absent" would suppress the
       display entirely on a working simulation. */
    s_present = 1U;

    memset(s_shadow, 0, sizeof(s_shadow));
}

uint8_t DispBk_IsPresent(void)
{
    return s_present;
}

void DispBk_Clear(void)
{
    uint8_t r;

    LCD1602_Clear();

    for (r = 0U; r < LCD1602_ROWS; r++)
    {
        memset(s_shadow[r], 0, sizeof(s_shadow[r]));
    }
}

void DispBk_Flush(void)
{
    /* Every write goes straight through; nothing is buffered. */
}

void DispBk_Text(int16_t x, int16_t y, const char *text, uint8_t font)
{
    uint8_t row;
    uint8_t col = 0U;
    uint8_t i = 0U;
    char    line[LCD1602_COLS + 1U];

    (void)font;     /* a 1602 has one font; size is not selectable */

    if (text == 0)
    {
        return;
    }

    row = row_of(y);

    /* x is always 0 for the layouts this backend supports; ignore any other
       value rather than silently drawing in the wrong place. */
    if (x > 0)
    {
        col = (uint8_t)(x / 6);
    }
    if (col >= LCD1602_COLS)
    {
        return;
    }

    /* Build the full row width, padding with spaces so old text cannot remain. */    for (i = 0U; i < col; i++)
    {
        line[i] = (s_shadow[row][i] != '\0') ? s_shadow[row][i] : ' ';
    }
    while ((i < LCD1602_COLS) && (*text != '\0'))
    {
        line[i] = *text++;
        i++;
    }
    while (i < LCD1602_COLS)
    {
        line[i] = ' ';
        i++;
    }
    line[LCD1602_COLS] = '\0';

    /* Skip the bus traffic when nothing changed. A 1602 write is slow (tens of
       microseconds per character) and the status screen is redrawn several times
       a second. */
    if (strcmp(line, s_shadow[row]) == 0)
    {
        return;
    }

    memcpy(s_shadow[row], line, sizeof(s_shadow[row]));

    LCD1602_WriteString(row, 0U, line);
}

void DispBk_HLine(int16_t x0, int16_t x1, int16_t y)
{
    /* A character panel has no pixel graphics. The title underline that the OLED
       layout uses becomes a row of dashes, which reads the same way to a user. */
    uint8_t row = row_of(y);
    char    rule[LCD1602_COLS + 1U];
    uint8_t i;

    (void)x0;
    (void)x1;

    for (i = 0U; i < LCD1602_COLS; i++)
    {
        rule[i] = '-';
    }
    rule[LCD1602_COLS] = '\0';

    if (strcmp(rule, s_shadow[row]) == 0)
    {
        return;
    }

    memcpy(s_shadow[row], rule, sizeof(s_shadow[row]));

    LCD1602_WriteString(row, 0U, rule);
}

int16_t DispBk_Width(void)
{
    return (int16_t)(LCD1602_COLS * 6U);    /* 96 "pixels" at the small font */
}

int16_t DispBk_Height(void)
{
    return (int16_t)(LCD1602_ROWS * 8U);    /* 16 */
}
