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

int atoi(const char *nptr)
{
    int x = 0;
    // This tiny atoi() only accepts leading spaces followed by decimal digits.
    // Signs, overflow handling, and locale rules are outside klib's required
    // subset for the AM tests.
    while (*nptr == ' ')
    {
        nptr++;
    }

    while (*nptr >= '0' && *nptr <= '9')
    {
        x = x * 10 + *nptr - '0';
        nptr++;
    }

    return x;
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

#endif
