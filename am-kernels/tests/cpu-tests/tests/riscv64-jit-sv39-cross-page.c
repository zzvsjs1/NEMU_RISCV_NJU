#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 64

#include <stdint.h>

typedef int (*generated_fn_t)(void);

/*
 * Exercise a translated native block that starts at the last instruction slot
 * of one Sv39 page and continues in the next page.  The old RV64 JIT stayed
 * correct by splitting this into tiny blocks; the regression gate checks that a
 * real translated cross-page block is compiled, then verifies that remapping the
 * second page is still observed.
 */
#define PAGE_SIZE 4096ull
#define WORDS_PER_PAGE (PAGE_SIZE / sizeof(uint32_t))
#define GENERATED_INCREMENTS 9u
#define HOT_CALLS 4096u

#define PTE_V 0x001ull
#define PTE_R 0x002ull
#define PTE_W 0x004ull
#define PTE_X 0x008ull
#define PTE_A 0x040ull
#define PTE_D 0x080ull

#define SATP_MODE_SV39 (8ull << 60)
#define MSTATUS_MPP_MASK (3ull << 11)
#define MSTATUS_MPP_MPIE_MASK (MSTATUS_MPP_MASK | (1ull << 7))
#define MSTATUS_MPP_S (1ull << 11)

#define IDENTITY_BASE 0x80000000ull
#define IDENTITY_PAGES 32768ull
#define IDENTITY_L1_ENTRIES (IDENTITY_PAGES / 512ull)
#define ALIAS_BASE 0x80400000ull
#define ALIAS_ENTRY (ALIAS_BASE + PAGE_SIZE - sizeof(uint32_t))

static uint64_t root_pt[512] __attribute__((aligned(PAGE_SIZE)));
static uint64_t identity_l1[512] __attribute__((aligned(PAGE_SIZE)));
static uint64_t identity_l0[IDENTITY_L1_ENTRIES][512] __attribute__((aligned(PAGE_SIZE)));
static uint64_t alias_l0[512] __attribute__((aligned(PAGE_SIZE)));
static uint32_t code_pair_a[WORDS_PER_PAGE * 2u] __attribute__((aligned(PAGE_SIZE)));
static uint32_t code_second_b[WORDS_PER_PAGE] __attribute__((aligned(PAGE_SIZE)));

asm(".globl rv64_sv39_cross_page_unexpected_trap\n"
    "rv64_sv39_cross_page_unexpected_trap:\n"
    "  csrr a0, mcause\n"
    "  addi a0, a0, 16\n"
    "  .word 0x0000006b\n");

extern void rv64_sv39_cross_page_unexpected_trap(void);

static uint64_t vpn2(uint64_t va)
{
    return (va >> 30) & 0x1ffull;
}

static uint64_t vpn1(uint64_t va)
{
    return (va >> 21) & 0x1ffull;
}

static uint64_t vpn0(uint64_t va)
{
    return (va >> 12) & 0x1ffull;
}

static uint64_t pte_for_page(const void *page, uint64_t flags)
{
    const uintptr_t pa = (uintptr_t)page;
    return ((uint64_t)(pa >> 12) << 10) | flags;
}

static uint32_t addi_a0_zero_imm(uint32_t imm)
{
    return ((imm & 0xfffu) << 20) | (10u << 7) | 0x13u;
}

static uint32_t addi_a0_a0_imm(uint32_t imm)
{
    return ((imm & 0xfffu) << 20) | (10u << 15) | (10u << 7) | 0x13u;
}

static void clear_page_table(uint64_t *pt)
{
    for (uint32_t i = 0; i < 512u; i++)
    {
        pt[i] = 0;
    }
}

static void map_identity_window(void)
{
    const uint64_t leaf_flags = PTE_V | PTE_R | PTE_W | PTE_X | PTE_A | PTE_D;

    for (uint64_t l1 = 0; l1 < IDENTITY_L1_ENTRIES; l1++)
    {
        identity_l1[vpn1(IDENTITY_BASE) + l1] =
            pte_for_page(identity_l0[l1], PTE_V);

        for (uint64_t i = 0; i < 512ull; i++)
        {
            const uintptr_t page_index = (uintptr_t)l1 * 512u + (uintptr_t)i;
            const uintptr_t pa = (uintptr_t)IDENTITY_BASE + page_index * PAGE_SIZE;
            identity_l0[l1][i] = ((uint64_t)(pa >> 12) << 10) | leaf_flags;
        }
    }
}

static void install_page_tables(void)
{
    const uint64_t table_flags = PTE_V;
    const uint64_t code_flags = PTE_V | PTE_R | PTE_X | PTE_A;

    clear_page_table(root_pt);
    clear_page_table(identity_l1);
    for (uint64_t i = 0; i < IDENTITY_L1_ENTRIES; i++)
    {
        clear_page_table(identity_l0[i]);
    }
    clear_page_table(alias_l0);
    map_identity_window();

    root_pt[vpn2(IDENTITY_BASE)] = pte_for_page(identity_l1, table_flags);

    for (uint64_t i = 0; i < 512ull; i++)
    {
        const uintptr_t pa =
            (uintptr_t)((vpn1(ALIAS_BASE) * 512ull + i) * PAGE_SIZE + IDENTITY_BASE);
        alias_l0[i] = ((uint64_t)(pa >> 12) << 10) |
                      (PTE_V | PTE_R | PTE_W | PTE_X | PTE_A | PTE_D);
    }

    identity_l1[vpn1(ALIAS_BASE)] = pte_for_page(alias_l0, table_flags);
    alias_l0[vpn0(ALIAS_BASE)] = pte_for_page(code_pair_a, code_flags);
    alias_l0[vpn0(ALIAS_BASE) + 1u] =
        pte_for_page(&code_pair_a[WORDS_PER_PAGE], code_flags);
}

static void enable_sv39(void)
{
    const uintptr_t root_ppn = (uintptr_t)root_pt >> 12;
    const uint64_t satp = SATP_MODE_SV39 | (uint64_t)root_ppn;

    asm volatile("csrw satp, %0" : : "r"(satp) : "memory");
}

static void sfence_vma_all(void)
{
    asm volatile(".word 0x12000073" : : : "memory");
}

static void install_unexpected_trap_handler(void)
{
    asm volatile("csrw mtvec, %0"
                 :
                 : "r"(rv64_sv39_cross_page_unexpected_trap)
                 : "memory");
}

static void enter_supervisor_mode(void)
{
    uintptr_t mstatus;

    asm volatile(
        "csrr %[mstatus], mstatus\n"
        "li t0, %[mpp_mask]\n"
        "not t0, t0\n"
        "and %[mstatus], %[mstatus], t0\n"
        "li t0, %[mpp_s]\n"
        "or %[mstatus], %[mstatus], t0\n"
        "csrw mstatus, %[mstatus]\n"
        "la t0, 1f\n"
        "csrw mepc, t0\n"
        "mret\n"
        "1:\n"
        : [mstatus] "=&r"(mstatus)
        : [mpp_mask] "i"(MSTATUS_MPP_MPIE_MASK),
          [mpp_s] "i"(MSTATUS_MPP_S)
        : "t0", "memory");
}

static void prepare_generated_code(void)
{
    code_pair_a[WORDS_PER_PAGE - 1u] = addi_a0_zero_imm(1);
    for (uint32_t i = 0; i < GENERATED_INCREMENTS; i++)
    {
        code_pair_a[WORDS_PER_PAGE + i] = addi_a0_a0_imm(1);
        code_second_b[i] = addi_a0_a0_imm(2);
    }
    code_pair_a[WORDS_PER_PAGE + GENERATED_INCREMENTS] = 0x00008067u;
    code_second_b[GENERATED_INCREMENTS] = 0x00008067u;
}

#endif

int main(void)
{
#if defined(__riscv) && __riscv_xlen == 64
    prepare_generated_code();
    install_page_tables();
    install_unexpected_trap_handler();
    enable_sv39();
    sfence_vma_all();
    enter_supervisor_mode();

    generated_fn_t fn = (generated_fn_t)(uintptr_t)ALIAS_ENTRY;

    const int first = fn();
    check(first == 1 + (int)GENERATED_INCREMENTS);

    alias_l0[vpn0(ALIAS_BASE) + 1u] =
        pte_for_page(code_second_b, PTE_V | PTE_R | PTE_X | PTE_A);
    sfence_vma_all();

    const int second = fn();
    check(second == 1 + (int)GENERATED_INCREMENTS * 2);

    volatile int total = 0;
    for (uint32_t i = 0; i < HOT_CALLS; i++)
    {
        total += fn();
    }
    check(total == (int)(HOT_CALLS * (1u + GENERATED_INCREMENTS * 2u)));
#endif

    return 0;
}
