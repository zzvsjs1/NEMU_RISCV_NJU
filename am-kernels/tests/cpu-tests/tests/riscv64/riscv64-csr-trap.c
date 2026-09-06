#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 64

#include <stdint.h>

volatile uint64_t rv64_csr_saved_mstatus = 0;
volatile uint64_t rv64_csr_saved_mcause = 0;
volatile uint64_t rv64_csr_saved_mtval = 0;
volatile uint64_t rv64_csr_saved_a7 = 0;
volatile uint64_t rv64_csr_restore_mtvec = 0;
volatile uint64_t rv64_csr_resume_pc = 0;

/*
 * The handler is written without a C prologue so the test controls exactly
 * which registers are touched.  It records trap state, skips the faulting
 * instruction (or uses an explicitly requested resume PC), restores mtvec,
 * and then forces MPP=M before MRET so tests that deliberately enter lower
 * privilege can continue running the following C code.
 */
asm(".section .text\n"
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
    "  la t0, rv64_csr_resume_pc\n"
    "  ld t0, 0(t0)\n"
    "  bnez t0, 1f\n"
    "  csrr t0, mepc\n"
    "  addi t0, t0, 4\n"
    "1:\n"
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
    MSTATUS_MPP_S = 1ull << 11,
    MSTATUS_MPP_M = 3ull << 11,
    MSTATUS_MPRV = 1ull << 17,
    MSTATUS_SUM = 1ull << 18,
    MSTATUS_TVM = 1ull << 20,
};

/*
 * RV64 mstatus stores the XLEN selected for U-mode and S-mode in two adjacent
 * WARL fields.  Each field is two bits wide, and encoding 2 means 64-bit
 * execution.  NEMU implements no 32-bit lower-privilege mode, so every possible
 * incoming encoding must be canonicalised to 2 rather than allowing reserved
 * encoding 3 to remain visible.
 */
#define MSTATUS_XLEN_FIELD_WIDTH 2u
#define MSTATUS_XLEN_VALUE_MASK ((1ull << MSTATUS_XLEN_FIELD_WIDTH) - 1ull)
#define MSTATUS_UXL_SHIFT 32u
#define MSTATUS_SXL_SHIFT 34u
#define MSTATUS_UXL_MASK (MSTATUS_XLEN_VALUE_MASK << MSTATUS_UXL_SHIFT)
#define MSTATUS_SXL_MASK (MSTATUS_XLEN_VALUE_MASK << MSTATUS_SXL_SHIFT)
#define MSTATUS_XLEN_64 2ull
#define MSTATUS_UBE (1ull << 6)
#define MSTATUS_SBE (1ull << 36)
#define MSTATUS_MBE (1ull << 37)
#define MSTATUS_ENDIAN_MASK (MSTATUS_UBE | MSTATUS_SBE | MSTATUS_MBE)
#define SATP_MODE_SHIFT 60u
#define SATP_MODE_MASK 15u
#define SATP_MODE_SV39 8u
#define SATP_ASID_SHIFT 44u
#define SATP_SV39 ((uintptr_t)SATP_MODE_SV39 << SATP_MODE_SHIFT)

enum
{
    SATP_PROBE_READ,
    SATP_PROBE_SWAP,
    SATP_PROBE_SET,
    SATP_PROBE_CLEAR,
    SATP_PROBE_SWAP_IMMEDIATE,
    SATP_PROBE_SET_IMMEDIATE,
    SATP_PROBE_CLEAR_IMMEDIATE,
    SATP_PROBE_COUNT,
    SATP_PROBE_DESTINATION_SENTINEL = 0x5a5,
    CAUSE_ILLEGAL_INSTRUCTION = 2,
    CAUSE_SUPERVISOR_ECALL = 9,
};

/*
 * Each probe slot is a four-byte CSR instruction followed by a four-byte
 * ECALL.  TVM should trap on the CSR; with TVM clear, ECALL returns control to
 * M-mode after the successful access.  The handler's explicit resume PC skips
 * both instructions, making the successful and trapping paths independently
 * observable without relying on an accidental second trap.
 *
 * Arguments follow the guest ABI: a0=mstatus, a1=slot, a2=CSR source value.
 * a7 is deliberately distinct from the source and remains a sentinel on traps.
 */
asm(".section .text\n"
    ".option push\n"
    ".option norvc\n"
    ".align 2\n"
    ".globl rv64_csr_probe_supervisor_satp\n"
    "rv64_csr_probe_supervisor_satp:\n"
    "  la t0, rv64_csr_satp_resume\n"
    "  la t1, rv64_csr_resume_pc\n"
    "  sd t0, 0(t1)\n"
    "  la t0, rv64_csr_satp_probes\n"
    "  slli a1, a1, 3\n"
    "  add t0, t0, a1\n"
    "  csrw mepc, t0\n"
    "  csrw mstatus, a0\n"
    "  li a7, 0x5a5\n"
    "  mret\n"
    "rv64_csr_satp_probes:\n"
    "  csrr a7, satp\n"
    "  ecall\n"
    "  csrrw a7, satp, a2\n"
    "  ecall\n"
    "  csrrs a7, satp, a2\n"
    "  ecall\n"
    "  csrrc a7, satp, a2\n"
    "  ecall\n"
    "  csrrwi a7, satp, 0\n"
    "  ecall\n"
    "  csrrsi a7, satp, 0\n"
    "  ecall\n"
    "  csrrci a7, satp, 0\n"
    "  ecall\n"
    "rv64_csr_satp_resume:\n"
    "  ret\n"
    ".option pop\n");

extern void rv64_csr_probe_supervisor_satp(uintptr_t status, uintptr_t operation, uintptr_t source);

static inline uintptr_t read_satp(void)
{
    uintptr_t value;
    asm volatile("csrr %0, satp" : "=r"(value));
    return value;
}

static inline void write_satp(uintptr_t value)
{
    asm volatile("csrw satp, %0" : : "r"(value) : "memory");
}

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
    rv64_csr_resume_pc = 0;
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

    asm volatile("li a7, 42; ecall" : : : "t0", "t1", "a7", "memory");

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

    asm volatile("csrw mstatus, %[before]\n"
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

static void test_mstatus_write_normalises_uxl_and_sxl(void)
{
    uintptr_t old_mstatus = read_mstatus();
    const uintptr_t xlen_fields_mask = MSTATUS_UXL_MASK | MSTATUS_SXL_MASK;

    /*
     * Exercise all four encodings in each field independently.  Keeping the
     * other field at the supported value makes a failure identify which input
     * was not normalised, rather than allowing one malformed field to obscure
     * the other.
     */
    for (uintptr_t incoming = 0; incoming <= MSTATUS_XLEN_VALUE_MASK; incoming++)
    {
        uintptr_t requested = (old_mstatus & ~xlen_fields_mask) | (incoming << MSTATUS_UXL_SHIFT) | (MSTATUS_XLEN_64 << MSTATUS_SXL_SHIFT);

        write_mstatus(requested);
        uintptr_t observed = read_mstatus();

        check(((observed & MSTATUS_UXL_MASK) >> MSTATUS_UXL_SHIFT) == MSTATUS_XLEN_64);
        check(((observed & MSTATUS_SXL_MASK) >> MSTATUS_SXL_SHIFT) == MSTATUS_XLEN_64);
    }

    for (uintptr_t incoming = 0; incoming <= MSTATUS_XLEN_VALUE_MASK; incoming++)
    {
        uintptr_t requested = (old_mstatus & ~xlen_fields_mask) | (MSTATUS_XLEN_64 << MSTATUS_UXL_SHIFT) | (incoming << MSTATUS_SXL_SHIFT);

        write_mstatus(requested);
        uintptr_t observed = read_mstatus();

        check(((observed & MSTATUS_UXL_MASK) >> MSTATUS_UXL_SHIFT) == MSTATUS_XLEN_64);
        check(((observed & MSTATUS_SXL_MASK) >> MSTATUS_SXL_SHIFT) == MSTATUS_XLEN_64);
    }

    write_mstatus(old_mstatus);
}

static void test_satp_ignores_unsupported_modes(void)
{
    const uintptr_t saved_satp = read_satp();
    const uintptr_t supported = SATP_SV39 | ((uintptr_t)0x1357 << SATP_ASID_SHIFT) | 0x80000u;

    /*
     * M-mode fetches stay physical while Sv39 is selected.  Give the retained
     * value and each attempted write different ASIDs and PPNs, so changing
     * either field despite rejecting MODE cannot pass this whole-CSR check.
     */
    write_satp(supported);
    check(read_satp() == supported);

    for (uintptr_t mode = 1; mode <= SATP_MODE_MASK; mode++)
    {
        if (mode == SATP_MODE_SV39)
        {
            continue;
        }

        const uintptr_t attempted = (mode << SATP_MODE_SHIFT) | ((uintptr_t)0x2468 << SATP_ASID_SHIFT) | 0x80001u;
        uintptr_t old = 0;

        asm volatile("csrrw %0, satp, %1" : "=r"(old) : "r"(attempted) : "memory");

        check(old == supported);
        check(read_satp() == supported);
    }

    write_satp(0);
    check(read_satp() == 0);
    write_satp(saved_satp);
}

static void test_mstatus_endianness_is_read_only_zero(void)
{
    const uintptr_t saved_mstatus = read_mstatus();
    const uintptr_t requests[] = {MSTATUS_UBE, MSTATUS_SBE, MSTATUS_MBE, MSTATUS_ENDIAN_MASK};
    const uint8_t bytes[4] __attribute__((aligned(4))) = {1, 2, 3, 4};

    for (size_t request = 0; request < sizeof(requests) / sizeof(requests[0]); request++)
    {
        write_mstatus(saved_mstatus | requests[request]);
        check((read_mstatus() & MSTATUS_ENDIAN_MASK) == 0);

        /*
         * The readback rule must agree with real memory execution.  Repeating
         * the load also exercises cached native loads in JIT configurations.
         */
        for (unsigned iteration = 0; iteration < 16; iteration++)
        {
            uintptr_t value;
            asm volatile("lw %0, 0(%1)" : "=r"(value) : "r"(bytes) : "memory");
            check(value == 0x04030201u);
        }
    }

    write_mstatus(saved_mstatus);
}

static void test_supervisor_satp_tvm_gate(void)
{
    const uintptr_t saved_mstatus = read_mstatus();
    const uintptr_t saved_satp = read_satp();
    const uintptr_t supervisor_status =
        (saved_mstatus & ~(MSTATUS_MPP_MASK | MSTATUS_MPRV | MSTATUS_MIE | MSTATUS_MPIE | MSTATUS_TVM)) | MSTATUS_MPP_S;

    /* Bare mode keeps the probe code and trap handler reachable in both modes. */
    write_satp(0);

    for (unsigned tvm = 0; tvm <= 1; tvm++)
    {
        for (uintptr_t operation = 0; operation < SATP_PROBE_COUNT; operation++)
        {
            prepare_trap();
            rv64_csr_probe_supervisor_satp(supervisor_status | (tvm ? MSTATUS_TVM : 0), operation, tvm ? SATP_SV39 : 0);
            write_mstatus(saved_mstatus);

            check(rv64_csr_saved_mcause == (tvm ? CAUSE_ILLEGAL_INSTRUCTION : CAUSE_SUPERVISOR_ECALL));
            check(rv64_csr_saved_a7 == (tvm ? SATP_PROBE_DESTINATION_SENTINEL : 0));
            check((rv64_csr_saved_mstatus & MSTATUS_MPP_MASK) == MSTATUS_MPP_S);
            check(read_satp() == 0);
        }
    }

    write_satp(saved_satp);
}

static void test_user_mode_ecall_and_csr_faults(void)
{
    uintptr_t old_mstatus = read_mstatus();
    uintptr_t user_mstatus = old_mstatus;

    prepare_trap();
    user_mstatus &= ~MSTATUS_MPP_MASK;
    user_mstatus |= MSTATUS_MPIE;

    asm volatile("csrw mstatus, %[status]\n"
                 "la t0, 1f\n"
                 "csrw mepc, t0\n"
                 "mret\n"
                 "1:\n"
                 "li a7, 42\n"
                 "ecall\n"
                 :
                 : [status] "r"(user_mstatus)
                 : "t0", "t1", "a7", "memory");

    write_mstatus(old_mstatus);

    check(rv64_csr_saved_mcause == 8u);
    check(rv64_csr_saved_a7 == 42u);
    check(rv64_csr_saved_mtval == 0u);

    prepare_trap();
    user_mstatus = old_mstatus;
    user_mstatus &= ~MSTATUS_MPP_MASK;
    user_mstatus |= MSTATUS_MPIE;

    asm volatile("csrw mstatus, %[status]\n"
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
    test_mstatus_write_normalises_uxl_and_sxl();
    test_satp_ignores_unsupported_modes();
    test_mstatus_endianness_is_read_only_zero();
    test_supervisor_satp_tvm_gate();
    test_user_mode_ecall_and_csr_faults();
    test_wfi_is_non_blocking_hint();
#endif

    return 0;
}
