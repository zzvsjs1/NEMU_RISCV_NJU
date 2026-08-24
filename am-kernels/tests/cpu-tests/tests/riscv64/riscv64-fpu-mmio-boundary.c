#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 64

#include <stdint.h>

enum
{
    MSTATUS_FS_SHIFT = 13,
};

#define MSTATUS_FS_MASK ((uintptr_t)3u << MSTATUS_FS_SHIFT)
#define MSTATUS_FS_INITIAL ((uintptr_t)1u << MSTATUS_FS_SHIFT)
#define NEMU_VGACTL_TEST_MMIO ((uintptr_t)0xa0000108ull)

/*
 * Exercise every FP memory operation through the conservative Bare-MMIO arm.
 * The first FSD receives the one-shot outer-boundary injection. FLW must box
 * the low backing word, FLD must preserve the full payload, and the final FSW
 * changes only the low word.
 */
asm(".section .text\n"
    ".align 2\n"
    ".option push\n"
    ".option norvc\n"
    ".option arch, +f\n"
    ".option arch, +d\n"
    ".globl rv64_fpu_mmio_boundary_all\n"
    ".type rv64_fpu_mmio_boundary_all, @function\n"
    "rv64_fpu_mmio_boundary_all:\n"
    "  fmv.d.x f0, a1\n"
    "  fsd f0, 0(a0)\n"
    "  flw f1, 0(a0)\n"
    "  fld f2, 0(a0)\n"
    "  fmv.x.d t0, f1\n"
    "  sd t0, 0(a3)\n"
    "  fmv.x.d t0, f2\n"
    "  sd t0, 8(a3)\n"
    "  fmv.w.x f3, a2\n"
    "  fsw f3, 0(a0)\n"
    "  addi a0, zero, 7\n"
    "  ret\n"
    ".size rv64_fpu_mmio_boundary_all, "
    ".-rv64_fpu_mmio_boundary_all\n"
    ".option pop\n");

extern uint64_t rv64_fpu_mmio_boundary_all(uintptr_t, uint64_t, uint32_t, uint64_t observed[2]);

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

#endif

int main(void)
{
#if defined(__riscv) && __riscv_xlen == 64
    const uintptr_t old_mstatus = read_mstatus();
    const uint64_t double_pattern = UINT64_C(0x13579bdf2468ace0);
    const uint32_t final_word = UINT32_C(0x5a17c3e9);
    const uint64_t boxed_word = UINT64_C(0xffffffff2468ace0);
    uint64_t observed[2] = {0};
    volatile uint64_t *const scratch = (volatile uint64_t *)NEMU_VGACTL_TEST_MMIO;

    write_mstatus((old_mstatus & ~MSTATUS_FS_MASK) | MSTATUS_FS_INITIAL);
    check(rv64_fpu_mmio_boundary_all(NEMU_VGACTL_TEST_MMIO, double_pattern, final_word, observed) == 7);
    check(observed[0] == boxed_word);
    check(observed[1] == double_pattern);
    check(*scratch == ((double_pattern & UINT64_C(0xffffffff00000000)) | final_word));
    write_mstatus(old_mstatus);
#endif

    return 0;
}
