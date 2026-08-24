#include "trap.h"

static uint32_t sub_jnz_loop(uint32_t n)
{
    uint32_t out = 0;

    asm volatile("movl %[n], %%ecx\n\t"
                 "xorl %%eax, %%eax\n\t"
                 "1:\n\t"
                 "addl %%ecx, %%eax\n\t"
                 "subl $1, %%ecx\n\t"
                 "jnz 1b\n\t"
                 "movl %%eax, %[out]"
                 : [out] "=m"(out)
                 : [n] "r"(n)
                 : "eax", "ecx", "memory", "cc");

    return out;
}

static uint32_t bare_sub_jnz_loop(uint32_t n)
{
    uint32_t out = 1;

    asm volatile("movl %[n], %%ecx\n\t"
                 "1:\n\t"
                 "subl $1, %%ecx\n\t"
                 "jnz 1b\n\t"
                 "movl %%ecx, %[out]"
                 : [out] "=m"(out)
                 : [n] "r"(n)
                 : "ecx", "memory", "cc");

    return out;
}

static uint32_t cmp_jl_loop(uint32_t limit)
{
    uint32_t out = 0;

    asm volatile("xorl %%eax, %%eax\n\t"
                 "xorl %%edx, %%edx\n\t"
                 "1:\n\t"
                 "addl %%eax, %%edx\n\t"
                 "addl $1, %%eax\n\t"
                 "cmpl %[limit], %%eax\n\t"
                 "jl 1b\n\t"
                 "movl %%edx, %[out]"
                 : [out] "=m"(out)
                 : [limit] "r"(limit)
                 : "eax", "edx", "memory", "cc");

    return out;
}

static uint32_t test_jnz_loop(uint32_t mask)
{
    uint32_t out = 0;

    asm volatile("movl %[mask], %%ecx\n\t"
                 "xorl %%eax, %%eax\n\t"
                 "1:\n\t"
                 "addl %%ecx, %%eax\n\t"
                 "shrl $1, %%ecx\n\t"
                 "testl %%ecx, %%ecx\n\t"
                 "jnz 1b\n\t"
                 "movl %%eax, %[out]"
                 : [out] "=m"(out)
                 : [mask] "r"(mask)
                 : "eax", "ecx", "memory", "cc");

    return out;
}

int main()
{
    check(sub_jnz_loop(4096u) == (4096u * 4097u) / 2u);
    check(bare_sub_jnz_loop(4096u) == 0u);
    check(cmp_jl_loop(4096u) == (4095u * 4096u) / 2u);
    check(test_jnz_loop(0x80000000u) == 0xffffffffu);
    return 0;
}
