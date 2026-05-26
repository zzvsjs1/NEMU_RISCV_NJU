#include "trap.h"

static uint32_t rcl32_once(uint32_t value, uint8_t *cf, uint8_t *of)
{
    uint8_t carry, overflow;
    asm volatile("stc; rcll $1, %0; setc %1; seto %2"
                 : "+r"(value), "=qm"(carry), "=qm"(overflow)
                 :
                 : "cc");
    *cf = carry;
    *of = overflow;
    return value;
}

static uint32_t rcr32_once(uint32_t value, uint8_t *cf, uint8_t *of)
{
    uint8_t carry, overflow;
    asm volatile("stc; rcrl $1, %0; setc %1; seto %2"
                 : "+r"(value), "=qm"(carry), "=qm"(overflow)
                 :
                 : "cc");
    *cf = carry;
    *of = overflow;
    return value;
}

static uint16_t rcl16_cl(uint16_t value, uint8_t count, uint8_t *cf)
{
    uint8_t carry;
    asm volatile("clc; rclw %%cl, %0; setc %1"
                 : "+r"(value), "=qm"(carry)
                 : "c"(count)
                 : "cc");
    *cf = carry;
    return value;
}

static uint8_t rcr8_imm(uint8_t value, uint8_t *cf)
{
    uint8_t carry;
    asm volatile("stc; rcrb $2, %0; setc %1"
                 : "+q"(value), "=qm"(carry)
                 :
                 : "cc");
    *cf = carry;
    return value;
}

int main()
{
    uint8_t cf, of;

    check(rcl32_once(0x80000000u, &cf, &of) == 0x00000001u);
    check(cf == 1);
    check(of == 1);

    check(rcr32_once(0x00000001u, &cf, &of) == 0x80000000u);
    check(cf == 1);
    check(of == 1);

    check(rcl16_cl(0x8000u, 2, &cf) == 0x0001u);
    check(cf == 0);

    check(rcr8_imm(0x01u, &cf) == 0xc0u);
    check(cf == 0);

    return 0;
}
