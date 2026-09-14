/**
  ******************************************************************************
  * @file    OLED.h
  * @brief   OLED12864 / SSD1306-compatible 128x64 OLED over shared I2C.
  *
  * Public API of the JiangXie Technology (jiangxiekeji.com) open-source SSD1306
  * driver, V2.0, 2024.10.20, with the hardware layer re-targeted onto MyI2C.
  *
  * Coordinate system: the top-left pixel is (0, 0), X grows to the right
  * (0..127) and Y grows downwards (0..63).
  *
  * All drawing functions only touch the RAM framebuffer. Nothing appears on the
  * panel until OLED_Update() (or OLED_UpdateArea()) is called.
  ******************************************************************************
  */

#ifndef __OLED_H
#define __OLED_H

#include <stdint.h>
#include "OLED_Data.h"

/*===========================================================================*/
/*  Parameters                                                               */
/*===========================================================================*/

/* FontSize argument. The value doubles as the character cell width in pixels. */
#define OLED_8X16				8
#define OLED_6X8				6

/* IsFilled argument */
#define OLED_UNFILLED			0
#define OLED_FILLED				1

/*===========================================================================*/
/*  Initialisation and panel control                                         */
/*===========================================================================*/

/** Initialise the I2C bus, configure the SSD1306 and clear the panel. */
void OLED_Init(void);

/** @return 1 when the panel answered on the bus, 0 when it is absent. */
uint8_t OLED_IsConnected(void);

/*===========================================================================*/
/*  Framebuffer flush                                                        */
/*===========================================================================*/

/** Push the whole 128x64 framebuffer to the panel. */
void OLED_Update(void);

/** Push only the given rectangle to the panel - faster for small updates. */
void OLED_UpdateArea(int16_t X, int16_t Y, uint8_t Width, uint8_t Height);

/*===========================================================================*/
/*  Framebuffer control                                                      */
/*===========================================================================*/

void OLED_Clear(void);
void OLED_ClearArea(int16_t X, int16_t Y, uint8_t Width, uint8_t Height);
void OLED_Reverse(void);
void OLED_ReverseArea(int16_t X, int16_t Y, uint8_t Width, uint8_t Height);

/*===========================================================================*/
/*  Text                                                                     */
/*===========================================================================*/

void OLED_ShowChar(int16_t X, int16_t Y, char Char, uint8_t FontSize);
void OLED_ShowString(int16_t X, int16_t Y, const char *String, uint8_t FontSize);
void OLED_ShowNum(int16_t X, int16_t Y, uint32_t Number, uint8_t Length, uint8_t FontSize);
void OLED_ShowSignedNum(int16_t X, int16_t Y, int32_t Number, uint8_t Length, uint8_t FontSize);
void OLED_ShowHexNum(int16_t X, int16_t Y, uint32_t Number, uint8_t Length, uint8_t FontSize);
void OLED_ShowBinNum(int16_t X, int16_t Y, uint32_t Number, uint8_t Length, uint8_t FontSize);
void OLED_ShowFixedNum(int16_t X, int16_t Y, int32_t Scaled, uint8_t IntLength, uint8_t FraLength, uint8_t FontSize);

/**
  * @brief  printf-style text into the framebuffer.
  * @note   Only integer conversions are usable unless the C library printf is
  *         built with floating-point support (nano printf has none by default).
  */
void OLED_Printf(int16_t X, int16_t Y, uint8_t FontSize, const char *format, ...);

/*===========================================================================*/
/*  Bitmaps and drawing                                                      */
/*===========================================================================*/

void OLED_ShowImage(int16_t X, int16_t Y, uint8_t Width, uint8_t Height, const uint8_t *Image);

void OLED_DrawPoint(int16_t X, int16_t Y);
uint8_t OLED_GetPoint(int16_t X, int16_t Y);
void OLED_DrawLine(int16_t X0, int16_t Y0, int16_t X1, int16_t Y1);
void OLED_DrawRectangle(int16_t X, int16_t Y, uint8_t Width, uint8_t Height, uint8_t IsFilled);
void OLED_DrawTriangle(int16_t X0, int16_t Y0, int16_t X1, int16_t Y1, int16_t X2, int16_t Y2, uint8_t IsFilled);
void OLED_DrawCircle(int16_t X, int16_t Y, uint8_t Radius, uint8_t IsFilled);
void OLED_DrawEllipse(int16_t X, int16_t Y, uint8_t A, uint8_t B, uint8_t IsFilled);
void OLED_DrawArc(int16_t X, int16_t Y, uint8_t Radius, int16_t StartAngle, int16_t EndAngle, uint8_t IsFilled);

#endif /* __OLED_H */

/*****************江协科技|版权所有****************/
/*****************jiangxiekeji.com*****************/
