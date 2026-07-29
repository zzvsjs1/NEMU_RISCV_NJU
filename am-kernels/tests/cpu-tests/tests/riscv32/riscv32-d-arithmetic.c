#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 32

#include <stddef.h>
#include <stdint.h>

volatile uint32_t rv32_d_arithmetic_trap_count = 0;

#define MSTATUS_FS_MASK ((uintptr_t)3u << 13)
#define MSTATUS_FS_INITIAL ((uintptr_t)1u << 13)

enum
{
    FFLAG_NX = 1u << 0,
    FFLAG_UF = 1u << 1,
    FFLAG_OF = 1u << 2,
    FFLAG_DZ = 1u << 3,
    FFLAG_NV = 1u << 4,
};

/*
 * The RED configuration implements F but not D.  This integer-only handler
 * skips each rejected four-byte D instruction so the test reaches an ordinary
 * failed check instead of recursing through an FP-using handler or hanging.
 */
asm(
    ".section .text\n"
    ".align 2\n"
    ".option push\n"
    ".option norvc\n"
    ".globl rv32_d_arithmetic_trap_handler\n"
    ".type rv32_d_arithmetic_trap_handler, @function\n"
    "rv32_d_arithmetic_trap_handler:\n"
    "  la t0, rv32_d_arithmetic_trap_count\n"
    "  lw t1, 0(t0)\n"
    "  addi t1, t1, 1\n"
    "  sw t1, 0(t0)\n"
    "  csrr t0, mepc\n"
    "  addi t0, t0, 4\n"
    "  csrw mepc, t0\n"
    "  mret\n"
    ".size rv32_d_arithmetic_trap_handler, "
    ".-rv32_d_arithmetic_trap_handler\n"
    ".option pop\n");

extern void rv32_d_arithmetic_trap_handler(void);

/*
 * RV32 has no architectural FMV.X.D or FMV.D.X instruction.  Every binary64
 * operand and result therefore crosses the integer-only ILP32 ABI through an
 * explicitly eight-byte-aligned memory object.  This also catches an
 * implementation that accidentally truncates an FLD/FSD transfer to XLEN.
 */
#define FP_ASM_BINARY_D(name, instruction) \
    ".globl " #name "\n" \
    ".type " #name ", @function\n" \
    #name ":\n" \
    "  fld f0, 0(a0)\n" \
    "  fld f1, 0(a1)\n" \
    "  " #instruction " f0, f0, f1, rne\n" \
    "  fsd f0, 0(a2)\n" \
    "  ret\n" \
    ".size " #name ", .-" #name "\n"

#define FP_ASM_UNARY_D(name, instruction) \
    ".globl " #name "\n" \
    ".type " #name ", @function\n" \
    #name ":\n" \
    "  fld f0, 0(a0)\n" \
    "  " #instruction " f0, f0, rne\n" \
    "  fsd f0, 0(a1)\n" \
    "  ret\n" \
    ".size " #name ", .-" #name "\n"

#define FP_ASM_FMA_D(name, instruction) \
    ".globl " #name "\n" \
    ".type " #name ", @function\n" \
    #name ":\n" \
    "  fld f0, 0(a0)\n" \
    "  fld f1, 0(a1)\n" \
    "  fld f2, 0(a2)\n" \
    "  " #instruction " f0, f0, f1, f2, rne\n" \
    "  fsd f0, 0(a3)\n" \
    "  ret\n" \
    ".size " #name ", .-" #name "\n"

#define FP_ASM_CVT_W_D_STATIC(name, rounding_mode) \
    ".globl " #name "\n" \
    ".type " #name ", @function\n" \
    #name ":\n" \
    "  fld f0, 0(a0)\n" \
    "  fcvt.w.d a0, f0, " #rounding_mode "\n" \
    "  ret\n" \
    ".size " #name ", .-" #name "\n"

#define FP_ASM_CVT_S_D_STATIC(name, rounding_mode) \
    ".globl " #name "\n" \
    ".type " #name ", @function\n" \
    #name ":\n" \
    "  fld f0, 0(a0)\n" \
    "  fcvt.s.d f0, f0, " #rounding_mode "\n" \
    "  fsd f0, 0(a1)\n" \
    "  ret\n" \
    ".size " #name ", .-" #name "\n"

asm(
    ".section .text\n"
    ".align 2\n"
    ".option push\n"
    ".option norvc\n"
    ".option arch, +f\n"
    ".option arch, +d\n"

    FP_ASM_BINARY_D(rv32_d_add, fadd.d)
    FP_ASM_BINARY_D(rv32_d_sub, fsub.d)
    FP_ASM_BINARY_D(rv32_d_mul, fmul.d)
    FP_ASM_BINARY_D(rv32_d_div, fdiv.d)
    FP_ASM_UNARY_D(rv32_d_sqrt, fsqrt.d)

    FP_ASM_FMA_D(rv32_d_madd, fmadd.d)
    FP_ASM_FMA_D(rv32_d_msub, fmsub.d)
    FP_ASM_FMA_D(rv32_d_nmsub, fnmsub.d)
    FP_ASM_FMA_D(rv32_d_nmadd, fnmadd.d)

    FP_ASM_CVT_W_D_STATIC(rv32_d_cvt_w_d_rne, rne)
    FP_ASM_CVT_W_D_STATIC(rv32_d_cvt_w_d_rtz, rtz)
    FP_ASM_CVT_W_D_STATIC(rv32_d_cvt_w_d_rdn, rdn)
    FP_ASM_CVT_W_D_STATIC(rv32_d_cvt_w_d_rup, rup)
    FP_ASM_CVT_W_D_STATIC(rv32_d_cvt_w_d_rmm, rmm)

    FP_ASM_CVT_S_D_STATIC(rv32_d_cvt_s_d_rne, rne)
    FP_ASM_CVT_S_D_STATIC(rv32_d_cvt_s_d_rtz, rtz)
    FP_ASM_CVT_S_D_STATIC(rv32_d_cvt_s_d_rdn, rdn)
    FP_ASM_CVT_S_D_STATIC(rv32_d_cvt_s_d_rup, rup)
    FP_ASM_CVT_S_D_STATIC(rv32_d_cvt_s_d_rmm, rmm)

    ".globl rv32_d_cvt_w_d_dyn\n"
    ".type rv32_d_cvt_w_d_dyn, @function\n"
    "rv32_d_cvt_w_d_dyn:\n"
    "  fld f0, 0(a0)\n"
    "  fcvt.w.d a0, f0, dyn\n"
    "  ret\n"
    ".size rv32_d_cvt_w_d_dyn, .-rv32_d_cvt_w_d_dyn\n"

    ".globl rv32_d_cvt_s_d_dyn\n"
    ".type rv32_d_cvt_s_d_dyn, @function\n"
    "rv32_d_cvt_s_d_dyn:\n"
    "  fld f0, 0(a0)\n"
    "  fcvt.s.d f0, f0, dyn\n"
    "  fsd f0, 0(a1)\n"
    "  ret\n"
    ".size rv32_d_cvt_s_d_dyn, .-rv32_d_cvt_s_d_dyn\n"

    ".globl rv32_d_cvt_wu_d\n"
    ".type rv32_d_cvt_wu_d, @function\n"
    "rv32_d_cvt_wu_d:\n"
    "  fld f0, 0(a0)\n"
    "  fcvt.wu.d a0, f0, rtz\n"
    "  ret\n"
    ".size rv32_d_cvt_wu_d, .-rv32_d_cvt_wu_d\n"

    ".globl rv32_d_cvt_d_w\n"
    ".type rv32_d_cvt_d_w, @function\n"
    "rv32_d_cvt_d_w:\n"
    "  fcvt.d.w f0, a0\n"
    "  fsd f0, 0(a1)\n"
    "  ret\n"
    ".size rv32_d_cvt_d_w, .-rv32_d_cvt_d_w\n"

    ".globl rv32_d_cvt_d_wu\n"
    ".type rv32_d_cvt_d_wu, @function\n"
    "rv32_d_cvt_d_wu:\n"
    "  fcvt.d.wu f0, a0\n"
    "  fsd f0, 0(a1)\n"
    "  ret\n"
    ".size rv32_d_cvt_d_wu, .-rv32_d_cvt_d_wu\n"

    ".globl rv32_d_cvt_s_d\n"
    ".type rv32_d_cvt_s_d, @function\n"
    "rv32_d_cvt_s_d:\n"
    "  fld f0, 0(a0)\n"
    "  fcvt.s.d f0, f0, rne\n"
    /*
     * FSD deliberately observes the complete FLEN=64 destination register.
     * A correct FCVT.S.D result has its binary32 payload NaN-boxed.
     */
    "  fsd f0, 0(a1)\n"
    "  ret\n"
    ".size rv32_d_cvt_s_d, .-rv32_d_cvt_s_d\n"

    ".globl rv32_d_cvt_d_s\n"
    ".type rv32_d_cvt_d_s, @function\n"
    "rv32_d_cvt_d_s:\n"
    "  flw f0, 0(a0)\n"
    "  fcvt.d.s f0, f0\n"
    "  fsd f0, 0(a1)\n"
    "  ret\n"
    ".size rv32_d_cvt_d_s, .-rv32_d_cvt_d_s\n"

    ".globl rv32_d_cvt_d_malformed_s\n"
    ".type rv32_d_cvt_d_malformed_s, @function\n"
    "rv32_d_cvt_d_malformed_s:\n"
    /*
     * Loading all 64 bits permits construction of a deliberately malformed
     * binary32 NaN box.  FCVT.D.S must consume it as a canonical NaN.
     */
    "  fld f0, 0(a0)\n"
    "  fcvt.d.s f0, f0\n"
    "  fsd f0, 0(a1)\n"
    "  ret\n"
    ".size rv32_d_cvt_d_malformed_s, .-rv32_d_cvt_d_malformed_s\n"

    ".option pop\n");

typedef void (*binary_d_operation_t)(const uint64_t *, const uint64_t *,
                                     uint64_t *);
typedef void (*unary_d_operation_t)(const uint64_t *, uint64_t *);
typedef void (*fma_d_operation_t)(const uint64_t *, const uint64_t *,
                                  const uint64_t *, uint64_t *);
typedef uint32_t (*cvt_w_d_operation_t)(const uint64_t *);
typedef void (*cvt_s_d_operation_t)(const uint64_t *, uint64_t *);

extern void rv32_d_add(const uint64_t *, const uint64_t *, uint64_t *);
extern void rv32_d_sub(const uint64_t *, const uint64_t *, uint64_t *);
extern void rv32_d_mul(const uint64_t *, const uint64_t *, uint64_t *);
extern void rv32_d_div(const uint64_t *, const uint64_t *, uint64_t *);
extern void rv32_d_sqrt(const uint64_t *, uint64_t *);
extern void rv32_d_madd(const uint64_t *, const uint64_t *,
                        const uint64_t *, uint64_t *);
extern void rv32_d_msub(const uint64_t *, const uint64_t *,
                        const uint64_t *, uint64_t *);
extern void rv32_d_nmsub(const uint64_t *, const uint64_t *,
                         const uint64_t *, uint64_t *);
extern void rv32_d_nmadd(const uint64_t *, const uint64_t *,
                         const uint64_t *, uint64_t *);
extern uint32_t rv32_d_cvt_w_d_rne(const uint64_t *);
extern uint32_t rv32_d_cvt_w_d_rtz(const uint64_t *);
extern uint32_t rv32_d_cvt_w_d_rdn(const uint64_t *);
extern uint32_t rv32_d_cvt_w_d_rup(const uint64_t *);
extern uint32_t rv32_d_cvt_w_d_rmm(const uint64_t *);
extern uint32_t rv32_d_cvt_w_d_dyn(const uint64_t *);
extern void rv32_d_cvt_s_d_rne(const uint64_t *, uint64_t *);
extern void rv32_d_cvt_s_d_rtz(const uint64_t *, uint64_t *);
extern void rv32_d_cvt_s_d_rdn(const uint64_t *, uint64_t *);
extern void rv32_d_cvt_s_d_rup(const uint64_t *, uint64_t *);
extern void rv32_d_cvt_s_d_rmm(const uint64_t *, uint64_t *);
extern void rv32_d_cvt_s_d_dyn(const uint64_t *, uint64_t *);
extern uint32_t rv32_d_cvt_wu_d(const uint64_t *);
extern void rv32_d_cvt_d_w(uint32_t, uint64_t *);
extern void rv32_d_cvt_d_wu(uint32_t, uint64_t *);
extern void rv32_d_cvt_s_d(const uint64_t *, uint64_t *);
extern void rv32_d_cvt_d_s(const uint32_t *, uint64_t *);
extern void rv32_d_cvt_d_malformed_s(const uint64_t *, uint64_t *);

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

static uint64_t run_binary_d(binary_d_operation_t operation,
                             uint64_t lhs_bits, uint64_t rhs_bits)
{
    uint64_t lhs __attribute__((aligned(8))) = lhs_bits;
    uint64_t rhs __attribute__((aligned(8))) = rhs_bits;
    uint64_t result __attribute__((aligned(8))) =
        UINT64_C(0xfeedfacecafebeef);

    operation(&lhs, &rhs, &result);
    return result;
}

static uint64_t run_unary_d(unary_d_operation_t operation,
                            uint64_t source_bits)
{
    uint64_t source __attribute__((aligned(8))) = source_bits;
    uint64_t result __attribute__((aligned(8))) =
        UINT64_C(0xfeedfacecafebeef);

    operation(&source, &result);
    return result;
}

static uint64_t run_fma_d(fma_d_operation_t operation,
                          uint64_t lhs_bits, uint64_t rhs_bits,
                          uint64_t addend_bits)
{
    uint64_t lhs __attribute__((aligned(8))) = lhs_bits;
    uint64_t rhs __attribute__((aligned(8))) = rhs_bits;
    uint64_t addend __attribute__((aligned(8))) = addend_bits;
    uint64_t result __attribute__((aligned(8))) =
        UINT64_C(0xfeedfacecafebeef);

    operation(&lhs, &rhs, &addend, &result);
    return result;
}

static void test_arithmetic_operations(void)
{
    write_fflags(0);

    /* 1.5 + 2.25 = 3.75 exactly in binary64. */
    check(run_binary_d(rv32_d_add,
                       UINT64_C(0x3ff8000000000000),
                       UINT64_C(0x4002000000000000)) ==
          UINT64_C(0x400e000000000000));
    check(run_binary_d(rv32_d_sub,
                       UINT64_C(0x400e000000000000),
                       UINT64_C(0x4002000000000000)) ==
          UINT64_C(0x3ff8000000000000));
    check(run_binary_d(rv32_d_mul,
                       UINT64_C(0x3ff8000000000000),
                       UINT64_C(0x4000000000000000)) ==
          UINT64_C(0x4008000000000000));
    check(run_binary_d(rv32_d_div,
                       UINT64_C(0x4008000000000000),
                       UINT64_C(0x4000000000000000)) ==
          UINT64_C(0x3ff8000000000000));
    check(run_unary_d(rv32_d_sqrt,
                      UINT64_C(0x4010000000000000)) ==
          UINT64_C(0x4000000000000000));

    /*
     * IEEE-754 square root preserves the sign of zero.  This distinguishes
     * negative zero from an implementation that normalises both zeros.
     */
    check(run_unary_d(rv32_d_sqrt,
                      UINT64_C(0x8000000000000000)) ==
          UINT64_C(0x8000000000000000));
    check(read_fflags() == 0);
}

static void test_fused_operations(void)
{
    const uint64_t two = UINT64_C(0x4000000000000000);
    const uint64_t three = UINT64_C(0x4008000000000000);
    const uint64_t four = UINT64_C(0x4010000000000000);

    write_fflags(0);
    check(run_fma_d(rv32_d_madd, two, three, four) ==
          UINT64_C(0x4024000000000000));
    check(run_fma_d(rv32_d_msub, two, three, four) ==
          UINT64_C(0x4000000000000000));
    check(run_fma_d(rv32_d_nmsub, two, three, four) ==
          UINT64_C(0xc000000000000000));
    check(run_fma_d(rv32_d_nmadd, two, three, four) ==
          UINT64_C(0xc024000000000000));
    check(read_fflags() == 0);

    /*
     * Let a=1+2^-52 and b=1-2^-53.  The exact value a*b-1 is
     * 2^-53-2^-105, encoded as 0x3c9ffffffffffffe after one rounding.
     * A separately rounded multiply becomes exactly one and would incorrectly
     * produce positive zero, so this literal detects a non-fused FMSUB.
     */
    check(run_fma_d(rv32_d_msub,
                    UINT64_C(0x3ff0000000000001),
                    UINT64_C(0x3fefffffffffffff),
                    UINT64_C(0x3ff0000000000000)) ==
          UINT64_C(0x3c9ffffffffffffe));

    /*
     * Infinity multiplied by zero is invalid even when the addend is already
     * a quiet NaN.  The architectural result is the canonical binary64 NaN.
     */
    write_fflags(0);
    check(run_fma_d(rv32_d_madd,
                    UINT64_C(0x7ff0000000000000),
                    UINT64_C(0x0000000000000000),
                    UINT64_C(0x7ff8000000000123)) ==
          UINT64_C(0x7ff8000000000000));
    check(read_fflags() == FFLAG_NV);
}

static void test_static_and_dynamic_rounding_modes(void)
{
    /*
     * These three hand-derived conversions form a unique tuple for every
     * standard rounding mode.  The results are signed RV32 integer bit
     * patterns, not host floating-point calculations.
     */
    static const uint64_t input[3] __attribute__((aligned(8))) = {
        UINT64_C(0x4004000000000000), /* +2.5 */
        UINT64_C(0xc004000000000000), /* -2.5 */
        UINT64_C(0x4006000000000000), /* +2.75 */
    };
    static const uint32_t expected[5][3] = {
        {2u, UINT32_C(0xfffffffe), 3u}, /* RNE */
        {2u, UINT32_C(0xfffffffe), 2u}, /* RTZ */
        {2u, UINT32_C(0xfffffffd), 2u}, /* RDN */
        {3u, UINT32_C(0xfffffffe), 3u}, /* RUP */
        {3u, UINT32_C(0xfffffffd), 3u}, /* RMM */
    };
    static const cvt_w_d_operation_t static_operation[5] = {
        rv32_d_cvt_w_d_rne,
        rv32_d_cvt_w_d_rtz,
        rv32_d_cvt_w_d_rdn,
        rv32_d_cvt_w_d_rup,
        rv32_d_cvt_w_d_rmm,
    };

    for (size_t rm = 0; rm < 5; ++rm)
    {
        for (size_t case_index = 0; case_index < 3; ++case_index)
        {
            write_fflags(0);
            check(static_operation[rm](&input[case_index]) ==
                  expected[rm][case_index]);
            check(read_fflags() == FFLAG_NX);
        }
    }

    for (uintptr_t rm = 0; rm < 5; ++rm)
    {
        write_frm(rm);

        for (size_t case_index = 0; case_index < 3; ++case_index)
        {
            write_fflags(0);
            check(rv32_d_cvt_w_d_dyn(&input[case_index]) ==
                  expected[rm][case_index]);
            check(read_fflags() == FFLAG_NX);
        }
    }

    write_frm(0);
}

static void test_cross_precision_rounding_modes(void)
{
    /*
     * The first two inputs are exact halfway cases on either side of ±1.0f.
     * The third lies three quarters of one binary32 ULP above +1.0f. Their
     * three-result tuple distinguishes every standard rounding mode.
     */
    static const uint64_t input[3] __attribute__((aligned(8))) = {
        UINT64_C(0x3ff0000010000000),
        UINT64_C(0xbff0000010000000),
        UINT64_C(0x3ff0000018000000),
    };
    static const uint64_t expected[5][3] = {
        {UINT64_C(0xffffffff3f800000),
         UINT64_C(0xffffffffbf800000),
         UINT64_C(0xffffffff3f800001)}, /* RNE */
        {UINT64_C(0xffffffff3f800000),
         UINT64_C(0xffffffffbf800000),
         UINT64_C(0xffffffff3f800000)}, /* RTZ */
        {UINT64_C(0xffffffff3f800000),
         UINT64_C(0xffffffffbf800001),
         UINT64_C(0xffffffff3f800000)}, /* RDN */
        {UINT64_C(0xffffffff3f800001),
         UINT64_C(0xffffffffbf800000),
         UINT64_C(0xffffffff3f800001)}, /* RUP */
        {UINT64_C(0xffffffff3f800001),
         UINT64_C(0xffffffffbf800001),
         UINT64_C(0xffffffff3f800001)}, /* RMM */
    };
    static const cvt_s_d_operation_t static_operation[5] = {
        rv32_d_cvt_s_d_rne,
        rv32_d_cvt_s_d_rtz,
        rv32_d_cvt_s_d_rdn,
        rv32_d_cvt_s_d_rup,
        rv32_d_cvt_s_d_rmm,
    };
    uint64_t result __attribute__((aligned(8)));

    for (size_t rm = 0; rm < 5; ++rm)
    {
        for (size_t case_index = 0; case_index < 3; ++case_index)
        {
            write_fflags(0);
            static_operation[rm](&input[case_index], &result);
            check(result == expected[rm][case_index]);
            check(read_fflags() == FFLAG_NX);
        }
    }

    for (uintptr_t rm = 0; rm < 5; ++rm)
    {
        write_frm(rm);

        for (size_t case_index = 0; case_index < 3; ++case_index)
        {
            write_fflags(0);
            rv32_d_cvt_s_d_dyn(&input[case_index], &result);
            check(result == expected[rm][case_index]);
            check(read_fflags() == FFLAG_NX);
        }
    }

    write_frm(0);
}

static void test_nan_infinity_and_exception_flags(void)
{
    /*
     * A signalling NaN raises NV and arithmetic always returns the canonical
     * RISC-V binary64 NaN, discarding the input payload.
     */
    write_fflags(0);
    check(run_binary_d(rv32_d_add,
                       UINT64_C(0x7ff0000000000001),
                       UINT64_C(0x3ff0000000000000)) ==
          UINT64_C(0x7ff8000000000000));
    check(read_fflags() == FFLAG_NV);

    /* A quiet NaN is also canonicalised, but it does not itself raise NV. */
    write_fflags(0);
    check(run_binary_d(rv32_d_add,
                       UINT64_C(0x7ff8000000000123),
                       UINT64_C(0x3ff0000000000000)) ==
          UINT64_C(0x7ff8000000000000));
    check(read_fflags() == 0);

    write_fflags(0);
    check(run_binary_d(rv32_d_div,
                       UINT64_C(0x3ff0000000000000),
                       UINT64_C(0x0000000000000000)) ==
          UINT64_C(0x7ff0000000000000));
    check(read_fflags() == FFLAG_DZ);

    /* An exact later operation must not clear an accrued divide-by-zero flag. */
    check(run_binary_d(rv32_d_mul,
                       UINT64_C(0x4000000000000000),
                       UINT64_C(0x4008000000000000)) ==
          UINT64_C(0x4018000000000000));
    check(read_fflags() == FFLAG_DZ);

    write_fflags(0);
    check(run_unary_d(rv32_d_sqrt,
                      UINT64_C(0xbff0000000000000)) ==
          UINT64_C(0x7ff8000000000000));
    check(read_fflags() == FFLAG_NV);

    write_fflags(0);
    check(run_binary_d(rv32_d_mul,
                       UINT64_C(0x7fefffffffffffff),
                       UINT64_C(0x4000000000000000)) ==
          UINT64_C(0x7ff0000000000000));
    check(read_fflags() == (FFLAG_OF | FFLAG_NX));

    /*
     * The exact product is slightly below the smallest normal number, but it
     * rounds to 0x0010000000000000.  RISC-V detects tininess after rounding,
     * so this case raises NX without UF.
     */
    write_fflags(0);
    check(run_binary_d(rv32_d_mul,
                       UINT64_C(0x000ffffffffffff0),
                       UINT64_C(0x3ff0000000000010)) ==
          UINT64_C(0x0010000000000000));
    check(read_fflags() == FFLAG_NX);

    /* An exactly representable subnormal result raises no exception flags. */
    write_fflags(0);
    check(run_binary_d(rv32_d_mul,
                       UINT64_C(0x0010000000000000),
                       UINT64_C(0x3fe0000000000000)) ==
          UINT64_C(0x0008000000000000));
    check(read_fflags() == 0);

    /* A tiny, inexact halfway result rounds to even and raises UF together with NX. */
    write_fflags(0);
    check(run_binary_d(rv32_d_mul,
                       UINT64_C(0x0010000000000001),
                       UINT64_C(0x3fe0000000000000)) ==
          UINT64_C(0x0008000000000000));
    check(read_fflags() == (FFLAG_UF | FFLAG_NX));

    /*
     * Accrued flags are sticky.  These four operations contribute DZ,
     * OF|NX, NV, and UF|NX respectively, producing all five low fflags bits.
     */
    write_fflags(0);
    (void)run_binary_d(rv32_d_div,
                       UINT64_C(0x3ff0000000000000),
                       UINT64_C(0x0000000000000000));
    (void)run_binary_d(rv32_d_mul,
                       UINT64_C(0x7fefffffffffffff),
                       UINT64_C(0x4000000000000000));
    (void)run_unary_d(rv32_d_sqrt,
                      UINT64_C(0xbff0000000000000));
    (void)run_binary_d(rv32_d_mul,
                       UINT64_C(0x0010000000000001),
                       UINT64_C(0x3fe0000000000000));
    check(read_fflags() ==
          (FFLAG_NV | FFLAG_DZ | FFLAG_OF | FFLAG_UF | FFLAG_NX));
}

static void test_integer_and_cross_precision_conversions(void)
{
    uint64_t source __attribute__((aligned(8)));
    uint64_t result __attribute__((aligned(8)));
    uint32_t single_source;

    source = UINT64_C(0x4008000000000000); /* +3.0 */
    write_fflags(0);
    check(rv32_d_cvt_w_d_rne(&source) == 3u);
    check(rv32_d_cvt_wu_d(&source) == 3u);
    check(read_fflags() == 0);

    /* Invalid integer conversions clip to the endpoints prescribed by D. */
    source = UINT64_C(0x7ff0000000000000);
    write_fflags(0);
    check(rv32_d_cvt_w_d_rne(&source) == UINT32_C(0x7fffffff));
    check(read_fflags() == FFLAG_NV);

    source = UINT64_C(0xbff0000000000000); /* -1.0 */
    write_fflags(0);
    check(rv32_d_cvt_wu_d(&source) == 0);
    check(read_fflags() == FFLAG_NV);

    /*
     * Every signed or unsigned 32-bit integer is exactly representable in
     * binary64, including UINT32_MAX.
     */
    result = UINT64_C(0xfeedfacecafebeef);
    write_fflags(0);
    rv32_d_cvt_d_w(UINT32_C(0xfffffffe), &result);
    check(result == UINT64_C(0xc000000000000000));
    check(read_fflags() == 0);

    result = UINT64_C(0xfeedfacecafebeef);
    rv32_d_cvt_d_wu(UINT32_C(0xffffffff), &result);
    check(result == UINT64_C(0x41efffffffe00000));
    check(read_fflags() == 0);

    /*
     * FCVT.S.D writes a NaN-boxed binary32 value because D makes FLEN=64.
     * FSD exposes the complete register so missing upper one bits are visible.
     */
    source = UINT64_C(0x3ff8000000000000);
    result = UINT64_C(0xfeedfacecafebeef);
    write_fflags(0);
    rv32_d_cvt_s_d(&source, &result);
    check(result == UINT64_C(0xffffffff3fc00000));
    check(read_fflags() == 0);

    single_source = UINT32_C(0x3fc00000);
    result = UINT64_C(0xfeedfacecafebeef);
    rv32_d_cvt_d_s(&single_source, &result);
    check(result == UINT64_C(0x3ff8000000000000));
    check(read_fflags() == 0);

    /*
     * The next binary64 number above one is not representable in binary32.
     * RNE returns exactly 1.0f, NaN-boxed, and accrues NX.
     */
    source = UINT64_C(0x3ff0000000000001);
    result = UINT64_C(0xfeedfacecafebeef);
    write_fflags(0);
    rv32_d_cvt_s_d(&source, &result);
    check(result == UINT64_C(0xffffffff3f800000));
    check(read_fflags() == FFLAG_NX);

    /*
     * Bits [63:32] are not all one, so the source is not a valid NaN-boxed
     * binary32 value.  FCVT.D.S must treat it as a canonical quiet NaN.
     */
    source = UINT64_C(0x000000003f800000);
    result = UINT64_C(0xfeedfacecafebeef);
    write_fflags(0);
    rv32_d_cvt_d_malformed_s(&source, &result);
    check(result == UINT64_C(0x7ff8000000000000));
    check(read_fflags() == 0);
}

#endif

int main(void)
{
#if defined(__riscv) && __riscv_xlen == 32
    const uintptr_t old_mstatus = read_mstatus();
    const uintptr_t old_mtvec = read_mtvec();
    uint32_t trap_count;

    rv32_d_arithmetic_trap_count = 0;
    write_mtvec((uintptr_t)rv32_d_arithmetic_trap_handler);
    write_mstatus((old_mstatus & ~MSTATUS_FS_MASK) | MSTATUS_FS_INITIAL);

    test_arithmetic_operations();
    test_fused_operations();
    test_static_and_dynamic_rounding_modes();
    test_cross_precision_rounding_modes();
    test_nan_infinity_and_exception_flags();
    test_integer_and_cross_precision_conversions();

    trap_count = rv32_d_arithmetic_trap_count;
    write_mstatus(old_mstatus);
    write_mtvec(old_mtvec);
    check(trap_count == 0);
#endif

    return 0;
}
