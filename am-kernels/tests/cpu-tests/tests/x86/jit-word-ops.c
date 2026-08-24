#include "trap.h"

#define WORD_OP_ITERS 4096u

static uint16_t cell;

static uint32_t word_operand_loop(uint32_t iters)
{
    uint32_t result;

    /*
     * Keep this as explicit IA-32 assembly so the test always exercises
     * operand-size override forms used by microbench's 16-bit data paths.
     */
    asm volatile("movl %[cell], %%edx\n"
                 "movw $0, (%%edx)\n"
                 "movl %[iters], %%ecx\n"
                 "1:\n"
                 "incw (%%edx)\n"
                 "movzwl (%%edx), %%eax\n"
                 "cmpw $0x7fff, %%ax\n"
                 "decl %%ecx\n"
                 "jne 1b\n"
                 "movzwl (%%edx), %%eax\n"
                 : "=&a"(result)
                 : [cell] "r"(&cell), [iters] "r"(iters)
                 : "ecx", "edx", "cc", "memory");

    return result;
}

int main()
{
    check(word_operand_loop(WORD_OP_ITERS) == WORD_OP_ITERS);
    return 0;
}
