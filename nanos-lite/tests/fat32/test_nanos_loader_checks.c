#include <stdint.h>
#include <stdio.h>

#include "../../src/loader_checks.h"

#define CHECK(condition) \
    do \
    { \
        if (!(condition)) \
        { \
            printf("check failed at line %d: %s\n", __LINE__, #condition); \
            return 1; \
        } \
    } while (0)

enum
{
    PAGE_SIZE = 4096u,
    STACK_PAGES = 8u,
};

static int test_load_range_rejects_kernel_and_stack_addresses(void)
{
    const uintptr_t user_start = 0x40000000u;
    const uintptr_t user_end = 0x80000000u;
    const size_t stack_bytes = STACK_PAGES * PAGE_SIZE;
    uintptr_t segment_end = 0;

    CHECK(nanos_loader_load_range_fits(user_start, user_end, stack_bytes, 0x40001000u, 0x2000u, &segment_end));
    CHECK(segment_end == 0x40003000u);
    CHECK(nanos_loader_load_range_fits(user_start, user_end, stack_bytes, user_end - stack_bytes - PAGE_SIZE, PAGE_SIZE, &segment_end));
    CHECK(segment_end == user_end - stack_bytes);

    CHECK(!nanos_loader_load_range_fits(user_start, user_end, stack_bytes, 0x80000000u, PAGE_SIZE, &segment_end));
    CHECK(!nanos_loader_load_range_fits(user_start, user_end, stack_bytes, user_end - stack_bytes - 1u, 2u, &segment_end));
    CHECK(!nanos_loader_load_range_fits(user_start, user_end, stack_bytes, UINTPTR_MAX - 7u, 16u, &segment_end));

    return 0;
}

static int test_stack_layout_rejects_underflow(void)
{
    const uintptr_t stack_base = 0x7fff8000u;
    const uintptr_t stack_end = 0x80000000u;
    const size_t stack_bytes = (size_t)(stack_end - stack_base);
    const size_t pointer_words = 3u;
    const size_t pointer_bytes = pointer_words * sizeof(uintptr_t);
    uintptr_t initial_sp = 0;

    CHECK(nanos_loader_stack_layout_fits(stack_base, stack_end, stack_bytes - pointer_bytes, pointer_words, &initial_sp));
    CHECK(initial_sp == stack_base);

    CHECK(!nanos_loader_stack_layout_fits(stack_base, stack_end, stack_bytes, pointer_words, &initial_sp));
    CHECK(!nanos_loader_stack_layout_fits(stack_base, stack_end, 0, stack_bytes / sizeof(uintptr_t) + 1u, &initial_sp));

    return 0;
}

int main(void)
{
    if (test_load_range_rejects_kernel_and_stack_addresses() != 0 || test_stack_layout_rejects_underflow() != 0)
    {
        return 1;
    }

    puts("nanos loader check tests passed");
    return 0;
}
