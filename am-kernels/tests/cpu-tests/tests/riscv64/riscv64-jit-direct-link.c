#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 64

#include <stdint.h>

/*
 * Build one loop lap that is longer than the JIT's 64-instruction block cap.
 * The first native block falls through to the second, and the second reaches
 * the loop head through an unconditional JAL.  After the first warm-up lap both
 * targets should already be in the cache, so direct links can carry execution
 * between blocks while the arithmetic result still checks normal CPU state.
 */
static uint64_t run_cross_block_loop(void)
{
    uint64_t out = 0;
    uint64_t laps = 64;

    asm volatile(
        "1:\n"
        ".rept 70\n"
        "addi %[out], %[out], 1\n"
        ".endr\n"
        "addi %[laps], %[laps], -1\n"
        "beqz %[laps], 2f\n"
        "jal zero, 1b\n"
        "2:\n"
        : [out] "+r"(out), [laps] "+r"(laps)
        :
        : "memory");

    return out;
}

/* Same cross-block shape, but the hot edge back to the loop head is a branch. */
static uint64_t run_cross_block_branch_loop(void)
{
    uint64_t out = 0;
    uint64_t laps = 64;

    asm volatile(
        "1:\n"
        ".rept 70\n"
        "addi %[out], %[out], 1\n"
        ".endr\n"
        "addi %[laps], %[laps], -1\n"
        "bnez %[laps], 1b\n"
        : [out] "+r"(out), [laps] "+r"(laps)
        :
        : "memory");

    return out;
}

/* Repeated taken branch to a non-loop-head target, so trace backedge chaining cannot hide it. */
static uint64_t run_cross_block_branch_fork(void)
{
    uint64_t out = 0;
    uint64_t laps = 64;

    asm volatile(
        "1:\n"
        "andi t0, %[laps], 1\n"
        "bnez t0, 2f\n"
        ".rept 70\n"
        "addi %[out], %[out], 1\n"
        ".endr\n"
        "jal zero, 3f\n"
        "2:\n"
        ".rept 70\n"
        "addi %[out], %[out], 1\n"
        ".endr\n"
        "3:\n"
        "addi %[laps], %[laps], -1\n"
        "bnez %[laps], 1b\n"
        : [out] "+r"(out), [laps] "+r"(laps)
        :
        : "t0", "memory");

    return out;
}

static void test_direct_block_links(void)
{
    check(run_cross_block_loop() == 70u * 64u);
    check(run_cross_block_branch_loop() == 70u * 64u);
    check(run_cross_block_branch_fork() == 70u * 64u);
}

#endif

/* Keep the source buildable outside RV64 while exercising the RV64-only path. */
int main(void)
{
#if defined(__riscv) && __riscv_xlen == 64
    test_direct_block_links();
#endif

    return 0;
}
