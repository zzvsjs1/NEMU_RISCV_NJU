#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 32

#include <stdint.h>

volatile uint32_t rv32_d_transfer_trap_count = 0;

enum
{
    MSTATUS_FS_SHIFT = 13,
    FFLAG_NV = 1u << 4,
};

#define MSTATUS_FS_MASK ((uintptr_t)3u << MSTATUS_FS_SHIFT)
#define MSTATUS_FS_INITIAL ((uintptr_t)1u << MSTATUS_FS_SHIFT)

/*
 * This test is written before RV32D is enabled.  An integer-only handler skips
 * an unexpected four-byte instruction so a missing or incorrectly gated D
 * operation produces an ordinary failed check instead of trapping forever.
 * A conforming RV32D run must finish with the literal trap count zero.
 */
asm(
    ".section .text\n"
    ".align 2\n"
    ".option push\n"
    ".option norvc\n"
    ".globl rv32_d_transfer_trap_handler\n"
    ".type rv32_d_transfer_trap_handler, @function\n"
    "rv32_d_transfer_trap_handler:\n"
    "  la t0, rv32_d_transfer_trap_count\n"
    "  lw t1, 0(t0)\n"
    "  addi t1, t1, 1\n"
    "  sw t1, 0(t0)\n"
    "  csrr t0, mepc\n"
    "  addi t0, t0, 4\n"
    "  csrw mepc, t0\n"
    "  mret\n"
    ".size rv32_d_transfer_trap_handler, "
    ".-rv32_d_transfer_trap_handler\n"
    ".option pop\n");

extern void rv32_d_transfer_trap_handler(void);

#define RV32_D_BINARY_RESULT(name, instruction) \
    ".globl " #name "\n" \
    ".type " #name ", @function\n" \
    #name ":\n" \
    "  fld f0, 0(a0)\n" \
    "  fld f1, 0(a1)\n" \
    "  " #instruction " f2, f0, f1\n" \
    "  fsd f2, 0(a2)\n" \
    "  ret\n" \
    ".size " #name ", .-" #name "\n"

#define RV32_D_COMPARE_RESULT(name, instruction) \
    ".globl " #name "\n" \
    ".type " #name ", @function\n" \
    #name ":\n" \
    "  fld f0, 0(a0)\n" \
    "  fld f1, 0(a1)\n" \
    "  " #instruction " a0, f0, f1\n" \
    "  ret\n" \
    ".size " #name ", .-" #name "\n"

/*
 * Every helper has an integer-only ILP32 signature.  Double-precision payloads
 * cross the C/assembly boundary through naturally aligned memory because RV32D
 * deliberately has no FMV.X.D or FMV.D.X instruction.
 */
asm(
    ".section .text\n"
    ".align 2\n"
    ".option push\n"
    ".option norvc\n"
    ".option arch, +f\n"
    ".option arch, +d\n"

    RV32_D_BINARY_RESULT(rv32_d_transfer_sgnj_d, fsgnj.d)
    RV32_D_BINARY_RESULT(rv32_d_transfer_sgnjn_d, fsgnjn.d)
    RV32_D_BINARY_RESULT(rv32_d_transfer_sgnjx_d, fsgnjx.d)
    RV32_D_BINARY_RESULT(rv32_d_transfer_min_d, fmin.d)
    RV32_D_BINARY_RESULT(rv32_d_transfer_max_d, fmax.d)

    RV32_D_COMPARE_RESULT(rv32_d_transfer_eq_d, feq.d)
    RV32_D_COMPARE_RESULT(rv32_d_transfer_lt_d, flt.d)
    RV32_D_COMPARE_RESULT(rv32_d_transfer_le_d, fle.d)

    ".globl rv32_d_transfer_class_d\n"
    ".type rv32_d_transfer_class_d, @function\n"
    "rv32_d_transfer_class_d:\n"
    "  fld f0, 0(a0)\n"
    "  fclass.d a0, f0\n"
    "  ret\n"
    ".size rv32_d_transfer_class_d, .-rv32_d_transfer_class_d\n"

    ".globl rv32_d_transfer_raw_d\n"
    ".type rv32_d_transfer_raw_d, @function\n"
    "rv32_d_transfer_raw_d:\n"
    "  fld f0, 0(a0)\n"
    "  fsd f0, 0(a1)\n"
    "  ret\n"
    ".size rv32_d_transfer_raw_d, .-rv32_d_transfer_raw_d\n"

    ".globl rv32_d_transfer_box_s\n"
    ".type rv32_d_transfer_box_s, @function\n"
    "rv32_d_transfer_box_s:\n"
    "  flw f0, 0(a0)\n"
    "  fsd f0, 0(a1)\n"
    "  ret\n"
    ".size rv32_d_transfer_box_s, .-rv32_d_transfer_box_s\n"

    ".globl rv32_d_transfer_fmv_w_x\n"
    ".type rv32_d_transfer_fmv_w_x, @function\n"
    "rv32_d_transfer_fmv_w_x:\n"
    "  fmv.w.x f0, a0\n"
    "  fsd f0, 0(a1)\n"
    "  ret\n"
    ".size rv32_d_transfer_fmv_w_x, .-rv32_d_transfer_fmv_w_x\n"

    ".globl rv32_d_transfer_fsw_raw_low\n"
    ".type rv32_d_transfer_fsw_raw_low, @function\n"
    "rv32_d_transfer_fsw_raw_low:\n"
    "  fld f0, 0(a0)\n"
    "  fsw f0, 0(a1)\n"
    "  ret\n"
    ".size rv32_d_transfer_fsw_raw_low, "
    ".-rv32_d_transfer_fsw_raw_low\n"

    ".globl rv32_d_transfer_fmv_x_w_raw_low\n"
    ".type rv32_d_transfer_fmv_x_w_raw_low, @function\n"
    "rv32_d_transfer_fmv_x_w_raw_low:\n"
    "  fld f0, 0(a0)\n"
    "  fmv.x.w a0, f0\n"
    "  ret\n"
    ".size rv32_d_transfer_fmv_x_w_raw_low, "
    ".-rv32_d_transfer_fmv_x_w_raw_low\n"

    ".globl rv32_d_transfer_cvt_d_s\n"
    ".type rv32_d_transfer_cvt_d_s, @function\n"
    "rv32_d_transfer_cvt_d_s:\n"
    "  flw f0, 0(a0)\n"
    "  fcvt.d.s f1, f0\n"
    "  fsd f1, 0(a1)\n"
    "  ret\n"
    ".size rv32_d_transfer_cvt_d_s, .-rv32_d_transfer_cvt_d_s\n"

    ".globl rv32_d_transfer_cvt_s_d\n"
    ".type rv32_d_transfer_cvt_s_d, @function\n"
    "rv32_d_transfer_cvt_s_d:\n"
    "  fld f0, 0(a0)\n"
    "  fcvt.s.d f1, f0, rne\n"
    "  fsd f1, 0(a1)\n"
    "  ret\n"
    ".size rv32_d_transfer_cvt_s_d, .-rv32_d_transfer_cvt_s_d\n"

    ".globl rv32_d_transfer_malformed_add_s\n"
    ".type rv32_d_transfer_malformed_add_s, @function\n"
    "rv32_d_transfer_malformed_add_s:\n"
    "  fld f0, 0(a0)\n"
    "  fadd.s f1, f0, f0, rne\n"
    "  fsd f1, 0(a1)\n"
    "  ret\n"
    ".size rv32_d_transfer_malformed_add_s, "
    ".-rv32_d_transfer_malformed_add_s\n"

    ".globl rv32_d_transfer_malformed_cvt_d_s\n"
    ".type rv32_d_transfer_malformed_cvt_d_s, @function\n"
    "rv32_d_transfer_malformed_cvt_d_s:\n"
    "  fld f0, 0(a0)\n"
    "  fcvt.d.s f1, f0\n"
    "  fsd f1, 0(a1)\n"
    "  ret\n"
    ".size rv32_d_transfer_malformed_cvt_d_s, "
    ".-rv32_d_transfer_malformed_cvt_d_s\n"

    ".option pop\n");

extern void rv32_d_transfer_sgnj_d(const uint64_t *, const uint64_t *,
                                    uint64_t *);
extern void rv32_d_transfer_sgnjn_d(const uint64_t *, const uint64_t *,
                                     uint64_t *);
extern void rv32_d_transfer_sgnjx_d(const uint64_t *, const uint64_t *,
                                     uint64_t *);
extern void rv32_d_transfer_min_d(const uint64_t *, const uint64_t *,
                                   uint64_t *);
extern void rv32_d_transfer_max_d(const uint64_t *, const uint64_t *,
                                   uint64_t *);
extern uint32_t rv32_d_transfer_eq_d(const uint64_t *, const uint64_t *);
extern uint32_t rv32_d_transfer_lt_d(const uint64_t *, const uint64_t *);
extern uint32_t rv32_d_transfer_le_d(const uint64_t *, const uint64_t *);
extern uint32_t rv32_d_transfer_class_d(const uint64_t *);
extern void rv32_d_transfer_raw_d(const uint64_t *, uint64_t *);
extern void rv32_d_transfer_box_s(const uint32_t *, uint64_t *);
extern void rv32_d_transfer_fmv_w_x(uint32_t, uint64_t *);
extern void rv32_d_transfer_fsw_raw_low(const uint64_t *, uint32_t *);
extern uint32_t rv32_d_transfer_fmv_x_w_raw_low(const uint64_t *);
extern void rv32_d_transfer_cvt_d_s(const uint32_t *, uint64_t *);
extern void rv32_d_transfer_cvt_s_d(const uint64_t *, uint64_t *);
extern void rv32_d_transfer_malformed_add_s(const uint64_t *, uint64_t *);
extern void rv32_d_transfer_malformed_cvt_d_s(const uint64_t *, uint64_t *);

/*
 * These ten hand-written encodings independently specify every FCLASS.D result
 * bit.  Classification is a raw inspection and must not accrue NV even for the
 * signalling-NaN entry.
 */
static const struct __attribute__((aligned(8)))
{
    uint64_t bits;
    uintptr_t expected;
    uint32_t padding;
} fclass_d_cases[] = {
    {UINT64_C(0xfff0000000000000), 1u << 0, 0}, /* Negative infinity. */
    {UINT64_C(0xbff0000000000000), 1u << 1, 0}, /* Negative normal. */
    {UINT64_C(0x8000000000000001), 1u << 2, 0}, /* Negative subnormal. */
    {UINT64_C(0x8000000000000000), 1u << 3, 0}, /* Negative zero. */
    {UINT64_C(0x0000000000000000), 1u << 4, 0}, /* Positive zero. */
    {UINT64_C(0x0000000000000001), 1u << 5, 0}, /* Positive subnormal. */
    {UINT64_C(0x3ff0000000000000), 1u << 6, 0}, /* Positive normal. */
    {UINT64_C(0x7ff0000000000000), 1u << 7, 0}, /* Positive infinity. */
    {UINT64_C(0x7ff0000000000001), 1u << 8, 0}, /* Signalling NaN. */
    {UINT64_C(0x7ff8000000000000), 1u << 9, 0}, /* Quiet NaN. */
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

static void test_d_sign_injection(void)
{
    const uint64_t payload __attribute__((aligned(8))) =
        UINT64_C(0x7ff8123456789abc);
    const uint64_t negative_one __attribute__((aligned(8))) =
        UINT64_C(0xbff0000000000000);
    const uint64_t negative_payload __attribute__((aligned(8))) =
        UINT64_C(0xfff8123456789abc);
    const uint64_t signalling_nan __attribute__((aligned(8))) =
        UINT64_C(0x7ff0000000000123);
    const uint64_t positive_one __attribute__((aligned(8))) =
        UINT64_C(0x3ff0000000000000);
    uint64_t result __attribute__((aligned(8))) = UINT64_MAX;

    write_fflags(0);

    rv32_d_transfer_sgnj_d(&payload, &negative_one, &result);
    check(result == UINT64_C(0xfff8123456789abc));

    rv32_d_transfer_sgnjn_d(&payload, &negative_one, &result);
    check(result == UINT64_C(0x7ff8123456789abc));

    rv32_d_transfer_sgnjx_d(&negative_payload, &negative_one, &result);
    check(result == UINT64_C(0x7ff8123456789abc));

    /*
     * Sign injection copies bits rather than performing arithmetic.  The
     * signalling payload must survive unchanged and NV must remain clear.
     */
    rv32_d_transfer_sgnj_d(&signalling_nan, &positive_one, &result);
    check(result == UINT64_C(0x7ff0000000000123));
    check(read_fflags() == 0);
}

static void test_d_min_max_and_signed_zero(void)
{
    const uint64_t positive_zero __attribute__((aligned(8))) = 0;
    const uint64_t negative_zero __attribute__((aligned(8))) =
        UINT64_C(0x8000000000000000);
    const uint64_t one __attribute__((aligned(8))) =
        UINT64_C(0x3ff0000000000000);
    const uint64_t two __attribute__((aligned(8))) =
        UINT64_C(0x4000000000000000);
    const uint64_t quiet_nan_a __attribute__((aligned(8))) =
        UINT64_C(0x7ff8000000000123);
    const uint64_t quiet_nan_b __attribute__((aligned(8))) =
        UINT64_C(0x7ff8000000000456);
    const uint64_t signalling_nan __attribute__((aligned(8))) =
        UINT64_C(0x7ff0000000000123);
    uint64_t result __attribute__((aligned(8))) = UINT64_MAX;

    write_fflags(0);
    rv32_d_transfer_min_d(&positive_zero, &negative_zero, &result);
    check(result == UINT64_C(0x8000000000000000));
    rv32_d_transfer_max_d(&positive_zero, &negative_zero, &result);
    check(result == UINT64_C(0x0000000000000000));

    rv32_d_transfer_min_d(&two, &one, &result);
    check(result == UINT64_C(0x3ff0000000000000));
    rv32_d_transfer_max_d(&two, &one, &result);
    check(result == UINT64_C(0x4000000000000000));
    check(read_fflags() == 0);

    /* One quiet NaN selects the numeric operand without raising NV. */
    rv32_d_transfer_min_d(&quiet_nan_a, &one, &result);
    check(result == UINT64_C(0x3ff0000000000000));
    check(read_fflags() == 0);

    /* One signalling NaN still selects the number, but it must accrue NV. */
    write_fflags(0);
    rv32_d_transfer_max_d(&signalling_nan, &one, &result);
    check(result == UINT64_C(0x3ff0000000000000));
    check(read_fflags() == FFLAG_NV);

    /* Two quiet NaNs produce the literal canonical binary64 quiet NaN. */
    write_fflags(0);
    rv32_d_transfer_min_d(&quiet_nan_a, &quiet_nan_b, &result);
    check(result == UINT64_C(0x7ff8000000000000));
    check(read_fflags() == 0);

    /* A signalling NaN among two NaNs adds NV to the same canonical result. */
    write_fflags(0);
    rv32_d_transfer_max_d(&signalling_nan, &quiet_nan_b, &result);
    check(result == UINT64_C(0x7ff8000000000000));
    check(read_fflags() == FFLAG_NV);
}

static void test_d_compare_and_classify(void)
{
    const uint64_t negative_one __attribute__((aligned(8))) =
        UINT64_C(0xbff0000000000000);
    const uint64_t positive_zero __attribute__((aligned(8))) = 0;
    const uint64_t one __attribute__((aligned(8))) =
        UINT64_C(0x3ff0000000000000);
    const uint64_t quiet_nan __attribute__((aligned(8))) =
        UINT64_C(0x7ff8000000000000);
    const uint64_t signalling_nan __attribute__((aligned(8))) =
        UINT64_C(0x7ff0000000000001);

    write_fflags(0);
    check(rv32_d_transfer_eq_d(&one, &one) == 1);
    check(rv32_d_transfer_lt_d(&negative_one, &positive_zero) == 1);
    check(rv32_d_transfer_le_d(&one, &one) == 1);
    check(read_fflags() == 0);

    /* FEQ is quiet for qNaN, but it raises NV for a signalling NaN. */
    check(rv32_d_transfer_eq_d(&quiet_nan, &one) == 0);
    check(read_fflags() == 0);
    write_fflags(0);
    check(rv32_d_transfer_eq_d(&signalling_nan, &one) == 0);
    check(read_fflags() == FFLAG_NV);

    /* FLT and FLE are signalling comparisons for every kind of NaN. */
    write_fflags(0);
    check(rv32_d_transfer_lt_d(&quiet_nan, &one) == 0);
    check(read_fflags() == FFLAG_NV);
    write_fflags(0);
    check(rv32_d_transfer_le_d(&quiet_nan, &one) == 0);
    check(read_fflags() == FFLAG_NV);

    write_fflags(0);
    for (unsigned i = 0;
         i < sizeof(fclass_d_cases) / sizeof(fclass_d_cases[0]); ++i)
    {
        check(rv32_d_transfer_class_d(&fclass_d_cases[i].bits) ==
              fclass_d_cases[i].expected);
        check(read_fflags() == 0);
    }
}

static void test_d_raw_transfer_conversion_and_nan_boxing(void)
{
    static const uint64_t raw_cases[] __attribute__((aligned(8))) = {
        UINT64_C(0x0000000000000000),
        UINT64_C(0x8000000000000000),
        UINT64_C(0x7ff0000000000123),
        UINT64_C(0xfff8fedcba987654),
    };
    const uint32_t single_signalling_nan __attribute__((aligned(4))) =
        UINT32_C(0x7f800123);
    const uint32_t single_one_and_half __attribute__((aligned(4))) =
        UINT32_C(0x3fc00000);
    const uint64_t double_negative_two_and_quarter
        __attribute__((aligned(8))) = UINT64_C(0xc002000000000000);
    const uint64_t malformed_single_one __attribute__((aligned(8))) =
        UINT64_C(0x000000003f800000);
    const uint64_t malformed_raw_transfer __attribute__((aligned(8))) =
        UINT64_C(0x01234567deadbeef);
    uint32_t raw_low_word __attribute__((aligned(4))) = 0;
    uint64_t result __attribute__((aligned(8))) =
        UINT64_C(0xdeadbeefcafebabe);

    write_fflags(0);

    /*
     * FLD and FSD are raw transfers.  NaN payloads and signs must not be
     * canonicalised merely because the bits passed through an FPR.
     */
    for (unsigned i = 0;
         i < sizeof(raw_cases) / sizeof(raw_cases[0]); ++i)
    {
        result = UINT64_C(0xdeadbeefcafebabe);
        rv32_d_transfer_raw_d(&raw_cases[i], &result);
        check(result == raw_cases[i]);
    }

    /* FLW into an FLEN=64 register writes a complete NaN-boxed S value. */
    rv32_d_transfer_box_s(&single_signalling_nan, &result);
    check(result == UINT64_C(0xffffffff7f800123));

    /*
     * FMV.W.X is the other narrow transfer into an FPR and must create the
     * same NaN box as FLW. Conversely, FSW and FMV.X.W transfer the raw low
     * word even when the source is not a valid NaN-boxed single.
     */
    rv32_d_transfer_fmv_w_x(single_signalling_nan, &result);
    check(result == UINT64_C(0xffffffff7f800123));
    rv32_d_transfer_fsw_raw_low(&malformed_raw_transfer, &raw_low_word);
    check(raw_low_word == UINT32_C(0xdeadbeef));
    check(rv32_d_transfer_fmv_x_w_raw_low(&malformed_raw_transfer) ==
          UINT32_C(0xdeadbeef));

    /* Both exact cross-precision conversions have literal IEEE-754 results. */
    rv32_d_transfer_cvt_d_s(&single_one_and_half, &result);
    check(result == UINT64_C(0x3ff8000000000000));
    rv32_d_transfer_cvt_s_d(&double_negative_two_and_quarter, &result);
    check(result == UINT64_C(0xffffffffc0100000));
    check(read_fflags() == 0);

    /*
     * The low word encodes S +1.0, but the upper word is not all ones.  Every
     * computational S consumer must therefore substitute canonical S qNaN.
     */
    rv32_d_transfer_malformed_add_s(&malformed_single_one, &result);
    check(result == UINT64_C(0xffffffff7fc00000));
    rv32_d_transfer_malformed_cvt_d_s(&malformed_single_one, &result);
    check(result == UINT64_C(0x7ff8000000000000));
    check(read_fflags() == 0);
}

#endif

int main(void)
{
#if defined(__riscv) && __riscv_xlen == 32
    const uintptr_t old_mstatus = read_mstatus();
    const uintptr_t old_mtvec = read_mtvec();

    rv32_d_transfer_trap_count = 0;
    write_mtvec((uintptr_t)rv32_d_transfer_trap_handler);
    write_mstatus((old_mstatus & ~MSTATUS_FS_MASK) | MSTATUS_FS_INITIAL);

    test_d_sign_injection();
    test_d_min_max_and_signed_zero();
    test_d_compare_and_classify();
    test_d_raw_transfer_conversion_and_nan_boxing();

    write_mstatus(old_mstatus);
    write_mtvec(old_mtvec);
    check(rv32_d_transfer_trap_count == 0);
#endif

    return 0;
}
