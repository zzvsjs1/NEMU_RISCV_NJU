#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 32

#include <stdint.h>

volatile uint32_t csr_trap_saved_mstatus = 0;
volatile uint32_t csr_trap_saved_mcause = 0;
volatile uint32_t csr_trap_restore_mtvec = 0;

asm(
    ".section .text\n"
    ".option push\n"
    ".option norvc\n"
    ".align 2\n"
    ".globl csr_trap_mstatus_handler\n"
    "csr_trap_mstatus_handler:\n"
    "  csrr t1, mstatus\n"
    "  la t0, csr_trap_saved_mstatus\n"
    "  sw t1, 0(t0)\n"
    "  csrr t1, mcause\n"
    "  la t0, csr_trap_saved_mcause\n"
    "  sw t1, 0(t0)\n"
    "  la t0, csr_trap_restore_mtvec\n"
    "  lw t1, 0(t0)\n"
    "  csrw mtvec, t1\n"
    "  csrr t0, mepc\n"
    "  addi t0, t0, 4\n"
    "  csrw mepc, t0\n"
    "  mret\n"
    ".option pop\n");

extern void csr_trap_mstatus_handler(void);

enum
{
    MSTATUS_MIE = 1u << 3,
    MSTATUS_MPIE = 1u << 7,
    MSTATUS_MPP_MASK = 3u << 11,
    MSTATUS_MPP_M = 3u << 11,
    MSTATUS_MPRV = 1u << 17,
};

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

static inline void write_mstatus(uintptr_t v)
{
    asm volatile("csrw mstatus, %0" : : "r"(v) : "memory");
}

static inline void write_mscratch(uintptr_t v)
{
    asm volatile("csrw mscratch, %0" : : "r"(v) : "memory");
}

static inline void write_mtvec(uintptr_t v)
{
    asm volatile("csrw mtvec, %0" : : "r"(v) : "memory");
}

static void test_csr_read_write_source_ordering(void)
{
    /*
     * CSRRW reads the old CSR value into rd and writes the original rs1 value
     * into the CSR. When rd and rs1 are the same register, the implementation
     * must not overwrite rs1 before taking the CSR write operand.
     */
    uintptr_t reg = 0x2468ace0u;

    write_mscratch(0x13579bdfu);
    asm volatile("csrrw %0, mscratch, %0" : "+r"(reg) : : "memory");

    check(reg == 0x13579bdfu);
    check(read_mscratch() == 0x2468ace0u);
}

static void test_csr_zero_register_rules(void)
{
    /*
     * CSRRS/CSRRC with rs1=x0 are pure reads: they must not set or clear any CSR
     * bits. CSRRW with rd=x0 suppresses the architectural CSR read, but it still
     * writes the rs1 value to the CSR.
     */
    uintptr_t old = 0;

    write_mscratch(0x55u);
    asm volatile("csrrs %0, mscratch, x0" : "=r"(old) : : "memory");
    check(old == 0x55u);
    check(read_mscratch() == 0x55u);

    asm volatile("csrrc %0, mscratch, x0" : "=r"(old) : : "memory");
    check(old == 0x55u);
    check(read_mscratch() == 0x55u);

    old = 0xa5a5u;
    asm volatile("csrrw x0, mscratch, %0" : : "r"(old) : "memory");
    check(read_mscratch() == 0xa5a5u);
}

static void test_trap_entry_mstatus_stack_fields(void)
{
    /*
     * Trap entry into M-mode must snapshot the interrupted privilege in MPP,
     * copy MIE into MPIE, and clear MIE before the handler runs. The handler
     * stores its entry mstatus before it adjusts mepc and returns.
     */
    uintptr_t old_mstatus = read_mstatus();
    uintptr_t old_mtvec = read_mtvec();
    uintptr_t armed = old_mstatus;

    csr_trap_saved_mstatus = 0;
    csr_trap_saved_mcause = 0xffffffffu;
    csr_trap_restore_mtvec = old_mtvec;

    armed |= MSTATUS_MIE;
    armed &= ~MSTATUS_MPIE;
    write_mstatus(armed);
    write_mtvec((uintptr_t)csr_trap_mstatus_handler);

    asm volatile("li a7, 11; ecall" : : : "a7", "memory");

    write_mstatus(old_mstatus);

    check(csr_trap_saved_mcause == 11u);
    check((csr_trap_saved_mstatus & MSTATUS_MPP_MASK) == MSTATUS_MPP_M);
    check((csr_trap_saved_mstatus & MSTATUS_MPIE) != 0);
    check((csr_trap_saved_mstatus & MSTATUS_MIE) == 0);
}

static void test_mret_restores_machine_mstatus_fields(void)
{
    /*
     * MRET first chooses the new privilege from MPP, then updates the interrupt
     * stack fields: MIE <- MPIE, MPIE <- 1, and MPP <- U. Returning to M-mode
     * must not clear MPRV, because MPRV is cleared only when the new privilege
     * is less privileged than M.
     */
    uintptr_t old_mstatus = read_mstatus();
    uintptr_t before = old_mstatus;
    uintptr_t after = 0;

    before &= ~(MSTATUS_MIE | MSTATUS_MPIE | MSTATUS_MPP_MASK);
    before |= MSTATUS_MPIE | MSTATUS_MPP_M | MSTATUS_MPRV;

    asm volatile(
        "csrw mstatus, %[before]\n"
        "la t0, 1f\n"
        "csrw mepc, t0\n"
        "mret\n"
        "1:\n"
        "csrr %[after], mstatus\n"
        : [after] "=&r"(after)
        : [before] "r"(before)
        : "t0", "memory");

    write_mstatus(old_mstatus);

    check((after & MSTATUS_MIE) != 0);
    check((after & MSTATUS_MPIE) != 0);
    check((after & MSTATUS_MPP_MASK) == 0);
    check((after & MSTATUS_MPRV) != 0);
}

#endif

int main(void)
{
#if defined(__riscv) && __riscv_xlen == 32
    test_csr_read_write_source_ordering();
    test_csr_zero_register_rules();
    test_trap_entry_mstatus_stack_fields();
    test_mret_restores_machine_mstatus_fields();
#endif

    return 0;
}
