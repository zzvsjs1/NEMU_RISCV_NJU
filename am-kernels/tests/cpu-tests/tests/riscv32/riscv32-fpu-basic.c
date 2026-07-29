#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 32

#include <stdint.h>

volatile uint32_t rv32_fpu_basic_trap_count = 0;

#define MSTATUS_FS_MASK ((uintptr_t)3u << 13)
#define MSTATUS_FS_INITIAL ((uintptr_t)1u << 13)

/*
 * The handler deliberately uses integer state only.  If NEMU advertises no
 * usable F implementation, each four-byte FP instruction is skipped so that
 * the test can restore mtvec and report a normal check failure.
 */
asm(
    ".section .text\n"
    ".align 2\n"
    ".option push\n"
    ".option norvc\n"
    ".globl rv32_fpu_basic_trap_handler\n"
    ".type rv32_fpu_basic_trap_handler, @function\n"
    "rv32_fpu_basic_trap_handler:\n"
    "  la t0, rv32_fpu_basic_trap_count\n"
    "  lw t1, 0(t0)\n"
    "  addi t1, t1, 1\n"
    "  sw t1, 0(t0)\n"
    "  csrr t0, mepc\n"
    "  addi t0, t0, 4\n"
    "  csrw mepc, t0\n"
    "  mret\n"
    ".size rv32_fpu_basic_trap_handler, .-rv32_fpu_basic_trap_handler\n"
    ".option pop\n");

extern void rv32_fpu_basic_trap_handler(void);

/*
 * Keep the translation unit on the integer-only ILP32 ABI.  Raw IEEE-754 bits
 * enter and leave through integer registers, while the local option enables
 * only the single-precision F instructions under test.
 */
asm(
    ".section .text\n"
    ".align 2\n"
    ".option push\n"
    ".option norvc\n"
    ".option arch, +f\n"

    ".globl rv32_fpu_add_s_bits\n"
    ".type rv32_fpu_add_s_bits, @function\n"
    "rv32_fpu_add_s_bits:\n"
    "  fmv.w.x f0, a0\n"
    "  fmv.w.x f1, a1\n"
    "  fadd.s f0, f0, f1, rne\n"
    "  fmv.x.w a0, f0\n"
    "  ret\n"
    ".size rv32_fpu_add_s_bits, .-rv32_fpu_add_s_bits\n"

    ".globl rv32_fpu_roundtrip_w\n"
    ".type rv32_fpu_roundtrip_w, @function\n"
    "rv32_fpu_roundtrip_w:\n"
    "  flw f0, 0(a0)\n"
    "  fsw f0, 0(a1)\n"
    "  fmv.x.w a0, f0\n"
    "  ret\n"
    ".size rv32_fpu_roundtrip_w, .-rv32_fpu_roundtrip_w\n"

    ".option pop\n");

extern uint32_t rv32_fpu_add_s_bits(uint32_t lhs, uint32_t rhs);
extern uint32_t rv32_fpu_roundtrip_w(const uint32_t *src, uint32_t *dst);

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

static void test_basic_single_precision(void)
{
    const uintptr_t old_mstatus = read_mstatus();
    const uintptr_t old_mtvec = read_mtvec();
    const uint32_t signalling_nan = UINT32_C(0x7f800123);
    uint32_t stored = 0;
    uint32_t sum;
    uint32_t moved;

    rv32_fpu_basic_trap_count = 0;
    write_mtvec((uintptr_t)rv32_fpu_basic_trap_handler);
    write_mstatus((old_mstatus & ~MSTATUS_FS_MASK) | MSTATUS_FS_INITIAL);

    /* 1.5 + 2.25 = 3.75 exactly in binary32. */
    sum = rv32_fpu_add_s_bits(UINT32_C(0x3fc00000),
                             UINT32_C(0x40100000));

    /*
     * FLW, FSW, and FMV.X.W are raw transfers.  They must preserve a
     * signalling-NaN payload rather than replacing it with a canonical NaN.
     */
    moved = rv32_fpu_roundtrip_w(&signalling_nan, &stored);

    write_mstatus(old_mstatus);
    write_mtvec(old_mtvec);

    check(rv32_fpu_basic_trap_count == 0);
    check(sum == UINT32_C(0x40700000));
    check(moved == signalling_nan);
    check(stored == signalling_nan);
}

#endif

int main(void)
{
#if defined(__riscv) && __riscv_xlen == 32
    test_basic_single_precision();
#endif

    return 0;
}
