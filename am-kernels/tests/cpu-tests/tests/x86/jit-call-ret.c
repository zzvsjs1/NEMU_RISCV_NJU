#include "trap.h"

__attribute__((noinline)) static uint32_t leaf(uint32_t value)
{
    return value + 0x31u;
}

__attribute__((noinline)) static uint32_t pair_sum(uint32_t value)
{
    return leaf(value + 1u) + leaf(value + 2u);
}

int main()
{
    uint32_t got = 0;
    uint32_t expected = 0;

    for (uint32_t i = 0; i < 32u; i++)
    {
        got += pair_sum(i);
        expected += (i + 1u + 0x31u) + (i + 2u + 0x31u);
    }

    check(got == expected);
    return 0;
}
