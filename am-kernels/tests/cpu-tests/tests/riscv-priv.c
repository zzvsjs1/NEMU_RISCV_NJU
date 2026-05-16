#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 32

#include <stdint.h>

volatile uint32_t riscv_priv_trap_seen = 0;
volatile uint32_t riscv_priv_saved_mstatus = 0;
volatile uint32_t riscv_priv_saved_mcause = 0;
volatile uint32_t riscv_priv_saved_mtval = 0;
volatile uint32_t riscv_priv_saved_a7 = 0;
volatile uint32_t riscv_priv_restore_mtvec = 0;

/*
 * The vector table is written in inline assembly so the test controls exact
 * instruction placement and avoids compiler-generated prologue code. mtvec
 * vectored mode still sends synchronous exceptions to BASE, so slot zero must
 * be the only handler reached by the ecall test.
 */
asm(
    ".section .text\n"
    ".option push\n"
    ".option norvc\n"
    ".align 6\n"
    ".globl riscv_priv_vector_table\n"
    "riscv_priv_vector_table:\n"
    "  j riscv_priv_sync_handler\n"
    "  j riscv_priv_vector_wrong\n"
    "  j riscv_priv_vector_wrong\n"
    "  j riscv_priv_vector_wrong\n"
    "  j riscv_priv_vector_wrong\n"
    "  j riscv_priv_vector_wrong\n"
    "  j riscv_priv_vector_wrong\n"
    "  j riscv_priv_vector_wrong\n"
    "  j riscv_priv_vector_wrong\n"
    "  j riscv_priv_vector_wrong\n"
    "  j riscv_priv_vector_wrong\n"
    "  j riscv_priv_vector_wrong\n"
    "  j riscv_priv_vector_wrong\n"
    "  j riscv_priv_vector_wrong\n"
    "  j riscv_priv_vector_wrong\n"
    "  j riscv_priv_vector_wrong\n"
    "riscv_priv_sync_handler:\n"
    "  csrr t0, mepc\n"
    "  addi t0, t0, 4\n"
    "  csrw mepc, t0\n"
    "  la t0, riscv_priv_trap_seen\n"
    "  li t1, 1\n"
    "  sw t1, 0(t0)\n"
    "  mret\n"
    "riscv_priv_vector_wrong:\n"
    "  li a0, 1\n"
    "  .word 0x0000006b\n"
    "  j riscv_priv_vector_wrong\n"
    ".globl riscv_priv_mstatus_handler\n"
    "riscv_priv_mstatus_handler:\n"
    "  csrr t1, mstatus\n"
    "  la t0, riscv_priv_saved_mstatus\n"
    "  sw t1, 0(t0)\n"
    "  la t0, riscv_priv_restore_mtvec\n"
    "  lw t1, 0(t0)\n"
    "  csrw mtvec, t1\n"
    "  csrr t0, mepc\n"
    "  addi t0, t0, 4\n"
    "  csrw mepc, t0\n"
    "  mret\n"
    ".globl riscv_priv_cause_handler\n"
    "riscv_priv_cause_handler:\n"
    "  csrr t1, mcause\n"
    "  la t0, riscv_priv_saved_mcause\n"
    "  sw t1, 0(t0)\n"
    "  csrr t1, mtval\n"
    "  la t0, riscv_priv_saved_mtval\n"
    "  sw t1, 0(t0)\n"
    "  la t0, riscv_priv_saved_a7\n"
    "  sw a7, 0(t0)\n"
    "  la t0, riscv_priv_restore_mtvec\n"
    "  lw t1, 0(t0)\n"
    "  csrw mtvec, t1\n"
    "  csrr t0, mepc\n"
    "  addi t0, t0, 4\n"
    "  csrw mepc, t0\n"
    "  csrr t0, mstatus\n"
    "  li t1, 0x1800\n"
    "  or t0, t0, t1\n"
    "  csrw mstatus, t0\n"
    "  mret\n"
    ".option pop\n");

extern void riscv_priv_vector_table(void);
extern void riscv_priv_mstatus_handler(void);
extern void riscv_priv_cause_handler(void);

static inline uintptr_t read_mstatus(void)
{
    uintptr_t v;
    asm volatile("csrr %0, mstatus" : "=r"(v));
    return v;
}

static inline uintptr_t read_mscratch(void)
{
    uintptr_t v;
    asm volatile("csrr %0, mscratch" : "=r"(v));
    return v;
}

static inline uintptr_t read_mtvec(void)
{
    uintptr_t v;
    asm volatile("csrr %0, mtvec" : "=r"(v));
    return v;
}

static inline void write_mtvec(uintptr_t v)
{
    asm volatile("csrw mtvec, %0" : : "r"(v) : "memory");
}

static void prepare_cause_test(void)
{
    uintptr_t old_mtvec = read_mtvec();

    riscv_priv_saved_mcause = 0xffffffffu;
    riscv_priv_saved_mtval = 0xffffffffu;
    riscv_priv_saved_a7 = 0xffffffffu;
    riscv_priv_restore_mtvec = old_mtvec;
    write_mtvec((uintptr_t)riscv_priv_cause_handler);
}

static void test_csr_immediate(void)
{
    /*
   * CSR immediate instructions use a five-bit unsigned immediate encoded in the
   * instruction itself. mscratch is safe scratch state for this test because the
   * cpu-test harness does not rely on AM's trap nesting convention here.
   */
    uintptr_t old;

    asm volatile("csrw mscratch, %0" : : "r"(0x13579u) : "memory");

    asm volatile("csrrwi %0, mscratch, 5" : "=r"(old) : : "memory");
    check(old == 0x13579u);
    check(read_mscratch() == 5u);

    asm volatile("csrrsi %0, mscratch, 18" : "=r"(old) : : "memory");
    check(old == 5u);
    check(read_mscratch() == (5u | 18u));

    asm volatile("csrrci %0, mscratch, 16" : "=r"(old) : : "memory");
    check(old == (5u | 18u));
    check(read_mscratch() == ((5u | 18u) & ~16u));
}

static void test_vectored_mtvec_keeps_sync_exception_at_base(void)
{
    /*
   * RISC-V vectored mtvec applies its per-cause offset only to interrupts.
   * Synchronous exceptions such as ecall must jump to BASE, which is exactly
   * what this test records through riscv_priv_trap_seen.
   */
    uintptr_t old_mtvec = read_mtvec();

    riscv_priv_trap_seen = 0;
    write_mtvec(((uintptr_t)riscv_priv_vector_table) | 1u);

    asm volatile("li a7, 11; ecall" : : : "a7", "memory");

    write_mtvec(old_mtvec);
    check(riscv_priv_trap_seen == 1);
}

static void test_ecall_uses_architectural_mcause(void)
{
    /*
     * The syscall ABI keeps the service number in a7.  The privileged ISA keeps
     * mcause for the trap class only, so an M-mode ecall must report cause 11
     * even when a7 contains an unrelated syscall number.
     */
    prepare_cause_test();
    asm volatile("li a7, 42; ecall" : : : "a7", "memory");
    check(riscv_priv_saved_mcause == 11u);
    check(riscv_priv_saved_a7 == 42u);
    check(riscv_priv_saved_mtval == 0u);
}

static void test_user_ecall_uses_user_cause(void)
{
    /*
     * Enter U-mode with mret, execute ecall, and let the trap handler return to
     * the instruction after that ecall.  The cause must be 8 regardless of a7.
     */
    uintptr_t old_mstatus = read_mstatus();
    uintptr_t mstatus = old_mstatus;

    prepare_cause_test();
    mstatus &= ~(3u << 11); // MPP = U
    mstatus |= (1u << 7);   // MPIE = 1

    asm volatile(
        "csrw mstatus, %[mstatus]\n"
        "la t0, 1f\n"
        "csrw mepc, t0\n"
        "mret\n"
        "1:\n"
        "li a7, 42\n"
        "ecall\n"
        :
        : [mstatus] "r"(mstatus)
        : "t0", "a7", "memory");

    check(riscv_priv_saved_mcause == 8u);
    check(riscv_priv_saved_a7 == 42u);
    check(riscv_priv_saved_mtval == 0u);
}

static void test_user_csr_access_is_illegal_instruction(void)
{
    /*
     * CSR address bits [9:8] encode the minimum privilege level.  mtvec is an
     * M-mode CSR, so U-mode reads must trap as illegal instructions rather than
     * exposing machine state.
     */
    uintptr_t old_mstatus = read_mstatus();
    uintptr_t mstatus = old_mstatus;

    prepare_cause_test();
    mstatus &= ~(3u << 11); // MPP = U
    mstatus |= (1u << 7);   // MPIE = 1

    asm volatile(
        "csrw mstatus, %[mstatus]\n"
        "la t0, 1f\n"
        "csrw mepc, t0\n"
        "mret\n"
        "1:\n"
        "csrr t1, mtvec\n"
        :
        : [mstatus] "r"(mstatus)
        : "t0", "t1", "memory");

    check(riscv_priv_saved_mcause == 2u);
    check(riscv_priv_saved_mtval == 0u);
}

static void test_breakpoint_and_illegal_instruction_causes(void)
{
    /*
     * EBREAK has its own architectural cause.  A reserved all-zero instruction is
     * illegal in the base ISA and should trap instead of aborting the emulator.
     * An unimplemented CSR access is also an illegal instruction.
     */
    prepare_cause_test();
    asm volatile("ebreak" : : : "memory");
    check(riscv_priv_saved_mcause == 3u);

    prepare_cause_test();
    asm volatile(".word 0x00000000" : : : "memory");
    check(riscv_priv_saved_mcause == 2u);

    prepare_cause_test();
    asm volatile(".word 0x99902073" : : : "memory"); // csrr x0, 0x999
    check(riscv_priv_saved_mcause == 2u);
}

static void test_misaligned_load_store_causes(void)
{
    /*
     * This EEI chooses visible traps for naturally misaligned scalar accesses.
     * mtval should carry the bad virtual address so software can diagnose or
     * emulate the access.
     */
    static volatile uint32_t value = 0x11223344u;
    uintptr_t bad = ((uintptr_t)&value) + 1u;

    prepare_cause_test();
    asm volatile("lw t0, 0(%0)" : : "r"(bad) : "t0", "memory");
    check(riscv_priv_saved_mcause == 4u);
    check(riscv_priv_saved_mtval == bad);

    prepare_cause_test();
    asm volatile("sw t0, 0(%0)" : : "r"(bad) : "t0", "memory");
    check(riscv_priv_saved_mcause == 6u);
    check(riscv_priv_saved_mtval == bad);
    check(value == 0x11223344u);
}

static void test_misaligned_control_transfer_causes(void)
{
    /*
     * Taken control transfers report instruction-address-misaligned on the
     * control-transfer instruction itself.  The target address goes in mtval and
     * normal side effects, such as link-register writes, must not happen.
     */
    uintptr_t bad = 0;
    uintptr_t link_after = 0;

    prepare_cause_test();
    asm volatile(
        "la t0, 1f\n"
        "addi %[bad], t0, 2\n"
        "li t2, 0x13579\n"
        "1:\n"
        ".word 0x002003ef\n" // jal t2, 2
        "2:\n"
        "mv %[link_after], t2\n"
        : [bad] "=&r"(bad), [link_after] "=&r"(link_after)
        :
        : "t0", "t2", "memory");
    check(riscv_priv_saved_mcause == 0u);
    check(riscv_priv_saved_mtval == bad);
    check(link_after == 0x13579u);

    prepare_cause_test();
    asm volatile(
        "la t0, 1f\n"
        "addi %[bad], t0, 2\n"
        "li t2, 0x2468a\n"
        "jalr t2, 2(t0)\n"
        "1:\n"
        "mv %[link_after], t2\n"
        : [bad] "=&r"(bad), [link_after] "=&r"(link_after)
        :
        : "t0", "t2", "memory");
    check(riscv_priv_saved_mcause == 0u);
    check(riscv_priv_saved_mtval == bad);
    check(link_after == 0x2468au);

    prepare_cause_test();
    asm volatile(
        "la %[bad], 1f\n"
        "addi %[bad], %[bad], 2\n"
        "1:\n"
        ".word 0x00000163\n" // beq zero, zero, 2
        "2:\n"
        : [bad] "=&r"(bad)
        :
        : "memory");
    check(riscv_priv_saved_mcause == 0u);
    check(riscv_priv_saved_mtval == bad);

    prepare_cause_test();
    asm volatile(".word 0x00001163" : : : "memory"); // bne zero, zero, 2
    write_mtvec(riscv_priv_restore_mtvec);
    check(riscv_priv_saved_mcause == 0xffffffffu);
}

static void test_mret_clears_mprv_when_returning_to_u_mode(void)
{
    /*
   * The privileged spec requires mret to clear MPRV when returning below
   * machine mode. NEMU must implement that side effect or later memory accesses
   * can run with stale effective privilege.
   */
    uintptr_t old_mstatus = read_mstatus();
    uintptr_t old_mtvec = read_mtvec();
    uintptr_t mstatus = old_mstatus;

    mstatus |= (1u << 17);  // MPRV = 1
    mstatus &= ~(3u << 11); // MPP = U
    mstatus |= (1u << 7);   // MPIE = 1
    riscv_priv_saved_mstatus = 0;
    riscv_priv_restore_mtvec = old_mtvec;
    write_mtvec((uintptr_t)riscv_priv_mstatus_handler);

    asm volatile(
        "csrw mstatus, %[mstatus]\n"
        "la t0, 1f\n"
        "csrw mepc, t0\n"
        "mret\n"
        "1:\n"
        "li a7, 11\n"
        "ecall\n"
        :
        : [mstatus] "r"(mstatus)
        : "t0", "a7", "memory");

    check((riscv_priv_saved_mstatus & (1u << 17)) == 0);
}

#endif

int main(void)
{
#if defined(__riscv) && __riscv_xlen == 32
    test_csr_immediate();
    test_ecall_uses_architectural_mcause();
    test_user_ecall_uses_user_cause();
    test_user_csr_access_is_illegal_instruction();
    test_breakpoint_and_illegal_instruction_causes();
    test_misaligned_load_store_causes();
    test_misaligned_control_transfer_causes();
    test_vectored_mtvec_keeps_sync_exception_at_base();
    test_mret_clears_mprv_when_returning_to_u_mode();
#endif

    return 0;
}
