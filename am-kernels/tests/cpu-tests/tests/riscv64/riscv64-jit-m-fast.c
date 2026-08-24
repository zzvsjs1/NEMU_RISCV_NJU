#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 64

#include <stdint.h>

#define DEFINE_RV64_M_OP(name, mnemonic) \
    static uint64_t name(uint64_t lhs, uint64_t rhs) \
    { \
        uint64_t result; \
        asm volatile(#mnemonic " %0, %1, %2" : "=r"(result) : "r"(lhs), "r"(rhs)); \
        return result; \
    }

DEFINE_RV64_M_OP(op_mul, mul)
DEFINE_RV64_M_OP(op_mulh, mulh)
DEFINE_RV64_M_OP(op_mulhsu, mulhsu)
DEFINE_RV64_M_OP(op_mulhu, mulhu)
DEFINE_RV64_M_OP(op_div, div)
DEFINE_RV64_M_OP(op_divu, divu)
DEFINE_RV64_M_OP(op_rem, rem)
DEFINE_RV64_M_OP(op_remu, remu)
DEFINE_RV64_M_OP(op_mulw, mulw)
DEFINE_RV64_M_OP(op_divw, divw)
DEFINE_RV64_M_OP(op_divuw, divuw)
DEFINE_RV64_M_OP(op_remw, remw)
DEFINE_RV64_M_OP(op_remuw, remuw)

/*
 * Fixed architectural registers make the alias relationships independent of
 * the C compiler's register allocator. Each source pair is consumed before the
 * aliased destination is written, which is also the contract required from the
 * native RDX:RAX lowering.
 */
asm(".section .text\n"
    ".align 2\n"
    ".option push\n"
    ".option norvc\n"
    ".option arch, +m\n"

    ".globl rv64_m_alias_rd_rs1_mulh\n"
    ".type rv64_m_alias_rd_rs1_mulh, @function\n"
    "rv64_m_alias_rd_rs1_mulh:\n"
    "  mulh a0, a0, a1\n"
    "  ret\n"
    ".size rv64_m_alias_rd_rs1_mulh, "
    ".-rv64_m_alias_rd_rs1_mulh\n"

    ".globl rv64_m_alias_rd_rs2_mulhsu\n"
    ".type rv64_m_alias_rd_rs2_mulhsu, @function\n"
    "rv64_m_alias_rd_rs2_mulhsu:\n"
    "  mulhsu a1, a0, a1\n"
    "  mv a0, a1\n"
    "  ret\n"
    ".size rv64_m_alias_rd_rs2_mulhsu, "
    ".-rv64_m_alias_rd_rs2_mulhsu\n"

    ".globl rv64_m_alias_equal_mulhu\n"
    ".type rv64_m_alias_equal_mulhu, @function\n"
    "rv64_m_alias_equal_mulhu:\n"
    "  mulhu a0, a0, a0\n"
    "  ret\n"
    ".size rv64_m_alias_equal_mulhu, "
    ".-rv64_m_alias_equal_mulhu\n"

    ".globl rv64_m_alias_rd_rs1_div\n"
    ".type rv64_m_alias_rd_rs1_div, @function\n"
    "rv64_m_alias_rd_rs1_div:\n"
    "  div a0, a0, a1\n"
    "  ret\n"
    ".size rv64_m_alias_rd_rs1_div, "
    ".-rv64_m_alias_rd_rs1_div\n"

    ".globl rv64_m_alias_rd_rs2_rem\n"
    ".type rv64_m_alias_rd_rs2_rem, @function\n"
    "rv64_m_alias_rd_rs2_rem:\n"
    "  rem a1, a0, a1\n"
    "  mv a0, a1\n"
    "  ret\n"
    ".size rv64_m_alias_rd_rs2_rem, "
    ".-rv64_m_alias_rd_rs2_rem\n"

    ".globl rv64_m_alias_equal_divu\n"
    ".type rv64_m_alias_equal_divu, @function\n"
    "rv64_m_alias_equal_divu:\n"
    "  divu a0, a0, a0\n"
    "  ret\n"
    ".size rv64_m_alias_equal_divu, "
    ".-rv64_m_alias_equal_divu\n"

    ".globl rv64_m_alias_rd_rs1_remu\n"
    ".type rv64_m_alias_rd_rs1_remu, @function\n"
    "rv64_m_alias_rd_rs1_remu:\n"
    "  remu a0, a0, a1\n"
    "  ret\n"
    ".size rv64_m_alias_rd_rs1_remu, "
    ".-rv64_m_alias_rd_rs1_remu\n"

    ".globl rv64_m_alias_rd_rs1_divw\n"
    ".type rv64_m_alias_rd_rs1_divw, @function\n"
    "rv64_m_alias_rd_rs1_divw:\n"
    "  divw a0, a0, a1\n"
    "  ret\n"
    ".size rv64_m_alias_rd_rs1_divw, "
    ".-rv64_m_alias_rd_rs1_divw\n"

    ".globl rv64_m_alias_rd_rs2_remw\n"
    ".type rv64_m_alias_rd_rs2_remw, @function\n"
    "rv64_m_alias_rd_rs2_remw:\n"
    "  remw a1, a0, a1\n"
    "  mv a0, a1\n"
    "  ret\n"
    ".size rv64_m_alias_rd_rs2_remw, "
    ".-rv64_m_alias_rd_rs2_remw\n"

    ".globl rv64_m_alias_equal_divuw\n"
    ".type rv64_m_alias_equal_divuw, @function\n"
    "rv64_m_alias_equal_divuw:\n"
    "  divuw a0, a0, a0\n"
    "  ret\n"
    ".size rv64_m_alias_equal_divuw, "
    ".-rv64_m_alias_equal_divuw\n"

    ".globl rv64_m_alias_rd_rs1_remuw\n"
    ".type rv64_m_alias_rd_rs1_remuw, @function\n"
    "rv64_m_alias_rd_rs1_remuw:\n"
    "  remuw a0, a0, a1\n"
    "  ret\n"
    ".size rv64_m_alias_rd_rs1_remuw, "
    ".-rv64_m_alias_rd_rs1_remuw\n"

    ".globl rv64_m_alias_rd_rs1_mulw\n"
    ".type rv64_m_alias_rd_rs1_mulw, @function\n"
    "rv64_m_alias_rd_rs1_mulw:\n"
    "  mulw a0, a0, a1\n"
    "  ret\n"
    ".size rv64_m_alias_rd_rs1_mulw, "
    ".-rv64_m_alias_rd_rs1_mulw\n"

    ".globl rv64_m_alias_rd_rs2_mulw\n"
    ".type rv64_m_alias_rd_rs2_mulw, @function\n"
    "rv64_m_alias_rd_rs2_mulw:\n"
    "  mulw a1, a0, a1\n"
    "  mv a0, a1\n"
    "  ret\n"
    ".size rv64_m_alias_rd_rs2_mulw, "
    ".-rv64_m_alias_rd_rs2_mulw\n"

    /*
     * Every operation writes x0, including all inputs that would trap an
     * unguarded x86 DIV/IDIV. A legal dead-result lowering may emit no host
     * arithmetic, but it must still retire each guest instruction.
     */
    ".globl rv64_m_x0_probe\n"
    ".type rv64_m_x0_probe, @function\n"
    "rv64_m_x0_probe:\n"
    "  li t0, -1\n"
    "  li t1, 0\n"
    "  mul zero, t0, t1\n"
    "  mulh zero, t0, t1\n"
    "  mulhsu zero, t0, t1\n"
    "  mulhu zero, t0, t1\n"
    "  div zero, t0, t1\n"
    "  divu zero, t0, t1\n"
    "  rem zero, t0, t1\n"
    "  remu zero, t0, t1\n"
    "  mulw zero, t0, t1\n"
    "  divw zero, t0, t1\n"
    "  divuw zero, t0, t1\n"
    "  remw zero, t0, t1\n"
    "  remuw zero, t0, t1\n"
    "  li t0, 0x8000000000000000\n"
    "  li t1, -1\n"
    "  div zero, t0, t1\n"
    "  rem zero, t0, t1\n"
    "  li t0, 0x80000000\n"
    "  divw zero, t0, t1\n"
    "  remw zero, t0, t1\n"
    "  mv a0, zero\n"
    "  ret\n"
    ".size rv64_m_x0_probe, .-rv64_m_x0_probe\n"

    /*
     * Encode x0 explicitly as a source so constant-result specialisations do
     * not depend on the C compiler's register choices.
     */
    ".globl rv64_m_source_x0_probe\n"
    ".type rv64_m_source_x0_probe, @function\n"
    "rv64_m_source_x0_probe:\n"
    "  mul t0, zero, a0\n"
    "  sd t0, 0(a1)\n"
    "  mulh t0, a0, zero\n"
    "  sd t0, 8(a1)\n"
    "  mulhsu t0, zero, a0\n"
    "  sd t0, 16(a1)\n"
    "  mulhu t0, a0, zero\n"
    "  sd t0, 24(a1)\n"
    "  div t0, a0, zero\n"
    "  sd t0, 32(a1)\n"
    "  divu t0, a0, zero\n"
    "  sd t0, 40(a1)\n"
    "  rem t0, a0, zero\n"
    "  sd t0, 48(a1)\n"
    "  remu t0, a0, zero\n"
    "  sd t0, 56(a1)\n"
    "  mulw t0, a0, zero\n"
    "  sd t0, 64(a1)\n"
    "  divw t0, a0, zero\n"
    "  sd t0, 72(a1)\n"
    "  divuw t0, a0, zero\n"
    "  sd t0, 80(a1)\n"
    "  remw t0, a0, zero\n"
    "  sd t0, 88(a1)\n"
    "  remuw t0, a0, zero\n"
    "  sd t0, 96(a1)\n"
    "  ret\n"
    ".size rv64_m_source_x0_probe, .-rv64_m_source_x0_probe\n"

    /*
     * The divisor is held in a1 even when its run-time value is zero. These
     * instructions therefore exercise the emitted TEST/Jcc guards rather than
     * the architectural-x0 specialisations above.
     */
    ".globl rv64_m_runtime_zero_probe\n"
    ".type rv64_m_runtime_zero_probe, @function\n"
    "rv64_m_runtime_zero_probe:\n"
    "  div t0, a0, a1\n"
    "  sd t0, 0(a2)\n"
    "  divu t0, a0, a1\n"
    "  sd t0, 8(a2)\n"
    "  rem t0, a0, a1\n"
    "  sd t0, 16(a2)\n"
    "  remu t0, a0, a1\n"
    "  sd t0, 24(a2)\n"
    "  divw t0, a0, a1\n"
    "  sd t0, 32(a2)\n"
    "  divuw t0, a0, a1\n"
    "  sd t0, 40(a2)\n"
    "  remw t0, a0, a1\n"
    "  sd t0, 48(a2)\n"
    "  remuw t0, a0, a1\n"
    "  sd t0, 56(a2)\n"
    "  ret\n"
    ".size rv64_m_runtime_zero_probe, .-rv64_m_runtime_zero_probe\n"

    /*
     * Keep the specification-recommended high/low multiply and quotient/
     * remainder pairs adjacent and in identical source order. Pair fusion is a
     * later optimisation; this regression first fixes their observable values.
     */
    ".globl rv64_m_pair_probe\n"
    ".type rv64_m_pair_probe, @function\n"
    "rv64_m_pair_probe:\n"
    "  mulh t0, a0, a1\n"
    "  mul t1, a0, a1\n"
    "  div t2, a0, a1\n"
    "  rem t3, a0, a1\n"
    "  sd t0, 0(a2)\n"
    "  sd t1, 8(a2)\n"
    "  sd t2, 16(a2)\n"
    "  sd t3, 24(a2)\n"
    "  ret\n"
    ".size rv64_m_pair_probe, .-rv64_m_pair_probe\n"

    /*
     * Cover the other three architecturally recommended adjacent pairs with
     * identical source order and destinations distinct from both inputs.
     */
    ".globl rv64_m_unsigned_pair_probe\n"
    ".type rv64_m_unsigned_pair_probe, @function\n"
    "rv64_m_unsigned_pair_probe:\n"
    "  mulhsu t0, a0, a1\n"
    "  mul t1, a0, a1\n"
    "  mulhu t2, a0, a1\n"
    "  mul t3, a0, a1\n"
    "  divu t4, a0, a1\n"
    "  remu t5, a0, a1\n"
    "  sd t0, 0(a2)\n"
    "  sd t1, 8(a2)\n"
    "  sd t2, 16(a2)\n"
    "  sd t3, 24(a2)\n"
    "  sd t4, 32(a2)\n"
    "  sd t5, 40(a2)\n"
    "  ret\n"
    ".size rv64_m_unsigned_pair_probe, "
    ".-rv64_m_unsigned_pair_probe\n"

    /*
     * The function entry is also the loop header, so the backwards branch is a
     * real block self-backedge. Six loop-carried GPRs fit within the seven-slot
     * helper-free cache. R8 must remain untouched by every native M emitter.
     */
    ".globl rv64_m_stable_loop\n"
    ".type rv64_m_stable_loop, @function\n"
    "rv64_m_stable_loop:\n"
    "  mul a0, a0, a1\n"
    "  mulhu a2, a2, a3\n"
    "  divu a4, a4, a1\n"
    "  addi a5, a5, -1\n"
    "  bne a5, zero, rv64_m_stable_loop\n"
    "  xor a0, a0, a2\n"
    "  xor a0, a0, a4\n"
    "  ret\n"
    ".size rv64_m_stable_loop, .-rv64_m_stable_loop\n"

    ".option pop\n");

extern uint64_t rv64_m_alias_rd_rs1_mulh(uint64_t, uint64_t);
extern uint64_t rv64_m_alias_rd_rs2_mulhsu(uint64_t, uint64_t);
extern uint64_t rv64_m_alias_equal_mulhu(uint64_t);
extern uint64_t rv64_m_alias_rd_rs1_div(uint64_t, uint64_t);
extern uint64_t rv64_m_alias_rd_rs2_rem(uint64_t, uint64_t);
extern uint64_t rv64_m_alias_equal_divu(uint64_t);
extern uint64_t rv64_m_alias_rd_rs1_remu(uint64_t, uint64_t);
extern uint64_t rv64_m_alias_rd_rs1_divw(uint64_t, uint64_t);
extern uint64_t rv64_m_alias_rd_rs2_remw(uint64_t, uint64_t);
extern uint64_t rv64_m_alias_equal_divuw(uint64_t);
extern uint64_t rv64_m_alias_rd_rs1_remuw(uint64_t, uint64_t);
extern uint64_t rv64_m_alias_rd_rs1_mulw(uint64_t, uint64_t);
extern uint64_t rv64_m_alias_rd_rs2_mulw(uint64_t, uint64_t);
extern uint64_t rv64_m_x0_probe(void);
extern void rv64_m_source_x0_probe(uint64_t, uint64_t *);
extern void rv64_m_runtime_zero_probe(uint64_t, uint64_t, uint64_t *);
extern void rv64_m_pair_probe(uint64_t, uint64_t, uint64_t *);
extern void rv64_m_unsigned_pair_probe(uint64_t, uint64_t, uint64_t *);
extern uint64_t rv64_m_stable_loop(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

static void test_ordinary_results(void)
{
    check(op_mul(UINT64_MAX, 3) == UINT64_C(0xfffffffffffffffd));
    check(op_mulh(UINT64_C(0xffffffffffffff00), UINT64_C(0x1000000000000001)) == UINT64_C(0xffffffffffffffef));
    check(op_mulhsu(UINT64_MAX, 2) == UINT64_MAX);
    check(op_mulhsu(2, UINT64_MAX) == 1);
    check(op_mulhsu((uint64_t)(int64_t)-2, UINT64_MAX) == UINT64_C(0xfffffffffffffffe));
    check(op_mulhu(UINT64_MAX, 2) == 1);

    check(op_div((uint64_t)(int64_t)-42, 5) == (uint64_t)(int64_t)-8);
    check(op_rem((uint64_t)(int64_t)-42, 5) == (uint64_t)(int64_t)-2);
    check(op_div(42, (uint64_t)(int64_t)-5) == (uint64_t)(int64_t)-8);
    check(op_rem(42, (uint64_t)(int64_t)-5) == 2);
    check(op_div((uint64_t)(int64_t)-42, (uint64_t)(int64_t)-5) == 8);
    check(op_rem((uint64_t)(int64_t)-42, (uint64_t)(int64_t)-5) == (uint64_t)(int64_t)-2);
    check(op_divu(42, 5) == 8);
    check(op_remu(42, 5) == 2);
    check(op_divu(UINT64_MAX, 2) == UINT64_C(0x7fffffffffffffff));
    check(op_remu(UINT64_MAX, 2) == 1);

    check(op_mulw(UINT64_C(0xaaaaaaaa80000000), 2) == 0);
    check(op_mulw(UINT64_C(0x123456787fffffff), 2) == UINT64_C(0xfffffffffffffffe));
    check(op_divw((uint64_t)(int64_t)-10, 3) == (uint64_t)(int64_t)-3);
    check(op_remw((uint64_t)(int64_t)-10, 3) == (uint64_t)(int64_t)-1);
    check(op_divuw(UINT64_C(0xfeedbeef80000000), UINT64_C(0x1234567800000001)) == UINT64_C(0xffffffff80000000));
    check(op_divuw(UINT64_C(0x12345678ffffffff), 2) == UINT64_C(0x000000007fffffff));
    check(op_remuw(UINT64_C(0x12345678ffffffff), UINT64_C(0xabcdef010000000a)) == 5);
}

static void test_division_exceptions(void)
{
    const uint64_t full_lhs = UINT64_C(0x8000000012345678);
    const uint64_t word_lhs = UINT64_C(0xfeedbeef80001234);
    uint64_t runtime_zero[8] = {0, 0, 0, 0, 0, 0, 0, 0};

    /* These calls may select the architectural-x0 source specialisation. */
    check(op_div(full_lhs, 0) == UINT64_MAX);
    check(op_divu(full_lhs, 0) == UINT64_MAX);
    check(op_rem(full_lhs, 0) == full_lhs);
    check(op_remu(full_lhs, 0) == full_lhs);

    check(op_divw(word_lhs, 0) == UINT64_MAX);
    check(op_divuw(word_lhs, 0) == UINT64_MAX);
    check(op_remw(word_lhs, 0) == UINT64_C(0xffffffff80001234));
    check(op_remuw(word_lhs, 0) == UINT64_C(0xffffffff80001234));

    /*
     * The same cases, encoded with a non-x0 divisor register, prove that the
     * generated host DIV/IDIV guards handle a run-time zero without trapping.
     */
    rv64_m_runtime_zero_probe(word_lhs, 0, runtime_zero);
    check(runtime_zero[0] == UINT64_MAX);
    check(runtime_zero[1] == UINT64_MAX);
    check(runtime_zero[2] == word_lhs);
    check(runtime_zero[3] == word_lhs);
    check(runtime_zero[4] == UINT64_MAX);
    check(runtime_zero[5] == UINT64_MAX);
    check(runtime_zero[6] == UINT64_C(0xffffffff80001234));
    check(runtime_zero[7] == UINT64_C(0xffffffff80001234));

    check(op_div(UINT64_C(0x8000000000000000), UINT64_MAX) == UINT64_C(0x8000000000000000));
    check(op_rem(UINT64_C(0x8000000000000000), UINT64_MAX) == 0);

    /*
     * Upper halves are deliberate garbage. W overflow depends only on the low
     * 32-bit INT_MIN and -1 encodings, then sign-extends the result to XLEN.
     */
    check(op_divw(UINT64_C(0x1234567880000000), UINT64_C(0xabcdef01ffffffff)) == UINT64_C(0xffffffff80000000));
    check(op_remw(UINT64_C(0x1234567880000000), UINT64_C(0xabcdef01ffffffff)) == 0);
}

static void test_register_aliases(void)
{
    check(rv64_m_alias_rd_rs1_mulh(UINT64_C(0xffffffffffffff00), UINT64_C(0x1000000000000001)) == UINT64_C(0xffffffffffffffef));
    check(rv64_m_alias_rd_rs2_mulhsu(UINT64_MAX, 2) == UINT64_MAX);
    check(rv64_m_alias_equal_mulhu(UINT64_MAX) == UINT64_C(0xfffffffffffffffe));

    check(rv64_m_alias_rd_rs1_div((uint64_t)(int64_t)-42, 5) == (uint64_t)(int64_t)-8);
    check(rv64_m_alias_rd_rs2_rem((uint64_t)(int64_t)-42, 5) == (uint64_t)(int64_t)-2);
    check(rv64_m_alias_equal_divu(37) == 1);
    check(rv64_m_alias_rd_rs1_remu(42, 5) == 2);

    check(rv64_m_alias_rd_rs1_divw((uint64_t)(int64_t)-10, 3) == (uint64_t)(int64_t)-3);
    check(rv64_m_alias_rd_rs2_remw((uint64_t)(int64_t)-10, 3) == (uint64_t)(int64_t)-1);
    check(rv64_m_alias_equal_divuw(UINT64_C(0x1234567880000000)) == 1);
    check(rv64_m_alias_rd_rs1_remuw(UINT64_C(0x12345678ffffffff), UINT64_C(0xabcdef010000000a)) == 5);
    check(rv64_m_alias_rd_rs1_mulw(UINT64_C(0x1234567840000000), 2) == UINT64_C(0xffffffff80000000));
    check(rv64_m_alias_rd_rs2_mulw(2, UINT64_C(0x1234567840000000)) == UINT64_C(0xffffffff80000000));
}

static void test_dead_destination_and_pairs(void)
{
    uint64_t pair[4] = {0, 0, 0, 0};
    uint64_t unsigned_pair[6] = {0, 0, 0, 0, 0, 0};
    uint64_t source_zero[13] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    const uint64_t word_lhs = UINT64_C(0xfeedbeef80001234);

    check(rv64_m_x0_probe() == 0);
    rv64_m_source_x0_probe(word_lhs, source_zero);
    check(source_zero[0] == 0);
    check(source_zero[1] == 0);
    check(source_zero[2] == 0);
    check(source_zero[3] == 0);
    check(source_zero[4] == UINT64_MAX);
    check(source_zero[5] == UINT64_MAX);
    check(source_zero[6] == word_lhs);
    check(source_zero[7] == word_lhs);
    check(source_zero[8] == 0);
    check(source_zero[9] == UINT64_MAX);
    check(source_zero[10] == UINT64_MAX);
    check(source_zero[11] == UINT64_C(0xffffffff80001234));
    check(source_zero[12] == UINT64_C(0xffffffff80001234));

    rv64_m_pair_probe((uint64_t)(int64_t)-42, 5, pair);
    check(pair[0] == UINT64_MAX);
    check(pair[1] == UINT64_C(0xffffffffffffff2e));
    check(pair[2] == (uint64_t)(int64_t)-8);
    check(pair[3] == (uint64_t)(int64_t)-2);

    rv64_m_unsigned_pair_probe(2, UINT64_MAX, unsigned_pair);
    check(unsigned_pair[0] == 1);
    check(unsigned_pair[1] == UINT64_C(0xfffffffffffffffe));
    check(unsigned_pair[2] == 1);
    check(unsigned_pair[3] == UINT64_C(0xfffffffffffffffe));
    check(unsigned_pair[4] == 0);
    check(unsigned_pair[5] == 2);
}

static void test_stable_m_loop(void)
{
    check(rv64_m_stable_loop(3, 1, UINT64_MAX, 2, 99, 64) == (3 ^ 99));
}

static void run_all_m_checks(void)
{
    test_ordinary_results();
    test_division_exceptions();
    test_register_aliases();
    test_dead_destination_and_pairs();
    test_stable_m_loop();
}

#endif

int main(void)
{
#if defined(__riscv) && __riscv_xlen == 64
    /* Repeat identical guest PCs so the regression covers cold and warm JIT. */
    run_all_m_checks();
    run_all_m_checks();
#endif

    return 0;
}
