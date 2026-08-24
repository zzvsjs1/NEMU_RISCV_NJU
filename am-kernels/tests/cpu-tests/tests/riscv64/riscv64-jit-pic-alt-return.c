#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 64

#include <stdint.h>

/*
 * A JALR with rd=x0 and rs1=x5 carries the ISA's alternate return-stack pop
 * hint. The non-zero immediate is deliberate: Table 3 derives this hint from
 * register operands, not the canonical RET encoding. This monomorphic source
 * must therefore retain the patched PIC rather than use the no-PIC jump-table
 * policy. The architectural target remains label 2 because t0 holds 2f - 4.
 */
static uint64_t run_alternate_return_hint_loop(void)
{
    uint64_t sum = 0;
    uint64_t laps = 4096;

    asm volatile(".option push\n"
                 ".option norvc\n"
                 "1:\n"
                 "  la t0, 2f\n"
                 "  addi t0, t0, -4\n"
                 "  jalr zero, 4(t0)\n"
                 ".balign 4\n"
                 "2:\n"
                 "  addi %[sum], %[sum], 1\n"
                 "  addi %[laps], %[laps], -1\n"
                 "  bnez %[laps], 1b\n"
                 ".option pop\n"
                 : [sum] "+&r"(sum), [laps] "+&r"(laps)
                 :
                 : "t0", "memory");

    return sum;
}

#endif

int main(void)
{
#if defined(__riscv) && __riscv_xlen == 64
    check(run_alternate_return_hint_loop() == 4096u);
#endif

    return 0;
}
