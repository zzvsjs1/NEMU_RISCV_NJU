#include "trap.h"
#include <stdint.h>

static volatile uint32_t mem_values[16] __attribute__((aligned(4096)));

static uint32_t not_reg_loop(uint32_t seed)
{
    uint32_t value = seed;

    for (int i = 0; i < 4096; i++)
    {
        asm volatile("notl %0" : "+r"(value));
        value += (uint32_t)i * 17u + 3u;
    }

    return value;
}

static uint32_t not_reg_ref(uint32_t seed)
{
    uint32_t value = seed;

    for (int i = 0; i < 4096; i++)
    {
        value = ~value;
        value += (uint32_t)i * 17u + 3u;
    }

    return value;
}

int main(void)
{
    check(not_reg_loop(0x01234567u) == not_reg_ref(0x01234567u));
    check(not_reg_loop(0x89abcdefu) == not_reg_ref(0x89abcdefu));

    for (int i = 0; i < 16; i++)
    {
        mem_values[i] = (uint32_t)i * 0x1020304u + 0x55667788u;
        uint32_t before = mem_values[i];
        asm volatile("notl %0" : "+m"(mem_values[i]) : : "memory");
        check(mem_values[i] == ~before);
    }

    return 0;
}
