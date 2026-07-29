#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 32

#include <stdint.h>

volatile uint32_t rv32_d_basic_trap_count = 0;

enum
{
    MSTATUS_FS_SHIFT = 13,
};

#define MSTATUS_FS_MASK ((uintptr_t)3u << MSTATUS_FS_SHIFT)
#define MSTATUS_FS_INITIAL ((uintptr_t)1u << MSTATUS_FS_SHIFT)
#define MSTATUS_FS_DIRTY ((uintptr_t)3u << MSTATUS_FS_SHIFT)
#define MSTATUS_SD ((uintptr_t)1u << 31)

/*
 * The RED-phase configuration deliberately lacks D.  Skipping one complete
 * four-byte instruction lets the test reach its literal checks and fail
 * because D is absent, rather than becoming stuck in the trap handler.
 */
asm(
    ".section .text\n"
    ".align 2\n"
    ".option push\n"
    ".option norvc\n"
    ".globl rv32_d_basic_trap_handler\n"
    ".type rv32_d_basic_trap_handler, @function\n"
    "rv32_d_basic_trap_handler:\n"
    "  la t0, rv32_d_basic_trap_count\n"
    "  lw t1, 0(t0)\n"
    "  addi t1, t1, 1\n"
    "  sw t1, 0(t0)\n"
    "  csrr t0, mepc\n"
    "  addi t0, t0, 4\n"
    "  csrw mepc, t0\n"
    "  mret\n"
    ".size rv32_d_basic_trap_handler, "
    ".-rv32_d_basic_trap_handler\n"
    ".option pop\n");

extern void rv32_d_basic_trap_handler(void);

/*
 * RV32 has no base-D instruction that moves all 64 raw bits between one FPR
 * and one integer register.  These helpers therefore use pointers and the
 * architecturally defined FLD/FSD transfer instructions.  The translation
 * unit retains the normal integer-only ILP32 ABI.
 */
asm(
    ".section .text\n"
    ".align 2\n"
    ".option push\n"
    ".option norvc\n"
    ".option arch, +f\n"
    ".option arch, +d\n"

    ".globl rv32_d_add_bits\n"
    ".type rv32_d_add_bits, @function\n"
    "rv32_d_add_bits:\n"
    "  fld f0, 0(a0)\n"
    "  fld f1, 0(a1)\n"
    "  fadd.d f0, f0, f1, rne\n"
    "  fsd f0, 0(a2)\n"
    "  ret\n"
    ".size rv32_d_add_bits, .-rv32_d_add_bits\n"

    ".globl rv32_d_roundtrip_bits\n"
    ".type rv32_d_roundtrip_bits, @function\n"
    "rv32_d_roundtrip_bits:\n"
    "  fld f0, 0(a0)\n"
    "  fsd f0, 0(a1)\n"
    "  ret\n"
    ".size rv32_d_roundtrip_bits, .-rv32_d_roundtrip_bits\n"

    ".globl rv32_d_box_single\n"
    ".type rv32_d_box_single, @function\n"
    "rv32_d_box_single:\n"
    "  flw f0, 0(a0)\n"
    "  fsd f0, 0(a1)\n"
    "  ret\n"
    ".size rv32_d_box_single, .-rv32_d_box_single\n"

    ".globl rv32_d_malformed_box_add_single\n"
    ".type rv32_d_malformed_box_add_single, @function\n"
    "rv32_d_malformed_box_add_single:\n"
    "  fld f0, 0(a0)\n"
    "  fld f1, 0(a1)\n"
    "  fadd.s f0, f0, f1, rne\n"
    "  fsd f0, 0(a2)\n"
    "  ret\n"
    ".size rv32_d_malformed_box_add_single, "
    ".-rv32_d_malformed_box_add_single\n"

    ".option pop\n");

extern void rv32_d_add_bits(const uint64_t *lhs, const uint64_t *rhs,
                            uint64_t *result);
extern void rv32_d_roundtrip_bits(const uint64_t *source, uint64_t *result);
extern void rv32_d_box_single(const uint32_t *source, uint64_t *result);
extern void rv32_d_malformed_box_add_single(const uint64_t *lhs,
                                             const uint64_t *rhs,
                                             uint64_t *result);

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

static uintptr_t read_misa(void)
{
    uintptr_t value;
    asm volatile("csrr %0, misa" : "=r"(value));
    return value;
}

static void test_rv32d_state_and_basic_execution(void)
{
    const uintptr_t old_mstatus = read_mstatus();
    const uintptr_t old_mtvec = read_mtvec();
    const uint64_t lhs = UINT64_C(0x3ff8000000000000); /* 1.5 */
    const uint64_t rhs = UINT64_C(0x4002000000000000); /* 2.25 */
    const uint64_t signalling_nan = UINT64_C(0x7ff0000000000123);
    const uint32_t single_signalling_nan = UINT32_C(0x7f800123);
    const uint64_t malformed_one = UINT64_C(0x000000003f800000);
    const uint64_t boxed_one = UINT64_C(0xffffffff3f800000);
    uint64_t sum = 0;
    uint64_t transferred = 0;
    uint64_t boxed_single = 0;
    uint64_t malformed_result = 0;
    uintptr_t misa;
    uintptr_t dirty_mstatus;

    rv32_d_basic_trap_count = 0;
    write_mtvec((uintptr_t)rv32_d_basic_trap_handler);
    write_mstatus((old_mstatus & ~MSTATUS_FS_MASK) | MSTATUS_FS_INITIAL);

    misa = read_misa();
    rv32_d_add_bits(&lhs, &rhs, &sum);
    dirty_mstatus = read_mstatus();
    rv32_d_roundtrip_bits(&signalling_nan, &transferred);
    rv32_d_box_single(&single_signalling_nan, &boxed_single);
    rv32_d_malformed_box_add_single(&malformed_one, &boxed_one,
                                     &malformed_result);

    write_mstatus(old_mstatus);
    write_mtvec(old_mtvec);

    check(rv32_d_basic_trap_count == 0);
    check((misa >> 30) == 1);
    check((misa & ((uintptr_t)1u << ('F' - 'A'))) != 0);
    check((misa & ((uintptr_t)1u << ('D' - 'A'))) != 0);
    check((dirty_mstatus & MSTATUS_FS_MASK) == MSTATUS_FS_DIRTY);
    check((dirty_mstatus & MSTATUS_SD) != 0);

    /* 1.5 + 2.25 is exactly 3.75 in IEEE-754 binary64. */
    check(sum == UINT64_C(0x400e000000000000));

    /* FLD/FSD are raw transfers and retain a signalling-NaN payload. */
    check(transferred == signalling_nan);

    /* A binary32 transfer into FLEN=64 must set every upper bit to one. */
    check(boxed_single == UINT64_C(0xffffffff7f800123));

    /*
     * A computational single-precision consumer treats a malformed box as
     * canonical binary32 NaN, then boxes the binary32 result again.
     */
    check(malformed_result == UINT64_C(0xffffffff7fc00000));
}

#endif

int main(void)
{
#if defined(__riscv) && __riscv_xlen == 32
    test_rv32d_state_and_basic_execution();
#endif

    return 0;
}
