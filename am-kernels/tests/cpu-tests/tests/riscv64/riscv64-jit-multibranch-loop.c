#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 64

#include <stdint.h>

/*
 * Exercise a seven-register loop with an interior conditional exit before the
 * final backedge. A stable loop mapping must reject this shape because the
 * interior branch can leave after earlier native laps have accumulated retired
 * instructions in host registers.
 */
static uint64_t run_multibranch_loop(void)
{
    uint64_t out;

    asm volatile("li t0, 2048\n"
                 "li t1, 0\n"
                 "li t2, 0\n"
                 "li t3, 0\n"
                 "li t4, 0\n"
                 "li t5, 0\n"
                 "li t6, 1024\n"
                 "1:\n"
                 "addi t0, t0, -1\n"
                 "addi t1, t1, 1\n"
                 "addi t2, t2, 2\n"
                 "beq t0, t6, 2f\n"
                 "addi t3, t3, 3\n"
                 "addi t4, t4, 4\n"
                 "addi t5, t5, 5\n"
                 "bnez t0, 1b\n"
                 "2:\n"
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

#endif

int main(void)
{
#if defined(__riscv) && __riscv_xlen == 64
    check(run_multibranch_loop() == 16372ull);
#endif

    return 0;
}
