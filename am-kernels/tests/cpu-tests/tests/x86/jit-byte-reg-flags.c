#include "trap.h"
#include <stdint.h>

static uint32_t pack_result(uint32_t value, uint8_t zf, uint8_t sf,
                            uint8_t cf, uint8_t of)
{
    return value ^ ((uint32_t)zf << 24) ^ ((uint32_t)sf << 25) ^
           ((uint32_t)cf << 26) ^ ((uint32_t)of << 27);
}

static uint32_t add_al_flags(uint32_t value)
{
    uint32_t after = 0;
    uint8_t zf = 0, sf = 0, cf = 0, of = 0;

    asm volatile(
        "movl %[value], %%eax\n\t"
        "addb $0x01, %%al\n\t"
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

    return pack_result(after, zf, sf, cf, of);
}

static uint32_t add_ah_flags(uint32_t value)
{
    uint32_t after = 0;
    uint8_t zf = 0, sf = 0, cf = 0, of = 0;

    asm volatile(
        "movl %[value], %%eax\n\t"
        "addb $0x01, %%ah\n\t"
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

    return pack_result(after, zf, sf, cf, of);
}

static uint32_t neg_ah_flags(uint32_t value)
{
    uint32_t after = 0;
    uint8_t zf = 0, sf = 0, cf = 0, of = 0;

    asm volatile(
        "movl %[value], %%eax\n\t"
        "negb %%ah\n\t"
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

    return pack_result(after, zf, sf, cf, of);
}

static uint32_t add_dh_flags(uint32_t value)
{
    uint32_t after = 0;
    uint8_t zf = 0, sf = 0, cf = 0, of = 0;

    asm volatile(
        "movl %[value], %%edx\n\t"
        "addb $0x01, %%dh\n\t"
        "setz %[zf]\n\t"
        "sets %[sf]\n\t"
        "setc %[cf]\n\t"
        "seto %[of]\n\t"
        "movl %%edx, %[after]\n\t"
        : [after] "=m"(after),
          [zf] "=qm"(zf),
          [sf] "=qm"(sf),
          [cf] "=qm"(cf),
          [of] "=qm"(of)
        : [value] "r"(value)
        : "edx", "cc", "memory");

    return pack_result(after, zf, sf, cf, of);
}

int main(void)
{
    check(add_al_flags(0x123456ffu) ==
          pack_result(0x12345600u, 1, 0, 1, 0));
    check(add_ah_flags(0x1234ff78u) ==
          pack_result(0x12340078u, 1, 0, 1, 0));
    check(add_ah_flags(0x12347f78u) ==
          pack_result(0x12348078u, 0, 1, 0, 1));
    check(neg_ah_flags(0x12340578u) ==
          pack_result(0x1234fb78u, 0, 1, 1, 0));
    check(neg_ah_flags(0x12340078u) ==
          pack_result(0x12340078u, 1, 0, 0, 0));
    check(add_dh_flags(0x5678ff9au) ==
          pack_result(0x5678009au, 1, 0, 1, 0));
    return 0;
}
