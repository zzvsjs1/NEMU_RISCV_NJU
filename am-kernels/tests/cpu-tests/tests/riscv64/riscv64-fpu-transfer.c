#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 64

#include <stdint.h>

#define MSTATUS_FS_MASK ((uintptr_t)3u << 13)
#define MSTATUS_FS_INITIAL ((uintptr_t)1u << 13)

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

    ".option pop\n");

extern uint64_t rv64_fp_sgnj_s(uint32_t, uint32_t);
extern uint64_t rv64_fp_sgnjn_s(uint32_t, uint32_t);
extern uint64_t rv64_fp_sgnjx_s(uint32_t, uint32_t);
extern uint64_t rv64_fp_sgnj_d(uint64_t, uint64_t);
extern uint64_t rv64_fp_sgnjn_d(uint64_t, uint64_t);
extern uint64_t rv64_fp_sgnjx_d(uint64_t, uint64_t);
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

    write_fflags(0);
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

    /*
     * Sign injection is bitwise and never signals, including for a signalling
     * NaN payload.
     */
    check(rv64_fp_sgnj_s(UINT32_C(0x7f800123),
                        UINT32_C(0x3f800000)) ==
          UINT64_C(0xffffffff7f800123));
    check(read_fflags() == 0);
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

    write_fflags(0);

    for (unsigned i = 0;
         i < sizeof(fclass_s_cases) / sizeof(fclass_s_cases[0]); ++i)
    {
        check(rv64_fp_class_s(fclass_s_cases[i].bits) ==
              fclass_s_cases[i].expected);
        check(read_fflags() == 0);
    }

    for (unsigned i = 0;
         i < sizeof(fclass_d_cases) / sizeof(fclass_d_cases[0]); ++i)
    {
        check(rv64_fp_class_d(fclass_d_cases[i].bits) ==
              fclass_d_cases[i].expected);
        check(read_fflags() == 0);
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

#endif

int main(void)
{
#if defined(__riscv) && __riscv_xlen == 64
    const uintptr_t old_mstatus = enable_initial_fp_state();

    test_sign_injection();
    test_min_max_and_signed_zero();
    test_compare_and_classify();
    test_nan_boxing_and_raw_store_exception();

    write_mstatus(old_mstatus);
#endif

    return 0;
}
