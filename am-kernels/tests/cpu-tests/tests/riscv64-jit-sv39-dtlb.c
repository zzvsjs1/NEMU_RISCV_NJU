#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 64

#include <stdint.h>

#define PAGE_SIZE 4096ull
#define WORDS_PER_PAGE (PAGE_SIZE / sizeof(uint64_t))

/* Sv39 PTE flag bits used by this test's hand-built page tables. */
#define PTE_V 0x001ull
#define PTE_R 0x002ull
#define PTE_W 0x004ull
#define PTE_X 0x008ull
#define PTE_A 0x040ull
#define PTE_D 0x080ull

/* satp.MODE=8 selects Sv39 on RV64; MPP=S makes translation active after mret. */
#define SATP_MODE_SV39 (8ull << 60)
#define MSTATUS_MPP_MASK (3ull << 11)
#define MSTATUS_MPP_MPIE_MASK (MSTATUS_MPP_MASK | (1ull << 7))
#define MSTATUS_MPP_S (1ull << 11)

#define IDENTITY_BASE 0x80000000ull
#define IDENTITY_PAGES 32768ull
#define IDENTITY_L1_ENTRIES (IDENTITY_PAGES / 512ull)
#define DATA_ALIAS_VA 0x80400000ull

static uint64_t root_pt[512] __attribute__((aligned(PAGE_SIZE)));
static uint64_t identity_l1[512] __attribute__((aligned(PAGE_SIZE)));
static uint64_t identity_l0[IDENTITY_L1_ENTRIES][512] __attribute__((aligned(PAGE_SIZE)));
static uint64_t data_alias_l0[512] __attribute__((aligned(PAGE_SIZE)));
static uint64_t data_page_a[WORDS_PER_PAGE] __attribute__((aligned(PAGE_SIZE)));
static uint64_t data_page_b[WORDS_PER_PAGE] __attribute__((aligned(PAGE_SIZE)));
static volatile uint64_t source_write_input = 41;

/*
 * Unexpected traps are test failures.  Encoding mcause into the bad-trap code
 * keeps page-table setup mistakes visible.
 */
asm(".globl rv64_sv39_dtlb_unexpected_trap\n"
    "rv64_sv39_dtlb_unexpected_trap:\n"
    "  csrr a0, mcause\n"
    "  addi a0, a0, 16\n"
    "  .word 0x0000006b\n");

extern void rv64_sv39_dtlb_unexpected_trap(void);

/* Return the Sv39 root-table index for a canonical virtual address. */
static uint64_t vpn2(uint64_t va)
{
    return (va >> 30) & 0x1ffull;
}

/* Return the Sv39 middle-table index for a canonical virtual address. */
static uint64_t vpn1(uint64_t va)
{
    return (va >> 21) & 0x1ffull;
}

/* Return the Sv39 leaf-table index for a canonical virtual address. */
static uint64_t vpn0(uint64_t va)
{
    return (va >> 12) & 0x1ffull;
}

/* Encode a 4 KiB physical page pointer and permission bits as an Sv39 PTE. */
static uint64_t pte_for_page(const void *page, uint64_t flags)
{
    const uintptr_t pa = (uintptr_t)page;

    return ((uint64_t)(pa >> 12) << 10) | flags;
}

/* Clear one page-table page before installing the exact entries needed. */
static void clear_page_table(uint64_t *pt)
{
    for (uint32_t i = 0; i < 512u; i++)
    {
        pt[i] = 0;
    }
}

/* Identity-map the normal NEMU PMEM window used by code, stack, and data. */
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

/* Install one alias leaf that points DATA_ALIAS_VA at the selected data page. */
static void map_data_alias(const void *page)
{
    const uint64_t leaf_flags = PTE_V | PTE_R | PTE_W | PTE_X | PTE_A | PTE_D;
    const uint64_t table_flags = PTE_V;

    for (uint64_t i = 0; i < 512ull; i++)
    {
        const uintptr_t pa =
            (uintptr_t)((vpn1(DATA_ALIAS_VA) * 512ull + i) * PAGE_SIZE +
                        IDENTITY_BASE);

        data_alias_l0[i] = ((uint64_t)(pa >> 12) << 10) | leaf_flags;
    }

    identity_l1[vpn1(DATA_ALIAS_VA)] = pte_for_page(data_alias_l0, table_flags);
    data_alias_l0[vpn0(DATA_ALIAS_VA)] = pte_for_page(page, leaf_flags);
}

/* Install a three-level Sv39 tree with identity code and one data alias. */
static void install_page_tables(void)
{
    clear_page_table(root_pt);
    clear_page_table(identity_l1);
    clear_page_table(data_alias_l0);

    for (uint64_t i = 0; i < IDENTITY_L1_ENTRIES; i++)
    {
        clear_page_table(identity_l0[i]);
    }

    map_identity_window();
    map_data_alias(data_page_a);
    root_pt[vpn2(IDENTITY_BASE)] = pte_for_page(identity_l1, PTE_V);
}

/* Write satp with the root page-table physical page number. */
static void enable_sv39(void)
{
    const uintptr_t root_ppn = (uintptr_t)root_pt >> 12;
    const uint64_t satp = SATP_MODE_SV39 | (uint64_t)root_ppn;

    asm volatile("csrw satp, %0" : : "r"(satp) : "memory");
}

/* Execute a global SFENCE.VMA; 0x12000073 encodes sfence.vma x0, x0. */
static void sfence_vma_all(void)
{
    asm volatile(".word 0x12000073" : : : "memory");
}

/* Point mtvec at a no-stack failure path before entering translated S-mode. */
static void install_unexpected_trap_handler(void)
{
    asm volatile("csrw mtvec, %0" : : "r"(rv64_sv39_dtlb_unexpected_trap) : "memory");
}

/* Enter S-mode so satp controls explicit loads and stores. */
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

/* Issue several same-page loads so a data-TLB fill can be followed by hits. */
static uint64_t load_alias_repeated(uint64_t *alias)
{
    uint64_t a0;
    uint64_t a1;
    uint64_t a2;
    uint64_t a3;

    asm volatile(
        "ld %[a0], 0(%[alias])\n"
        "ld %[a1], 8(%[alias])\n"
        "ld %[a2], 16(%[alias])\n"
        "ld %[a3], 24(%[alias])\n"
        : [a0] "=&r"(a0),
          [a1] "=&r"(a1),
          [a2] "=&r"(a2),
          [a3] "=&r"(a3)
        : [alias] "r"(alias)
        : "memory");

    return a0 ^ a1 ^ a2 ^ a3;
}

/* Store through the alias to exercise the write-translation helper path too. */
static void store_alias_value(uint64_t *alias, uint64_t value)
{
    asm volatile("sd %[value], 32(%[alias])"
                 :
                 : [alias] "r"(alias),
                   [value] "r"(value)
                 : "memory");
}

/* Small call target that should become a native block before the source rewrite. */
static uint64_t __attribute__((noinline)) source_write_target(uint64_t value)
{
    return value + 0x1234ull;
}

/* Rewrite one compiled instruction with the same bytes through translated PMEM. */
static void rewrite_source_word_same_value(void)
{
    volatile uint32_t *target = (volatile uint32_t *)(uintptr_t)source_write_target;
    const uint32_t original = *target;

    /*
     * The value is unchanged, so architectural behaviour stays stable.  The
     * store still targets compiled source bytes and must therefore take the
     * helper path so native block invalidation happens before more JIT entry.
     */
    asm volatile("sw %[original], 0(%[target])"
                 :
                 : [target] "r"(target),
                   [original] "r"(original)
                 : "memory");
}

/* Exercise the source-chunk guard used by inline translated stores. */
static void test_paged_source_write_fallback(void)
{
    const uint64_t input = source_write_input;

    check(source_write_target(input) == input + 0x1234ull);
    check(source_write_target(input + 1ull) == input + 0x1235ull);
    rewrite_source_word_same_value();
    check(source_write_target(input + 2ull) == input + 0x1236ull);
}

/* Verify Sv39 data-TLB hits and SFENCE invalidation stay architecturally strict. */
static void test_sv39_data_tlb(void)
{
    uint64_t *alias = (uint64_t *)(uintptr_t)DATA_ALIAS_VA;

    data_page_a[0] = 0x1111111111111111ull;
    data_page_a[1] = 0x2222222222222222ull;
    data_page_a[2] = 0x3333333333333333ull;
    data_page_a[3] = 0x4444444444444444ull;
    data_page_b[0] = 0x5555555555555555ull;
    data_page_b[1] = 0x6666666666666666ull;
    data_page_b[2] = 0x7777777777777777ull;
    data_page_b[3] = 0x8888888888888888ull;

    check(load_alias_repeated(alias) == 0x4444444444444444ull);
    store_alias_value(alias, 0x123456789abcdef0ull);
    check(data_page_a[4] == 0x123456789abcdef0ull);
    test_paged_source_write_fallback();
    check(load_alias_repeated(alias) == 0x4444444444444444ull);

    /*
     * The PTE is modified through the identity mapping, then SFENCE.VMA makes
     * the new translation visible.  A stale JIT data-TLB entry would keep
     * reading data_page_a and fail the second checksum below.
     */
    data_alias_l0[vpn0(DATA_ALIAS_VA)] =
        pte_for_page(data_page_b, PTE_V | PTE_R | PTE_W | PTE_X | PTE_A | PTE_D);
    sfence_vma_all();

    check(load_alias_repeated(alias) == 0xccccccccccccccccull);
    store_alias_value(alias, 0x0fedcba987654321ull);
    check(data_page_b[4] == 0x0fedcba987654321ull);
    check(data_page_a[4] == 0x123456789abcdef0ull);
}

#endif

/* Keep the source buildable outside RV64 while exercising the RV64-only path. */
int main(void)
{
#if defined(__riscv) && __riscv_xlen == 64
    install_page_tables();
    install_unexpected_trap_handler();
    enable_sv39();
    sfence_vma_all();
    enter_supervisor_mode();
    test_sv39_data_tlb();
#endif

    return 0;
}
