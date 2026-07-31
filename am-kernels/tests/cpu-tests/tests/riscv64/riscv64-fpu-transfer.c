#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 64

#include <stdint.h>

#define MSTATUS_FS_MASK ((uintptr_t)3u << 13)
#define MSTATUS_FS_INITIAL ((uintptr_t)1u << 13)
#define MSTATUS_FS_CLEAN ((uintptr_t)2u << 13)
#define MSTATUS_FS_DIRTY ((uintptr_t)3u << 13)
#define MSTATUS_SD (UINT64_C(1) << 63)

#define FP_ASM_BITWISE_S(name, instruction) \
    ".globl " #name "\n" \
    ".type " #name ", @function\n" \
    #name ":\n" \
    "  fmv.w.x f0, a0\n" \
    "  fmv.w.x f1, a1\n" \
    "  " #instruction " f0, f0, f1\n" \
    "  fmv.x.d a0, f0\n" \
    "  ret\n" \
    ".size " #name ", .-" #name "\n"

#define FP_ASM_BITWISE_D(name, instruction) \
    ".globl " #name "\n" \
    ".type " #name ", @function\n" \
    #name ":\n" \
    "  fmv.d.x f0, a0\n" \
    "  fmv.d.x f1, a1\n" \
    "  " #instruction " f0, f0, f1\n" \
    "  fmv.x.d a0, f0\n" \
    "  ret\n" \
    ".size " #name ", .-" #name "\n"

#define FP_ASM_COMPARE_S(name, instruction) \
    ".globl " #name "\n" \
    ".type " #name ", @function\n" \
    #name ":\n" \
    "  fmv.w.x f0, a0\n" \
    "  fmv.w.x f1, a1\n" \
    "  " #instruction " a0, f0, f1\n" \
    "  ret\n" \
    ".size " #name ", .-" #name "\n"

#define FP_ASM_COMPARE_D(name, instruction) \
    ".globl " #name "\n" \
    ".type " #name ", @function\n" \
    #name ":\n" \
    "  fmv.d.x f0, a0\n" \
    "  fmv.d.x f1, a1\n" \
    "  " #instruction " a0, f0, f1\n" \
    "  ret\n" \
    ".size " #name ", .-" #name "\n"

asm(
    ".section .text\n"
    ".align 2\n"
    ".option push\n"
    ".option norvc\n"
    ".option arch, +f\n"
    ".option arch, +d\n"

    FP_ASM_BITWISE_S(rv64_fp_sgnj_s, fsgnj.s)
    FP_ASM_BITWISE_S(rv64_fp_sgnjn_s, fsgnjn.s)
    FP_ASM_BITWISE_S(rv64_fp_sgnjx_s, fsgnjx.s)
    FP_ASM_BITWISE_D(rv64_fp_sgnj_d, fsgnj.d)
    FP_ASM_BITWISE_D(rv64_fp_sgnjn_d, fsgnjn.d)
    FP_ASM_BITWISE_D(rv64_fp_sgnjx_d, fsgnjx.d)
    FP_ASM_BITWISE_S(rv64_fp_min_s, fmin.s)
    FP_ASM_BITWISE_S(rv64_fp_max_s, fmax.s)
    FP_ASM_BITWISE_D(rv64_fp_min_d, fmin.d)
    FP_ASM_BITWISE_D(rv64_fp_max_d, fmax.d)

    /*
     * The macro-generated sign probes cover rd==rs1. These two probes retain
     * distinct source values while making rd==rs2, so native stores cannot
     * accidentally overwrite a source before both inputs have been consumed.
     */
    ".globl rv64_fp_sgnj_s_rd_rs2\n"
    ".type rv64_fp_sgnj_s_rd_rs2, @function\n"
    "rv64_fp_sgnj_s_rd_rs2:\n"
    "  fmv.w.x f0, a0\n"
    "  fmv.w.x f1, a1\n"
    "  fsgnj.s f1, f0, f1\n"
    "  fmv.x.d a0, f1\n"
    "  ret\n"
    ".size rv64_fp_sgnj_s_rd_rs2, .-rv64_fp_sgnj_s_rd_rs2\n"

    ".globl rv64_fp_sgnj_d_rd_rs2\n"
    ".type rv64_fp_sgnj_d_rd_rs2, @function\n"
    "rv64_fp_sgnj_d_rd_rs2:\n"
    "  fmv.d.x f0, a0\n"
    "  fmv.d.x f1, a1\n"
    "  fsgnj.d f1, f0, f1\n"
    "  fmv.x.d a0, f1\n"
    "  ret\n"
    ".size rv64_fp_sgnj_d_rd_rs2, .-rv64_fp_sgnj_d_rd_rs2\n"

    FP_ASM_COMPARE_S(rv64_fp_eq_s, feq.s)
    FP_ASM_COMPARE_S(rv64_fp_lt_s, flt.s)
    FP_ASM_COMPARE_S(rv64_fp_le_s, fle.s)
    FP_ASM_COMPARE_D(rv64_fp_eq_d, feq.d)
    FP_ASM_COMPARE_D(rv64_fp_lt_d, flt.d)
    FP_ASM_COMPARE_D(rv64_fp_le_d, fle.d)

    ".globl rv64_fp_class_s\n"
    ".type rv64_fp_class_s, @function\n"
    "rv64_fp_class_s:\n"
    "  fmv.w.x f0, a0\n"
    "  fclass.s a0, f0\n"
    "  ret\n"
    ".size rv64_fp_class_s, .-rv64_fp_class_s\n"

    ".globl rv64_fp_class_d\n"
    ".type rv64_fp_class_d, @function\n"
    "rv64_fp_class_d:\n"
    "  fmv.d.x f0, a0\n"
    "  fclass.d a0, f0\n"
    "  ret\n"
    ".size rv64_fp_class_d, .-rv64_fp_class_d\n"

    ".globl rv64_fp_move_x_w\n"
    ".type rv64_fp_move_x_w, @function\n"
    "rv64_fp_move_x_w:\n"
    "  fmv.w.x f0, a0\n"
    "  fmv.x.w a0, f0\n"
    "  ret\n"
    ".size rv64_fp_move_x_w, .-rv64_fp_move_x_w\n"

    ".globl rv64_fp_malformed_s_add\n"
    ".type rv64_fp_malformed_s_add, @function\n"
    "rv64_fp_malformed_s_add:\n"
    "  fmv.d.x f0, a0\n"
    "  fmv.w.x f1, a1\n"
    "  fadd.s f0, f0, f1, rne\n"
    "  fmv.x.d a0, f0\n"
    "  ret\n"
    ".size rv64_fp_malformed_s_add, .-rv64_fp_malformed_s_add\n"

    ".globl rv64_fp_store_malformed_s\n"
    ".type rv64_fp_store_malformed_s, @function\n"
    "rv64_fp_store_malformed_s:\n"
    "  fmv.d.x f0, a0\n"
    "  fsw f0, 0(a1)\n"
    "  ret\n"
    ".size rv64_fp_store_malformed_s, .-rv64_fp_store_malformed_s\n"

    /*
     * These probes deliberately create malformed S boxes through the raw D
     * move. Computational S operations must substitute canonical qNaN, while
     * FMV.X.W must continue to expose the untouched low word.
     */
    ".globl rv64_fp_malformed_s_sgnj\n"
    ".type rv64_fp_malformed_s_sgnj, @function\n"
    "rv64_fp_malformed_s_sgnj:\n"
    "  fmv.d.x f0, a0\n"
    "  fmv.d.x f1, a1\n"
    "  fsgnj.s f0, f0, f1\n"
    "  fmv.x.d a0, f0\n"
    "  ret\n"
    ".size rv64_fp_malformed_s_sgnj, "
    ".-rv64_fp_malformed_s_sgnj\n"

    ".globl rv64_fp_malformed_s_class\n"
    ".type rv64_fp_malformed_s_class, @function\n"
    "rv64_fp_malformed_s_class:\n"
    "  fmv.d.x f0, a0\n"
    "  fclass.s a0, f0\n"
    "  ret\n"
    ".size rv64_fp_malformed_s_class, "
    ".-rv64_fp_malformed_s_class\n"

    ".globl rv64_fp_malformed_move_x_w\n"
    ".type rv64_fp_malformed_move_x_w, @function\n"
    "rv64_fp_malformed_move_x_w:\n"
    "  fmv.d.x f0, a0\n"
    "  fmv.x.w a0, f0\n"
    "  ret\n"
    ".size rv64_fp_malformed_move_x_w, "
    ".-rv64_fp_malformed_move_x_w\n"

    /*
     * Isolate read-only and FPR-writing instructions so FS state transitions
     * cannot be hidden by setup or result-transfer instructions.
     */
    ".globl rv64_fp_write_w_x_only\n"
    ".type rv64_fp_write_w_x_only, @function\n"
    "rv64_fp_write_w_x_only:\n"
    "  fmv.w.x f0, a0\n"
    "  ret\n"
    ".size rv64_fp_write_w_x_only, .-rv64_fp_write_w_x_only\n"

    ".globl rv64_fp_read_x_w_only\n"
    ".type rv64_fp_read_x_w_only, @function\n"
    "rv64_fp_read_x_w_only:\n"
    "  fmv.x.w a0, f0\n"
    "  ret\n"
    ".size rv64_fp_read_x_w_only, .-rv64_fp_read_x_w_only\n"

    ".globl rv64_fp_class_s_only\n"
    ".type rv64_fp_class_s_only, @function\n"
    "rv64_fp_class_s_only:\n"
    "  fclass.s a0, f0\n"
    "  ret\n"
    ".size rv64_fp_class_s_only, .-rv64_fp_class_s_only\n"

    ".globl rv64_fp_sgnjn_s_alias_only\n"
    ".type rv64_fp_sgnjn_s_alias_only, @function\n"
    "rv64_fp_sgnjn_s_alias_only:\n"
    "  fsgnjn.s f0, f0, f0\n"
    "  ret\n"
    ".size rv64_fp_sgnjn_s_alias_only, "
    ".-rv64_fp_sgnjn_s_alias_only\n"

    /*
     * The entry is the real self-backedge target. The four GPRs are therefore
     * eligible for the stable cache only when every exact FP operation remains
     * helper-free and preserves R8.
     */
    ".globl rv64_fp_exact_stable_loop\n"
    ".type rv64_fp_exact_stable_loop, @function\n"
    "rv64_fp_exact_stable_loop:\n"
    "  fmv.d.x f0, a0\n"
    "  fmv.d.x f1, a1\n"
    "  fsgnj.d f2, f0, f1\n"
    "  fmv.x.d a0, f2\n"
    "  fclass.d a3, f2\n"
    "  xor a0, a0, a3\n"
    "  addi a2, a2, -1\n"
    "  bne a2, zero, rv64_fp_exact_stable_loop\n"
    "  ret\n"
    ".size rv64_fp_exact_stable_loop, "
    ".-rv64_fp_exact_stable_loop\n"

    ".option pop\n");

extern uint64_t rv64_fp_sgnj_s(uint32_t, uint32_t);
extern uint64_t rv64_fp_sgnjn_s(uint32_t, uint32_t);
extern uint64_t rv64_fp_sgnjx_s(uint32_t, uint32_t);
extern uint64_t rv64_fp_sgnj_d(uint64_t, uint64_t);
extern uint64_t rv64_fp_sgnjn_d(uint64_t, uint64_t);
extern uint64_t rv64_fp_sgnjx_d(uint64_t, uint64_t);
extern uint64_t rv64_fp_sgnj_s_rd_rs2(uint32_t, uint32_t);
extern uint64_t rv64_fp_sgnj_d_rd_rs2(uint64_t, uint64_t);
extern uint64_t rv64_fp_min_s(uint32_t, uint32_t);
extern uint64_t rv64_fp_max_s(uint32_t, uint32_t);
extern uint64_t rv64_fp_min_d(uint64_t, uint64_t);
extern uint64_t rv64_fp_max_d(uint64_t, uint64_t);
extern uint64_t rv64_fp_eq_s(uint32_t, uint32_t);
extern uint64_t rv64_fp_lt_s(uint32_t, uint32_t);
extern uint64_t rv64_fp_le_s(uint32_t, uint32_t);
extern uint64_t rv64_fp_eq_d(uint64_t, uint64_t);
extern uint64_t rv64_fp_lt_d(uint64_t, uint64_t);
extern uint64_t rv64_fp_le_d(uint64_t, uint64_t);
extern uint64_t rv64_fp_class_s(uint32_t);
extern uint64_t rv64_fp_class_d(uint64_t);
extern uint64_t rv64_fp_move_x_w(uint32_t);
extern uint64_t rv64_fp_malformed_s_add(uint64_t, uint32_t);
extern void rv64_fp_store_malformed_s(uint64_t, uint32_t *);
extern uint64_t rv64_fp_malformed_s_sgnj(uint64_t, uint64_t);
extern uint64_t rv64_fp_malformed_s_class(uint64_t);
extern uint64_t rv64_fp_malformed_move_x_w(uint64_t);
extern uint64_t rv64_fp_write_w_x_only(uint64_t);
extern uint64_t rv64_fp_read_x_w_only(void);
extern uint64_t rv64_fp_class_s_only(void);
extern void rv64_fp_sgnjn_s_alias_only(void);
extern uint64_t rv64_fp_exact_stable_loop(uint64_t, uint64_t, uint64_t);

/*
 * These literal encodings cover every architectural FCLASS result bit.  The
 * signalling NaN cases are particularly important because classification
 * observes their payload without raising the invalid-operation flag.
 */
static const struct
{
    uint32_t bits;
    uintptr_t expected;
} fclass_s_cases[] = {
    {0xff800000u, 1u << 0}, /* Negative infinity. */
    {0xbf800000u, 1u << 1}, /* Negative normal. */
    {0x80000001u, 1u << 2}, /* Negative subnormal. */
    {0x80000000u, 1u << 3}, /* Negative zero. */
    {0x00000000u, 1u << 4}, /* Positive zero. */
    {0x00000001u, 1u << 5}, /* Positive subnormal. */
    {0x3f800000u, 1u << 6}, /* Positive normal. */
    {0x7f800000u, 1u << 7}, /* Positive infinity. */
    {0x7f800001u, 1u << 8}, /* Signalling NaN. */
    {0x7fc00000u, 1u << 9}, /* Quiet NaN. */
};

static const struct
{
    uint64_t bits;
    uintptr_t expected;
} fclass_d_cases[] = {
    {UINT64_C(0xfff0000000000000), 1u << 0},
    {UINT64_C(0xbff0000000000000), 1u << 1},
    {UINT64_C(0x8000000000000001), 1u << 2},
    {UINT64_C(0x8000000000000000), 1u << 3},
    {UINT64_C(0x0000000000000000), 1u << 4},
    {UINT64_C(0x0000000000000001), 1u << 5},
    {UINT64_C(0x3ff0000000000000), 1u << 6},
    {UINT64_C(0x7ff0000000000000), 1u << 7},
    {UINT64_C(0x7ff0000000000001), 1u << 8},
    {UINT64_C(0x7ff8000000000000), 1u << 9},
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

static void test_sign_injection(void)
{
    const uint32_t s_payload = UINT32_C(0x7fc12345);
    const uint32_t s_negative = UINT32_C(0xbf800000);
    const uint64_t d_payload = UINT64_C(0x7ff8123456789abc);
    const uint64_t d_negative = UINT64_C(0xbff0000000000000);

    write_fflags(UINT64_C(0x1f));
    check(rv64_fp_sgnj_s(s_payload, s_negative) ==
          (UINT64_C(0xffffffff) << 32 | UINT32_C(0xffc12345)));
    check(rv64_fp_sgnjn_s(s_payload, s_negative) ==
          (UINT64_C(0xffffffff) << 32 | s_payload));
    check(rv64_fp_sgnjx_s(UINT32_C(0xffc12345), s_negative) ==
          (UINT64_C(0xffffffff) << 32 | s_payload));

    check(rv64_fp_sgnj_d(d_payload, d_negative) ==
          UINT64_C(0xfff8123456789abc));
    check(rv64_fp_sgnjn_d(d_payload, d_negative) == d_payload);
    check(rv64_fp_sgnjx_d(UINT64_C(0xfff8123456789abc), d_negative) ==
          d_payload);

    check(rv64_fp_sgnj_s_rd_rs2(s_payload, s_negative) ==
          UINT64_C(0xffffffffffc12345));
    check(rv64_fp_sgnj_d_rd_rs2(d_payload, d_negative) ==
          UINT64_C(0xfff8123456789abc));

    /*
     * Sign injection is bitwise and never signals, including for a signalling
     * NaN payload.
     */
    check(rv64_fp_sgnj_s(UINT32_C(0x7f800123),
                        UINT32_C(0x3f800000)) ==
          UINT64_C(0xffffffff7f800123));
    check(read_fflags() == UINT64_C(0x1f));
}

static void test_min_max_and_signed_zero(void)
{
    write_fflags(0);
    check(rv64_fp_min_s(UINT32_C(0x00000000), UINT32_C(0x80000000)) ==
          UINT64_C(0xffffffff80000000));
    check(rv64_fp_max_s(UINT32_C(0x00000000), UINT32_C(0x80000000)) ==
          UINT64_C(0xffffffff00000000));
    check(rv64_fp_min_d(UINT64_C(0x0000000000000000),
                       UINT64_C(0x8000000000000000)) ==
          UINT64_C(0x8000000000000000));
    check(rv64_fp_max_d(UINT64_C(0x0000000000000000),
                       UINT64_C(0x8000000000000000)) ==
          UINT64_C(0x0000000000000000));
    check(read_fflags() == 0);

    /* A signalling NaN returns the numeric operand and accrues NV. */
    check(rv64_fp_min_s(UINT32_C(0x7f800001), UINT32_C(0x3f800000)) ==
          UINT64_C(0xffffffff3f800000));
    check((read_fflags() & UINT64_C(0x10)) != 0);
}

static void test_compare_and_classify(void)
{
    write_fflags(0);
    check(rv64_fp_eq_s(UINT32_C(0x3f800000), UINT32_C(0x3f800000)) == 1);
    check(rv64_fp_lt_s(UINT32_C(0xbf800000), UINT32_C(0x00000000)) == 1);
    check(rv64_fp_le_s(UINT32_C(0x3f800000), UINT32_C(0x3f800000)) == 1);
    check(rv64_fp_eq_d(UINT64_C(0x3ff0000000000000),
                       UINT64_C(0x3ff0000000000000)) == 1);
    check(rv64_fp_lt_d(UINT64_C(0xbff0000000000000),
                       UINT64_C(0x0000000000000000)) == 1);
    check(rv64_fp_le_d(UINT64_C(0x3ff0000000000000),
                       UINT64_C(0x3ff0000000000000)) == 1);
    check(read_fflags() == 0);

    /* FEQ is quiet for qNaN, while FLT/FLE are signalling for every NaN. */
    check(rv64_fp_eq_s(UINT32_C(0x7fc00000), UINT32_C(0x3f800000)) == 0);
    check(read_fflags() == 0);
    check(rv64_fp_lt_s(UINT32_C(0x7fc00000), UINT32_C(0x3f800000)) == 0);
    check((read_fflags() & UINT64_C(0x10)) != 0);

    write_fflags(UINT64_C(0x1f));

    for (unsigned i = 0;
         i < sizeof(fclass_s_cases) / sizeof(fclass_s_cases[0]); ++i)
    {
        check(rv64_fp_class_s(fclass_s_cases[i].bits) ==
              fclass_s_cases[i].expected);
        check(read_fflags() == UINT64_C(0x1f));
    }

    for (unsigned i = 0;
         i < sizeof(fclass_d_cases) / sizeof(fclass_d_cases[0]); ++i)
    {
        check(rv64_fp_class_d(fclass_d_cases[i].bits) ==
              fclass_d_cases[i].expected);
        check(read_fflags() == UINT64_C(0x1f));
    }
}

static void test_nan_boxing_and_raw_store_exception(void)
{
    uint32_t stored = 0;

    /*
     * A computational .S consumer treats a malformed box as canonical qNaN,
     * but FSW remains a raw low-word transfer and must preserve its payload.
     */
    check(rv64_fp_malformed_s_add(UINT64_C(0x000000003f800000),
                                 UINT32_C(0x3f800000)) ==
          UINT64_C(0xffffffff7fc00000));

    rv64_fp_store_malformed_s(UINT64_C(0x012345677f800123), &stored);
    check(stored == UINT32_C(0x7f800123));

    /*
     * FMV.X.W is also a raw transfer, but RV64 sign-extends bit 31.  Both
     * positive and negative NaN payloads remain otherwise unchanged.
     */
    check(rv64_fp_move_x_w(UINT32_C(0x7f800123)) ==
          UINT64_C(0x000000007f800123));
    check(rv64_fp_move_x_w(UINT32_C(0xff800123)) ==
          UINT64_C(0xffffffffff800123));
}

static void test_malformed_exact_inputs(void)
{
    /*
     * A malformed computational S operand becomes canonical qNaN. The sign
     * source is unboxed independently, so a malformed negative-looking rhs is
     * nevertheless the positive canonical qNaN for sign selection.
     */
    check(rv64_fp_malformed_s_sgnj(
              UINT64_C(0x000000003f800000),
              UINT64_C(0xffffffffbf800000)) ==
          UINT64_C(0xffffffffffc00000));
    check(rv64_fp_malformed_s_sgnj(
              UINT64_C(0xffffffff3f800000),
              UINT64_C(0x00000000bf800000)) ==
          UINT64_C(0xffffffff3f800000));
    check(rv64_fp_malformed_s_class(
              UINT64_C(0x000000003f800000)) == (1u << 9));

    /*
     * FMV.X.W is a transfer rather than a computational consumer: it ignores
     * the malformed upper half and sign-extends only the raw low word.
     */
    check(rv64_fp_malformed_move_x_w(
              UINT64_C(0x000000007fa12345)) ==
          UINT64_C(0x000000007fa12345));
    check(rv64_fp_malformed_move_x_w(
              UINT64_C(0x0123456781234567)) ==
          UINT64_C(0xffffffff81234567));
}

static void test_exact_fp_state_effects(void)
{
    uintptr_t status = read_mstatus();

    write_fflags(UINT64_C(0x1f));
    write_mstatus((status & ~MSTATUS_FS_MASK) | MSTATUS_FS_INITIAL);
    (void)rv64_fp_write_w_x_only(UINT64_C(0x0123456781234567));

    status = read_mstatus();
    check((status & MSTATUS_FS_MASK) == MSTATUS_FS_DIRTY);
    check((status & MSTATUS_SD) != 0);
    check(read_fflags() == UINT64_C(0x1f));

    /*
     * Explicitly mark the already-populated state Clean. Raw moves from an FPR
     * and classification are read-only. NEMU deliberately tracks them
     * precisely, so this implementation regression requires FS to remain Clean
     * and SD clear; the ISA also permits conservative imprecise Dirty tracking.
     */
    write_mstatus((status & ~MSTATUS_FS_MASK) | MSTATUS_FS_CLEAN);
    check(rv64_fp_read_x_w_only() ==
          UINT64_C(0xffffffff81234567));
    check((read_mstatus() & MSTATUS_FS_MASK) == MSTATUS_FS_CLEAN);
    check((read_mstatus() & MSTATUS_SD) == 0);

    check(rv64_fp_class_s_only() == (1u << 1));
    check((read_mstatus() & MSTATUS_FS_MASK) == MSTATUS_FS_CLEAN);
    check((read_mstatus() & MSTATUS_SD) == 0);
    check(read_fflags() == UINT64_C(0x1f));

    /*
     * Self-aliased sign negation flips the seeded negative value's sign bit.
     * Because the FPR contents really change, the specification requires the
     * Clean-to-Dirty transition rather than leaving it implementation-defined.
     */
    rv64_fp_sgnjn_s_alias_only();
    status = read_mstatus();
    check((status & MSTATUS_FS_MASK) == MSTATUS_FS_DIRTY);
    check((status & MSTATUS_SD) != 0);
    check(rv64_fp_read_x_w_only() ==
          UINT64_C(0x0000000001234567));
}

static void test_exact_ops_ignore_reserved_frm(void)
{
    for (uintptr_t frm = 5; frm <= 7; ++frm)
    {
        write_frm(frm);
        check(rv64_fp_sgnj_d(
                  UINT64_C(0x3ff123456789abcd),
                  UINT64_C(0xbff0000000000000)) ==
              UINT64_C(0xbff123456789abcd));
        check(rv64_fp_class_d(
                  UINT64_C(0x7ff0000000000001)) == (1u << 8));
    }
}

static void test_exact_fp_stable_loop(void)
{
    write_fflags(UINT64_C(0x1f));
    check(rv64_fp_exact_stable_loop(
              UINT64_C(0x3ff0000000000000),
              UINT64_C(0xbff0000000000000), 64) ==
          UINT64_C(0xbff0000000000000));
    check(read_fflags() == UINT64_C(0x1f));
}

#endif

int main(void)
{
#if defined(__riscv) && __riscv_xlen == 64
    const uintptr_t old_mstatus = enable_initial_fp_state();

    test_sign_injection();
    test_min_max_and_signed_zero();
    test_compare_and_classify();
    test_nan_boxing_and_raw_store_exception();
    test_malformed_exact_inputs();
    test_exact_fp_state_effects();
    test_exact_ops_ignore_reserved_frm();
    test_exact_fp_stable_loop();

    write_mstatus(old_mstatus);
#endif

    return 0;
}
