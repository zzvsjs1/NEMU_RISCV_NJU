#include "trap.h"
#include <stdint.h>

static uint32_t test_ah_01_flags(uint32_t value)
{
    uint32_t result = 0;
    uint32_t after = 0;
    uint8_t zf = 0;
    uint8_t sf = 0;
    uint8_t cf = 0;
    uint8_t of = 0;

    asm volatile(
        "movl %[value], %%eax\n\t"
        "testb $0x01, %%ah\n\t"
        "setz %[zf]\n\t"
        "sets %[sf]\n\t"
        "setc %[cf]\n\t"
        "seto %[of]\n\t"
        "movl %%eax, %[after]\n\t"
        : [after] "=m"(after),
          [zf] "=qm"(zf),
          [sf] "=qm"(sf),
          [cf] "=qm"(cf),
          [of] "=qm"(of)
        : [value] "r"(value)
        : "eax", "cc", "memory");

    result |= after;
    result ^= (uint32_t)zf << 24;
    result ^= (uint32_t)sf << 25;
    result ^= (uint32_t)cf << 26;
    result ^= (uint32_t)of << 27;
    return result;
}

static uint32_t test_ah_80_flags(uint32_t value)
{
    uint32_t result = 0;
    uint32_t after = 0;
    uint8_t zf = 0;
    uint8_t sf = 0;

    asm volatile(
        "movl %[value], %%eax\n\t"
        "testb $0x80, %%ah\n\t"
        "setz %[zf]\n\t"
        "sets %[sf]\n\t"
        "movl %%eax, %[after]\n\t"
        : [after] "=m"(after),
          [zf] "=qm"(zf),
          [sf] "=qm"(sf)
        : [value] "r"(value)
        : "eax", "cc", "memory");

    result |= after;
    result ^= (uint32_t)zf << 24;
    result ^= (uint32_t)sf << 25;
    return result;
}

static uint32_t test_dh_40_flags(uint32_t value)
{
    uint32_t result = 0;
    uint32_t after = 0;
    uint8_t zf = 0;
    uint8_t sf = 0;

    asm volatile(
        "movl %[value], %%edx\n\t"
        "testb $0x40, %%dh\n\t"
        "setz %[zf]\n\t"
        "sets %[sf]\n\t"
        "movl %%edx, %[after]\n\t"
        : [after] "=m"(after),
          [zf] "=qm"(zf),
          [sf] "=qm"(sf)
        : [value] "r"(value)
        : "edx", "cc", "memory");

    result |= after;
    result ^= (uint32_t)zf << 24;
    result ^= (uint32_t)sf << 25;
    return result;
}

int main(void)
{
    check(test_ah_01_flags(0x12340000u) == (0x12340000u ^ (1u << 24)));
    check(test_ah_80_flags(0x12348000u) == (0x12348000u ^ (1u << 25)));
    check(test_dh_40_flags(0x56770000u) == (0x56770000u ^ (1u << 24)));
    check(test_dh_40_flags(0x56774000u) == 0x56774000u);
    return 0;
}
