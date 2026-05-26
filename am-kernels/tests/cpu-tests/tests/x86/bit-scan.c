#include "trap.h"

static uint32_t bsr32_reg(uint32_t value)
{
    uint32_t result;
    asm volatile("bsrl %1, %0" : "=r"(result) : "r"(value) : "cc");
    return result;
}

static uint16_t bsr16_reg(uint16_t value)
{
    uint16_t result;
    asm volatile("bsrw %1, %0" : "=r"(result) : "r"(value) : "cc");
    return result;
}

static uint32_t bsr32_mem(const uint32_t *value)
{
    uint32_t result;
    asm volatile("bsrl %1, %0" : "=r"(result) : "m"(*value) : "cc");
    return result;
}

static uint8_t bsr32_zero_sets_zf(uint32_t value)
{
    uint32_t result = 0xdeadbeef;
    uint8_t zf;
    asm volatile("bsrl %2, %0; setz %1"
                 : "+r"(result), "=qm"(zf)
                 : "r"(value)
                 : "cc");
    return zf;
}

int main()
{
    uint32_t mem = 0x00010000;

    check(bsr32_reg(0x80000000) == 31);
    check(bsr32_reg(0x00008000) == 15);
    check(bsr32_reg(0x00000001) == 0);
    check(bsr16_reg(0x8000) == 15);
    check(bsr16_reg(0x0001) == 0);
    check(bsr32_mem(&mem) == 16);
    check(bsr32_zero_sets_zf(0) == 1);
    check(bsr32_zero_sets_zf(1) == 0);

    return 0;
}
