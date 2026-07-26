#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 64

#include <stdint.h>

#define MSTATUS_FS_MASK ((uintptr_t)3u << 13)
#define MSTATUS_FS_INITIAL ((uintptr_t)1u << 13)

#define FP_ASM_BINARY_S(name, instruction) \
    ".globl " #name "\n" \
    ".type " #name ", @function\n" \
    #name ":\n" \
    "  fmv.w.x f0, a0\n" \
    "  fmv.w.x f1, a1\n" \
    "  " #instruction " f0, f0, f1, rne\n" \
    "  fmv.x.d a0, f0\n" \
    "  ret\n" \
    ".size " #name ", .-" #name "\n"

#define FP_ASM_BINARY_D(name, instruction) \
    ".globl " #name "\n" \
    ".type " #name ", @function\n" \
    #name ":\n" \
    "  fmv.d.x f0, a0\n" \
    "  fmv.d.x f1, a1\n" \
    "  " #instruction " f0, f0, f1, rne\n" \
    "  fmv.x.d a0, f0\n" \
    "  ret\n" \
    ".size " #name ", .-" #name "\n"

#define FP_ASM_UNARY_S(name, instruction) \
    ".globl " #name "\n" \
    ".type " #name ", @function\n" \
    #name ":\n" \
    "  fmv.w.x f0, a0\n" \
    "  " #instruction " f0, f0, rne\n" \
    "  fmv.x.d a0, f0\n" \
    "  ret\n" \
    ".size " #name ", .-" #name "\n"

#define FP_ASM_UNARY_D(name, instruction) \
    ".globl " #name "\n" \
    ".type " #name ", @function\n" \
    #name ":\n" \
    "  fmv.d.x f0, a0\n" \
    "  " #instruction " f0, f0, rne\n" \
    "  fmv.x.d a0, f0\n" \
    "  ret\n" \
    ".size " #name ", .-" #name "\n"

#define FP_ASM_FMA_S(name, instruction) \
    ".globl " #name "\n" \
    ".type " #name ", @function\n" \
    #name ":\n" \
    "  fmv.w.x f0, a0\n" \
    "  fmv.w.x f1, a1\n" \
    "  fmv.w.x f2, a2\n" \
    "  " #instruction " f0, f0, f1, f2, rne\n" \
    "  fmv.x.d a0, f0\n" \
    "  ret\n" \
    ".size " #name ", .-" #name "\n"

#define FP_ASM_FMA_D(name, instruction) \
    ".globl " #name "\n" \
    ".type " #name ", @function\n" \
    #name ":\n" \
    "  fmv.d.x f0, a0\n" \
    "  fmv.d.x f1, a1\n" \
    "  fmv.d.x f2, a2\n" \
    "  " #instruction " f0, f0, f1, f2, rne\n" \
    "  fmv.x.d a0, f0\n" \
    "  ret\n" \
    ".size " #name ", .-" #name "\n"

/*
 * All helpers keep raw values in the integer calling convention.  This allows
 * the architectural F/D execution tests to coexist with AM's intentional lp64
 * soft-float ABI until full operating-system FP context switching is added.
 */
asm(
    ".section .text\n"
    ".align 2\n"
    ".option push\n"
    ".option norvc\n"
    ".option arch, +f\n"
    ".option arch, +d\n"

    FP_ASM_BINARY_S(rv64_fp_add_s, fadd.s)
    FP_ASM_BINARY_S(rv64_fp_sub_s, fsub.s)
    FP_ASM_BINARY_S(rv64_fp_mul_s, fmul.s)
    FP_ASM_BINARY_S(rv64_fp_div_s, fdiv.s)
    FP_ASM_UNARY_S(rv64_fp_sqrt_s, fsqrt.s)
    FP_ASM_BINARY_D(rv64_fp_add_d, fadd.d)
    FP_ASM_BINARY_D(rv64_fp_sub_d, fsub.d)
    FP_ASM_BINARY_D(rv64_fp_mul_d, fmul.d)
    FP_ASM_BINARY_D(rv64_fp_div_d, fdiv.d)
    FP_ASM_UNARY_D(rv64_fp_sqrt_d, fsqrt.d)

    FP_ASM_FMA_S(rv64_fp_madd_s, fmadd.s)
    FP_ASM_FMA_S(rv64_fp_msub_s, fmsub.s)
    FP_ASM_FMA_S(rv64_fp_nmsub_s, fnmsub.s)
    FP_ASM_FMA_S(rv64_fp_nmadd_s, fnmadd.s)
    FP_ASM_FMA_D(rv64_fp_madd_d, fmadd.d)
    FP_ASM_FMA_D(rv64_fp_msub_d, fmsub.d)
    FP_ASM_FMA_D(rv64_fp_nmsub_d, fnmsub.d)
    FP_ASM_FMA_D(rv64_fp_nmadd_d, fnmadd.d)

    ".globl rv64_fp_cvt_w_s_dyn\n"
    ".type rv64_fp_cvt_w_s_dyn, @function\n"
    "rv64_fp_cvt_w_s_dyn:\n"
    "  fmv.w.x f0, a0\n"
    "  fcvt.w.s a0, f0, dyn\n"
    "  ret\n"
    ".size rv64_fp_cvt_w_s_dyn, .-rv64_fp_cvt_w_s_dyn\n"

    ".globl rv64_fp_cvt_wu_s\n"
    ".type rv64_fp_cvt_wu_s, @function\n"
    "rv64_fp_cvt_wu_s:\n"
    "  fmv.w.x f0, a0\n"
    "  fcvt.wu.s a0, f0, rtz\n"
    "  ret\n"
    ".size rv64_fp_cvt_wu_s, .-rv64_fp_cvt_wu_s\n"

    ".globl rv64_fp_cvt_w_d\n"
    ".type rv64_fp_cvt_w_d, @function\n"
    "rv64_fp_cvt_w_d:\n"
    "  fmv.d.x f0, a0\n"
    "  fcvt.w.d a0, f0, rne\n"
    "  ret\n"
    ".size rv64_fp_cvt_w_d, .-rv64_fp_cvt_w_d\n"

    ".globl rv64_fp_cvt_wu_d\n"
    ".type rv64_fp_cvt_wu_d, @function\n"
    "rv64_fp_cvt_wu_d:\n"
    "  fmv.d.x f0, a0\n"
    "  fcvt.wu.d a0, f0, rne\n"
    "  ret\n"
    ".size rv64_fp_cvt_wu_d, .-rv64_fp_cvt_wu_d\n"

    ".globl rv64_fp_cvt_l_s\n"
    ".type rv64_fp_cvt_l_s, @function\n"
    "rv64_fp_cvt_l_s:\n"
    "  fmv.w.x f0, a0\n"
    "  fcvt.l.s a0, f0, rne\n"
    "  ret\n"
    ".size rv64_fp_cvt_l_s, .-rv64_fp_cvt_l_s\n"

    ".globl rv64_fp_cvt_lu_s\n"
    ".type rv64_fp_cvt_lu_s, @function\n"
    "rv64_fp_cvt_lu_s:\n"
    "  fmv.w.x f0, a0\n"
    "  fcvt.lu.s a0, f0, rne\n"
    "  ret\n"
    ".size rv64_fp_cvt_lu_s, .-rv64_fp_cvt_lu_s\n"

    ".globl rv64_fp_cvt_l_d\n"
    ".type rv64_fp_cvt_l_d, @function\n"
    "rv64_fp_cvt_l_d:\n"
    "  fmv.d.x f0, a0\n"
    "  fcvt.l.d a0, f0, rne\n"
    "  ret\n"
    ".size rv64_fp_cvt_l_d, .-rv64_fp_cvt_l_d\n"

    ".globl rv64_fp_cvt_lu_d\n"
    ".type rv64_fp_cvt_lu_d, @function\n"
    "rv64_fp_cvt_lu_d:\n"
    "  fmv.d.x f0, a0\n"
    "  fcvt.lu.d a0, f0, rne\n"
    "  ret\n"
    ".size rv64_fp_cvt_lu_d, .-rv64_fp_cvt_lu_d\n"

    ".globl rv64_fp_cvt_s_w\n"
    ".type rv64_fp_cvt_s_w, @function\n"
    "rv64_fp_cvt_s_w:\n"
    "  fcvt.s.w f0, a0, rne\n"
    "  fmv.x.d a0, f0\n"
    "  ret\n"
    ".size rv64_fp_cvt_s_w, .-rv64_fp_cvt_s_w\n"

    ".globl rv64_fp_cvt_s_wu\n"
    ".type rv64_fp_cvt_s_wu, @function\n"
    "rv64_fp_cvt_s_wu:\n"
    "  fcvt.s.wu f0, a0, rne\n"
    "  fmv.x.d a0, f0\n"
    "  ret\n"
    ".size rv64_fp_cvt_s_wu, .-rv64_fp_cvt_s_wu\n"

    ".globl rv64_fp_cvt_s_l\n"
    ".type rv64_fp_cvt_s_l, @function\n"
    "rv64_fp_cvt_s_l:\n"
    "  fcvt.s.l f0, a0, rne\n"
    "  fmv.x.d a0, f0\n"
    "  ret\n"
    ".size rv64_fp_cvt_s_l, .-rv64_fp_cvt_s_l\n"

    ".globl rv64_fp_cvt_s_lu\n"
    ".type rv64_fp_cvt_s_lu, @function\n"
    "rv64_fp_cvt_s_lu:\n"
    "  fcvt.s.lu f0, a0, rne\n"
    "  fmv.x.d a0, f0\n"
    "  ret\n"
    ".size rv64_fp_cvt_s_lu, .-rv64_fp_cvt_s_lu\n"

    ".globl rv64_fp_cvt_d_w\n"
    ".type rv64_fp_cvt_d_w, @function\n"
    "rv64_fp_cvt_d_w:\n"
    "  fcvt.d.w f0, a0\n"
    "  fmv.x.d a0, f0\n"
    "  ret\n"
    ".size rv64_fp_cvt_d_w, .-rv64_fp_cvt_d_w\n"

    ".globl rv64_fp_cvt_d_wu\n"
    ".type rv64_fp_cvt_d_wu, @function\n"
    "rv64_fp_cvt_d_wu:\n"
    "  fcvt.d.wu f0, a0\n"
    "  fmv.x.d a0, f0\n"
    "  ret\n"
    ".size rv64_fp_cvt_d_wu, .-rv64_fp_cvt_d_wu\n"

    ".globl rv64_fp_cvt_d_l\n"
    ".type rv64_fp_cvt_d_l, @function\n"
    "rv64_fp_cvt_d_l:\n"
    "  fcvt.d.l f0, a0, rne\n"
    "  fmv.x.d a0, f0\n"
    "  ret\n"
    ".size rv64_fp_cvt_d_l, .-rv64_fp_cvt_d_l\n"

    ".globl rv64_fp_cvt_d_lu\n"
    ".type rv64_fp_cvt_d_lu, @function\n"
    "rv64_fp_cvt_d_lu:\n"
    "  fcvt.d.lu f0, a0, rne\n"
    "  fmv.x.d a0, f0\n"
    "  ret\n"
    ".size rv64_fp_cvt_d_lu, .-rv64_fp_cvt_d_lu\n"

    ".globl rv64_fp_cvt_s_d\n"
    ".type rv64_fp_cvt_s_d, @function\n"
    "rv64_fp_cvt_s_d:\n"
    "  fmv.d.x f0, a0\n"
    "  fcvt.s.d f0, f0, rne\n"
    "  fmv.x.d a0, f0\n"
    "  ret\n"
    ".size rv64_fp_cvt_s_d, .-rv64_fp_cvt_s_d\n"

    ".globl rv64_fp_cvt_d_s\n"
    ".type rv64_fp_cvt_d_s, @function\n"
    "rv64_fp_cvt_d_s:\n"
    "  fmv.w.x f0, a0\n"
    "  fcvt.d.s f0, f0\n"
    "  fmv.x.d a0, f0\n"
    "  ret\n"
    ".size rv64_fp_cvt_d_s, .-rv64_fp_cvt_d_s\n"

    ".option pop\n");

extern uint64_t rv64_fp_add_s(uint32_t, uint32_t);
extern uint64_t rv64_fp_sub_s(uint32_t, uint32_t);
extern uint64_t rv64_fp_mul_s(uint32_t, uint32_t);
extern uint64_t rv64_fp_div_s(uint32_t, uint32_t);
extern uint64_t rv64_fp_sqrt_s(uint32_t);
extern uint64_t rv64_fp_add_d(uint64_t, uint64_t);
extern uint64_t rv64_fp_sub_d(uint64_t, uint64_t);
extern uint64_t rv64_fp_mul_d(uint64_t, uint64_t);
extern uint64_t rv64_fp_div_d(uint64_t, uint64_t);
extern uint64_t rv64_fp_sqrt_d(uint64_t);
extern uint64_t rv64_fp_madd_s(uint32_t, uint32_t, uint32_t);
extern uint64_t rv64_fp_msub_s(uint32_t, uint32_t, uint32_t);
extern uint64_t rv64_fp_nmsub_s(uint32_t, uint32_t, uint32_t);
extern uint64_t rv64_fp_nmadd_s(uint32_t, uint32_t, uint32_t);
extern uint64_t rv64_fp_madd_d(uint64_t, uint64_t, uint64_t);
extern uint64_t rv64_fp_msub_d(uint64_t, uint64_t, uint64_t);
extern uint64_t rv64_fp_nmsub_d(uint64_t, uint64_t, uint64_t);
extern uint64_t rv64_fp_nmadd_d(uint64_t, uint64_t, uint64_t);
extern uint64_t rv64_fp_cvt_w_s_dyn(uint32_t);
extern uint64_t rv64_fp_cvt_wu_s(uint32_t);
extern uint64_t rv64_fp_cvt_w_d(uint64_t);
extern uint64_t rv64_fp_cvt_wu_d(uint64_t);
extern uint64_t rv64_fp_cvt_l_s(uint32_t);
extern uint64_t rv64_fp_cvt_lu_s(uint32_t);
extern uint64_t rv64_fp_cvt_l_d(uint64_t);
extern uint64_t rv64_fp_cvt_lu_d(uint64_t);
extern uint64_t rv64_fp_cvt_s_w(uint64_t);
extern uint64_t rv64_fp_cvt_s_wu(uint64_t);
extern uint64_t rv64_fp_cvt_s_l(uint64_t);
extern uint64_t rv64_fp_cvt_s_lu(uint64_t);
extern uint64_t rv64_fp_cvt_d_w(uint64_t);
extern uint64_t rv64_fp_cvt_d_wu(uint64_t);
extern uint64_t rv64_fp_cvt_d_l(uint64_t);
extern uint64_t rv64_fp_cvt_d_lu(uint64_t);
extern uint64_t rv64_fp_cvt_s_d(uint64_t);
extern uint64_t rv64_fp_cvt_d_s(uint32_t);

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

static uintptr_t read_fflags(void)
{
    uintptr_t value;
    asm volatile("csrr %0, 0x001" : "=r"(value));
    return value;
}

static void write_fflags(uintptr_t value)
{
    asm volatile("csrw 0x001, %0" : : "r"(value) : "memory");
}

static void write_frm(uintptr_t value)
{
    asm volatile("csrw 0x002, %0" : : "r"(value) : "memory");
}

static uintptr_t enable_initial_fp_state(void)
{
    const uintptr_t old = read_mstatus();
    write_mstatus((old & ~MSTATUS_FS_MASK) | MSTATUS_FS_INITIAL);
    return old;
}

static void test_arithmetic_operations(void)
{
    check(rv64_fp_sub_s(UINT32_C(0x40700000), UINT32_C(0x40100000)) ==
          UINT64_C(0xffffffff3fc00000));
    check(rv64_fp_mul_s(UINT32_C(0x3fc00000), UINT32_C(0x40000000)) ==
          UINT64_C(0xffffffff40400000));
    check(rv64_fp_div_s(UINT32_C(0x40400000), UINT32_C(0x40000000)) ==
          UINT64_C(0xffffffff3fc00000));
    check(rv64_fp_sqrt_s(UINT32_C(0x40800000)) ==
          UINT64_C(0xffffffff40000000));

    check(rv64_fp_sub_d(UINT64_C(0x400e000000000000),
                       UINT64_C(0x4002000000000000)) ==
          UINT64_C(0x3ff8000000000000));
    check(rv64_fp_mul_d(UINT64_C(0x3ff8000000000000),
                       UINT64_C(0x4000000000000000)) ==
          UINT64_C(0x4008000000000000));
    check(rv64_fp_div_d(UINT64_C(0x4008000000000000),
                       UINT64_C(0x4000000000000000)) ==
          UINT64_C(0x3ff8000000000000));
    check(rv64_fp_sqrt_d(UINT64_C(0x4010000000000000)) ==
          UINT64_C(0x4000000000000000));
}

static void test_fused_operations(void)
{
    const uint32_t s_two = UINT32_C(0x40000000);
    const uint32_t s_three = UINT32_C(0x40400000);
    const uint32_t s_four = UINT32_C(0x40800000);
    const uint64_t d_two = UINT64_C(0x4000000000000000);
    const uint64_t d_three = UINT64_C(0x4008000000000000);
    const uint64_t d_four = UINT64_C(0x4010000000000000);

    check(rv64_fp_madd_s(s_two, s_three, s_four) ==
          UINT64_C(0xffffffff41200000));
    check(rv64_fp_msub_s(s_two, s_three, s_four) ==
          UINT64_C(0xffffffff40000000));
    check(rv64_fp_nmsub_s(s_two, s_three, s_four) ==
          UINT64_C(0xffffffffc0000000));
    check(rv64_fp_nmadd_s(s_two, s_three, s_four) ==
          UINT64_C(0xffffffffc1200000));

    check(rv64_fp_madd_d(d_two, d_three, d_four) ==
          UINT64_C(0x4024000000000000));
    check(rv64_fp_msub_d(d_two, d_three, d_four) ==
          UINT64_C(0x4000000000000000));
    check(rv64_fp_nmsub_d(d_two, d_three, d_four) ==
          UINT64_C(0xc000000000000000));
    check(rv64_fp_nmadd_d(d_two, d_three, d_four) ==
          UINT64_C(0xc024000000000000));

    /*
     * This product is just above one before subtracting one.  A fused FMSUB
     * retains the low product bits and returns 0x337ffffe; separate rounded
     * multiply and subtract operations would incorrectly return zero.
     */
    check(rv64_fp_msub_s(UINT32_C(0x3f800001),
                        UINT32_C(0x3f7fffff),
                        UINT32_C(0x3f800000)) ==
          UINT64_C(0xffffffff337ffffe));
}

static void test_rounding_and_accrued_flags(void)
{
    /*
     * 2.5 distinguishes ties-to-even (2) from ties-to-maximum-magnitude (3);
     * 1.5 would produce 2 in both modes and leave an RMM wiring bug hidden.
     */
    static const uint64_t expected[5] = {2, 2, 2, 3, 3};

    for (uintptr_t rm = 0; rm < 5; rm++)
    {
        write_fflags(0);
        write_frm(rm);
        check(rv64_fp_cvt_w_s_dyn(UINT32_C(0x40200000)) == expected[rm]);
        check((read_fflags() & 1u) != 0);
    }

    write_fflags(0);
    check(rv64_fp_div_s(UINT32_C(0x3f800000), 0) ==
          UINT64_C(0xffffffff7f800000));
    check(read_fflags() == UINT64_C(0x08));

    /* A later exact operation must not clear an earlier accrued DZ flag. */
    check(rv64_fp_mul_d(UINT64_C(0x4000000000000000),
                       UINT64_C(0x4008000000000000)) ==
          UINT64_C(0x4018000000000000));
    check(read_fflags() == UINT64_C(0x08));

    write_fflags(0);
    check(rv64_fp_sqrt_s(UINT32_C(0xbf800000)) ==
          UINT64_C(0xffffffff7fc00000));
    check(read_fflags() == UINT64_C(0x10));

    write_fflags(0);
    check(rv64_fp_mul_s(UINT32_C(0x7f7fffff), UINT32_C(0x40000000)) ==
          UINT64_C(0xffffffff7f800000));
    check(read_fflags() == UINT64_C(0x05));
}

static void test_nan_underflow_and_invalid_conversion_edges(void)
{
    /*
     * Arithmetic canonicalises NaNs.  A signalling NaN raises NV, whereas a
     * quiet NaN produces the same canonical result without an exception.
     */
    write_fflags(0);
    check(rv64_fp_add_s(UINT32_C(0x7f800001),
                       UINT32_C(0x3f800000)) ==
          UINT64_C(0xffffffff7fc00000));
    check(read_fflags() == UINT64_C(0x10));

    write_fflags(0);
    check(rv64_fp_add_d(UINT64_C(0x7ff8000000000123),
                       UINT64_C(0x3ff0000000000000)) ==
          UINT64_C(0x7ff8000000000000));
    check(read_fflags() == 0);

    /*
     * RISC-V requires NV for infinity multiplied by zero even when the fused
     * addend is already a quiet NaN.
     */
    write_fflags(0);
    check(rv64_fp_madd_s(UINT32_C(0x7f800000),
                        UINT32_C(0x00000000),
                        UINT32_C(0x7fc00000)) ==
          UINT64_C(0xffffffff7fc00000));
    check(read_fflags() == UINT64_C(0x10));

    /*
     * Tininess is detected after rounding.  The first result rounds from the
     * subnormal range to the smallest normal value: before-rounding detection
     * would raise UF|NX, whereas RISC-V's after-rounding rule raises only NX.
     */
    write_fflags(0);
    check(rv64_fp_mul_s(UINT32_C(0x007ffff0),
                       UINT32_C(0x3f800010)) ==
          UINT64_C(0xffffffff00800000));
    check(read_fflags() == UINT64_C(0x01));

    /* An exactly representable subnormal result raises no exception flags. */
    write_fflags(0);
    check(rv64_fp_mul_s(UINT32_C(0x00800000),
                       UINT32_C(0x3f000000)) ==
          UINT64_C(0xffffffff00400000));
    check(read_fflags() == 0);

    /* A tiny, inexact subnormal result raises both UF and NX. */
    write_fflags(0);
    check(rv64_fp_mul_s(UINT32_C(0x00800001),
                       UINT32_C(0x3f000000)) ==
          UINT64_C(0xffffffff00400000));
    check(read_fflags() == UINT64_C(0x03));

    /*
     * Invalid conversions clip to the RISC-V-defined endpoint.  W and WU
     * results are then sign-extended to XLEN, including the unsigned all-ones
     * word result.
     */
    write_fflags(0);
    check(rv64_fp_cvt_w_d(UINT64_C(0x7ff0000000000000)) ==
          UINT64_C(0x000000007fffffff));
    check(read_fflags() == UINT64_C(0x10));

    write_fflags(0);
    check(rv64_fp_cvt_wu_d(UINT64_C(0x7ff0000000000000)) ==
          UINT64_MAX);
    check(read_fflags() == UINT64_C(0x10));

    write_fflags(0);
    check(rv64_fp_cvt_l_s(UINT32_C(0x7fc00000)) ==
          UINT64_C(0x7fffffffffffffff));
    check(read_fflags() == UINT64_C(0x10));

    write_fflags(0);
    check(rv64_fp_cvt_lu_s(UINT32_C(0xbf800000)) == 0);
    check(read_fflags() == UINT64_C(0x10));
}

static void test_integer_and_cross_precision_conversions(void)
{
    check(rv64_fp_cvt_wu_s(UINT32_C(0x40400000)) == 3);
    check(rv64_fp_cvt_l_d(UINT64_C(0x4270000000000000)) ==
          UINT64_C(0x0000010000000000));
    check(rv64_fp_cvt_lu_d(UINT64_C(0x43e0000000000000)) ==
          UINT64_C(0x8000000000000000));

    check(rv64_fp_cvt_s_w(UINT64_C(0xfffffffffffffffe)) ==
          UINT64_C(0xffffffffc0000000));
    check(rv64_fp_cvt_s_wu(UINT64_C(0x00000000ffffffff)) ==
          UINT64_C(0xffffffff4f800000));
    check(rv64_fp_cvt_s_l(UINT64_C(0xfffffffffffffffe)) ==
          UINT64_C(0xffffffffc0000000));
    check(rv64_fp_cvt_s_lu(UINT64_C(0x0000000100000000)) ==
          UINT64_C(0xffffffff4f800000));

    check(rv64_fp_cvt_d_w(UINT64_C(0xfffffffffffffffe)) ==
          UINT64_C(0xc000000000000000));
    check(rv64_fp_cvt_d_wu(UINT64_C(0x00000000ffffffff)) ==
          UINT64_C(0x41efffffffe00000));
    check(rv64_fp_cvt_d_l(UINT64_C(0xfffffffffffffffe)) ==
          UINT64_C(0xc000000000000000));
    check(rv64_fp_cvt_d_lu(UINT64_C(0x0000000100000000)) ==
          UINT64_C(0x41f0000000000000));

    check(rv64_fp_cvt_s_d(UINT64_C(0x3ff8000000000000)) ==
          UINT64_C(0xffffffff3fc00000));
    check(rv64_fp_cvt_d_s(UINT32_C(0x3fc00000)) ==
          UINT64_C(0x3ff8000000000000));
}

#endif

int main(void)
{
#if defined(__riscv) && __riscv_xlen == 64
    const uintptr_t old_mstatus = enable_initial_fp_state();

    test_arithmetic_operations();
    test_fused_operations();
    test_rounding_and_accrued_flags();
    test_nan_underflow_and_invalid_conversion_edges();
    test_integer_and_cross_precision_conversions();

    write_mstatus(old_mstatus);
#endif

    return 0;
}
