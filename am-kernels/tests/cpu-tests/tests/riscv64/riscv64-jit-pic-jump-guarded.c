#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 64

#include <stdint.h>

/*
 * Model a compiler jump table with nine destinations and no architectural
 * link result. The target register is t2 rather than x1/x5, whose encodings
 * carry return-address-stack hints. Every selector arm uses a direct JAL to the
 * one indirect source so the JIT cannot split cache state between duplicated
 * copies of the architectural JALR.
 *
 * With compressed instructions disabled, the nine target blocks below are
 * exactly eight bytes apart. Targets zero through seven therefore occupy the
 * eight even entries of a raw 16-way `(pc >> 2)` cache, while target eight
 * deliberately aliases target zero. An accidental eight-entry cache aliases
 * four additional round-robin pairs and cannot meet the checker hit bound.
 */
static uint64_t run_megamorphic_jump_loop(void)
{
    uint64_t sum = 0;
    uint64_t laps = 1800;
    uint64_t phase = 0;

    asm volatile(".option push\n"
                 ".option norvc\n"
                 "1:\n"
                 "  beqz %[phase], 4f\n"
                 "  addi t2, zero, 1\n"
                 "  beq %[phase], t2, 5f\n"
                 "  addi t2, zero, 2\n"
                 "  beq %[phase], t2, 6f\n"
                 "  addi t2, zero, 3\n"
                 "  beq %[phase], t2, 7f\n"
                 "  addi t2, zero, 4\n"
                 "  beq %[phase], t2, 8f\n"
                 "  addi t2, zero, 5\n"
                 "  beq %[phase], t2, 9f\n"
                 "  addi t2, zero, 6\n"
                 "  beq %[phase], t2, 10f\n"
                 "  addi t2, zero, 7\n"
                 "  beq %[phase], t2, 11f\n"
                 "  la t2, 28f\n"
                 "  j 2f\n"
                 "4:\n"
                 "  la t2, 20f\n"
                 "  j 2f\n"
                 "5:\n"
                 "  la t2, 21f\n"
                 "  j 2f\n"
                 "6:\n"
                 "  la t2, 22f\n"
                 "  j 2f\n"
                 "7:\n"
                 "  la t2, 23f\n"
                 "  j 2f\n"
                 "8:\n"
                 "  la t2, 24f\n"
                 "  j 2f\n"
                 "9:\n"
                 "  la t2, 25f\n"
                 "  j 2f\n"
                 "10:\n"
                 "  la t2, 26f\n"
                 "  j 2f\n"
                 "11:\n"
                 "  la t2, 27f\n"
                 "  j 2f\n"
                 "2:\n"
                 "  ori t2, t2, 1\n"
                 "  jalr zero, 0(t2)\n"
                 ".balign 64\n"
                 "20:\n"
                 "  addi %[sum], %[sum], 1\n"
                 "  j 3f\n"
                 "21:\n"
                 "  addi %[sum], %[sum], 2\n"
                 "  j 3f\n"
                 "22:\n"
                 "  addi %[sum], %[sum], 3\n"
                 "  j 3f\n"
                 "23:\n"
                 "  addi %[sum], %[sum], 4\n"
                 "  j 3f\n"
                 "24:\n"
                 "  addi %[sum], %[sum], 5\n"
                 "  j 3f\n"
                 "25:\n"
                 "  addi %[sum], %[sum], 6\n"
                 "  j 3f\n"
                 "26:\n"
                 "  addi %[sum], %[sum], 7\n"
                 "  j 3f\n"
                 "27:\n"
                 "  addi %[sum], %[sum], 8\n"
                 "  j 3f\n"
                 "28:\n"
                 "  addi %[sum], %[sum], 9\n"
                 "  j 3f\n"
                 "3:\n"
                 "  addi %[phase], %[phase], 1\n"
                 "  addi t2, zero, 9\n"
                 "  bne %[phase], t2, 29f\n"
                 "  mv %[phase], zero\n"
                 "29:\n"
                 "  addi %[laps], %[laps], -1\n"
                 "  bnez %[laps], 1b\n"
                 ".option pop\n"
                 : [sum] "+&r"(sum), [laps] "+&r"(laps), [phase] "+&r"(phase)
                 :
                 : "t2", "memory");

    return sum;
}

#endif

int main(void)
{
#if defined(__riscv) && __riscv_xlen == 64
    /* One complete nine-lap cycle adds 1 + ... + 9 = 45. */
    check(run_megamorphic_jump_loop() == 9000u);
#endif

    return 0;
}
