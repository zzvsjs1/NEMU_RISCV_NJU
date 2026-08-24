#include "trap.h"

static uint32_t mul_src[4] __attribute__((aligned(16))) = {
    3u,
    7u,
    0xfffffff9u,
    5u,
};

static uint32_t do_mul_mem(uint32_t lhs, uint32_t *hi)
{
    uint32_t lo = 0;
    uint32_t out_hi = 0;

    asm volatile("mull %[src]" : "=a"(lo), "=d"(out_hi) : "a"(lhs), "d"(0u), [src] "m"(mul_src[0]) : "cc");

    *hi = out_hi;
    return lo;
}

static uint32_t do_imul_mem(uint32_t lhs, uint32_t *hi)
{
    uint32_t lo = 0;
    uint32_t out_hi = 0;

    asm volatile("imull %[src]" : "=a"(lo), "=d"(out_hi) : "a"(lhs), "d"(0u), [src] "m"(mul_src[2]) : "cc");

    *hi = out_hi;
    return lo;
}

static uint32_t do_div_mem(uint32_t hi, uint32_t lo, uint32_t *rem)
{
    uint32_t quo = 0;
    uint32_t out_rem = 0;

    asm volatile("divl %[src]" : "=a"(quo), "=d"(out_rem) : "a"(lo), "d"(hi), [src] "m"(mul_src[0]) : "cc");

    *rem = out_rem;
    return quo;
}

static uint32_t do_div_reg(uint32_t hi, uint32_t lo, uint32_t divisor, uint32_t *rem)
{
    uint32_t quo = 0;
    uint32_t out_rem = 0;

    asm volatile("divl %%ecx" : "=a"(quo), "=d"(out_rem) : "a"(lo), "d"(hi), "c"(divisor) : "cc");

    *rem = out_rem;
    return quo;
}

int main()
{
    uint32_t hi = 0;
    uint32_t rem = 0;
    uint32_t div3_quo = 0x55555555u;
    uint32_t div3_rem = 1u;
    uint32_t div7_quo = 14285u;
    uint32_t div7_rem = 5u;

    for (uint32_t i = 0; i < 4096u; i++)
    {
        const uint32_t lhs = 0x12345000u + i;
        check(do_mul_mem(lhs, &hi) == lhs * mul_src[0]);
        check(hi == 0);

        check(do_imul_mem(lhs, &hi) == 0u - lhs * 7u);
        check(hi == 0xffffffffu);

        check(do_div_mem(1u, i, &rem) == div3_quo);
        check(rem == div3_rem);

        check(do_div_reg(0u, 100000u + i, mul_src[1], &rem) == div7_quo);
        check(rem == div7_rem);

        div3_rem++;

        if (div3_rem == mul_src[0])
        {
            div3_rem = 0;
            div3_quo++;
        }

        div7_rem++;

        if (div7_rem == mul_src[1])
        {
            div7_rem = 0;
            div7_quo++;
        }
    }

    return 0;
}
