#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 64

#include <stdint.h>

/*
 * Alternate between two JAL call sites that share one small leaf ending in
 * RET.  The leaf's dynamic return target therefore changes every lap, which
 * proves that a return link uses the full runtime target rather than one
 * compile-time address.  Save and restore the enclosing C function's RA
 * because the inline JAL instructions temporarily own the architectural link
 * register.
 */
static uint64_t run_two_target_return_loop(void)
{
    uint64_t out = 0;
    uint64_t laps = 4096;

    asm volatile(
        "mv t3, ra\n"
        "1:\n"
        "andi t0, %[laps], 1\n"
        "beqz t0, 2f\n"
        "jal ra, 4f\n"
        "addi %[out], %[out], 1\n"
        "j 3f\n"
        "2:\n"
        "jal ra, 4f\n"
        "addi %[out], %[out], 2\n"
        "3:\n"
        "addi %[laps], %[laps], -1\n"
        "bnez %[laps], 1b\n"
        "mv ra, t3\n"
        "j 5f\n"
        "4:\n"
        "addi %[out], %[out], 3\n"
        "ret\n"
        "5:\n"
        : [out] "+&r"(out), [laps] "+&r"(laps)
        :
        : "t0", "t3", "ra", "memory");

    return out;
}

static void test_two_target_return_link(void)
{
    /*
     * Every lap adds three in the shared leaf.  Half of the 4,096 laps add one
     * at the first return address and half add two at the second:
     * 4,096 * 3 + 2,048 * (1 + 2) = 18,432.
     */
    check(run_two_target_return_loop() == 18432u);
}

#endif

/* Keep the source buildable outside RV64 while exercising the RV64-only path. */
int main(void)
{
#if defined(__riscv) && __riscv_xlen == 64
    test_two_target_return_link();
#endif

    return 0;
}
