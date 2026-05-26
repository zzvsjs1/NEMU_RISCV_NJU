#include "trap.h"

static uint32_t shld32_cl(uint32_t dest, uint32_t src, uint8_t count)
{
    asm volatile("shldl %%cl, %2, %0"
                 : "+r"(dest)
                 : "c"(count), "r"(src)
                 : "cc");
    return dest;
}

static uint32_t shrd32_cl(uint32_t dest, uint32_t src, uint8_t count)
{
    asm volatile("shrdl %%cl, %2, %0"
                 : "+r"(dest)
                 : "c"(count), "r"(src)
                 : "cc");
    return dest;
}

static uint32_t shld32_imm(uint32_t dest, uint32_t src)
{
    asm volatile("shldl $4, %1, %0" : "+r"(dest) : "r"(src) : "cc");
    return dest;
}

static uint32_t shrd32_imm(uint32_t dest, uint32_t src)
{
    asm volatile("shrdl $4, %1, %0" : "+r"(dest) : "r"(src) : "cc");
    return dest;
}

static uint16_t shld16_cl(uint16_t dest, uint16_t src, uint8_t count)
{
    asm volatile("shldw %%cl, %2, %0"
                 : "+r"(dest)
                 : "c"(count), "r"(src)
                 : "cc");
    return dest;
}

static uint16_t shrd16_cl(uint16_t dest, uint16_t src, uint8_t count)
{
    asm volatile("shrdw %%cl, %2, %0"
                 : "+r"(dest)
                 : "c"(count), "r"(src)
                 : "cc");
    return dest;
}

int main()
{
    check(shld32_cl(0x89abcdef, 0x12345678, 4) == 0x9abcdef1);
    check(shrd32_cl(0x89abcdef, 0x12345678, 4) == 0x889abcde);
    check(shld32_imm(0x89abcdef, 0x12345678) == 0x9abcdef1);
    check(shrd32_imm(0x89abcdef, 0x12345678) == 0x889abcde);
    check(shld32_cl(0x89abcdef, 0x12345678, 0) == 0x89abcdef);
    check(shrd32_cl(0x89abcdef, 0x12345678, 0) == 0x89abcdef);
    check(shld16_cl(0x1234, 0xabcd, 4) == 0x234a);
    check(shrd16_cl(0x1234, 0xabcd, 4) == 0xd123);

    return 0;
}
