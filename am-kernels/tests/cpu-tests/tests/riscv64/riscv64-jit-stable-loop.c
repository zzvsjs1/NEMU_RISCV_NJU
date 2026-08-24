#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 64

#include <stdint.h>

/*
 * Keep seven guest registers live across a helper-free self-loop. The loop is
 * deliberately longer than one native-entry device budget, so the JIT must
 * preserve exact retired counts and architectural values across several
 * over-budget exits. The result is a simple arithmetic series, independent of
 * the JIT register allocation used to execute it.
 */
static uint64_t run_stable_register_loop(void)
{
    uint64_t out;

    asm volatile("li t0, 20000\n"
                 "li t1, 0\n"
                 "li t2, 0\n"
                 "li t3, 0\n"
                 "li t4, 0\n"
                 "li t5, 0\n"
                 "li t6, 0\n"
                 "1:\n"
                 "addi t0, t0, -1\n"
                 "addi t1, t1, 1\n"
                 "addi t2, t2, 2\n"
                 "addi t3, t3, 3\n"
                 "addi t4, t4, 4\n"
                 "addi t5, t5, 5\n"
                 "addi t6, t6, 6\n"
                 "bnez t0, 1b\n"
                 "add %[out], t1, t2\n"
                 "add %[out], %[out], t3\n"
                 "add %[out], %[out], t4\n"
                 "add %[out], %[out], t5\n"
                 "add %[out], %[out], t6\n"
                 : [out] "=&r"(out)
                 :
                 : "t0", "t1", "t2", "t3", "t4", "t5", "t6", "memory");

    return out;
}

/*
 * Cover cached compare destinations whose byte encodings need particular care:
 * t1 maps to BPL, t2 maps to R12B, and the seventh slot t6 maps to R8B. The
 * first two comparisons also alias the destination with one source operand.
 */
__attribute__((noinline)) static uint64_t run_cached_compare_loop(void)
{
    uint64_t out;

    asm volatile("li t0, 2\n"
                 "li t1, -1\n"
                 "li t2, 1\n"
                 "li t3, 1\n"
                 "li t4, 2\n"
                 "li t5, 3\n"
                 "li t6, 0\n"
                 "1:\n"
                 "slt t1, t1, t2\n"
                 "sltu t2, t3, t2\n"
                 "sltu t6, t4, t5\n"
                 "addi t0, t0, -1\n"
                 "bnez t0, 1b\n"
                 "add %[out], t1, t2\n"
                 "add %[out], %[out], t6\n"
                 : [out] "=&r"(out)
                 :
                 : "t0", "t1", "t2", "t3", "t4", "t5", "t6", "memory");

    return out;
}

#endif

/* Keep the source buildable outside RV64 while exercising the RV64-only path. */
int main(void)
{
#if defined(__riscv) && __riscv_xlen == 64
    check(run_stable_register_loop() == 420000ull);
    check(run_cached_compare_loop() == 1ull);
#endif

    return 0;
}
