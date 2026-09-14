/**
  ******************************************************************************
  * @file    Fmt.h
  * @brief   Minimal bounded string formatting without stdio.
  *
  * WHY NOT snprintf
  * ----------------
  * newlib's snprintf drags in a full conversion engine. On this target the
  * duplicate instantiation cost about 4.7 KB of flash the first time the display
  * module used it - roughly 7 % of the whole 64 KB part - for a handful of
  * fixed-format strings. The console has its own formatter, so the two could not
  * even share the code.
  *
  * These helpers cover exactly the conversions this project needs (unsigned
  * decimal, string, and a right-aligned signed temperature) in a few hundred
  * bytes, and every one of them is bounded, so a long input can never overrun the
  * destination.
  *
  * Supported conversions: %u %d %s %c %% and zero-padded/width forms such as
  * %02u and %-6s. No floating point, no 64-bit.
  ******************************************************************************
  */

#ifndef __FMT_H
#define __FMT_H

#include <stdint.h>
#include <stdarg.h>

/**
  * @brief  Bounded printf-like formatting for the conversions listed above.
  * @param  dst     Destination buffer.
  * @param  dstSize Total size of dst, including the terminator.
  * @param  fmt     Format string.
  * @return Number of characters written, excluding the terminator.
  * @note   Output is always NUL terminated when dstSize > 0. Unsupported
  *         conversions are emitted literally rather than silently dropped, so a
  *         mistake is visible on the panel instead of becoming a mystery.
  */
uint16_t Fmt_Format(char *dst, uint16_t dstSize, const char *fmt, ...);

/**
  * @brief  va_list form, for forwarding from another variadic function.
  * @note   Needed by wrappers such as OLED_Printf() that receive "..." and must
  *         pass it on; a plain va_list cannot be handed to Fmt_Format() because
  *         that function expects real arguments, not an argument cursor.
  */
uint16_t Fmt_VFormat(char *dst, uint16_t dstSize, const char *fmt, va_list args);

#endif /* __FMT_H */
