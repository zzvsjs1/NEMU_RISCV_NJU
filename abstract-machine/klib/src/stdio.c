#include <am.h>
#include <klib.h>
#include <klib-macros.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#if !defined(__ISA_NATIVE__) || defined(__NATIVE_USE_KLIB__)

/*
 * Minimal printf family implementation for bare‑metal runtime.
 *
 * Supported conversions:
 *   %c %s %d %i %u %x %X %o %O %p %%
 *
 * Flags:
 *   '-'   left align within the given width
 *   '0'   pad numeric output with leading zeroes
 *
 * Width:
 *   decimal integer constant (no '*' support)
 *
 * Length modifiers:
 *   l     long / unsigned long
 *   ll    long long / unsigned long long
 *   z     size_t / ssize_t
 *
 * Internals:
 *   A single 8 KiB buffer is allocated with malloc() on first call to printf() and reused forever
 */

#define ARRAY_LEN(arr) (sizeof(arr) / sizeof((arr)[0]))
#define PRINTF_BUFFER_SIZE (8 * 1024)

/* -------------------------------------------------------------------------- */
/*  Numeric helpers                                                           */
/* -------------------------------------------------------------------------- */

static int utoa_base(unsigned long long value,
                     char               *buf,
                     int                 base,
                     int                 uppercase)
{
    static const char *digits_lc = "0123456789abcdef";
    static const char *digits_uc = "0123456789ABCDEF";
    const char       *digits     = uppercase ? digits_uc : digits_lc;

    if (base < 2 || base > 16)
    {
        buf[0] = '\0';
        return 0;
    }

    char tmp[64];                  /* enough for binary of 64‑bit value */
    int  pos = 0;

    do
    {
        tmp[pos++] = digits[value % (unsigned)base];
        value      /= (unsigned)base;
    }
    while (value != 0ULL);

    for (int i = 0; i < pos; i++)
    {
        buf[i] = tmp[pos - 1 - i];
    }

    buf[pos] = '\0';
    return pos;
}

static int ltoa_dec(long long value, char *buf)
{
    if (value == 0)
    {
        buf[0] = '0';
        buf[1] = '\0';
        return 1;
    }

    unsigned long long v   = (value < 0) ? (unsigned long long)(-value)
                                            : (unsigned long long)value;
    int                neg = (value < 0);

    char tmp[64];
    int  pos = 0;

    while (v > 0ULL)
    {
        tmp[pos++] = (char)('0' + (v % 10ULL));
        v         /= 10ULL;
    }

    if (neg)
    {
        tmp[pos++] = '-';
    }

    for (int i = 0; i < pos; i++)
    {
        buf[i] = tmp[pos - 1 - i];
    }

    buf[pos] = '\0';
    return pos;
}

/* -------------------------------------------------------------------------- */
/*  Core writer helpers                                                       */
/* -------------------------------------------------------------------------- */

static void write_repeat(char         ch,
                         int          count,
                         char        *out,
                         size_t       n,
                         size_t      *out_pos,
                         size_t      *total_written)
{
    for (int i = 0; i < count; i++)
    {
        if (*out_pos + 1 < n)
        {
            out[*out_pos] = ch;
        }

        (*out_pos)++;
        (*total_written)++;
    }
}

static void write_str(const char *s,
                      int         len,
                      int         width,
                      int         left_align,
                      int         zero_pad,
                      char       *out,
                      size_t      n,
                      size_t     *out_pos,
                      size_t     *total_written)
{
    int pad = (width > len) ? (width - len) : 0;

    if (!left_align)
    {
        write_repeat(zero_pad ? '0' : ' ', pad, out, n, out_pos, total_written);
    }

    for (int i = 0; i < len; i++)
    {
        if (*out_pos + 1 < n)
        {
            out[*out_pos] = s[i];
        }

        (*out_pos)++;
        (*total_written)++;
    }

    if (left_align)
    {
        write_repeat(' ', pad, out, n, out_pos, total_written);
    }
}

/* -------------------------------------------------------------------------- */
/*  vsnprintf                                                                 */
/* -------------------------------------------------------------------------- */

int vsnprintf(char       *out,
              size_t      n,
              const char *fmt,
              va_list     ap)
{
    size_t total_written = 0U;
    size_t out_pos       = 0U;

    if (out == NULL)
    {
        n = 0U;                    /* write count only mode */
    }

    while (*fmt)
    {
        if (*fmt != '%')
        {
            if (out_pos + 1U < n)
            {
                out[out_pos] = *fmt;
            }

            out_pos++;
            total_written++;
            fmt++;
            continue;
        }

        /* ------------------------------------------------------------------ */
        /*  Parse flags and width                                             */
        /* ------------------------------------------------------------------ */

        fmt++;                      /* skip '%' */

        int left_align = 0;
        int zero_pad   = 0;
        int width      = 0;

        int parsing_flags = 1;

        while (parsing_flags)
        {
            switch (*fmt)
            {
                case '-':
                {
                    left_align = 1;
                    fmt++;
                    break;
                }
                case '0':
                {
                    zero_pad = 1;
                    fmt++;
                    break;
                }
                default:
                {
                    parsing_flags = 0;
                    break;
                }
            }
        }

        if (left_align)
        {
            zero_pad = 0;          /* '-' overrides '0' */
        }

        while (*fmt >= '0' && *fmt <= '9')
        {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        /* ------------------------------------------------------------------ */
        /*  Length modifier                                                   */
        /* ------------------------------------------------------------------ */

        int length_ll   = 0;
        int length_l    = 0;
        int length_size = 0;

        if (*fmt == 'l')
        {
            fmt++;
            if (*fmt == 'l')        /* ll */
            {
                length_ll = 1;
                fmt++;
            }
            else
            {
                length_l = 1;
            }
        }
        else if (*fmt == 'z')       /* size_t */
        {
            length_size = 1;
            fmt++;
        }

        char spec = *fmt;
        if (spec == '\0')          /* malformed, stop */
        {
            break;
        }

        fmt++;                      /* consume spec */

        /* ------------------------------------------------------------------ */
        /*  Dispatch                                                          */
        /* ------------------------------------------------------------------ */

        char tmp[64];
        int  len = 0;

        switch (spec)
        {
            case 'c':
            {
                int ch = va_arg(ap, int);
                tmp[0] = (char)ch;
                tmp[1] = '\0';
                len    = 1;
                write_str(tmp, len, width, left_align, 0, out, n,
                          &out_pos, &total_written);
                break;
            }

            case 's':
            {
                const char *s = va_arg(ap, const char *);
                if (!s)
                {
                    s = "(null)";
                }

                len = (int)strlen(s);
                write_str(s, len, width, left_align, 0, out, n,
                          &out_pos, &total_written);
                break;
            }

            case 'd':
            case 'i':
            {
                long long val;

                if (length_ll)
                {
                    val = va_arg(ap, long long);
                }
                else if (length_l)
                {
                    val = va_arg(ap, long);
                }
                // No ssize_t.
                
                // else if (length_size)
                // {
                //     val = (long long)va_arg(ap, ssize_t);
                // }
                else
                {
                    val = (long long)va_arg(ap, int);
                }

                len = ltoa_dec(val, tmp);
                write_str(tmp, len, width, left_align, zero_pad, out, n,
                          &out_pos, &total_written);
                break;
            }

            case 'u':
            case 'x':
            case 'X':
            case 'o':
            case 'O':
            {
                unsigned long long val;

                if (length_ll)
                {
                    val = va_arg(ap, unsigned long long);
                }
                else if (length_l)
                {
                    val = va_arg(ap, unsigned long);
                }
                else if (length_size)
                {
                    val = (unsigned long long)va_arg(ap, size_t);
                }
                else
                {
                    val = (unsigned long long)va_arg(ap, unsigned);
                }

                int base = 10;
                int upper = 0;

                if (spec == 'x' || spec == 'X')
                {
                    base  = 16;
                    upper = (spec == 'X');
                }
                else if (spec == 'o' || spec == 'O')
                {
                    base  = 8;
                    upper = (spec == 'O');
                }

                len = utoa_base(val, tmp, base, upper);
                write_str(tmp, len, width, left_align, zero_pad, out, n,
                          &out_pos, &total_written);
                break;
            }

            case 'p':
            {
                void *ptr = va_arg(ap, void *);
                uintptr_t addr = (uintptr_t)ptr;

                tmp[0] = '0';
                tmp[1] = 'x';
                len    = 2;

                int len_addr = utoa_base(addr, tmp + 2, 16, 0);
                len += len_addr;

                write_str(tmp, len, width, left_align, zero_pad, out, n,
                          &out_pos, &total_written);
                break;
            }

            case '%':
            {
                tmp[0] = '%';
                tmp[1] = '\0';
                len    = 1;
                write_str(tmp, len, width, left_align, 0, out, n,
                          &out_pos, &total_written);
                break;
            }

            default:
            {
                /* Unknown specifier, print literally */
                if (out_pos + 1U < n) { out[out_pos] = '%'; }
                out_pos++; total_written++;
                if (out_pos + 1U < n) { out[out_pos] = spec; }
                out_pos++; total_written++;
                break;
            }
        }
    }

    /* ---------------------------------------------------------------------- */
    /*  NUL terminator                                                        */
    /* ---------------------------------------------------------------------- */

    if (n > 0U)
    {
        if (out_pos < n)
        {
            out[out_pos] = '\0';
        }
        else
        {
            out[n - 1U] = '\0';
        }
    }

    return (int)total_written;
}

/* -------------------------------------------------------------------------- */
/*  Convenience wrappers                                                      */
/* -------------------------------------------------------------------------- */

int snprintf(char       *out,
             size_t      n,
             const char *fmt,
             ...)
{
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(out, n, fmt, ap);
    va_end(ap);
    return ret;
}

int sprintf(char       *out,
            const char *fmt,
            ...)
{
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(out, (size_t)-1, fmt, ap);
    va_end(ap);
    return ret;
}

int printf(const char *fmt, ...)
{
    static char *buffer = NULL;

    if (buffer == NULL)
    {
        buffer = (char *)malloc(PRINTF_BUFFER_SIZE);
        if (!buffer)
        {
            return -1;              /* allocation failed */
        }
    }

    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(buffer, PRINTF_BUFFER_SIZE, fmt, ap);
    va_end(ap);

    putstr(buffer);
    return ret;
}

#endif /* !defined(__ISA_NATIVE__) || defined(__NATIVE_USE_KLIB__) */
