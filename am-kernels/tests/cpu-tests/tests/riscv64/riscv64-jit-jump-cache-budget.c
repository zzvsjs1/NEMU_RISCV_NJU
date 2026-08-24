#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 64

#include <stdint.h>

/*
 * Reach one warmed non-linking JALR just before the outer 65,536-instruction
 * polling boundary. The long call contributes 64,443 instructions before the
 * target; fixed start-up work and three short calls leave room for the source
 * but not for its 181-instruction compiled destination. The guarded jump cache
 * must therefore reject its otherwise-valid hit and return through the normal
 * committed-JALR dispatcher path.
 *
 * FENCE.I is deliberately unsupported by the JIT and starts the counted
 * region in a fresh native entry without discarding compiled blocks. The first
 * two short calls compile the destination and fill the source cache, the third
 * proves a normal hit, and the long call must record exactly one rejection.
 */
__attribute__((noinline)) static uint64_t run_indirect_jump_cache_budget_case(uint64_t laps)
{
    uint64_t out = 0;

    asm volatile(".option push\n"
                 ".option norvc\n"
                 "  fence.i\n"
                 "1:\n"
                 ".rept 70\n"
                 "  addi %[out], %[out], 1\n"
                 ".endr\n"
                 "  addi %[laps], %[laps], -1\n"
                 "  bnez %[laps], 1b\n"
                 "  la t2, 2f\n"
                 "  ori t2, t2, 1\n"
                 "  jalr zero, 0(t2)\n"
                 "2:\n"
                 "  beq zero, zero, 3f\n"
                 ".rept 180\n"
                 "  addi %[out], %[out], 1\n"
                 ".endr\n"
                 "3:\n"
                 "  fence\n"
                 ".rept 179\n"
                 "  addi %[out], %[out], 1\n"
                 ".endr\n"
                 ".option pop\n"
                 : [out] "+&r"(out), [laps] "+&r"(laps)
                 :
                 : "t2", "memory");

    return out;
}

#endif

int main(void)
{
#if defined(__riscv) && __riscv_xlen == 64
    check(run_indirect_jump_cache_budget_case(2u) == 319u);
    check(run_indirect_jump_cache_budget_case(2u) == 319u);
    check(run_indirect_jump_cache_budget_case(2u) == 319u);
    check(run_indirect_jump_cache_budget_case(895u) == 62829u);
#endif

    return 0;
}
