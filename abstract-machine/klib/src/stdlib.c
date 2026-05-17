#include <am.h>
#include <klib.h>
#include <klib-macros.h>

#if !defined(__ISA_NATIVE__) || defined(__NATIVE_USE_KLIB__)
static unsigned long int next = 1;

int rand(void)
{
    // RAND_MAX assumed to be 32767
    next = next * 1103515245 + 12345;
    return (unsigned int)(next / 65536) % 32768;
}

void srand(unsigned int seed)
{
    next = seed;
}

int abs(int x)
{
    return x < 0 ? -x : x;
}

static int digit_value(int ch)
{
    if (ch >= '0' && ch <= '9')
    {
        return ch - '0';
    }

    if (ch >= 'a' && ch <= 'z')
    {
        return ch - 'a' + 10;
    }

    if (ch >= 'A' && ch <= 'Z')
    {
        return ch - 'A' + 10;
    }

    return -1;
}

static unsigned long long strtoull_core(const char *nptr, char **endptr, int base, int *negative)
{
    const char *p = nptr;
    unsigned long long value = 0;
    int any = 0;

    while (isspace(*p))
    {
        p++;
    }

    *negative = 0;
    if (*p == '+' || *p == '-')
    {
        *negative = (*p == '-');
        p++;
    }

    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
    {
        base = 16;
        p += 2;
    }
    else if (base == 0)
    {
        base = (p[0] == '0') ? 8 : 10;
    }

    if (base < 2 || base > 36)
    {
        if (endptr)
        {
            *endptr = (char *)nptr;
        }
        return 0;
    }

    while (*p)
    {
        int digit = digit_value(*p);
        if (digit < 0 || digit >= base)
        {
            break;
        }

        value = value * (unsigned)base + (unsigned)digit;
        p++;
        any = 1;
    }

    if (endptr)
    {
        *endptr = (char *)(any ? p : nptr);
    }

    return value;
}

long strtol(const char *nptr, char **endptr, int base)
{
    int negative;
    unsigned long long value = strtoull_core(nptr, endptr, base, &negative);
    return negative ? (long)(0ul - (unsigned long)value) : (long)value;
}

unsigned long strtoul(const char *nptr, char **endptr, int base)
{
    int negative;
    unsigned long long value = strtoull_core(nptr, endptr, base, &negative);
    return negative ? (0ul - (unsigned long)value) : (unsigned long)value;
}

long long strtoll(const char *nptr, char **endptr, int base)
{
    int negative;
    unsigned long long value = strtoull_core(nptr, endptr, base, &negative);
    return negative ? (long long)(0ull - value) : (long long)value;
}

unsigned long long strtoull(const char *nptr, char **endptr, int base)
{
    int negative;
    unsigned long long value = strtoull_core(nptr, endptr, base, &negative);
    return negative ? (0ull - value) : value;
}

int atoi(const char *nptr)
{
    // Keep atoi() in lock-step with strtol(): the conversion must skip all C
    // whitespace and accept an optional sign, not only a literal space followed
    // by unsigned decimal digits.
    return (int)strtol(nptr, NULL, 10);
}

static size_t si = 0;

#define MALLOC_ALIGN 16
#define MALLOC_ALIGN_MASK (MALLOC_ALIGN - 1)

static_assert((MALLOC_ALIGN & MALLOC_ALIGN_MASK) == 0);

static int add_overflows_uintptr(uintptr_t lhs, uintptr_t rhs, uintptr_t *sum)
{
    if (rhs > UINTPTR_MAX - lhs)
    {
        return 1;
    }

    *sum = lhs + rhs;
    return 0;
}

static int align_up_uintptr(uintptr_t value, uintptr_t *aligned)
{
    uintptr_t rounded;

    if (add_overflows_uintptr(value, MALLOC_ALIGN_MASK, &rounded))
    {
        return 1;
    }

    *aligned = rounded & ~(uintptr_t)MALLOC_ALIGN_MASK;
    return 0;
}

void *malloc(size_t size)
{
    if (size == 0)
    {
        return NULL;
    }

    // On native, malloc() will be called during initialisation of C runtime.
    // Therefore do not call panic() here, else it will yield a dead recursion:
    //   panic() -> putchar() -> (glibc) -> malloc() -> panic()
    // The allocator is a bump pointer over AM's heap area. It never reuses memory,
    // so every successful allocation only moves `si` forwards. All arithmetic is
    // checked before `si` changes, because wraparound would move the cursor back
    // into already-owned memory and make the next allocation overlap old data.
    uintptr_t heap_start = (uintptr_t)heap.start;
    uintptr_t heap_end = (uintptr_t)heap.end;
    uintptr_t cursor;
    uintptr_t addr;
    uintptr_t requested_end;
    uintptr_t next_addr;

    if (add_overflows_uintptr(heap_start, si, &cursor))
    {
        return NULL;
    }

    if (align_up_uintptr(cursor, &addr))
    {
        return NULL;
    }

    if (add_overflows_uintptr(addr, size, &requested_end))
    {
        return NULL;
    }

    if (align_up_uintptr(requested_end, &next_addr))
    {
        return NULL;
    }

    if (next_addr > heap_end)
    {
        return NULL;
    }

    assert(addr >= heap_start);
    assert(next_addr <= heap_end);

    si = next_addr - heap_start;
    return (void *)addr;
}

void *calloc(size_t nmemb, size_t size)
{
    if (nmemb != 0 && size > (size_t)-1 / nmemb)
    {
        return NULL;
    }

    size *= nmemb;
    void *ret = malloc(size);
    if (ret != NULL)
    {
        memset(ret, 0, size);
    }
    return ret;
}

void *realloc(void *ptr, size_t size)
{
    if (ptr == NULL)
    {
        return malloc(size);
    }

    if (size == 0)
    {
        free(ptr);
        return NULL;
    }

    /*
     * The AM bump allocator deliberately does not store allocation sizes.
     * Copying an existing allocation to a new block would therefore require
     * guessing how many bytes are valid. Report failure instead of silently
     * over-reading the old object.
     */
    return NULL;
}

// void *calloc(size_t nmemb, size_t size) {
//   size *= nmemb;
//   void *ret = malloc(size);
//   memset(ret, 0, size);
//   return ret;
// }

// void *realloc(void *ptr, size_t size) {
//   if (ptr == NULL) return malloc(size);
//   if (size == 0) {
//     free(ptr);
//     return NULL;
//   }
//   void *ret = malloc(size);
//   memcpy(ret, ptr, size);
//   free(ptr);
//   return ret;
// }

void free(void *ptr)
{
    if (ptr == NULL)
    {
        return;
    }

    // AM klib intentionally keeps malloc() as a bump allocator. Memory is not
    // reclaimed here because ownership of the global heap can also belong to
    // simple kernels and page allocators layered above AM.
}

void exit(int status)
{
    halt(status);
}

#endif
