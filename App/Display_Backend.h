/**
  ******************************************************************************
  * @file    Display_Backend.h
  * @brief   Private contract between the display logic and a panel driver.
  *
  * Display.h promises callers a hardware-independent vocabulary. This header is
  * the OTHER half of that promise: what a panel driver must provide. Keeping it
  * separate from Display.h means application code cannot accidentally depend on
  * primitives that only one panel happens to offer.
  *
  * The SSD1306-compatible OLED backend is used by both real and simulation
  * builds. The simulation changes storage only, not the display contract.
  ******************************************************************************
  */

#ifndef __DISPLAY_BACKEND_H
#define __DISPLAY_BACKEND_H

#include <stdint.h>

/** Bring up the panel. May be called before Delay_Init() is not allowed. */
void DispBk_Init(void);

/** @return 1 when the panel answered; a missing panel must not be fatal. */
uint8_t DispBk_IsPresent(void);

/** Blank the whole panel. */
void DispBk_Clear(void);

/** Flush any buffered drawing to the panel. */
void DispBk_Flush(void);

/* ---- Drawing primitives -------------------------------------------------- */
/* Coordinates are in pixels for the OLED backend. The character backend scales
 * internally: it maps y/8 to a row and x/6 to a column. Callers only ever pass
 * values produced by the layout constants in Display.c, which are chosen to fall
 * on whole character cells. */

#define DISPBK_FONT_SMALL   6U      /* 6x8  */
#define DISPBK_FONT_LARGE   8U      /* 8x16 */

void DispBk_Text(int16_t x, int16_t y, const char *text, uint8_t font);

/** Horizontal rule, used as the title underline. */
void DispBk_HLine(int16_t x0, int16_t x1, int16_t y);

/** @return Usable width in pixels, so the layout can adapt. */
int16_t DispBk_Width(void);

/** @return Usable height in pixels. */
int16_t DispBk_Height(void);

#endif /* __DISPLAY_BACKEND_H */
