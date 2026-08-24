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
 * Keep the C ABI integer-only for this first FPU milestone.  Each helper moves
 * raw IEEE-754 bits from integer argument registers into explicit FPRs, performs
 * one architectural operation, then returns the raw result through a0.
 *
 * The local architecture options let these helpers assemble while the wider
 * AM/Navy build deliberately remains rv64im/lp64.  That prevents one focused
 * emulator test from silently changing the ABI of every linked library.
 */
asm(".section .text\n"
    ".align 2\n"
    ".option push\n"
    ".option norvc\n"
    ".option arch, +f\n"
    ".option arch, +d\n"

    ".globl rv64_fpu_add_s_boxed\n"
    ".type rv64_fpu_add_s_boxed, @function\n"
    "rv64_fpu_add_s_boxed:\n"
    "  fmv.w.x f0, a0\n"
    "  fmv.w.x f1, a1\n"
    "  fadd.s f0, f0, f1, rne\n"
    "  fmv.x.d a0, f0\n"
    "  ret\n"
    ".size rv64_fpu_add_s_boxed, .-rv64_fpu_add_s_boxed\n"

    ".globl rv64_fpu_add_d_bits\n"
    ".type rv64_fpu_add_d_bits, @function\n"
    "rv64_fpu_add_d_bits:\n"
    "  fmv.d.x f0, a0\n"
    "  fmv.d.x f1, a1\n"
    "  fadd.d f0, f0, f1, rne\n"
    "  fmv.x.d a0, f0\n"
    "  ret\n"
    ".size rv64_fpu_add_d_bits, .-rv64_fpu_add_d_bits\n"

    ".globl rv64_fpu_roundtrip_w\n"
    ".type rv64_fpu_roundtrip_w, @function\n"
    "rv64_fpu_roundtrip_w:\n"
    "  flw f0, 0(a0)\n"
    "  fsw f0, 0(a1)\n"
    "  fmv.x.d a0, f0\n"
    "  ret\n"
    ".size rv64_fpu_roundtrip_w, .-rv64_fpu_roundtrip_w\n"

    ".globl rv64_fpu_roundtrip_d\n"
    ".type rv64_fpu_roundtrip_d, @function\n"
    "rv64_fpu_roundtrip_d:\n"
    "  fld f0, 0(a0)\n"
    "  fsd f0, 0(a1)\n"
    "  fmv.x.d a0, f0\n"
    "  ret\n"
    ".size rv64_fpu_roundtrip_d, .-rv64_fpu_roundtrip_d\n"

    ".option pop\n");

extern uint64_t rv64_fpu_add_s_boxed(uint32_t lhs, uint32_t rhs);
extern uint64_t rv64_fpu_add_d_bits(uint64_t lhs, uint64_t rhs);
extern uint64_t rv64_fpu_roundtrip_w(const uint32_t *src, uint32_t *dst);
extern uint64_t rv64_fpu_roundtrip_d(const uint64_t *src, uint64_t *dst);

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
    const uintptr_t next = (old & ~MSTATUS_FS_MASK) | MSTATUS_FS_INITIAL;

    write_mstatus(next);
    return old;
}

static void test_basic_single_and_double_arithmetic(void)
{
    /*
     * 1.5 + 2.25 = 3.75.  The single result must also have all upper 32 bits
     * set, proving that a narrow result is NaN-boxed in an FLEN=64 register.
     */
    check(rv64_fpu_add_s_boxed(UINT32_C(0x3fc00000), UINT32_C(0x40100000)) == UINT64_C(0xffffffff40700000));
    check(rv64_fpu_add_d_bits(UINT64_C(0x3ff8000000000000), UINT64_C(0x4002000000000000)) == UINT64_C(0x400e000000000000));
}

static void test_load_store_raw_bit_preservation(void)
{
    /*
     * Loads and stores are transfer operations: they must preserve NaN payload
     * bits instead of replacing them with the computational canonical NaN.
     */
    const uint32_t single_snan = UINT32_C(0x7f800123);
    const uint64_t double_snan = UINT64_C(0x7ff0000000000123);
    uint32_t single_out = 0;
    uint64_t double_out = 0;

    check(rv64_fpu_roundtrip_w(&single_snan, &single_out) == (UINT64_C(0xffffffff00000000) | single_snan));
    check(single_out == single_snan);

    check(rv64_fpu_roundtrip_d(&double_snan, &double_out) == double_snan);
    check(double_out == double_snan);
}

#endif

int main(void)
{
#if defined(__riscv) && __riscv_xlen == 64
    const uintptr_t old_mstatus = enable_initial_fp_state();

    test_basic_single_and_double_arithmetic();
    test_load_store_raw_bit_preservation();

    write_mstatus(old_mstatus);
#endif

    return 0;
}
