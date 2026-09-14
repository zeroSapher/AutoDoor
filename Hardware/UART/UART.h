/**
  ******************************************************************************
  * @file    UART.h
  * @brief   USART1 serial driver: interrupt-driven RX and TX ring buffers.
  *
  * Design notes:
  *   - Both directions are interrupt driven into ring buffers.
  *   - Reception was always buffered so that no incoming byte is lost while the
  *     application is busy with the OLED or the EEPROM.
  *   - Transmission is buffered as well, but for LATENCY, not throughput: while
  *     TX was a busy-wait, printing a long listing made the caller - the main
  *     loop - spend the entire transmission spinning on TXE, so the door state
  *     machine stopped for the duration. See UART_TxFree().
  *   - Both ring sizes must be powers of two (see Core/main.h): indices are
  *     wrapped with a mask instead of a modulo.
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

/**
  * @brief  Refill the transmit register from the TX ring.
  * @note   Called from USART1_IRQHandler() when TXE is set and the TXE interrupt
  *         is enabled. Disables that interrupt once the ring is empty, so it is
  *         not entered when there is nothing to send.
  */
void UART_TxIrqHandler(void);

/* ---- Transmit ------------------------------------------------------------ */

/**
  * @brief  Queue one byte for transmission.
  * @note   Normally non-blocking. It waits only when the TX ring is completely
  *         full, i.e. for at most the time it takes the interrupt to drain
  *         UART_TX_BUFFER_SIZE bytes. Callers that emit long output should check
  *         UART_TxFree() first and defer instead of waiting.
  */
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

/** Send "\r\n" (and UART_NewLine(), an alias kept for older call sites). */
void UART_SendLine(void);
void UART_NewLine(void);

/** @return Free space in the TX ring, in bytes. */
uint16_t UART_TxFree(void);

/** @return Bytes queued but not yet handed to the shift register. */
uint16_t UART_TxPending(void);

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
