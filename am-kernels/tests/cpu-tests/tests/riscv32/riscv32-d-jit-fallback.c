#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 32

#include <stdint.h>

volatile uint32_t rv32_d_jit_trap_count = 0;

#define MSTATUS_FS_MASK ((uintptr_t)3u << 13)
#define MSTATUS_FS_INITIAL ((uintptr_t)1u << 13)

/*
 * Before RV32D is enabled, each D instruction is expected to be rejected.
 * Skipping the rejected instruction lets the RED test reach its literal
 * result and trap-count checks without using floating-point state in the
 * handler itself.
 */
asm(
    ".section .text\n"
    ".align 2\n"
    ".option push\n"
    ".option norvc\n"
    ".globl rv32_d_jit_trap_handler\n"
    ".type rv32_d_jit_trap_handler, @function\n"
    "rv32_d_jit_trap_handler:\n"
    "  la t0, rv32_d_jit_trap_count\n"
    "  lw t1, 0(t0)\n"
    "  addi t1, t1, 1\n"
    "  sw t1, 0(t0)\n"
    "  csrr t0, mepc\n"
    "  addi t0, t0, 4\n"
    "  csrw mepc, t0\n"
    "  mret\n"
    ".size rv32_d_jit_trap_handler, .-rv32_d_jit_trap_handler\n"
    ".option pop\n");

extern void rv32_d_jit_trap_handler(void);

/*
 * Binary64 data crosses the ILP32 boundary only through aligned memory.
 *
 * The first helper begins with FLD, so a translated block must side-exit
 * before executing any guest instruction.  Its counter update follows all D
 * operations and therefore must occur exactly once after interpreter
 * fallback.
 *
 * The second helper updates its counter before FLD.  A side exit that resumes
 * at the beginning of the block would repeat that visible integer update,
 * while one that skips the unsupported instruction would leave the result
 * sentinel unchanged.  Together the two helpers pin both JIT boundary cases.
 */
asm(
    ".section .text\n"
    ".align 2\n"
    ".option push\n"
    ".option norvc\n"
    ".option arch, +f\n"
    ".option arch, +d\n"

    ".globl rv32_d_jit_at_block_entry\n"
    ".type rv32_d_jit_at_block_entry, @function\n"
    "rv32_d_jit_at_block_entry:\n"
    "  fld f0, 0(a0)\n"
    "  fadd.d f0, f0, f0, rne\n"
    "  fsd f0, 0(a1)\n"
    "  lw t0, 0(a2)\n"
    "  addi t0, t0, 1\n"
    "  sw t0, 0(a2)\n"
    "  ret\n"
    ".size rv32_d_jit_at_block_entry, .-rv32_d_jit_at_block_entry\n"

    ".globl rv32_d_jit_after_integer_prefix\n"
    ".type rv32_d_jit_after_integer_prefix, @function\n"
    "rv32_d_jit_after_integer_prefix:\n"
    "  lw t0, 0(a2)\n"
    "  addi t0, t0, 1\n"
    "  sw t0, 0(a2)\n"
    "  fld f0, 0(a0)\n"
    "  fadd.d f0, f0, f0, rne\n"
    "  fsd f0, 0(a1)\n"
    "  ret\n"
    ".size rv32_d_jit_after_integer_prefix, "
    ".-rv32_d_jit_after_integer_prefix\n"

    ".option pop\n");

extern void rv32_d_jit_at_block_entry(const uint64_t *, uint64_t *,
                                      uint32_t *);
extern void rv32_d_jit_after_integer_prefix(const uint64_t *, uint64_t *,
                                            uint32_t *);

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

static void test_d_jit_fallback_boundaries(void)
{
    uint64_t input __attribute__((aligned(8))) =
        UINT64_C(0x3ff8000000000000); /* +1.5 */
    uint64_t entry_result __attribute__((aligned(8))) =
        UINT64_C(0xfeedfacecafebeef);
    uint64_t prefix_result __attribute__((aligned(8))) =
        UINT64_C(0xfeedfacecafebeef);
    uint32_t entry_counter = 0;
    uint32_t prefix_counter = 0;

    rv32_d_jit_at_block_entry(&input, &entry_result, &entry_counter);
    rv32_d_jit_after_integer_prefix(&input, &prefix_result,
                                    &prefix_counter);

    /* 1.5 + 1.5 = 3.0 exactly in binary64. */
    check(entry_result == UINT64_C(0x4008000000000000));
    check(prefix_result == UINT64_C(0x4008000000000000));
    check(entry_counter == 1);
    check(prefix_counter == 1);
    check(rv32_d_jit_trap_count == 0);
}

#endif

int main(void)
{
#if defined(__riscv) && __riscv_xlen == 32
    const uintptr_t old_mstatus = read_mstatus();
    const uintptr_t old_mtvec = read_mtvec();

    rv32_d_jit_trap_count = 0;
    write_mtvec((uintptr_t)rv32_d_jit_trap_handler);
    write_mstatus((old_mstatus & ~MSTATUS_FS_MASK) | MSTATUS_FS_INITIAL);

    test_d_jit_fallback_boundaries();

    write_mstatus(old_mstatus);
    write_mtvec(old_mtvec);
#endif

    return 0;
}
