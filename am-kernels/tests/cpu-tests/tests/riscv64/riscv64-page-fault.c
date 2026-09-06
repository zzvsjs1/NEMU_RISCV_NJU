#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 64

#include <stdint.h>

/*
 * Privileged §§3.1.15, 3.1.16 and 12.4 require a failed Sv39 access to enter
 * the guest handler with the faulting instruction PC and virtual address.
 * These probes run in S-mode so translation applies to both data and fetch.
 * Their M-mode handler can inspect and repair the physical page tables safely.
 */
#define PAGE_BYTES 4096u
#define PAGE_TABLE_ENTRIES 512u
#define PAGE_SHIFT 12u
#define PTE_PPN_SHIFT 10u
#define PTE_V UINT64_C(0x001)
#define PTE_R UINT64_C(0x002)
#define PTE_W UINT64_C(0x004)
#define PTE_X UINT64_C(0x008)
#define PTE_A UINT64_C(0x040)
#define PTE_D UINT64_C(0x080)
#define SATP_SV39 (UINT64_C(8) << 60)
#define IDENTITY_BASE UINT64_C(0x80000000)
#define ALIAS_BASE UINT64_C(0x40000000)
#define NONCANONICAL_ADDRESS (UINT64_C(1) << 39)
#define MSTATUS_MIE (UINT64_C(1) << 3)
#define MSTATUS_MPIE (UINT64_C(1) << 7)
#define MSTATUS_MPP_MASK (UINT64_C(3) << 11)
#define MSTATUS_MPP_S (UINT64_C(1) << 11)
#define MSTATUS_FS_MASK (UINT64_C(3) << 13)
#define MSTATUS_FS_CLEAN (UINT64_C(2) << 13)
#define DATA_SENTINEL UINT64_C(0x1020304050607080)
#define REGISTER_SENTINEL UINT64_C(0x8877665544332211)
#define FPR_SENTINEL UINT64_C(0x400921fb54442d18)

enum
{
    CAUSE_INSTRUCTION_PAGE_FAULT = 12,
    CAUSE_LOAD_PAGE_FAULT = 13,
    CAUSE_STORE_PAGE_FAULT = 15,
    PREFIX_RESULT = 36,
    YOUNGER_BEFORE = 17,
    YOUNGER_AFTER = 18,
    INSN_ADDI_A1_A1_ONE = 0x00158593,
    INSN_ADDI_A3_A3_ONE = 0x00168693,
    INSN_ECALL = 0x00000073,
    PRESSURE_REGISTERS = 6,
};

static uint64_t root_pt[PAGE_TABLE_ENTRIES] __attribute__((aligned(PAGE_BYTES)));
static uint64_t alias_l1[PAGE_TABLE_ENTRIES] __attribute__((aligned(PAGE_BYTES)));
static uint64_t alias_l0[PAGE_TABLE_ENTRIES] __attribute__((aligned(PAGE_BYTES)));
static uint64_t data_page[PAGE_BYTES / sizeof(uint64_t)] __attribute__((aligned(PAGE_BYTES)));
static uint32_t code_page[PAGE_BYTES / sizeof(uint32_t)] __attribute__((aligned(PAGE_BYTES)));

volatile uint64_t rv64_pf_count;
volatile uint64_t rv64_pf_mcause;
volatile uint64_t rv64_pf_mepc;
volatile uint64_t rv64_pf_mtval;
volatile uint64_t rv64_pf_mstatus;
volatile uint64_t rv64_pf_address;
volatile uint64_t rv64_pf_destination;
volatile uint64_t rv64_pf_prefix;
volatile uint64_t rv64_pf_younger;
volatile uint64_t rv64_pf_final_younger;
volatile uint64_t rv64_pf_pressure[PRESSURE_REGISTERS];
volatile uint64_t rv64_pf_final_pressure;
volatile uintptr_t rv64_pf_resume;
volatile uintptr_t rv64_pf_repair_slot;
volatile uint64_t rv64_pf_repair_pte;

/*
 * Only t0/t1 are scratch in the handler. The captured a-registers therefore
 * expose the native prefix and the untouched destination before recovery.
 * ECALL from S-mode is the probe's completion marker, not another page fault.
 * The assembler names below describe architectural fields used by the entry
 * and exit sequences instead of concealing them in composite immediates.
 */
asm(".section .text\n"
    ".option push\n"
    ".option norvc\n"
    ".equ PF_MSTATUS_MPP_MASK, 3 << 11\n"
    ".equ PF_MSTATUS_MPP_S, 1 << 11\n"
    ".equ PF_MSTATUS_MPP_M, 3 << 11\n"
    ".equ PF_MSTATUS_MPRV, 1 << 17\n"
    ".equ PF_MSTATUS_MIE, 1 << 3\n"
    ".equ PF_MSTATUS_MPIE, 1 << 7\n"
    ".equ PF_CAUSE_S_ECALL, 9\n"
    ".balign 4\n"
    ".globl rv64_pf_handler\n"
    "rv64_pf_handler:\n"
    "  csrr t0, mcause\n"
    "  li t1, PF_CAUSE_S_ECALL\n"
    "  beq t0, t1, rv64_pf_complete\n"
    "  la t1, rv64_pf_mcause\n"
    "  sd t0, 0(t1)\n"
    "  csrr t0, mepc\n"
    "  la t1, rv64_pf_mepc\n"
    "  sd t0, 0(t1)\n"
    "  csrr t0, mtval\n"
    "  la t1, rv64_pf_mtval\n"
    "  sd t0, 0(t1)\n"
    "  csrr t0, mstatus\n"
    "  la t1, rv64_pf_mstatus\n"
    "  sd t0, 0(t1)\n"
    "  la t0, rv64_pf_address\n"
    "  sd a0, 0(t0)\n"
    "  la t0, rv64_pf_destination\n"
    "  sd a1, 0(t0)\n"
    "  la t0, rv64_pf_prefix\n"
    "  sd a4, 0(t0)\n"
    "  la t0, rv64_pf_younger\n"
    "  sd a3, 0(t0)\n"
    /* Six adjacent XLEN words retain t2/t3/t4/t5/t6/a2 in that order. */
    "  la t0, rv64_pf_pressure\n"
    "  sd t2, 0(t0)\n"
    "  sd t3, 8(t0)\n"
    "  sd t4, 16(t0)\n"
    "  sd t5, 24(t0)\n"
    "  sd t6, 32(t0)\n"
    "  sd a2, 40(t0)\n"
    "  la t0, rv64_pf_count\n"
    "  ld t1, 0(t0)\n"
    "  addi t1, t1, 1\n"
    "  sd t1, 0(t0)\n"
    "  la t0, rv64_pf_repair_slot\n"
    "  ld t0, 0(t0)\n"
    "  beqz t0, rv64_pf_skip\n"
    "  la t1, rv64_pf_repair_pte\n"
    "  ld t1, 0(t1)\n"
    "  sd t1, 0(t0)\n"
    "  sfence.vma zero, zero\n"
    "  mret\n"
    "rv64_pf_skip:\n"
    "  la t0, rv64_pf_resume\n"
    "  ld t0, 0(t0)\n"
    "  csrw mepc, t0\n"
    "  mret\n"
    "rv64_pf_complete:\n"
    "  csrr t0, mstatus\n"
    "  li t1, PF_MSTATUS_MPP_M\n"
    "  or t0, t0, t1\n"
    "  csrw mstatus, t0\n"
    "  la t0, rv64_pf_return\n"
    "  csrw mepc, t0\n"
    "  mret\n"

    ".globl rv64_pf_run\n"
    "rv64_pf_run:\n"
    "  csrw mepc, a0\n"
    "  mv a0, a1\n"
    "  mv a1, a2\n"
    "  li a3, 17\n"
    "  li a4, 36\n"
    "  csrr t0, mstatus\n"
    "  li t1, PF_MSTATUS_MPP_MASK | PF_MSTATUS_MPRV | PF_MSTATUS_MIE | PF_MSTATUS_MPIE\n"
    "  not t1, t1\n"
    "  and t0, t0, t1\n"
    "  li t1, PF_MSTATUS_MPP_S\n"
    "  or t0, t0, t1\n"
    "  csrw mstatus, t0\n"
    "  mret\n"
    "rv64_pf_return:\n"
    "  la t0, rv64_pf_final_younger\n"
    "  sd a3, 0(t0)\n"
    "  la t0, rv64_pf_final_pressure\n"
    "  sd a2, 0(t0)\n"
    "  mv a0, a1\n"
    "  ret\n"

    /* Each prefix must reach the handler before the younger ADDI executes. */
    ".globl rv64_pf_load\n"
    "rv64_pf_load:\n"
    "  li a4, 33\n"
    "  addi a4, a4, 3\n"
    ".globl rv64_pf_load_insn\n"
    "rv64_pf_load_insn:\n"
    "  ld a1, 0(a0)\n"
    "  addi a3, a3, 1\n"
    "  ecall\n"

    /* Byte accesses have a translation-fault exit without an alignment guard. */
    ".globl rv64_pf_byte_load\n"
    "rv64_pf_byte_load:\n"
    "  li a4, 33\n"
    "  addi a4, a4, 3\n"
    ".globl rv64_pf_byte_load_insn\n"
    "rv64_pf_byte_load_insn:\n"
    "  lb a1, 0(a0)\n"
    "  addi a3, a3, 1\n"
    "  ecall\n"

    ".globl rv64_pf_alias_load\n"
    "rv64_pf_alias_load:\n"
    "  li a4, 33\n"
    "  addi a4, a4, 3\n"
    ".globl rv64_pf_alias_load_insn\n"
    "rv64_pf_alias_load_insn:\n"
    "  ld a0, 0(a0)\n"
    "  addi a3, a3, 1\n"
    "  ecall\n"

    /* Unprivileged §2.6 requires this load to fault even though rd is x0. */
    ".globl rv64_pf_zero_load\n"
    "rv64_pf_zero_load:\n"
    "  li a4, 33\n"
    "  addi a4, a4, 3\n"
    ".globl rv64_pf_zero_load_insn\n"
    "rv64_pf_zero_load_insn:\n"
    "  ld zero, 0(a0)\n"
    "  addi a3, a3, 1\n"
    "  ecall\n"

    ".globl rv64_pf_store\n"
    "rv64_pf_store:\n"
    "  li a4, 33\n"
    "  addi a4, a4, 3\n"
    ".globl rv64_pf_store_insn\n"
    "rv64_pf_store_insn:\n"
    "  sd a1, 0(a0)\n"
    "  addi a3, a3, 1\n"
    "  ecall\n"

    /* Byte accesses have a translation-fault exit without an alignment guard. */
    ".globl rv64_pf_byte_store\n"
    "rv64_pf_byte_store:\n"
    "  li a4, 33\n"
    "  addi a4, a4, 3\n"
    ".globl rv64_pf_byte_store_insn\n"
    "rv64_pf_byte_store_insn:\n"
    "  sb a1, 0(a0)\n"
    "  addi a3, a3, 1\n"
    "  ecall\n"

    /*
     * Fill the six ordinary callee-saved cache slots with dirty live values.
     * Materialising the uncached address and then a1's store source can evict
     * them. The fault snapshot must describe the mappings after that eviction,
     * while preserving each evicted guest value already written to CPU_state.
     */
    ".globl rv64_pf_pressure_store\n"
    "rv64_pf_pressure_store:\n"
    "  li t2, 0x112\n"
    "  li t3, 0x223\n"
    "  li t4, 0x334\n"
    "  li t5, 0x445\n"
    "  li t6, 0x556\n"
    "  li a2, 0x667\n"
    ".globl rv64_pf_pressure_store_insn\n"
    "rv64_pf_pressure_store_insn:\n"
    "  sd a1, 0(a0)\n"
    "  addi a3, a3, 1\n"
    "  add a2, a2, t2\n"
    "  add a2, a2, t3\n"
    "  add a2, a2, t4\n"
    "  add a2, a2, t5\n"
    "  add a2, a2, t6\n"
    "  ecall\n"

    ".option arch, +f\n"
    ".option arch, +d\n"
    ".globl rv64_pf_fp_load\n"
    "rv64_pf_fp_load:\n"
    "  li a4, 33\n"
    "  addi a4, a4, 3\n"
    ".globl rv64_pf_fp_load_insn\n"
    "rv64_pf_fp_load_insn:\n"
    "  fld f0, 0(a0)\n"
    "  addi a3, a3, 1\n"
    "  ecall\n"

    ".globl rv64_pf_fp_store\n"
    "rv64_pf_fp_store:\n"
    "  li a4, 33\n"
    "  addi a4, a4, 3\n"
    ".globl rv64_pf_fp_store_insn\n"
    "rv64_pf_fp_store_insn:\n"
    "  fsd f0, 0(a0)\n"
    "  addi a3, a3, 1\n"
    "  ecall\n"

    ".globl rv64_pf_fp_word_load\n"
    "rv64_pf_fp_word_load:\n"
    "  li a4, 33\n"
    "  addi a4, a4, 3\n"
    ".globl rv64_pf_fp_word_load_insn\n"
    "rv64_pf_fp_word_load_insn:\n"
    "  flw f0, 0(a0)\n"
    "  addi a3, a3, 1\n"
    "  ecall\n"

    ".globl rv64_pf_fp_word_store\n"
    "rv64_pf_fp_word_store:\n"
    "  li a4, 33\n"
    "  addi a4, a4, 3\n"
    ".globl rv64_pf_fp_word_store_insn\n"
    "rv64_pf_fp_word_store_insn:\n"
    "  fsw f0, 0(a0)\n"
    "  addi a3, a3, 1\n"
    "  ecall\n"

    ".globl rv64_pf_ifetch_resume\n"
    "rv64_pf_ifetch_resume:\n"
    "  addi a3, a3, 1\n"
    "  ecall\n"
    ".option pop\n");

extern void rv64_pf_handler(void);
extern uint64_t rv64_pf_run(uintptr_t entry, uintptr_t address, uint64_t initial);
extern void rv64_pf_load(void), rv64_pf_load_insn(void);
extern void rv64_pf_byte_load(void), rv64_pf_byte_load_insn(void);
extern void rv64_pf_alias_load(void), rv64_pf_alias_load_insn(void);
extern void rv64_pf_zero_load(void), rv64_pf_zero_load_insn(void);
extern void rv64_pf_store(void), rv64_pf_store_insn(void);
extern void rv64_pf_byte_store(void), rv64_pf_byte_store_insn(void);
extern void rv64_pf_pressure_store(void), rv64_pf_pressure_store_insn(void);
extern void rv64_pf_fp_load(void), rv64_pf_fp_load_insn(void);
extern void rv64_pf_fp_store(void), rv64_pf_fp_store_insn(void);
extern void rv64_pf_fp_word_load(void), rv64_pf_fp_word_load_insn(void);
extern void rv64_pf_fp_word_store(void), rv64_pf_fp_word_store_insn(void);
extern void rv64_pf_ifetch_resume(void);

static uint64_t pte_for_page(const void *page, uint64_t flags)
{
    return (((uintptr_t)page >> PAGE_SHIFT) << PTE_PPN_SHIFT) | flags;
}

static void sfence_all(void)
{
    asm volatile("sfence.vma zero, zero" : : : "memory");
}

static void prepare_case(uintptr_t resume, uint64_t initial_pte, uint64_t repair_pte)
{
    rv64_pf_count = 0;
    rv64_pf_mcause = UINT64_MAX;
    rv64_pf_mepc = UINT64_MAX;
    rv64_pf_mtval = UINT64_MAX;
    rv64_pf_resume = resume;
    rv64_pf_repair_slot = repair_pte != 0 ? (uintptr_t)&alias_l0[0] : 0;
    rv64_pf_repair_pte = repair_pte;
    alias_l0[0] = initial_pte;
    data_page[0] = DATA_SENTINEL;
    sfence_all();
}

static void check_fault(uint64_t cause, uintptr_t instruction, uintptr_t address)
{
    check(rv64_pf_count == 1);
    check(rv64_pf_mcause == cause);
    check(rv64_pf_mepc == instruction);
    check(rv64_pf_mtval == address);
    check(rv64_pf_address == address);
    check(rv64_pf_destination == REGISTER_SENTINEL);
    check(rv64_pf_prefix == PREFIX_RESULT);
    check(rv64_pf_younger == YOUNGER_BEFORE);
    check(rv64_pf_final_younger == YOUNGER_AFTER);
    check((rv64_pf_mstatus & MSTATUS_MPP_MASK) == MSTATUS_MPP_S);
    check((rv64_pf_mstatus & MSTATUS_MIE) == 0);
    check((rv64_pf_mstatus & MSTATUS_MPIE) == 0);
}

static void test_load_faults(void)
{
    const uintptr_t instruction = (uintptr_t)rv64_pf_load_insn;

    prepare_case(instruction + sizeof(uint32_t), 0, 0);
    check(rv64_pf_run((uintptr_t)rv64_pf_load, ALIAS_BASE, REGISTER_SENTINEL) == REGISTER_SENTINEL);
    check_fault(CAUSE_LOAD_PAGE_FAULT, instruction, ALIAS_BASE);

    /* A noncanonical address must not wrap down to an otherwise valid PTE. */
    prepare_case(instruction + sizeof(uint32_t), pte_for_page(data_page, PTE_V | PTE_R | PTE_A), 0);
    check(rv64_pf_run((uintptr_t)rv64_pf_load, NONCANONICAL_ADDRESS, REGISTER_SENTINEL) == REGISTER_SENTINEL);
    check_fault(CAUSE_LOAD_PAGE_FAULT, instruction, NONCANONICAL_ADDRESS);

    prepare_case((uintptr_t)rv64_pf_alias_load_insn + sizeof(uint32_t), 0, 0);
    check(rv64_pf_run((uintptr_t)rv64_pf_alias_load, ALIAS_BASE, REGISTER_SENTINEL) == REGISTER_SENTINEL);
    check_fault(CAUSE_LOAD_PAGE_FAULT, (uintptr_t)rv64_pf_alias_load_insn, ALIAS_BASE);

    prepare_case((uintptr_t)rv64_pf_zero_load_insn + sizeof(uint32_t), 0, 0);
    check(rv64_pf_run((uintptr_t)rv64_pf_zero_load, ALIAS_BASE, REGISTER_SENTINEL) == REGISTER_SENTINEL);
    check_fault(CAUSE_LOAD_PAGE_FAULT, (uintptr_t)rv64_pf_zero_load_insn, ALIAS_BASE);

    prepare_case((uintptr_t)rv64_pf_byte_load_insn + sizeof(uint32_t), 0, 0);
    check(rv64_pf_run((uintptr_t)rv64_pf_byte_load, ALIAS_BASE, REGISTER_SENTINEL) == REGISTER_SENTINEL);
    check_fault(CAUSE_LOAD_PAGE_FAULT, (uintptr_t)rv64_pf_byte_load_insn, ALIAS_BASE);

    /* Leave MEPC unchanged after installing the page: the LD must run again. */
    prepare_case(instruction + sizeof(uint32_t), 0, pte_for_page(data_page, PTE_V | PTE_R | PTE_A));
    check(rv64_pf_run((uintptr_t)rv64_pf_load, ALIAS_BASE, REGISTER_SENTINEL) == DATA_SENTINEL);
    check_fault(CAUSE_LOAD_PAGE_FAULT, instruction, ALIAS_BASE);
}

static void test_store_faults(void)
{
    const uintptr_t instruction = (uintptr_t)rv64_pf_store_insn;
    const uint64_t read_only = pte_for_page(data_page, PTE_V | PTE_R | PTE_A);
    const uint64_t writable = read_only | PTE_W | PTE_D;

    prepare_case(instruction + sizeof(uint32_t), read_only, 0);
    check(rv64_pf_run((uintptr_t)rv64_pf_store, ALIAS_BASE, REGISTER_SENTINEL) == REGISTER_SENTINEL);
    check_fault(CAUSE_STORE_PAGE_FAULT, instruction, ALIAS_BASE);
    check(data_page[0] == DATA_SENTINEL);

    prepare_case(instruction + sizeof(uint32_t), read_only, writable);
    check(rv64_pf_run((uintptr_t)rv64_pf_store, ALIAS_BASE, REGISTER_SENTINEL) == REGISTER_SENTINEL);
    check_fault(CAUSE_STORE_PAGE_FAULT, instruction, ALIAS_BASE);
    check(data_page[0] == REGISTER_SENTINEL);

    prepare_case((uintptr_t)rv64_pf_byte_store_insn + sizeof(uint32_t), read_only, 0);
    check(rv64_pf_run((uintptr_t)rv64_pf_byte_store, ALIAS_BASE, REGISTER_SENTINEL) == REGISTER_SENTINEL);
    check_fault(CAUSE_STORE_PAGE_FAULT, (uintptr_t)rv64_pf_byte_store_insn, ALIAS_BASE);
    check(data_page[0] == DATA_SENTINEL);

    const uintptr_t pressure_instruction = (uintptr_t)rv64_pf_pressure_store_insn;
    const uint64_t expected_pressure[PRESSURE_REGISTERS] = {0x112, 0x223, 0x334, 0x445, 0x556, 0x667};
    uint64_t expected_sum = 0;

    prepare_case(pressure_instruction + sizeof(uint32_t), read_only, 0);
    check(rv64_pf_run((uintptr_t)rv64_pf_pressure_store, ALIAS_BASE, REGISTER_SENTINEL) == REGISTER_SENTINEL);
    check_fault(CAUSE_STORE_PAGE_FAULT, pressure_instruction, ALIAS_BASE);
    check(data_page[0] == DATA_SENTINEL);

    for (uint32_t i = 0; i < PRESSURE_REGISTERS; i++)
    {
        check(rv64_pf_pressure[i] == expected_pressure[i]);
        expected_sum += expected_pressure[i];
    }

    check(rv64_pf_final_pressure == expected_sum);
}

static void test_fetch_faults(void)
{
    const uint64_t nonexecutable = pte_for_page(code_page, PTE_V | PTE_R | PTE_A);
    const uint64_t executable = nonexecutable | PTE_X;

    prepare_case((uintptr_t)rv64_pf_ifetch_resume, nonexecutable, 0);
    check(rv64_pf_run(ALIAS_BASE, ALIAS_BASE, REGISTER_SENTINEL) == REGISTER_SENTINEL);
    check_fault(CAUSE_INSTRUCTION_PAGE_FAULT, ALIAS_BASE, ALIAS_BASE);

    prepare_case((uintptr_t)rv64_pf_ifetch_resume, nonexecutable, executable);
    check(rv64_pf_run(ALIAS_BASE, ALIAS_BASE, REGISTER_SENTINEL) == REGISTER_SENTINEL + 1);
    check_fault(CAUSE_INSTRUCTION_PAGE_FAULT, ALIAS_BASE, ALIAS_BASE);
}

static void test_fp_faults(void)
{
    uintptr_t saved_status;
    uint64_t observed;

    asm volatile("csrr %0, mstatus" : "=r"(saved_status));
    uintptr_t clean_status = (saved_status & ~MSTATUS_FS_MASK) | MSTATUS_FS_CLEAN;

    /* Initialise f0, then reset FS to Clean so a fault cannot hide dirtying. */
    asm volatile("csrw mstatus, %0" : : "r"(clean_status) : "memory");
    asm volatile(".option push\n.option arch, +f\n.option arch, +d\n"
                 "fmv.d.x f0, %0\n.option pop\n"
                 : : "r"(FPR_SENTINEL) : "memory");
    asm volatile("csrw mstatus, %0" : : "r"(clean_status) : "memory");

    const uintptr_t load_instruction = (uintptr_t)rv64_pf_fp_load_insn;
    prepare_case(load_instruction + sizeof(uint32_t), 0, 0);
    check(rv64_pf_run((uintptr_t)rv64_pf_fp_load, ALIAS_BASE, REGISTER_SENTINEL) == REGISTER_SENTINEL);
    check_fault(CAUSE_LOAD_PAGE_FAULT, load_instruction, ALIAS_BASE);
    check((rv64_pf_mstatus & MSTATUS_FS_MASK) == MSTATUS_FS_CLEAN);

    asm volatile(".option push\n.option arch, +f\n.option arch, +d\n"
                 "fmv.x.d %0, f0\n.option pop\n"
                 : "=r"(observed) : : "memory");
    check(observed == FPR_SENTINEL);

    const uintptr_t store_instruction = (uintptr_t)rv64_pf_fp_store_insn;
    prepare_case(store_instruction + sizeof(uint32_t), pte_for_page(data_page, PTE_V | PTE_R | PTE_A), 0);
    check(rv64_pf_run((uintptr_t)rv64_pf_fp_store, ALIAS_BASE, REGISTER_SENTINEL) == REGISTER_SENTINEL);
    check_fault(CAUSE_STORE_PAGE_FAULT, store_instruction, ALIAS_BASE);
    check(data_page[0] == DATA_SENTINEL);
    check((rv64_pf_mstatus & MSTATUS_FS_MASK) == MSTATUS_FS_CLEAN);

    const uintptr_t word_load_instruction = (uintptr_t)rv64_pf_fp_word_load_insn;
    prepare_case(word_load_instruction + sizeof(uint32_t), 0, 0);
    check(rv64_pf_run((uintptr_t)rv64_pf_fp_word_load, ALIAS_BASE, REGISTER_SENTINEL) == REGISTER_SENTINEL);
    check_fault(CAUSE_LOAD_PAGE_FAULT, word_load_instruction, ALIAS_BASE);
    check((rv64_pf_mstatus & MSTATUS_FS_MASK) == MSTATUS_FS_CLEAN);

    asm volatile(".option push\n.option arch, +f\n.option arch, +d\n"
                 "fmv.x.d %0, f0\n.option pop\n"
                 : "=r"(observed) : : "memory");
    check(observed == FPR_SENTINEL);

    const uintptr_t word_store_instruction = (uintptr_t)rv64_pf_fp_word_store_insn;
    prepare_case(word_store_instruction + sizeof(uint32_t), pte_for_page(data_page, PTE_V | PTE_R | PTE_A), 0);
    check(rv64_pf_run((uintptr_t)rv64_pf_fp_word_store, ALIAS_BASE, REGISTER_SENTINEL) == REGISTER_SENTINEL);
    check_fault(CAUSE_STORE_PAGE_FAULT, word_store_instruction, ALIAS_BASE);
    check(data_page[0] == DATA_SENTINEL);
    check((rv64_pf_mstatus & MSTATUS_FS_MASK) == MSTATUS_FS_CLEAN);

    asm volatile("csrw mstatus, %0" : : "r"(saved_status) : "memory");
}

#endif

int main(void)
{
#if defined(__riscv) && __riscv_xlen == 64
    uintptr_t saved_mtvec;
    uintptr_t saved_satp;
    uintptr_t saved_mstatus;

    asm volatile("csrr %0, mtvec" : "=r"(saved_mtvec));
    asm volatile("csrr %0, satp" : "=r"(saved_satp));
    asm volatile("csrr %0, mstatus" : "=r"(saved_mstatus));

    /* The aligned 1 GiB leaf covers AM code, stack, data and these tables. */
    root_pt[IDENTITY_BASE >> 30] = pte_for_page((void *)(uintptr_t)IDENTITY_BASE, PTE_V | PTE_R | PTE_W | PTE_X | PTE_A | PTE_D);
    root_pt[ALIAS_BASE >> 30] = pte_for_page(alias_l1, PTE_V);
    alias_l1[0] = pte_for_page(alias_l0, PTE_V);
    code_page[0] = INSN_ADDI_A1_A1_ONE;
    code_page[1] = INSN_ADDI_A3_A3_ONE;
    code_page[2] = INSN_ECALL;

    const uintptr_t satp = SATP_SV39 | ((uintptr_t)root_pt >> PAGE_SHIFT);
    asm volatile("csrw mtvec, %0" : : "r"(rv64_pf_handler) : "memory");
    asm volatile("csrw satp, %0" : : "r"(satp) : "memory");
    sfence_all();

    test_load_faults();
    test_store_faults();
    test_fetch_faults();
    test_fp_faults();

    asm volatile("csrw satp, %0" : : "r"(saved_satp) : "memory");
    sfence_all();
    asm volatile("csrw mtvec, %0" : : "r"(saved_mtvec) : "memory");
    asm volatile("csrw mstatus, %0" : : "r"(saved_mstatus) : "memory");
#endif

    return 0;
}
