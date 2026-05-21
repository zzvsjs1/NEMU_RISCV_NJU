#include "trap.h"
#include <stdint.h>

int main()
{
#if !defined(__ISA_NATIVE__) || defined(__NATIVE_USE_KLIB__)
    check(malloc(0) == NULL);

    free(NULL);

    void *first = malloc(1);
    void *second = malloc(17);

    check(first != NULL);
    check(second != NULL);
    check(((uintptr_t)first & 0xfu) == 0);
    check(((uintptr_t)second & 0xfu) == 0);
    check((uintptr_t)second > (uintptr_t)first);

    void *too_large = malloc((size_t)-1);
    check(too_large == NULL);

    void *after_failed = malloc(16);
    check(after_failed != NULL);
    check(((uintptr_t)after_failed & 0xfu) == 0);
#endif

    return 0;
}
