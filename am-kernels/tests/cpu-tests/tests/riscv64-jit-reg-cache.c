#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 64

#include <stdint.h>

static uint64_t reg_cache_slot;

/* Force more simultaneously live guest registers than the RV64 JIT cache holds. */
static uint64_t spill_sequence(void)
{
    uint64_t out;

    asm volatile(
        "addi t0, zero, 1\n"
        "addi t1, zero, 2\n"
        "addi t2, zero, 3\n"
        "addi t3, zero, 4\n"
        "addi t4, zero, 5\n"
        "addi t5, zero, 6\n"
        "addi t6, zero, 7\n"
        "add t0, t0, t6\n"
        "add t1, t1, t5\n"
        "add t2, t2, t4\n"
        "add t3, t3, t0\n"
        "add t4, t4, t1\n"
        "add t5, t5, t2\n"
        "add t6, t6, t3\n"
        "add %[out], t4, t5\n"
        "add %[out], %[out], t6\n"
        : [out] "=&r"(out)
        :
        : "t0", "t1", "t2", "t3", "t4", "t5", "t6", "memory");

    return out;
}

/* Check dirty cached values survive store helper exits and taken branch exits. */
static uint64_t store_and_branch_sequence(void)
{
    uint64_t out;

    reg_cache_slot = 0;
    asm volatile(
        "addi t0, zero, 40\n"
        "addi t1, t0, 2\n"
        "sd t1, 0(%[slot])\n"
        "ld t2, 0(%[slot])\n"
        "addi t3, t2, 5\n"
        "beq t1, t2, 1f\n"
        "li %[out], 0\n"
        "j 2f\n"
        "1:\n"
        "addi %[out], t3, 1\n"
        "2:\n"
        : [out] "=&r"(out)
        : [slot] "r"(&reg_cache_slot)
        : "t0", "t1", "t2", "t3", "memory");

    return out;
}

/* Check W-form sign extension, M-helper inputs, and ignored writes to x0. */
static uint64_t word_and_m_sequence(void)
{
    uint64_t out;
    const uint64_t pos = 0x1000000000000001ull;

    asm volatile(
        "li t0, 0x7fffffff\n"
        "addiw t0, t0, 1\n"
        "li t1, -256\n"
        "mulh t2, t1, %[pos]\n"
        "add t3, t0, t2\n"
        "addi zero, t3, 99\n"
        "add %[out], zero, t3\n"
        : [out] "=&r"(out)
        : [pos] "r"(pos)
        : "t0", "t1", "t2", "t3", "memory");

    return out;
}

/* Group behaviours that become fragile when guest registers live in host regs. */
static void test_register_cache_edges(void)
{
    check(spill_sequence() == 46ull);
    check(store_and_branch_sequence() == 48ull);
    check(word_and_m_sequence() == 0xffffffff7fffffefull);
    check(reg_cache_slot == 42ull);
}

#endif

/* Keep the source buildable outside RV64 while exercising the RV64-only path. */
int main(void)
{
#if defined(__riscv) && __riscv_xlen == 64
    test_register_cache_edges();
#endif

    return 0;
}
