#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 32

#include <stdint.h>

enum
{
    MSTATUS_FS_SHIFT = 13,
};

#define MSTATUS_FS_MASK ((uintptr_t)3u << MSTATUS_FS_SHIFT)
#define MSTATUS_FS_INITIAL ((uintptr_t)1u << MSTATUS_FS_SHIFT)

volatile uint32_t rv32_fpu_jit_trap_count = 0;

/*
 * This integer-only handler turns missing interpreter support into a bounded
 * assertion failure during the TDD RED run. A completed implementation never
 * enters it.
 */
asm(
    ".section .text\n"
    ".align 2\n"
    ".option push\n"
    ".option norvc\n"
    ".globl rv32_fpu_jit_trap_handler\n"
    ".type rv32_fpu_jit_trap_handler, @function\n"
    "rv32_fpu_jit_trap_handler:\n"
    "  la t0, rv32_fpu_jit_trap_count\n"
    "  lw t1, 0(t0)\n"
    "  addi t1, t1, 1\n"
    "  sw t1, 0(t0)\n"
    "  csrr t1, mepc\n"
    "  addi t1, t1, 4\n"
    "  csrw mepc, t1\n"
    "  mret\n"
    ".size rv32_fpu_jit_trap_handler, .-rv32_fpu_jit_trap_handler\n"
    ".option pop\n");

extern void rv32_fpu_jit_trap_handler(void);

/*
 * The first helper starts with an FP instruction. The second updates an
 * integer counter before reaching FP. Together they prove that an RV32 JIT
 * side exit resumes at the exact unsupported instruction: a wrong PC would
 * miss or repeat one of the visible counter updates.
 */
asm(
    ".section .text\n"
    ".align 2\n"
    ".option push\n"
    ".option norvc\n"
    ".option arch, +f\n"

    ".globl rv32_fpu_at_block_entry\n"
    ".type rv32_fpu_at_block_entry, @function\n"
    "rv32_fpu_at_block_entry:\n"
    "  fmv.w.x f0, a0\n"
    "  fadd.s f0, f0, f0, rne\n"
    "  lw t0, 0(a1)\n"
    "  addi t0, t0, 1\n"
    "  sw t0, 0(a1)\n"
    "  fmv.x.w a0, f0\n"
    "  ret\n"
    ".size rv32_fpu_at_block_entry, .-rv32_fpu_at_block_entry\n"

    ".globl rv32_fpu_after_integer_prefix\n"
    ".type rv32_fpu_after_integer_prefix, @function\n"
    "rv32_fpu_after_integer_prefix:\n"
    "  lw t0, 0(a1)\n"
    "  addi t0, t0, 1\n"
    "  sw t0, 0(a1)\n"
    "  fmv.w.x f0, a0\n"
    "  fadd.s f0, f0, f0, rne\n"
    "  fmv.x.w a0, f0\n"
    "  ret\n"
    ".size rv32_fpu_after_integer_prefix, .-rv32_fpu_after_integer_prefix\n"

    ".option pop\n");

extern uint32_t rv32_fpu_at_block_entry(uint32_t input,
                                        uint32_t *counter);
extern uint32_t rv32_fpu_after_integer_prefix(uint32_t input,
                                              uint32_t *counter);

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

static uintptr_t read_mtvec(void)
{
    uintptr_t value;
    asm volatile("csrr %0, mtvec" : "=r"(value));
    return value;
}

static void write_mtvec(uintptr_t value)
{
    asm volatile("csrw mtvec, %0" : : "r"(value) : "memory");
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
    const uint32_t one_point_five = UINT32_C(0x3fc00000);
    const uint32_t three = UINT32_C(0x40400000);
    uint32_t entry_counter = 0;
    uint32_t prefix_counter = 0;

    check(rv32_fpu_at_block_entry(one_point_five, &entry_counter) == three);
    check(entry_counter == 1);

    check(rv32_fpu_after_integer_prefix(one_point_five,
                                       &prefix_counter) == three);
    check(prefix_counter == 1);
    check(rv32_fpu_jit_trap_count == 0);
}

#endif

int main(void)
{
#if defined(__riscv) && __riscv_xlen == 32
    const uintptr_t old_mstatus = enable_initial_fp_state();
    const uintptr_t old_mtvec = read_mtvec();

    write_mtvec((uintptr_t)rv32_fpu_jit_trap_handler);
    rv32_fpu_jit_trap_count = 0;
    test_fpu_jit_fallback_boundaries();
    write_mstatus(old_mstatus);
    write_mtvec(old_mtvec);
#endif

    return 0;
}
