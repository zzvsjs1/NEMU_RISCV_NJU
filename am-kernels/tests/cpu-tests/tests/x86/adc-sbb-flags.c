#include "trap.h"

static uint8_t adc_overflow_from_carry_in(void)
{
    uint32_t value = 0;
    uint8_t of;

    asm volatile("xorl %%ecx, %%ecx; cmpl $1, %%ecx; adcl $0x7fffffff, %0; seto %1"
                 : "+r"(value), "=qm"(of)
                 :
                 : "ecx", "cc");

    check(value == 0x80000000u);
    return of;
}

static uint8_t sbb_overflow_from_borrow_in(void)
{
    uint32_t value = 0x80000000u;
    uint8_t of;

    asm volatile("xorl %%ecx, %%ecx; cmpl $1, %%ecx; sbbl $0x7fffffff, %0; seto %1"
                 : "+r"(value), "=qm"(of)
                 :
                 : "ecx", "cc");

    check(value == 0);
    return of;
}

int main()
{
    check(adc_overflow_from_carry_in() == 1);
    check(sbb_overflow_from_borrow_in() == 1);

    return 0;
}
