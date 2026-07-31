#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 64

#include <stdint.h>

/*
 * Reach one warmed final JALR just before the outer 65,536-instruction polling
 * boundary. The long call contributes 64,443 instructions before the target;
 * the fixed start-up and three warm calls leave enough budget for that source,
 * but not for its 181-instruction compiled destination. An always-taken branch
 * skips the 180 instructions that inflate that advertised size and reaches an
 * unsupported FENCE. A source-only budget bug can therefore finish normally,
 * but it records zero rejections instead of the one required by the checker.
 *
 * FENCE.I is deliberately unsupported by the JIT and therefore starts the
 * counted region in a fresh native entry without discarding compiled blocks.
 * Three short calls first compile, fill, and execute this exact PIC edge; the
 * long call must reject it before entering the already-compiled target.
 */
__attribute__((noinline)) static uint64_t
run_indirect_pic_budget_case(uint64_t laps)
{
    uint64_t out = 0;

    asm volatile(
        ".option push\n"
        ".option norvc\n"
        "  fence.i\n"
        "1:\n"
        ".rept 70\n"
        "  addi %[out], %[out], 1\n"
        ".endr\n"
        "  addi %[laps], %[laps], -1\n"
        "  bnez %[laps], 1b\n"
        "  la t0, 2f\n"
        "  jalr t1, 0(t0)\n"
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
        : "t0", "t1", "memory");

    return out;
}

#endif

int main(void)
{
#if defined(__riscv) && __riscv_xlen == 64
    check(run_indirect_pic_budget_case(2u) == 319u);
    check(run_indirect_pic_budget_case(2u) == 319u);
    check(run_indirect_pic_budget_case(2u) == 319u);
    check(run_indirect_pic_budget_case(895u) == 62829u);
#endif

    return 0;
}
