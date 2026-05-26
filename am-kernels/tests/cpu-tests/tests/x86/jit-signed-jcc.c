#include "trap.h"

static uint32_t signed_jcc_mask(void)
{
    uint32_t mask = 0;

    asm volatile(
        "movl $-1, %%eax\n\t"
        "cmpl $0, %%eax\n\t"
        "jl 1f\n\t"
        "jmp 2f\n\t"
        "1:\n\t"
        "orl $1, %[mask]\n\t"
        "2:\n\t"
        "movl $0, %%eax\n\t"
        "cmpl $0, %%eax\n\t"
        "jge 3f\n\t"
        "jmp 4f\n\t"
        "3:\n\t"
        "orl $2, %[mask]\n\t"
        "4:\n\t"
        "movl $0, %%eax\n\t"
        "cmpl $0, %%eax\n\t"
        "jle 5f\n\t"
        "jmp 6f\n\t"
        "5:\n\t"
        "orl $4, %[mask]\n\t"
        "6:\n\t"
        "movl $1, %%eax\n\t"
        "cmpl $0, %%eax\n\t"
        "jg 7f\n\t"
        "jmp 8f\n\t"
        "7:\n\t"
        "orl $8, %[mask]\n\t"
        "8:"
        : [mask] "+r"(mask)
        :
        : "eax", "cc");

    return mask;
}

static uint32_t signed_jcc_not_taken_mask(void)
{
    uint32_t mask = 0;

    asm volatile(
        "movl $1, %%eax\n\t"
        "cmpl $0, %%eax\n\t"
        "jl 1f\n\t"
        "orl $1, %[mask]\n\t"
        "1:\n\t"
        "movl $-1, %%eax\n\t"
        "cmpl $0, %%eax\n\t"
        "jge 2f\n\t"
        "orl $2, %[mask]\n\t"
        "2:\n\t"
        "movl $1, %%eax\n\t"
        "cmpl $0, %%eax\n\t"
        "jle 3f\n\t"
        "orl $4, %[mask]\n\t"
        "3:\n\t"
        "movl $0, %%eax\n\t"
        "cmpl $0, %%eax\n\t"
        "jg 4f\n\t"
        "orl $8, %[mask]\n\t"
        "4:"
        : [mask] "+r"(mask)
        :
        : "eax", "cc");

    return mask;
}

static int32_t signed_count_loop(void)
{
    int32_t value = -17;

    asm volatile(
        "1:\n\t"
        "addl $1, %[value]\n\t"
        "cmpl $5, %[value]\n\t"
        "jl 1b"
        : [value] "+r"(value)
        :
        : "cc");

    return value;
}

int main()
{
    check(signed_jcc_mask() == 0xfu);
    check(signed_jcc_not_taken_mask() == 0xfu);
    check(signed_count_loop() == 5);
    return 0;
}
