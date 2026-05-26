#include "trap.h"

static uint32_t inc32_flags(uint32_t value, uint8_t carry_in,
                            uint8_t *cf, uint8_t *zf, uint8_t *sf, uint8_t *of, uint8_t *pf)
{
    uint32_t out = value;
    uint8_t carry = 0, zero = 0, sign = 0, overflow = 0, parity = 0;

    if (carry_in)
    {
        asm volatile(
            "stc\n\t"
            "incl %[out]\n\t"
            "setc %[cf]\n\t"
            "setz %[zf]\n\t"
            "sets %[sf]\n\t"
            "seto %[of]\n\t"
            "setp %[pf]"
            : [out] "+r"(out),
              [cf] "=qm"(carry),
              [zf] "=qm"(zero),
              [sf] "=qm"(sign),
              [of] "=qm"(overflow),
              [pf] "=qm"(parity)
            :
            : "cc");
    }
    else
    {
        asm volatile(
            "clc\n\t"
            "incl %[out]\n\t"
            "setc %[cf]\n\t"
            "setz %[zf]\n\t"
            "sets %[sf]\n\t"
            "seto %[of]\n\t"
            "setp %[pf]"
            : [out] "+r"(out),
              [cf] "=qm"(carry),
              [zf] "=qm"(zero),
              [sf] "=qm"(sign),
              [of] "=qm"(overflow),
              [pf] "=qm"(parity)
            :
            : "cc");
    }

    *cf = carry;
    *zf = zero;
    *sf = sign;
    *of = overflow;
    *pf = parity;
    return out;
}

static uint32_t dec32_flags(uint32_t value, uint8_t carry_in,
                            uint8_t *cf, uint8_t *zf, uint8_t *sf, uint8_t *of, uint8_t *pf)
{
    uint32_t out = value;
    uint8_t carry = 0, zero = 0, sign = 0, overflow = 0, parity = 0;

    if (carry_in)
    {
        asm volatile(
            "stc\n\t"
            "decl %[out]\n\t"
            "setc %[cf]\n\t"
            "setz %[zf]\n\t"
            "sets %[sf]\n\t"
            "seto %[of]\n\t"
            "setp %[pf]"
            : [out] "+r"(out),
              [cf] "=qm"(carry),
              [zf] "=qm"(zero),
              [sf] "=qm"(sign),
              [of] "=qm"(overflow),
              [pf] "=qm"(parity)
            :
            : "cc");
    }
    else
    {
        asm volatile(
            "clc\n\t"
            "decl %[out]\n\t"
            "setc %[cf]\n\t"
            "setz %[zf]\n\t"
            "sets %[sf]\n\t"
            "seto %[of]\n\t"
            "setp %[pf]"
            : [out] "+r"(out),
              [cf] "=qm"(carry),
              [zf] "=qm"(zero),
              [sf] "=qm"(sign),
              [of] "=qm"(overflow),
              [pf] "=qm"(parity)
            :
            : "cc");
    }

    *cf = carry;
    *zf = zero;
    *sf = sign;
    *of = overflow;
    *pf = parity;
    return out;
}

int main()
{
    uint8_t cf = 0, zf = 0, sf = 0, of = 0, pf = 0;

    check(inc32_flags(0xffffffffu, 0, &cf, &zf, &sf, &of, &pf) == 0);
    check(cf == 0 && zf == 1 && sf == 0 && of == 0 && pf == 1);

    check(inc32_flags(0x7fffffffu, 1, &cf, &zf, &sf, &of, &pf) == 0x80000000u);
    check(cf == 1 && zf == 0 && sf == 1 && of == 1 && pf == 1);

    check(dec32_flags(0, 0, &cf, &zf, &sf, &of, &pf) == 0xffffffffu);
    check(cf == 0 && zf == 0 && sf == 1 && of == 0 && pf == 1);

    check(dec32_flags(0x80000000u, 1, &cf, &zf, &sf, &of, &pf) == 0x7fffffffu);
    check(cf == 1 && zf == 0 && sf == 0 && of == 1 && pf == 1);

    return 0;
}
