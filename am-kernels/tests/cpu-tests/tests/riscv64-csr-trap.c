#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 64

#include <stdint.h>

volatile uint64_t rv64_csr_saved_mstatus = 0;
volatile uint64_t rv64_csr_saved_mcause = 0;
volatile uint64_t rv64_csr_saved_mtval = 0;
volatile uint64_t rv64_csr_saved_a7 = 0;
volatile uint64_t rv64_csr_restore_mtvec = 0;

/*
 * The handler is written without a C prologue so the test controls exactly
 * which registers are touched.  It records trap state, skips the faulting
 * instruction, restores mtvec, and then forces MPP=M before MRET so tests that
 * deliberately enter U-mode can continue running the following C code.
 */
asm(
    ".section .text\n"
    ".option push\n"
    ".option norvc\n"
    ".align 2\n"
    ".globl rv64_csr_trap_handler\n"
    "rv64_csr_trap_handler:\n"
    "  csrr t1, mstatus\n"
    "  la t0, rv64_csr_saved_mstatus\n"
    "  sd t1, 0(t0)\n"
    "  csrr t1, mcause\n"
    "  la t0, rv64_csr_saved_mcause\n"
    "  sd t1, 0(t0)\n"
    "  csrr t1, mtval\n"
    "  la t0, rv64_csr_saved_mtval\n"
    "  sd t1, 0(t0)\n"
    "  la t0, rv64_csr_saved_a7\n"
    "  sd a7, 0(t0)\n"
    "  la t0, rv64_csr_restore_mtvec\n"
    "  ld t1, 0(t0)\n"
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

extern void rv64_csr_trap_handler(void);

enum
{
    MSTATUS_MIE = 1ull << 3,
    MSTATUS_MPIE = 1ull << 7,
    MSTATUS_MPP_MASK = 3ull << 11,
    MSTATUS_MPP_M = 3ull << 11,
    MSTATUS_MPRV = 1ull << 17,
    MSTATUS_SUM = 1ull << 18,
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

static void prepare_trap(void)
{
    rv64_csr_saved_mstatus = 0;
    rv64_csr_saved_mcause = UINT64_MAX;
    rv64_csr_saved_mtval = UINT64_MAX;
    rv64_csr_saved_a7 = UINT64_MAX;
    rv64_csr_restore_mtvec = read_mtvec();
    write_mtvec((uintptr_t)rv64_csr_trap_handler);
}

static void test_csr_read_write_rules(void)
{
    uintptr_t reg = 0x2468ace013579bdfull;
    uintptr_t old = 0;

    write_mscratch(0x13579bdf2468ace0ull);
    asm volatile("csrrw %0, mscratch, %0" : "+r"(reg) : : "memory");
    check(reg == 0x13579bdf2468ace0ull);
    check(read_mscratch() == 0x2468ace013579bdfull);

    write_mscratch(0x55u);
    asm volatile("csrrs %0, mscratch, x0" : "=r"(old) : : "memory");
    check(old == 0x55u);
    check(read_mscratch() == 0x55u);

    asm volatile("csrrwi %0, mscratch, 5" : "=r"(old) : : "memory");
    check(old == 0x55u);
    check(read_mscratch() == 5u);
}

static void test_m_mode_ecall_trap_entry(void)
{
    uintptr_t old_mstatus = read_mstatus();
    uintptr_t armed = old_mstatus;

    prepare_trap();

    armed |= MSTATUS_MIE;
    armed |= MSTATUS_SUM;
    armed &= ~MSTATUS_MPIE;
    write_mstatus(armed);

    asm volatile("li a7, 42; ecall" : : : "a7", "memory");

    write_mstatus(old_mstatus);

    check(rv64_csr_saved_mcause == 11u);
    check(rv64_csr_saved_a7 == 42u);
    check(rv64_csr_saved_mtval == 0u);
    check((rv64_csr_saved_mstatus & MSTATUS_MPP_MASK) == MSTATUS_MPP_M);
    check((rv64_csr_saved_mstatus & MSTATUS_MPIE) != 0);
    check((rv64_csr_saved_mstatus & MSTATUS_MIE) == 0);
    check((rv64_csr_saved_mstatus & MSTATUS_SUM) != 0);
}

static void test_mret_restores_machine_mstatus_fields(void)
{
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

static void test_mstatus_write_normalises_reserved_mpp(void)
{
    uintptr_t old_mstatus = read_mstatus();
    uintptr_t reserved_mstatus = old_mstatus;
    uintptr_t observed = 0;

    reserved_mstatus &= ~MSTATUS_MPP_MASK;
    reserved_mstatus |= 2ull << 11;

    write_mstatus(reserved_mstatus);
    observed = read_mstatus();
    write_mstatus(old_mstatus);

    check((observed & MSTATUS_MPP_MASK) != (2ull << 11));
}

static void test_user_mode_ecall_and_csr_faults(void)
{
    uintptr_t old_mstatus = read_mstatus();
    uintptr_t user_mstatus = old_mstatus;

    prepare_trap();
    user_mstatus &= ~MSTATUS_MPP_MASK;
    user_mstatus |= MSTATUS_MPIE;

    asm volatile(
        "csrw mstatus, %[status]\n"
        "la t0, 1f\n"
        "csrw mepc, t0\n"
        "mret\n"
        "1:\n"
        "li a7, 42\n"
        "ecall\n"
        :
        : [status] "r"(user_mstatus)
        : "t0", "a7", "memory");

    write_mstatus(old_mstatus);

    check(rv64_csr_saved_mcause == 8u);
    check(rv64_csr_saved_a7 == 42u);
    check(rv64_csr_saved_mtval == 0u);

    prepare_trap();
    user_mstatus = old_mstatus;
    user_mstatus &= ~MSTATUS_MPP_MASK;
    user_mstatus |= MSTATUS_MPIE;

    asm volatile(
        "csrw mstatus, %[status]\n"
        "la t0, 1f\n"
        "csrw mepc, t0\n"
        "mret\n"
        "1:\n"
        "csrr t1, mtvec\n"
        :
        : [status] "r"(user_mstatus)
        : "t0", "t1", "memory");

    write_mstatus(old_mstatus);

    check(rv64_csr_saved_mcause == 2u);
    check(rv64_csr_saved_mtval == 0u);
}

static void test_wfi_is_non_blocking_hint(void)
{
    asm volatile("wfi" : : : "memory");
}

#endif

int main(void)
{
#if defined(__riscv) && __riscv_xlen == 64
    test_csr_read_write_rules();
    test_m_mode_ecall_trap_entry();
    test_mret_restores_machine_mstatus_fields();
    test_mstatus_write_normalises_reserved_mpp();
    test_user_mode_ecall_and_csr_faults();
    test_wfi_is_non_blocking_hint();
#endif

    return 0;
}
