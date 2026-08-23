#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 64

#include <stdint.h>

volatile uint64_t rv64_jit_rollback_trap_count = 0;
volatile uint64_t rv64_jit_rollback_mcause = UINT64_MAX;
volatile uint64_t rv64_jit_rollback_mepc = UINT64_MAX;
volatile uint64_t rv64_jit_rollback_mtval = UINT64_MAX;

/*
 * The handler deliberately touches only t0 and t1.  The probe's dirty a0-a2
 * values must survive the illegal-instruction trap so its resumed suffix can
 * distinguish a complete emitter rollback from leaked partial native state.
 */
asm(
    ".section .text\n"
    ".align 2\n"
    ".option push\n"
    ".option norvc\n"
    ".globl rv64_jit_rollback_trap_handler\n"
    ".type rv64_jit_rollback_trap_handler, @function\n"
    "rv64_jit_rollback_trap_handler:\n"
    "  la t0, rv64_jit_rollback_trap_count\n"
    "  ld t1, 0(t0)\n"
    "  addi t1, t1, 1\n"
    "  sd t1, 0(t0)\n"
    "  csrr t1, mcause\n"
    "  la t0, rv64_jit_rollback_mcause\n"
    "  sd t1, 0(t0)\n"
    "  csrr t1, mepc\n"
    "  la t0, rv64_jit_rollback_mepc\n"
    "  sd t1, 0(t0)\n"
    "  csrr t1, mtval\n"
    "  la t0, rv64_jit_rollback_mtval\n"
    "  sd t1, 0(t0)\n"
    "  csrr t1, mepc\n"
    "  addi t1, t1, 4\n"
    "  csrw mepc, t1\n"
    "  mret\n"
    ".size rv64_jit_rollback_trap_handler, "
    ".-rv64_jit_rollback_trap_handler\n"
    ".option pop\n");

extern void rv64_jit_rollback_trap_handler(void);
extern const uint8_t rv64_jit_rollback_illegal_insn[];

/*
 * The invalid OP encoding has funct7=0x02 and opcode 0x33.  Its ADD-like
 * operands make the emitter materialise dirty a0 and a1 before it recognises
 * the unsupported sub-case.  The compiler must discard those partial bytes
 * and metadata, follow NEMU's characterised illegal-instruction fallback, then
 * resume at the suffix with a2 unchanged.
 */
asm(
    ".section .text\n"
    ".align 2\n"
    ".option push\n"
    ".option norvc\n"
    ".globl rv64_jit_rollback_probe\n"
    ".type rv64_jit_rollback_probe, @function\n"
    "rv64_jit_rollback_probe:\n"
    "  addi a0, a0, 1\n"
    "  addi a1, a1, 2\n"
    "  addi a2, a2, 4\n"
    ".globl rv64_jit_rollback_illegal_insn\n"
    "rv64_jit_rollback_illegal_insn:\n"
    "  .word 0x04b50633\n" /* Reserved OP funct7=0x02: a2, a0, a1. */
    ".globl rv64_jit_rollback_suffix\n"
    "rv64_jit_rollback_suffix:\n"
    "  slli a0, a0, 4\n"
    "  xor a0, a0, a1\n"
    "  add a0, a0, a2\n"
    "  addi a0, a0, 3\n"
    "  ret\n"
    ".size rv64_jit_rollback_probe, .-rv64_jit_rollback_probe\n"
    ".option pop\n");

extern uint64_t rv64_jit_rollback_probe(uint64_t first, uint64_t second,
                                        uint64_t third);

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

static void test_partial_emitter_rollback(void)
{
    const uintptr_t old_mtvec = read_mtvec();

    rv64_jit_rollback_trap_count = 0;
    rv64_jit_rollback_mcause = UINT64_MAX;
    rv64_jit_rollback_mepc = UINT64_MAX;
    rv64_jit_rollback_mtval = UINT64_MAX;
    write_mtvec((uintptr_t)rv64_jit_rollback_trap_handler);

    const uint64_t checksum = rv64_jit_rollback_probe(5, 7, 11);

    write_mtvec(old_mtvec);

    check(rv64_jit_rollback_trap_count == 1);
    /*
     * The ISA leaves reserved-instruction behaviour unspecified.  Cause 2 is
     * NEMU's policy for this encoding; once that trap is selected, mepc and
     * mtval below follow the privileged architecture's illegal-instruction
     * reporting rules.
     */
    check(rv64_jit_rollback_mcause == 2);
    check(rv64_jit_rollback_mepc ==
          (uintptr_t)rv64_jit_rollback_illegal_insn);
    /*
     * The privileged specification permits an illegal-instruction trap to
     * report either zero or the faulting instruction bits in mtval.  Accept
     * precisely those two architectural outcomes, not an unrelated value.
     */
    check(rv64_jit_rollback_mtval == 0 ||
          rv64_jit_rollback_mtval == UINT64_C(0x04b50633));
    check(checksum == UINT64_C(123));
}

#endif

int main(void)
{
#if defined(__riscv) && __riscv_xlen == 64
    test_partial_emitter_rollback();
#endif

    return 0;
}
