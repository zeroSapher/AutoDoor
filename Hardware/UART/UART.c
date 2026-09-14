/**
  ******************************************************************************
  * @file    UART.c
  * @brief   USART1 serial driver: interrupt-driven RX and TX ring buffers.
  ******************************************************************************
  */

#include "UART.h"
#include "main.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/*===========================================================================*/
/*  RX ring buffer                                                           */
/*===========================================================================*/

/*
 * Single producer (the USART1 interrupt) and single consumer (the main loop).
 * head is written only by the ISR, tail only by the consumer, so neither needs
 * to mask interrupts. Both counters are free-running modulo 2*SIZE, which makes
 * "how many bytes are buffered" a plain unsigned subtraction - the classic
 * no-extra-state ring buffer.
 */
#define UART_RX_MASK            (UART_RX_BUFFER_SIZE - 1U)

static volatile uint8_t  s_rxBuffer[UART_RX_BUFFER_SIZE];
static volatile uint16_t s_rxHead = 0U;     /* ISR write index   */
static volatile uint16_t s_rxTail = 0U;     /* consumer read idx */

/*===========================================================================*/
/*  TX ring buffer                                                           */
/*===========================================================================*/

/*
 * Same pattern with the roles swapped: head is written only by the main loop
 * (the producer) and tail only by the USART1 interrupt (the consumer). No
 * critical section is needed for the same reason as RX.
 *
 * The purpose is not throughput - the link is the bottleneck either way - but
 * LATENCY. A blocking TX makes every UART_SendByte() call part of the caller's
 * execution time, so whoever prints a long listing stops the door state machine
 * for the whole transmission. Buffering moves that time into the interrupt,
 * where it costs nothing that matters.
 *
 * When the ring is full the producer waits. That is deliberate: dropping console
 * output silently is worse than briefly stalling, and UART_TxFree() lets the one
 * caller that emits a lot (the log dump) avoid the wait entirely.
 */
#define UART_TX_MASK            (UART_TX_BUFFER_SIZE - 1U)

static volatile uint8_t  s_txBuffer[UART_TX_BUFFER_SIZE];
static volatile uint16_t s_txHead = 0U;     /* producer write idx */
static volatile uint16_t s_txTail = 0U;     /* ISR read index    */

/*===========================================================================*/
/*  Initialisation                                                           */
/*===========================================================================*/

void UART_Init(void)
{
    GPIO_InitTypeDef  GPIO_InitStructure;
    USART_InitTypeDef USART_InitStructure;
    NVIC_InitTypeDef  NVIC_InitStructure;

    /* Guard against a configuration mistake that would break the mask maths. */
#if (UART_RX_BUFFER_SIZE & UART_RX_MASK) != 0
#error "UART_RX_BUFFER_SIZE must be a power of two"
#endif
#if (UART_TX_BUFFER_SIZE & UART_TX_MASK) != 0
#error "UART_TX_BUFFER_SIZE must be a power of two"
#endif

    RCC_APB2PeriphClockCmd(UART_RCC | RCC_APB2Periph_USART1, ENABLE);

    /* TX: alternate function push-pull. */
    GPIO_InitStructure.GPIO_Pin   = UART_TX_PIN;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_AF_PP;
    GPIO_Init(UART_PORT, &GPIO_InitStructure);

    /* RX: floating input (the line idles high). */
    GPIO_InitStructure.GPIO_Pin  = UART_RX_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(UART_PORT, &GPIO_InitStructure);

    USART_InitStructure.USART_BaudRate            = UART_BAUDRATE;
    USART_InitStructure.USART_WordLength          = USART_WordLength_8b;
    USART_InitStructure.USART_StopBits            = USART_StopBits_1;
    USART_InitStructure.USART_Parity              = USART_Parity_No;
    USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_InitStructure.USART_Mode                = USART_Mode_Rx | USART_Mode_Tx;
    USART_Init(USART1, &USART_InitStructure);

    /* Receive-not-empty interrupt: fires once per received byte.
       TXE (transmit-register-empty) is deliberately left DISABLED here and is
       enabled by UART_SendByte() only while there is something to send, so the
       handler is never entered with an empty ring. An always-enabled TXE
       interrupt would fire continuously and eat the CPU. */
    USART_ITConfig(USART1, USART_IT_RXNE, ENABLE);

    NVIC_InitStructure.NVIC_IRQChannel                   = USART1_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 1;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority        = 1;
    NVIC_InitStructure.NVIC_IRQChannelCmd                = ENABLE;
    NVIC_Init(&NVIC_InitStructure);

    USART_Cmd(USART1, ENABLE);
}

/*===========================================================================*/
/*  Interrupt handler                                                        */
/*===========================================================================*/

/**
  * @brief  Buffer one received byte - called from USART1_IRQHandler().
  * @note   Reading DR is what clears RXNE, so this must be called as soon as
  *         the interrupt is dispatched.
  */
void UART_IrqHandler(void)
{
    uint8_t  byte = (uint8_t)(USART_ReceiveData(USART1) & 0xFFU);
    uint16_t next = (uint16_t)((s_rxHead + 1U) & UART_RX_MASK);

    if (next != (s_rxTail & UART_RX_MASK))
    {
        s_rxBuffer[s_rxHead] = byte;
        s_rxHead = next;
    }
    else
    {
        /* Buffer full: the newest byte is dropped. A visible overrun is
           better than silently overwriting unread data. */
        USART_ClearFlag(USART1, USART_FLAG_ORE);
    }
}

/**
  * @brief  Feed the transmit register from the ring - call on TXE.
  * @note   Drains as much as the hardware allows in one visit, then disables the
  *         TXE interrupt when the ring runs dry. Disabling it only after
  *         re-reading the indices is what makes the producer's
  *         "push then enable" sequence safe: the producer is the main loop and
  *         cannot run while this is executing, so a byte pushed before this
  *         point is seen here, and one pushed after it will enable the interrupt
  *         again (TXE is still set, so it fires immediately rather than being
  *         lost).
  */
void UART_TxIrqHandler(void)
{
    while ((s_txTail != s_txHead) &&
           (USART_GetFlagStatus(USART1, USART_FLAG_TXE) != RESET))
    {
        USART_SendData(USART1, s_txBuffer[s_txTail]);
        s_txTail = (uint16_t)((s_txTail + 1U) & UART_TX_MASK);
    }

    if (s_txTail == s_txHead)
    {
        USART_ITConfig(USART1, USART_IT_TXE, DISABLE);
    }
}

/*===========================================================================*/
/*  Transmit                                                                 */
/*===========================================================================*/

void UART_SendByte(uint8_t byte)
{
    uint16_t next = (uint16_t)((s_txHead + 1U) & UART_TX_MASK);

    /* Wait only when the ring is full; the interrupt is draining it. The wait
       is bounded by the ring size, not by the length of the output. */
    while (next == (s_txTail & UART_TX_MASK))
    {
    }

    s_txBuffer[s_txHead] = byte;
    s_txHead = next;

    USART_ITConfig(USART1, USART_IT_TXE, ENABLE);
}

uint16_t UART_TxFree(void)
{
    /* One slot is always left empty, which is what makes head == tail
       unambiguous. */
    return (uint16_t)(UART_TX_MASK - ((s_txHead - s_txTail) & UART_TX_MASK));
}

uint16_t UART_TxPending(void)
{
    return (uint16_t)((s_txHead - s_txTail) & UART_TX_MASK);
}

void UART_SendBytes(const uint8_t *data, uint16_t len)
{
    uint16_t i;

    if (data == 0)
    {
        return;
    }

    for (i = 0; i < len; i++)
    {
        UART_SendByte(data[i]);
    }
}

void UART_SendString(const char *str)
{
    if (str == 0)
    {
        return;
    }

    while (*str != '\0')
    {
        UART_SendByte((uint8_t)*str++);
    }
}

void UART_SendLine(void)
{
    UART_SendString("\r\n");
}

void UART_NewLine(void)
{
    UART_SendLine();
}

void UART_Printf(const char *format, ...)
{
    char    buffer[64];
    int     len;
    va_list args;

    va_start(args, format);
    len = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    if (len < 0)
    {
        return;
    }
    if (len > (int)(sizeof(buffer) - 1U))
    {
        len = (int)(sizeof(buffer) - 1U);   /* truncated */
    }

    UART_SendBytes((const uint8_t *)buffer, (uint16_t)len);
}

/*===========================================================================*/
/*  Receive                                                                  */
/*===========================================================================*/

uint16_t UART_Available(void)
{
    return (uint16_t)((s_rxHead - s_rxTail) & UART_RX_MASK);
}

uint8_t UART_DataAvailable(void)
{
    return (UART_Available() > 0U) ? 1U : 0U;
}

uint8_t UART_ReadByte(uint8_t *byte)
{
    if ((byte == 0) || (UART_Available() == 0U))
    {
        return 0U;
    }

    *byte = s_rxBuffer[s_rxTail];
    s_rxTail = (uint16_t)((s_rxTail + 1U) & UART_RX_MASK);

    return 1U;
}

uint8_t UART_Peek(void)
{
    if (UART_Available() == 0U)
    {
        return 0U;
    }
    return s_rxBuffer[s_rxTail];
}

void UART_Flush(void)
{
    s_rxTail = s_rxHead;
}

uint16_t UART_ReadLine(char *buffer, uint16_t maxLen)
{
    uint16_t count = 0U;
    uint8_t  byte;

    if ((buffer == 0) || (maxLen == 0U))
    {
        return 0U;
    }

    while (UART_ReadByte(&byte) != 0U)
    {
        /* CR is swallowed so CRLF and LF-only terminals behave the same. */
        if (byte == (uint8_t)'\r')
        {
            continue;
        }

        if (byte == (uint8_t)'\n')
        {
            buffer[count] = '\0';
            return count;
        }

        if (count < (uint16_t)(maxLen - 1U))
        {
            buffer[count++] = (char)byte;
        }
        /* Beyond maxLen-1 the byte is dropped, keeping the buffer terminated. */
    }

    /* No terminator yet: leave the partial line in the caller's buffer but
       report that no complete line is available. */
    buffer[count] = '\0';
    return 0U;
}

/*===========================================================================*/
/*  Demonstration consumer                                                   */
/*===========================================================================*/

void UART_Process(void)
{
    char line[64];
    uint16_t len;

    len = UART_ReadLine(line, sizeof(line));
    if (len == 0U)
    {
        return;
    }

    /* Echo and acknowledge. Replace this with real command handling. */
    UART_SendString("RX: ");
    UART_SendString(line);
    UART_SendLine();
}

/*===========================================================================*/
/*  printf retarget                                                          */
/*===========================================================================*/

/*
 * Only needed if application code calls the C library printf/puts directly;
 * UART_Printf() above works without any of this. The guard keeps the retarget
 * out of the way when the project is built with a full hosted C library.
 */
#if defined(__GNUC__) && !defined(__ARMCC_VERSION) && !defined(__MICROLIB)
int __io_putchar(int ch)
{
    UART_SendByte((uint8_t)ch);
    return ch;
}
#endif

#if defined(__ARMCC_VERSION)
#include <stdio.h>
#if defined(__MICROLIB)
/* MicroLIB: a single hook is enough for both stdout and stderr. */
int fputc(int ch, FILE *f)
{
    (void)f;
    UART_SendByte((uint8_t)ch);
    return ch;
}
#else
/* Standard C library: the semihosting stubs must still be overridden, which is
   why this function keeps the unused_* pragma. */
#pragma import(__use_no_semihosting)

struct __FILE
{
    int handle;
};

FILE __stdout;
FILE __stdin;

void _sys_exit(int x)
{
    (void)x;
}

void _ttywrch(int ch)
{
    (void)ch;
}

int fputc(int ch, FILE *f)
{
    (void)f;
    UART_SendByte((uint8_t)ch);
    return ch;
}
#endif /* __MICROLIB */
#endif /* __ARMCC_VERSION */
