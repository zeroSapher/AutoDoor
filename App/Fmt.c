/**
  ******************************************************************************
  * @file    Fmt.c
  * @brief   Minimal bounded string formatting without stdio. See Fmt.h.
  *
  * IMPLEMENTATION NOTES
  * --------------------
  * Output is built through a small writer that tracks the remaining space and
  * always keeps room for the terminator. Nothing here can overrun the caller's
  * buffer, which is the property that matters most: these formats are fed with
  * counters that grow without bound over a long runtime.
  *
  * Number formatting produces digits into a scratch buffer and then emits them
  * through the same writer, so width and padding are applied in one place rather
  * than duplicated per conversion.
  ******************************************************************************
  */

#include "Fmt.h"
#include <stdarg.h>

#define FMT_MAX_WIDTH   8U

/** Output cursor with a hard bound. */
typedef struct
{
    char     *dst;
    uint16_t  size;     /* total buffer size including terminator */
    uint16_t  used;     /* characters written, excluding terminator */
} Writer_t;

static void put_char(Writer_t *w, char c)
{
    if ((uint16_t)(w->used + 1U) < w->size)
    {
        w->dst[w->used] = c;
    }
    /* The count still advances past a full buffer so the return value reports
       how much WOULD have been written, which is what truncation detection
       needs; the buffer itself is simply not touched again. */
    w->used++;
}

static void put_str(Writer_t *w, const char *s)
{
    if (s == 0)
    {
        return;
    }
    while (*s != '\0')
    {
        put_char(w, *s++);
    }
}

/** Emit a string with left/right padding to a minimum field width. */
static void put_str_padded(Writer_t *w, const char *s, uint8_t width, uint8_t leftAlign)
{
    uint16_t len = 0U;
    uint16_t i;

    if (s == 0)
    {
        s = "";
    }

    while (s[len] != '\0')
    {
        len++;
    }

    if (leftAlign != 0U)
    {
        put_str(w, s);
        for (i = len; i < width; i++)
        {
            put_char(w, ' ');
        }
    }
    else
    {
        for (i = len; i < width; i++)
        {
            put_char(w, ' ');
        }
        put_str(w, s);
    }
}

/**
  * @brief  Emit an unsigned value in a given base with padding.
  * @param  zeroPad  1 to pad with '0' (numeric), 0 to pad with spaces.
  */
static void put_uint(Writer_t *w, uint32_t value, uint8_t base,
                     uint8_t width, uint8_t zeroPad, uint8_t leftAlign)
{
    char     scratch[12];       /* enough for 32 bits in base 10 */
    uint8_t  n = 0U;
    uint8_t  i;
    uint8_t  padCount;

    if (value == 0U)
    {
        scratch[n++] = '0';
    }
    else
    {
        while (value != 0U && n < (uint8_t)sizeof(scratch))
        {
            uint8_t digit = (uint8_t)(value % base);
            char    c;

            if (digit < 10U)
            {
                c = (char)('0' + (char)digit);
            }
            else
            {
                c = (char)('a' + (char)(digit - 10U));
            }
            scratch[n++] = c;
            value /= base;
        }
    }

    padCount = (n < width) ? (uint8_t)(width - n) : 0U;

    if (leftAlign == 0U && zeroPad == 0U)
    {
        for (i = 0U; i < padCount; i++)
        {
            put_char(w, ' ');
        }
    }

    if (leftAlign == 0U && zeroPad != 0U)
    {
        for (i = 0U; i < padCount; i++)
        {
            put_char(w, '0');
        }
    }

    /* Digits were generated least-significant first. */
    while (n > 0U)
    {
        put_char(w, scratch[--n]);
    }

    if (leftAlign != 0U)
    {
        for (i = 0U; i < padCount; i++)
        {
            put_char(w, ' ');
        }
    }
}

uint16_t Fmt_Format(char *dst, uint16_t dstSize, const char *fmt, ...)
{
    uint16_t written;
    va_list  args;

    va_start(args, fmt);
    written = Fmt_VFormat(dst, dstSize, fmt, args);
    va_end(args);

    return written;
}

uint16_t Fmt_VFormat(char *dst, uint16_t dstSize, const char *fmt, va_list args)
{
    Writer_t w;

    if ((dst == 0) || (dstSize == 0U))
    {
        return 0U;
    }

    w.dst  = dst;
    w.size = dstSize;
    w.used = 0U;

    while (*fmt != '\0')
    {
        uint8_t leftAlign = 0U;
        uint8_t zeroPad   = 0U;
        uint8_t width     = 0U;
        uint8_t haveWidth = 0U;

        if (*fmt != '%')
        {
            put_char(&w, *fmt++);
            continue;
        }

        fmt++;      /* consume '%' */

        if (*fmt == '%')
        {
            put_char(&w, '%');
            fmt++;
            continue;
        }

        /* ---- flags, width ---- */
        if (*fmt == '-')
        {
            leftAlign = 1U;
            fmt++;
        }
        if (*fmt == '0')
        {
            zeroPad = 1U;
            fmt++;
        }
        while ((*fmt >= '0') && (*fmt <= '9'))
        {
            if (haveWidth == 0U)
            {
                width = 0U;
                haveWidth = 1U;
            }
            if (width < FMT_MAX_WIDTH)
            {
                width = (uint8_t)((width * 10U) + (uint8_t)(*fmt - '0'));
            }
            fmt++;
        }

        /* A leading 'l' is accepted and ignored: the console passes unsigned
           long for values that are 32-bit on this target anyway. */
        while (*fmt == 'l')
        {
            fmt++;
        }

        switch (*fmt)
        {
            case 'u':
                put_uint(&w, va_arg(args, uint32_t), 10U, width, zeroPad, leftAlign);
                break;

            case 'd':
            {
                int32_t v = va_arg(args, int32_t);
                if (v < 0)
                {
                    put_char(&w, '-');
                    /* Negate in unsigned domain so INT32_MIN is handled. */
                    put_uint(&w, (uint32_t)0 - (uint32_t)v, 10U,
                             (width != 0U) ? (uint8_t)(width - 1U) : 0U,
                             zeroPad, leftAlign);
                }
                else
                {
                    put_uint(&w, (uint32_t)v, 10U, width, zeroPad, leftAlign);
                }
                break;
            }

            case 'x':
                put_uint(&w, va_arg(args, uint32_t), 16U, width, zeroPad, leftAlign);
                break;

            case 's':
                put_str_padded(&w, va_arg(args, const char *), width, leftAlign);
                break;

            case 'c':
                put_char(&w, (char)va_arg(args, int));
                break;

            case '\0':
                /* Trailing '%': emit it literally and stop cleanly. */
                put_char(&w, '%');
                fmt--;
                break;

            default:
                /* Unknown conversion: show it rather than dropping it, so a bad
                   format string is visible instead of becoming a silent gap. */
                put_char(&w, '%');
                put_char(&w, *fmt);
                break;
        }

        if (*fmt != '\0')
        {
            fmt++;
        }
    }

    /*
     * No va_end here: this is the va_list form. The va_list belongs to the
     * caller, which is the only frame allowed to start or end it.
     */

    /* Always terminate, clamping the stored length to the buffer. */
    if (w.used < w.size)
    {
        w.dst[w.used] = '\0';
    }
    else
    {
        w.dst[w.size - 1U] = '\0';
    }

    return w.used;
}
