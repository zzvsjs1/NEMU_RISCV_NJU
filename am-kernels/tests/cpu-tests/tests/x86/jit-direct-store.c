#include "trap.h"
#include <stdint.h>

static volatile uint32_t dst[64] __attribute__((aligned(4096)));
static uint32_t expected[64];

int main(void)
{
    uint32_t checksum = 0;

    for (int round = 0; round < 128; round++)
    {
        for (int i = 0; i < 64; i++)
        {
            uint32_t value = (uint32_t)(round * 131u + i * 17u + 0x12345678u);
            expected[i] = value;
            asm volatile("movl %1, %0" : "=m"(dst[i]) : "r"(value) : "memory");
        }
    }

    for (int i = 0; i < 64; i++)
    {
        check(dst[i] == expected[i]);
        checksum ^= dst[i] + (uint32_t)i;
    }

    check(checksum != 0);
    return 0;
}
