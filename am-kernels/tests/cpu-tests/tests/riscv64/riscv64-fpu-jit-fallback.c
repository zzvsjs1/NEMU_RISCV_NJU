#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 64

#include <stdint.h>

enum
{
    MSTATUS_FS_SHIFT = 13,
};

#define MSTATUS_FS_MASK ((uintptr_t)3u << MSTATUS_FS_SHIFT)
#define MSTATUS_FS_INITIAL ((uintptr_t)1u << MSTATUS_FS_SHIFT)

/*
 * The AM build deliberately remains rv64im/lp64.  Local architecture options
 * permit explicit F/D instructions without changing the ABI or asking the C
 * compiler to allocate floating-point registers.
 *
 * The first helper starts with an FP instruction.  The second performs a
 * visible integer counter update before reaching FP.  Together they exercise
 * both JIT cases: an unsupported instruction at block entry, and an unsupported
 * instruction after a translated prefix.  A fallback that resumes at the wrong
 * PC would either miss or repeat a counter update.
 */
asm(
    ".section .text\n"
    ".align 2\n"
    ".option push\n"
    ".option norvc\n"
    ".option arch, +f\n"
    ".option arch, +d\n"

    ".globl rv64_fpu_at_block_entry\n"
    ".type rv64_fpu_at_block_entry, @function\n"
    "rv64_fpu_at_block_entry:\n"
    "  fmv.d.x f0, a0\n"
    "  fadd.d f0, f0, f0, rne\n"
    "  ld t0, 0(a1)\n"
    "  addi t0, t0, 1\n"
    "  sd t0, 0(a1)\n"
    "  fmv.x.d a0, f0\n"
    "  ret\n"
    ".size rv64_fpu_at_block_entry, .-rv64_fpu_at_block_entry\n"

    ".globl rv64_fpu_after_integer_prefix\n"
    ".type rv64_fpu_after_integer_prefix, @function\n"
    "rv64_fpu_after_integer_prefix:\n"
    "  ld t0, 0(a1)\n"
    "  addi t0, t0, 1\n"
    "  sd t0, 0(a1)\n"
    "  fmv.d.x f0, a0\n"
    "  fadd.d f0, f0, f0, rne\n"
    "  fmv.x.d a0, f0\n"
    "  ret\n"
    ".size rv64_fpu_after_integer_prefix, .-rv64_fpu_after_integer_prefix\n"

    ".option pop\n");

extern uint64_t rv64_fpu_at_block_entry(uint64_t input,
                                         uint64_t *counter);
extern uint64_t rv64_fpu_after_integer_prefix(uint64_t input,
                                              uint64_t *counter);

static uintptr_t read_mstatus(void)
{
    uintptr_t value;
    asm volatile("csrr %0, mstatus" : "=r"(value));
    return value;
}

static void write_mstatus(uintptr_t value)
{
    asm volatile("csrw mstatus, %0" : : "r"(value) : "memory");
}

static uintptr_t enable_initial_fp_state(void)
{
    const uintptr_t old = read_mstatus();
    const uintptr_t next =
        (old & ~MSTATUS_FS_MASK) | MSTATUS_FS_INITIAL;

    write_mstatus(next);
    return old;
}

static void test_fpu_jit_fallback_boundaries(void)
{
    const uint64_t one_point_five = UINT64_C(0x3ff8000000000000);
    const uint64_t three = UINT64_C(0x4008000000000000);
    uint64_t entry_counter = 0;
    uint64_t prefix_counter = 0;

    check(rv64_fpu_at_block_entry(one_point_five, &entry_counter) == three);
    check(entry_counter == 1);

    check(rv64_fpu_after_integer_prefix(one_point_five,
                                        &prefix_counter) == three);
    check(prefix_counter == 1);
}

#endif

int main(void)
{
#if defined(__riscv) && __riscv_xlen == 64
    const uintptr_t old_mstatus = enable_initial_fp_state();

    test_fpu_jit_fallback_boundaries();

    write_mstatus(old_mstatus);
#endif

    return 0;
}
