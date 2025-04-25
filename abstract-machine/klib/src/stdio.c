#include <am.h>
#include <klib.h>
#include <klib-macros.h>
#include <stdarg.h>
#if !defined(__ISA_NATIVE__) || defined(__NATIVE_USE_KLIB__)

#ifdef ARRAY_LEN
#undef ARRAY_LEN
#endif
#define ARRAY_LEN(arr) (sizeof(arr) / sizeof(arr[0]))

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

static int utoa_base(unsigned long value, char *buf, int base, int uppercase) {
    static const char *digits_lc = "0123456789abcdef";
    static const char *digits_uc = "0123456789ABCDEF";
    const char *digits = uppercase ? digits_uc : digits_lc;

    if (base < 2 || base > 16) {
        buf[0] = '\0';
        return 0;
    }

    char tmp[32];
    int pos = 0;
    do {
        tmp[pos++] = digits[value % base];
        value /= base;
    } while (value != 0);

    for (int i = 0; i < pos; i++) {
        buf[i] = tmp[pos - 1 - i];
    }
    buf[pos] = '\0';
    return pos;
}

static int ltoa_dec(long value, char *buf) {
    if (value == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return 1;
    }
    unsigned long v = (value < 0) ? (unsigned long)(-value) : (unsigned long)value;
    int neg = (value < 0);

    char tmp[32];
    int pos = 0;
    while (v > 0) {
        tmp[pos++] = (char)('0' + (v % 10));
        v /= 10;
    }
    if (neg) {
        tmp[pos++] = '-';
    }
    for (int i = 0; i < pos; i++) {
        buf[i] = tmp[pos - 1 - i];
    }
    buf[pos] = '\0';
    return pos;
}

int vsnprintf(char *out, size_t n, const char *fmt, va_list ap) {
    size_t total_written = 0;
    size_t out_pos = 0;

    if (out == NULL && n > 0) {
        n = 0;
    }

    while (*fmt) {
        if (*fmt != '%') {
            if (out_pos + 1 < n) {
                out[out_pos] = *fmt;
            }
            out_pos++;
            total_written++;
            fmt++;
            continue;
        }

        fmt++;                     // skip '%'
        int long_flag = 0;
        if (*fmt == 'l') {         // check for single 'l'
            long_flag = 1;
            fmt++;
        }

        char spec = *fmt++;
        if (!spec) break;

        switch (spec) {
            case 'c': {
                int c = va_arg(ap, int);
                if (out_pos + 1 < n) {
                    out[out_pos] = (char)c;
                }
                out_pos++;
                total_written++;
                break;
            }
            case 's': {
                const char *s = va_arg(ap, const char*);
                if (!s) s = "(null)";
                while (*s) {
                    if (out_pos + 1 < n) {
                        out[out_pos] = *s;
                    }
                    out_pos++;
                    total_written++;
                    s++;
                }
                break;
            }
            case 'd': case 'i': {
                long val;
                if (long_flag) {
                    val = va_arg(ap, long);
                } else {
                    val = (long)va_arg(ap, int);
                }
                char tmp[32];
                ltoa_dec(val, tmp);
                for (char *p = tmp; *p; p++) {
                    if (out_pos + 1 < n) {
                        out[out_pos] = *p;
                    }
                    out_pos++;
                    total_written++;
                }
                break;
            }
            case 'u': {
                unsigned long val;
                if (long_flag) {
                    val = va_arg(ap, unsigned long);
                } else {
                    val = (unsigned long)va_arg(ap, unsigned);
                }
                char tmp[32];
                utoa_base(val, tmp, 10, 0);
                for (char *p = tmp; *p; p++) {
                    if (out_pos + 1 < n) {
                        out[out_pos] = *p;
                    }
                    out_pos++;
                    total_written++;
                }
                break;
            }
            case 'x': case 'X': {
                unsigned long val;
                if (long_flag) {
                    val = va_arg(ap, unsigned long);
                } else {
                    val = (unsigned long)va_arg(ap, unsigned);
                }
                char tmp[32];
                utoa_base(val, tmp, 16, spec == 'X');
                for (char *p = tmp; *p; p++) {
                    if (out_pos + 1 < n) {
                        out[out_pos] = *p;
                    }
                    out_pos++;
                    total_written++;
                }
                break;
            }
            case 'p': {
                void *ptr = va_arg(ap, void*);
                uintptr_t addr = (uintptr_t)ptr;
                char tmp[32];
                // '0x'
                if (out_pos + 1 < n) out[out_pos] = '0';
                out_pos++; total_written++;
                if (out_pos + 1 < n) out[out_pos] = 'x';
                out_pos++; total_written++;
                utoa_base((unsigned long)addr, tmp, 16, 0);
                for (char *p = tmp; *p; p++) {
                    if (out_pos + 1 < n) out[out_pos] = *p;
                    out_pos++;
                    total_written++;
                }
                break;
            }
            case '%': {
                if (out_pos + 1 < n) {
                    out[out_pos] = '%';
                }
                out_pos++;
                total_written++;
                break;
            }
            default: {
                // unknown: print it literally
                if (out_pos + 1 < n) out[out_pos] = '%';
                out_pos++; total_written++;
                if (spec) {
                    if (out_pos + 1 < n) out[out_pos] = spec;
                    out_pos++; total_written++;
                }
                break;
            }
        }
    }

    if (n > 0) {
        if (out_pos < n) {
            out[out_pos] = '\0';
        } else {
            out[n - 1] = '\0';
        }
    }
    return (int)total_written;
}

int snprintf(char *out, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(out, n, fmt, ap);
    va_end(ap);
    return ret;
}

int sprintf(char *out, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(out, (size_t)-1, fmt, ap);
    va_end(ap);
    return ret;
}

int printf(const char *fmt, ...) {
    char buffer[1024];
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);
    putstr(buffer);
    return ret;
}

#endif
