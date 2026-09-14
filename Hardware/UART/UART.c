/**
  ******************************************************************************
  * @file    UART.c
  * @brief   USART1 serial driver: interrupt-driven RX ring buffer, blocking TX.
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

    /* Receive-not-empty interrupt: fires once per received byte. */
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

/*===========================================================================*/
/*  Transmit                                                                 */
/*===========================================================================*/

void UART_SendByte(uint8_t byte)
{
    while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET)
    {
        /* wait for the transmit register to drain */
    }
    USART_SendData(USART1, byte);
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
