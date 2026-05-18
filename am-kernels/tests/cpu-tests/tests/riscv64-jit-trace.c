#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 64

#include <stdint.h>

/*
 * One loop lap is longer than the old 64-instruction native block cap.  A trace
 * can compile the whole hot fall-through path, including the final back-edge,
 * as one single-entry native region.
 */
static uint64_t run_long_fallthrough_trace(void)
{
    uint64_t out = 0;
    uint64_t laps = 32;

    asm volatile(
        "1:\n"
        ".rept 96\n"
        "addi %[out], %[out], 1\n"
        ".endr\n"
        "addi %[laps], %[laps], -1\n"
        "bnez %[laps], 1b\n"
        : [out] "+r"(out), [laps] "+r"(laps)
        :
        : "memory");

    return out;
}

static void test_trace_block(void)
{
    check(run_long_fallthrough_trace() == 96u * 32u);
}

#endif

/* Keep the source buildable outside RV64 while exercising the RV64-only path. */
int main(void)
{
#if defined(__riscv) && __riscv_xlen == 64
    test_trace_block();
#endif

    return 0;
}
