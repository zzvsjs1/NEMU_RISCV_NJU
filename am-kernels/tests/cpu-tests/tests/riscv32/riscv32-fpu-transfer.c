#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 32

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
    "  fmv.x.w a0, f0\n" \
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

/*
 * These helpers use only the integer calling convention.  The local ISA
 * option lets the assembler encode F instructions without changing the C ABI.
 */
asm(
    ".section .text\n"
    ".align 2\n"
    ".option push\n"
    ".option norvc\n"
    ".option arch,+f\n"

    FP_ASM_BITWISE_S(rv32_fp_sgnj_s, fsgnj.s)
    FP_ASM_BITWISE_S(rv32_fp_sgnjn_s, fsgnjn.s)
    FP_ASM_BITWISE_S(rv32_fp_sgnjx_s, fsgnjx.s)
    FP_ASM_BITWISE_S(rv32_fp_min_s, fmin.s)
    FP_ASM_BITWISE_S(rv32_fp_max_s, fmax.s)

    FP_ASM_COMPARE_S(rv32_fp_eq_s, feq.s)
    FP_ASM_COMPARE_S(rv32_fp_lt_s, flt.s)
    FP_ASM_COMPARE_S(rv32_fp_le_s, fle.s)

    ".globl rv32_fp_class_s\n"
    ".type rv32_fp_class_s, @function\n"
    "rv32_fp_class_s:\n"
    "  fmv.w.x f0, a0\n"
    "  fclass.s a0, f0\n"
    "  ret\n"
    ".size rv32_fp_class_s, .-rv32_fp_class_s\n"

    ".globl rv32_fp_move_round_trip\n"
    ".type rv32_fp_move_round_trip, @function\n"
    "rv32_fp_move_round_trip:\n"
    "  fmv.w.x f0, a0\n"
    "  fmv.x.w a0, f0\n"
    "  ret\n"
    ".size rv32_fp_move_round_trip, .-rv32_fp_move_round_trip\n"

    ".globl rv32_fp_load_word\n"
    ".type rv32_fp_load_word, @function\n"
    "rv32_fp_load_word:\n"
    "  flw f0, 0(a0)\n"
    "  fmv.x.w a0, f0\n"
    "  ret\n"
    ".size rv32_fp_load_word, .-rv32_fp_load_word\n"

    ".globl rv32_fp_store_word\n"
    ".type rv32_fp_store_word, @function\n"
    "rv32_fp_store_word:\n"
    "  fmv.w.x f0, a0\n"
    "  fsw f0, 0(a1)\n"
    "  ret\n"
    ".size rv32_fp_store_word, .-rv32_fp_store_word\n"

    ".globl rv32_fp_load_store_word\n"
    ".type rv32_fp_load_store_word, @function\n"
    "rv32_fp_load_store_word:\n"
    "  flw f0, 0(a0)\n"
    "  fsw f0, 0(a1)\n"
    "  ret\n"
    ".size rv32_fp_load_store_word, .-rv32_fp_load_store_word\n"

    ".globl rv32_fp_check_register_independence\n"
    ".type rv32_fp_check_register_independence, @function\n"
    "rv32_fp_check_register_independence:\n"
    "  fmv.w.x f0, a0\n"
    "  fmv.w.x f31, a1\n"
    "  fmv.x.w t0, f0\n"
    "  fmv.x.w t1, f31\n"
    "  sw t0, 0(a2)\n"
    "  sw t1, 4(a2)\n"
    "  ret\n"
    ".size rv32_fp_check_register_independence, "
    ".-rv32_fp_check_register_independence\n"

    ".option pop\n");

extern uint32_t rv32_fp_sgnj_s(uint32_t, uint32_t);
extern uint32_t rv32_fp_sgnjn_s(uint32_t, uint32_t);
extern uint32_t rv32_fp_sgnjx_s(uint32_t, uint32_t);
extern uint32_t rv32_fp_min_s(uint32_t, uint32_t);
extern uint32_t rv32_fp_max_s(uint32_t, uint32_t);
extern uint32_t rv32_fp_eq_s(uint32_t, uint32_t);
extern uint32_t rv32_fp_lt_s(uint32_t, uint32_t);
extern uint32_t rv32_fp_le_s(uint32_t, uint32_t);
extern uint32_t rv32_fp_class_s(uint32_t);
extern uint32_t rv32_fp_move_round_trip(uint32_t);
extern uint32_t rv32_fp_load_word(const uint32_t *);
extern void rv32_fp_store_word(uint32_t, uint32_t *);
extern void rv32_fp_load_store_word(const uint32_t *, uint32_t *);
extern void rv32_fp_check_register_independence(uint32_t, uint32_t,
                                                uint32_t *);

enum
{
    FFLAG_NV = 1u << 4,
};

/*
 * These literal encodings cover every architectural FCLASS result bit.  A
 * signalling NaN is included because classification must observe its payload
 * without raising the invalid-operation flag.
 */
static const struct
{
    uint32_t bits;
    uintptr_t expected;
} fclass_s_cases[] = {
    {UINT32_C(0xff800000), 1u << 0}, /* Negative infinity. */
    {UINT32_C(0xbf800000), 1u << 1}, /* Negative normal. */
    {UINT32_C(0x80000001), 1u << 2}, /* Negative subnormal. */
    {UINT32_C(0x80000000), 1u << 3}, /* Negative zero. */
    {UINT32_C(0x00000000), 1u << 4}, /* Positive zero. */
    {UINT32_C(0x00000001), 1u << 5}, /* Positive subnormal. */
    {UINT32_C(0x3f800000), 1u << 6}, /* Positive normal. */
    {UINT32_C(0x7f800000), 1u << 7}, /* Positive infinity. */
    {UINT32_C(0x7f800001), 1u << 8}, /* Signalling NaN. */
    {UINT32_C(0x7fc00000), 1u << 9}, /* Quiet NaN. */
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
    const uint32_t payload = UINT32_C(0x7fc12345);
    const uint32_t negative = UINT32_C(0xbf800000);

    write_fflags(0);
    check(rv32_fp_sgnj_s(payload, negative) == UINT32_C(0xffc12345));
    check(rv32_fp_sgnjn_s(payload, negative) == payload);
    check(rv32_fp_sgnjx_s(UINT32_C(0xffc12345), negative) == payload);

    /*
     * Sign injection is bitwise, does not canonicalise a signalling NaN, and
     * never raises a floating-point exception.
     */
    check(rv32_fp_sgnj_s(UINT32_C(0x7f800123),
                        UINT32_C(0x3f800000)) ==
          UINT32_C(0x7f800123));
    check(read_fflags() == 0);
}

static void test_min_max_and_signed_zero(void)
{
    write_fflags(0);
    check(rv32_fp_min_s(UINT32_C(0x00000000),
                       UINT32_C(0x80000000)) ==
          UINT32_C(0x80000000));
    check(rv32_fp_max_s(UINT32_C(0x00000000),
                       UINT32_C(0x80000000)) ==
          UINT32_C(0x00000000));
    check(rv32_fp_min_s(UINT32_C(0x40000000),
                       UINT32_C(0x3f800000)) ==
          UINT32_C(0x3f800000));
    check(rv32_fp_max_s(UINT32_C(0x40000000),
                       UINT32_C(0x3f800000)) ==
          UINT32_C(0x40000000));
    check(read_fflags() == 0);

    /* A quiet NaN yields the numeric operand without raising NV. */
    check(rv32_fp_min_s(UINT32_C(0x7fc00123),
                       UINT32_C(0x3f800000)) ==
          UINT32_C(0x3f800000));
    check(read_fflags() == 0);

    /* A signalling NaN also yields the numeric operand, but it accrues NV. */
    write_fflags(0);
    check(rv32_fp_max_s(UINT32_C(0x7f800001),
                       UINT32_C(0x3f800000)) ==
          UINT32_C(0x3f800000));
    check(read_fflags() == FFLAG_NV);

    /* Two NaN inputs produce the canonical single-precision quiet NaN. */
    write_fflags(0);
    check(rv32_fp_min_s(UINT32_C(0x7fc00123),
                       UINT32_C(0x7fc00456)) ==
          UINT32_C(0x7fc00000));
    check(read_fflags() == 0);
}

static void test_compare_and_classify(void)
{
    write_fflags(0);
    check(rv32_fp_eq_s(UINT32_C(0x3f800000),
                      UINT32_C(0x3f800000)) == 1);
    check(rv32_fp_lt_s(UINT32_C(0xbf800000),
                      UINT32_C(0x00000000)) == 1);
    check(rv32_fp_le_s(UINT32_C(0x3f800000),
                      UINT32_C(0x3f800000)) == 1);
    check(read_fflags() == 0);

    /* FEQ is quiet for qNaN, but signalling NaNs still raise NV. */
    check(rv32_fp_eq_s(UINT32_C(0x7fc00000),
                      UINT32_C(0x3f800000)) == 0);
    check(read_fflags() == 0);

    write_fflags(0);
    check(rv32_fp_eq_s(UINT32_C(0x7f800001),
                      UINT32_C(0x3f800000)) == 0);
    check(read_fflags() == FFLAG_NV);

    /* FLT and FLE are signalling comparisons for every NaN encoding. */
    write_fflags(0);
    check(rv32_fp_lt_s(UINT32_C(0x7fc00000),
                      UINT32_C(0x3f800000)) == 0);
    check(read_fflags() == FFLAG_NV);

    write_fflags(0);
    check(rv32_fp_le_s(UINT32_C(0x7fc00000),
                      UINT32_C(0x3f800000)) == 0);
    check(read_fflags() == FFLAG_NV);

    write_fflags(0);
    for (unsigned i = 0;
         i < sizeof(fclass_s_cases) / sizeof(fclass_s_cases[0]); ++i)
    {
        check(rv32_fp_class_s(fclass_s_cases[i].bits) ==
              fclass_s_cases[i].expected);
        check(read_fflags() == 0);
    }
}

static void test_raw_moves_and_memory_transfers(void)
{
    /*
     * FLEN and XLEN are both 32 in RV32F, so each case is a complete register
     * transfer rather than a NaN-boxed narrow value.  The NaN payloads make
     * accidental canonicalisation observable.
     */
    static const uint32_t raw_cases[] = {
        UINT32_C(0x00000000),
        UINT32_C(0x80000000),
        UINT32_C(0x7f800123),
        UINT32_C(0xffc54321),
    };
    uint32_t source __attribute__((aligned(4)));
    uint32_t stored __attribute__((aligned(4)));
    uint32_t independent[2] __attribute__((aligned(4)));

    write_fflags(0);

    for (unsigned i = 0;
         i < sizeof(raw_cases) / sizeof(raw_cases[0]); ++i)
    {
        const uint32_t bits = raw_cases[i];

        check(rv32_fp_move_round_trip(bits) == bits);

        source = bits;
        check(rv32_fp_load_word(&source) == bits);

        stored = UINT32_C(0xdeadbeef);
        rv32_fp_store_word(bits, &stored);
        check(stored == bits);

        stored = UINT32_C(0xdeadbeef);
        rv32_fp_load_store_word(&source, &stored);
        check(stored == bits);
    }

    /*
     * Distinct values in the first and last FPR make either an undersized
     * register file or a broken five-bit register index immediately visible.
     */
    rv32_fp_check_register_independence(UINT32_C(0x7f800123),
                                        UINT32_C(0xffc54321),
                                        independent);
    check(independent[0] == UINT32_C(0x7f800123));
    check(independent[1] == UINT32_C(0xffc54321));

    /* Raw moves, loads, and stores do not accrue floating-point exceptions. */
    check(read_fflags() == 0);
}

#endif

int main(void)
{
#if defined(__riscv) && __riscv_xlen == 32
    const uintptr_t old_mstatus = enable_initial_fp_state();

    test_sign_injection();
    test_min_max_and_signed_zero();
    test_compare_and_classify();
    test_raw_moves_and_memory_transfers();

    write_mstatus(old_mstatus);
#endif

    return 0;
}
