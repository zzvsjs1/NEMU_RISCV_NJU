#include "trap.h"
#include <stdint.h>

static int32_t divisors[4] __attribute__((aligned(16))) = {
    3,
    -7,
    11,
    -13,
};

static int32_t do_idiv_reg(int32_t dividend, int32_t divisor, int32_t *rem)
{
    int32_t quo = 0;
    int32_t out_rem = 0;

    asm volatile(
        "cdq\n\t"
        "idivl %%ecx"
        : "=a"(quo),
          "=d"(out_rem)
        : "a"(dividend),
          "c"(divisor)
        : "cc");

    *rem = out_rem;
    return quo;
}

static int32_t do_idiv_ebx(int32_t dividend, int32_t divisor, int32_t *rem)
{
    int32_t quo = 0;
    int32_t out_rem = 0;

    asm volatile(
        "movl %[divisor], %%ebx\n\t"
        "cdq\n\t"
        "idivl %%ebx"
        : "=a"(quo),
          "=d"(out_rem)
        : "a"(dividend),
          [divisor] "r"(divisor)
        : "ebx", "cc");

    *rem = out_rem;
    return quo;
}

static int32_t do_idiv_esi(int32_t dividend, int32_t divisor, int32_t *rem)
{
    int32_t quo = 0;
    int32_t out_rem = 0;

    asm volatile(
        "movl %[divisor], %%esi\n\t"
        "cdq\n\t"
        "idivl %%esi"
        : "=a"(quo),
          "=d"(out_rem)
        : "a"(dividend),
          [divisor] "r"(divisor)
        : "esi", "cc");

    *rem = out_rem;
    return quo;
}

static int32_t do_idiv_edi(int32_t dividend, int32_t divisor, int32_t *rem)
{
    int32_t quo = 0;
    int32_t out_rem = 0;

    asm volatile(
        "movl %[divisor], %%edi\n\t"
        "cdq\n\t"
        "idivl %%edi"
        : "=a"(quo),
          "=d"(out_rem)
        : "a"(dividend),
          [divisor] "r"(divisor)
        : "edi", "cc");

    *rem = out_rem;
    return quo;
}

static int32_t do_idiv_mem(int32_t dividend, int32_t *rem)
{
    int32_t quo = 0;
    int32_t out_rem = 0;

    asm volatile(
        "cdq\n\t"
        "idivl %[src]"
        : "=a"(quo),
          "=d"(out_rem)
        : "a"(dividend),
          [src] "m"(divisors[1])
        : "cc");

    *rem = out_rem;
    return quo;
}

int main(void)
{
    int32_t rem = 0;

    for (int32_t i = -2048; i < 2048; i++)
    {
        const int32_t reg_divisor = (i & 1) ? divisors[0] : divisors[2];
        const int32_t dividend = i * 97 + 5;

        check(do_idiv_reg(dividend, reg_divisor, &rem) ==
              dividend / reg_divisor);
        check(rem == dividend % reg_divisor);

        check(do_idiv_mem(dividend, &rem) == dividend / divisors[1]);
        check(rem == dividend % divisors[1]);

        check(do_idiv_ebx(dividend, divisors[0], &rem) == dividend / divisors[0]);
        check(rem == dividend % divisors[0]);

        check(do_idiv_esi(dividend, divisors[2], &rem) == dividend / divisors[2]);
        check(rem == dividend % divisors[2]);

        check(do_idiv_edi(dividend, divisors[3], &rem) == dividend / divisors[3]);
        check(rem == dividend % divisors[3]);
    }

    return 0;
}
