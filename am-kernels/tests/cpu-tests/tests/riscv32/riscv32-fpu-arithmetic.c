#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 32

#include <stdint.h>

#define MSTATUS_FS_MASK ((uintptr_t)3u << 13)
#define MSTATUS_FS_INITIAL ((uintptr_t)1u << 13)

#define FP_ASM_BINARY_S(name, instruction) \
    ".globl " #name "\n" \
    ".type " #name ", @function\n" #name ":\n" \
    "  fmv.w.x f0, a0\n" \
    "  fmv.w.x f1, a1\n" \
    "  " #instruction " f0, f0, f1, rne\n" \
    "  fmv.x.w a0, f0\n" \
    "  ret\n" \
    ".size " #name ", .-" #name "\n"

#define FP_ASM_UNARY_S(name, instruction) \
    ".globl " #name "\n" \
    ".type " #name ", @function\n" #name ":\n" \
    "  fmv.w.x f0, a0\n" \
    "  " #instruction " f0, f0, rne\n" \
    "  fmv.x.w a0, f0\n" \
    "  ret\n" \
    ".size " #name ", .-" #name "\n"

#define FP_ASM_FMA_S(name, instruction) \
    ".globl " #name "\n" \
    ".type " #name ", @function\n" #name ":\n" \
    "  fmv.w.x f0, a0\n" \
    "  fmv.w.x f1, a1\n" \
    "  fmv.w.x f2, a2\n" \
    "  " #instruction " f0, f0, f1, f2, rne\n" \
    "  fmv.x.w a0, f0\n" \
    "  ret\n" \
    ".size " #name ", .-" #name "\n"

#define FP_ASM_CVT_W_S_STATIC(name, rounding_mode) \
    ".globl " #name "\n" \
    ".type " #name ", @function\n" #name ":\n" \
    "  fmv.w.x f0, a0\n" \
    "  fcvt.w.s a0, f0, " #rounding_mode "\n" \
    "  ret\n" \
    ".size " #name ", .-" #name "\n"

/*
 * Keep every operand and result in integer argument registers.  This exercises
 * the architectural F instructions without changing AM's integer-only RV32 C
 * ABI or requiring the compiler to generate floating-point code.
 */
asm(".section .text\n"
    ".align 2\n"
    ".option push\n"
    ".option norvc\n"
    ".option arch,+f\n"

    FP_ASM_BINARY_S(rv32_fp_add_s, fadd.s) FP_ASM_BINARY_S(rv32_fp_sub_s, fsub.s) FP_ASM_BINARY_S(rv32_fp_mul_s, fmul.s)
        FP_ASM_BINARY_S(rv32_fp_div_s, fdiv.s) FP_ASM_UNARY_S(rv32_fp_sqrt_s, fsqrt.s)

            FP_ASM_FMA_S(rv32_fp_madd_s, fmadd.s) FP_ASM_FMA_S(rv32_fp_msub_s, fmsub.s) FP_ASM_FMA_S(rv32_fp_nmsub_s, fnmsub.s)
                FP_ASM_FMA_S(rv32_fp_nmadd_s, fnmadd.s)

                    ".globl rv32_fp_cvt_w_s_dyn\n"
                    ".type rv32_fp_cvt_w_s_dyn, @function\n"
                    "rv32_fp_cvt_w_s_dyn:\n"
                    "  fmv.w.x f0, a0\n"
                    "  fcvt.w.s a0, f0, dyn\n"
                    "  ret\n"
                    ".size rv32_fp_cvt_w_s_dyn, .-rv32_fp_cvt_w_s_dyn\n"

    FP_ASM_CVT_W_S_STATIC(rv32_fp_cvt_w_s_rne, rne) FP_ASM_CVT_W_S_STATIC(rv32_fp_cvt_w_s_rtz, rtz) FP_ASM_CVT_W_S_STATIC(rv32_fp_cvt_w_s_rdn, rdn)
        FP_ASM_CVT_W_S_STATIC(rv32_fp_cvt_w_s_rup, rup) FP_ASM_CVT_W_S_STATIC(rv32_fp_cvt_w_s_rmm, rmm)

            ".globl rv32_fp_cvt_wu_s\n"
            ".type rv32_fp_cvt_wu_s, @function\n"
            "rv32_fp_cvt_wu_s:\n"
            "  fmv.w.x f0, a0\n"
            "  fcvt.wu.s a0, f0, rtz\n"
            "  ret\n"
            ".size rv32_fp_cvt_wu_s, .-rv32_fp_cvt_wu_s\n"

            ".globl rv32_fp_cvt_s_w\n"
            ".type rv32_fp_cvt_s_w, @function\n"
            "rv32_fp_cvt_s_w:\n"
            "  fcvt.s.w f0, a0, rne\n"
            "  fmv.x.w a0, f0\n"
            "  ret\n"
            ".size rv32_fp_cvt_s_w, .-rv32_fp_cvt_s_w\n"

            ".globl rv32_fp_cvt_s_wu\n"
            ".type rv32_fp_cvt_s_wu, @function\n"
            "rv32_fp_cvt_s_wu:\n"
            "  fcvt.s.wu f0, a0, rne\n"
            "  fmv.x.w a0, f0\n"
            "  ret\n"
            ".size rv32_fp_cvt_s_wu, .-rv32_fp_cvt_s_wu\n"

            ".option pop\n");

extern uint32_t rv32_fp_add_s(uint32_t, uint32_t);
extern uint32_t rv32_fp_sub_s(uint32_t, uint32_t);
extern uint32_t rv32_fp_mul_s(uint32_t, uint32_t);
extern uint32_t rv32_fp_div_s(uint32_t, uint32_t);
extern uint32_t rv32_fp_sqrt_s(uint32_t);
extern uint32_t rv32_fp_madd_s(uint32_t, uint32_t, uint32_t);
extern uint32_t rv32_fp_msub_s(uint32_t, uint32_t, uint32_t);
extern uint32_t rv32_fp_nmsub_s(uint32_t, uint32_t, uint32_t);
extern uint32_t rv32_fp_nmadd_s(uint32_t, uint32_t, uint32_t);
extern uint32_t rv32_fp_cvt_w_s_dyn(uint32_t);
extern uint32_t rv32_fp_cvt_w_s_rne(uint32_t);
extern uint32_t rv32_fp_cvt_w_s_rtz(uint32_t);
extern uint32_t rv32_fp_cvt_w_s_rdn(uint32_t);
extern uint32_t rv32_fp_cvt_w_s_rup(uint32_t);
extern uint32_t rv32_fp_cvt_w_s_rmm(uint32_t);
extern uint32_t rv32_fp_cvt_wu_s(uint32_t);
extern uint32_t rv32_fp_cvt_s_w(uint32_t);
extern uint32_t rv32_fp_cvt_s_wu(uint32_t);

enum
{
    FFLAG_NX = 1u << 0,
    FFLAG_UF = 1u << 1,
    FFLAG_OF = 1u << 2,
    FFLAG_DZ = 1u << 3,
    FFLAG_NV = 1u << 4,
};

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
    write_fflags(0);

    check(rv32_fp_add_s(UINT32_C(0x3fc00000), UINT32_C(0x40000000)) == UINT32_C(0x40600000));
    check(rv32_fp_sub_s(UINT32_C(0x40700000), UINT32_C(0x40100000)) == UINT32_C(0x3fc00000));
    check(rv32_fp_mul_s(UINT32_C(0x3fc00000), UINT32_C(0x40000000)) == UINT32_C(0x40400000));
    check(rv32_fp_div_s(UINT32_C(0x40400000), UINT32_C(0x40000000)) == UINT32_C(0x3fc00000));
    check(rv32_fp_sqrt_s(UINT32_C(0x40800000)) == UINT32_C(0x40000000));
    check(read_fflags() == 0);
}

static void test_fused_operations(void)
{
    const uint32_t two = UINT32_C(0x40000000);
    const uint32_t three = UINT32_C(0x40400000);
    const uint32_t four = UINT32_C(0x40800000);

    write_fflags(0);

    check(rv32_fp_madd_s(two, three, four) == UINT32_C(0x41200000));
    check(rv32_fp_msub_s(two, three, four) == UINT32_C(0x40000000));
    check(rv32_fp_nmsub_s(two, three, four) == UINT32_C(0xc0000000));
    check(rv32_fp_nmadd_s(two, three, four) == UINT32_C(0xc1200000));
    check(read_fflags() == 0);

    /*
     * The exact product is just above one before subtracting one.  A fused
     * FMSUB retains its low product bits and returns 0x337ffffe; separate
     * rounded multiplication and subtraction would incorrectly return zero.
     */
    check(rv32_fp_msub_s(UINT32_C(0x3f800001), UINT32_C(0x3f7fffff), UINT32_C(0x3f800000)) == UINT32_C(0x337ffffe));
}

static void test_rounding_modes_and_accrued_flags(void)
{
    /*
     * No single input distinguishes every rounding mode.  These three inputs
     * form a unique result tuple for RNE, RTZ, RDN, RUP, and RMM respectively:
     * a positive tie, a negative tie, and a positive non-tie.
     */
    static const uint32_t input[3] = {
        UINT32_C(0x40200000), /* +2.5 */
        UINT32_C(0xc0200000), /* -2.5 */
        UINT32_C(0x40300000), /* +2.75 */
    };
    static const uint32_t expected[5][3] = {
        {2u, UINT32_C(0xfffffffe), 3u}, {2u, UINT32_C(0xfffffffe), 2u}, {2u, UINT32_C(0xfffffffd), 2u},
        {3u, UINT32_C(0xfffffffe), 3u}, {3u, UINT32_C(0xfffffffd), 3u},
    };

    for (uintptr_t rm = 0; rm < 5; rm++)
    {
        write_frm(rm);

        for (size_t case_index = 0; case_index < 3; case_index++)
        {
            write_fflags(0);
            check(rv32_fp_cvt_w_s_dyn(input[case_index]) == expected[rm][case_index]);
            check(read_fflags() == FFLAG_NX);
        }
    }

    /*
     * Dynamic rounding exercises frm, while these calls prove that every
     * non-dynamic instruction encoding reaches the same architectural mode.
     */
    write_fflags(0);
    check(rv32_fp_cvt_w_s_rne(UINT32_C(0x40200000)) == 2u);
    check(read_fflags() == FFLAG_NX);
    write_fflags(0);
    check(rv32_fp_cvt_w_s_rtz(UINT32_C(0x40300000)) == 2u);
    check(read_fflags() == FFLAG_NX);
    write_fflags(0);
    check(rv32_fp_cvt_w_s_rdn(UINT32_C(0xc0200000)) == UINT32_C(0xfffffffd));
    check(read_fflags() == FFLAG_NX);
    write_fflags(0);
    check(rv32_fp_cvt_w_s_rup(UINT32_C(0x40200000)) == 3u);
    check(read_fflags() == FFLAG_NX);
    write_fflags(0);
    check(rv32_fp_cvt_w_s_rmm(UINT32_C(0xc0200000)) == UINT32_C(0xfffffffd));
    check(read_fflags() == FFLAG_NX);

    write_fflags(0);
    check(rv32_fp_div_s(UINT32_C(0x3f800000), 0) == UINT32_C(0x7f800000));
    check(read_fflags() == FFLAG_DZ);

    /* A later exact operation must not clear the earlier accrued DZ flag. */
    check(rv32_fp_mul_s(UINT32_C(0x40000000), UINT32_C(0x40400000)) == UINT32_C(0x40c00000));
    check(read_fflags() == FFLAG_DZ);

    write_fflags(0);
    check(rv32_fp_sqrt_s(UINT32_C(0xbf800000)) == UINT32_C(0x7fc00000));
    check(read_fflags() == FFLAG_NV);

    write_fflags(0);
    check(rv32_fp_mul_s(UINT32_C(0x7f7fffff), UINT32_C(0x40000000)) == UINT32_C(0x7f800000));
    check(read_fflags() == (FFLAG_OF | FFLAG_NX));
}

static void test_nan_and_underflow_edges(void)
{
    /*
     * Arithmetic canonicalises NaNs.  A signalling NaN raises NV, whereas a
     * quiet NaN produces the same canonical result without an exception.
     */
    write_fflags(0);
    check(rv32_fp_add_s(UINT32_C(0x7f800001), UINT32_C(0x3f800000)) == UINT32_C(0x7fc00000));
    check(read_fflags() == FFLAG_NV);

    write_fflags(0);
    check(rv32_fp_add_s(UINT32_C(0x7fc00123), UINT32_C(0x3f800000)) == UINT32_C(0x7fc00000));
    check(read_fflags() == 0);

    /*
     * RISC-V requires NV for infinity multiplied by zero even when the fused
     * addend is already a quiet NaN.
     */
    write_fflags(0);
    check(rv32_fp_madd_s(UINT32_C(0x7f800000), UINT32_C(0x00000000), UINT32_C(0x7fc00000)) == UINT32_C(0x7fc00000));
    check(read_fflags() == FFLAG_NV);

    /*
     * Tininess is detected after rounding.  This value rounds from the
     * subnormal range to the smallest normal value, so only NX is raised.
     */
    write_fflags(0);
    check(rv32_fp_mul_s(UINT32_C(0x007ffff0), UINT32_C(0x3f800010)) == UINT32_C(0x00800000));
    check(read_fflags() == FFLAG_NX);

    /* An exactly representable subnormal result raises no exception flags. */
    write_fflags(0);
    check(rv32_fp_mul_s(UINT32_C(0x00800000), UINT32_C(0x3f000000)) == UINT32_C(0x00400000));
    check(read_fflags() == 0);

    /* A tiny, inexact subnormal result raises both UF and NX. */
    write_fflags(0);
    check(rv32_fp_mul_s(UINT32_C(0x00800001), UINT32_C(0x3f000000)) == UINT32_C(0x00400000));
    check(read_fflags() == (FFLAG_UF | FFLAG_NX));
}

static void test_w_and_wu_conversions(void)
{
    /*
     * Invalid conversions clip to the architecturally defined endpoint and
     * raise NV.  RV32 returns the complete W or WU result directly in rd.
     */
    write_fflags(0);
    check(rv32_fp_cvt_w_s_dyn(UINT32_C(0x7f800000)) == UINT32_C(0x7fffffff));
    check(read_fflags() == FFLAG_NV);

    write_fflags(0);
    check(rv32_fp_cvt_w_s_dyn(UINT32_C(0xff800000)) == UINT32_C(0x80000000));
    check(read_fflags() == FFLAG_NV);

    write_fflags(0);
    check(rv32_fp_cvt_w_s_dyn(UINT32_C(0x7fc00000)) == UINT32_C(0x7fffffff));
    check(read_fflags() == FFLAG_NV);

    write_fflags(0);
    check(rv32_fp_cvt_wu_s(UINT32_C(0x7f800000)) == UINT32_C(0xffffffff));
    check(read_fflags() == FFLAG_NV);

    write_fflags(0);
    check(rv32_fp_cvt_wu_s(UINT32_C(0xbf800000)) == 0);
    check(read_fflags() == FFLAG_NV);

    write_fflags(0);
    check(rv32_fp_cvt_wu_s(UINT32_C(0x40400000)) == 3);
    check(read_fflags() == 0);

    write_fflags(0);
    check(rv32_fp_cvt_s_w(UINT32_C(0xfffffffe)) == UINT32_C(0xc0000000));
    check(read_fflags() == 0);

    /*
     * UINT32_MAX rounds to 2^32 in single precision.  The result is therefore
     * 0x4f800000 and the discarded low bits accrue NX.
     */
    write_fflags(0);
    check(rv32_fp_cvt_s_wu(UINT32_C(0xffffffff)) == UINT32_C(0x4f800000));
    check(read_fflags() == FFLAG_NX);
}

#endif

int main(void)
{
#if defined(__riscv) && __riscv_xlen == 32
    const uintptr_t old_mstatus = enable_initial_fp_state();

    test_arithmetic_operations();
    test_fused_operations();
    test_rounding_modes_and_accrued_flags();
    test_nan_and_underflow_edges();
    test_w_and_wu_conversions();

    write_mstatus(old_mstatus);
#endif

    return 0;
}
