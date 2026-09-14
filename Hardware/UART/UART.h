/**
  ******************************************************************************
  * @file    UART.h
  * @brief   USART1 serial driver: interrupt-driven RX ring buffer, blocking TX.
  *
  * Design notes:
  *   - Reception is interrupt driven into a ring buffer, so no incoming byte is
  *     lost while the application is busy drawing to the OLED or sampling the
  *     DHT11 (which blocks for ~20 ms per read).
  *   - Transmission is blocking and length-based. Blocking TX keeps the code
  *     simple and makes the printf retarget below trivially safe; at 115200
  *     baud a 64-byte line costs well under a millisecond.
  *   - The ring buffer size must be a power of two (see UART_RX_BUFFER_SIZE in
  *     Core/main.h): indices are wrapped with a mask instead of a modulo.
  ******************************************************************************
  */

#ifndef __UART_H
#define __UART_H

#include "stm32f10x.h"
#include <stdint.h>

/** Initialise USART1 (PA9 = TX, PA10 = RX) and install the RX interrupt. */
void UART_Init(void);

/**
  * @brief  Buffer one received byte.
  * @note   Called from USART1_IRQHandler() in Core/stm32f10x_it.c after the
  *         RXNE flag has been tested. It reads the data register, which is what
  *         clears RXNE, so do not call it outside the interrupt.
  */
void UART_IrqHandler(void);

/* ---- Transmit ------------------------------------------------------------ */

void UART_SendByte(uint8_t byte);
void UART_SendBytes(const uint8_t *data, uint16_t len);
void UART_SendString(const char *str);

/**
  * @brief  printf-style transmission.
  * @note   Output is truncated to an internal 64-byte buffer, so long strings
  *         must be sent in pieces. Only integer conversions work unless the C
  *         library is built with floating-point printf support.
  */
void UART_Printf(const char *format, ...);

/** Blocking TX helpers - convenient for human-readable console output. */
void UART_SendLine(void);                       /**< send "\r\n"               */
void UART_NewLine(void);                        /**< alias of UART_SendLine()  */

/* ---- Receive ------------------------------------------------------------- */

/** @return Number of bytes currently waiting in the RX ring buffer. */
uint16_t UART_Available(void);

/** @return 1 when at least one byte is waiting, 0 otherwise. */
uint8_t  UART_DataAvailable(void);

/**
  * @brief  Pop one byte from the RX ring buffer.
  * @param  byte  Receives the byte.
  * @return 1 when a byte was returned, 0 when the buffer was empty.
  * @note   Callers that must not consume data should use UART_Peek().
  */
uint8_t  UART_ReadByte(uint8_t *byte);

/** @return The oldest byte without removing it, or 0 when the buffer is empty. */
uint8_t  UART_Peek(void);

/** Discard every buffered byte. */
void     UART_Flush(void);

/**
  * @brief  Collect bytes up to and including a line terminator.
  * @param  buffer  Destination, always NUL terminated on success.
  * @param  maxLen  Size of buffer, including the terminator.
  * @return Number of bytes stored (excluding the terminator), or 0 when no
  *         complete line is available yet. The line terminator is not stored.
  */
uint16_t UART_ReadLine(char *buffer, uint16_t maxLen);

/* ---- Tick ---------------------------------------------------------------- */

/**
  * @brief  Call from the main loop; drains the RX buffer and echoes commands.
  * @note   This is a demonstration consumer. Replace it with application logic
  *         once you start using the ring buffer directly.
  */
void UART_Process(void);

#endif /* __UART_H */
