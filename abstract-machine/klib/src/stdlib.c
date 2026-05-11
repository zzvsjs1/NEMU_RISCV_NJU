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

void *malloc(size_t size)
{
    // On native, malloc() will be called during initializaion of C runtime.
    // Therefore do not call panic() here, else it will yield a dead recursion:
    //   panic() -> putchar() -> (glibc) -> malloc() -> panic()
#if !(defined(__ISA_NATIVE__) && defined(__NATIVE_USE_KLIB__))
    // The allocator is a bump pointer over AM's heap area. It never reuses memory,
    // which is acceptable for the short-lived bare-metal tests and avoids needing
    // metadata before a kernel has its own memory manager.
    uintptr_t addr = ROUNDUP((uintptr_t)heap.start + si, MALLOC_ALIGN);
    uintptr_t next = ROUNDUP(addr + size, MALLOC_ALIGN);
    assert(next <= (uintptr_t)heap.end);
    si = next - (uintptr_t)heap.start;
    void *ret = (void *)addr;
    return ret;
#endif

    panic("Not implemented");
}

void free(void *ptr)
{
}

#endif
