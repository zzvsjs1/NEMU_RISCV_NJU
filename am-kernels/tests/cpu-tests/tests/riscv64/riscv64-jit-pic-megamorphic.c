#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 64

#include <stdint.h>

/*
 * Drive five destinations through one link-producing architectural JALR. A
 * two-way PIC cannot retain this round-robin working set, so blindly rewriting
 * native code on every replacement is much more expensive than staying on its
 * guarded lookup path. The unused t1 link keeps this source deliberately
 * eligible for patching; it must stop after bounded evidence of polymorphism.
 */
static uint64_t run_megamorphic_jalr_loop(void)
{
    uint64_t sum = 0;
    uint64_t laps = 5000;
    uint64_t phase = 0;

    asm volatile(
        ".option push\n"
        ".option norvc\n"
        "1:\n"
        "  beqz %[phase], 4f\n"
        "  addi t0, zero, 1\n"
        "  beq %[phase], t0, 5f\n"
        "  addi t0, zero, 2\n"
        "  beq %[phase], t0, 6f\n"
        "  addi t0, zero, 3\n"
        "  beq %[phase], t0, 7f\n"
        "  la t0, 12f\n"
        "  j 2f\n"
        "4:\n"
        "  la t0, 8f\n"
        "  j 2f\n"
        "5:\n"
        "  la t0, 9f\n"
        "  j 2f\n"
        "6:\n"
        "  la t0, 10f\n"
        "  j 2f\n"
        "7:\n"
        "  la t0, 11f\n"
        "2:\n"
        "  ori t0, t0, 1\n"
        "  jalr t1, 0(t0)\n"
        ".balign 4\n"
        "8:\n"
        "  addi %[sum], %[sum], 1\n"
        "  j 3f\n"
        ".balign 4\n"
        "9:\n"
        "  addi %[sum], %[sum], 2\n"
        "  j 3f\n"
        ".balign 4\n"
        "10:\n"
        "  addi %[sum], %[sum], 3\n"
        "  j 3f\n"
        ".balign 4\n"
        "11:\n"
        "  addi %[sum], %[sum], 4\n"
        "  j 3f\n"
        ".balign 4\n"
        "12:\n"
        "  addi %[sum], %[sum], 5\n"
        "3:\n"
        "  addi %[phase], %[phase], 1\n"
        "  addi t0, zero, 5\n"
        "  bne %[phase], t0, 13f\n"
        "  mv %[phase], zero\n"
        "13:\n"
        "  addi %[laps], %[laps], -1\n"
        "  bnez %[laps], 1b\n"
        ".option pop\n"
        : [sum] "+&r"(sum),
          [laps] "+&r"(laps),
          [phase] "+&r"(phase)
        :
        : "t0", "t1", "memory");

    return sum;
}

#endif

int main(void)
{
#if defined(__riscv) && __riscv_xlen == 64
    /* One complete five-lap cycle adds 1 + 2 + 3 + 4 + 5 = 15. */
    check(run_megamorphic_jalr_loop() == 15000u);
#endif

    return 0;
}
