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
static uint64_t data_page[WORDS_PER_PAGE] __attribute__((aligned(PAGE_SIZE)));

/*
 * Unexpected traps are test failures.  Encoding mcause into the bad-trap code
 * keeps page-table setup mistakes visible.
 */
asm(".globl rv64_sv39_data_unexpected_trap\n"
    "rv64_sv39_data_unexpected_trap:\n"
    "  csrr a0, mcause\n"
    "  addi a0, a0, 16\n"
    "  .word 0x0000006b\n");

extern void rv64_sv39_data_unexpected_trap(void);

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

/* Install one alias leaf that points DATA_ALIAS_VA at data_page. */
static void map_data_alias(void)
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
    data_alias_l0[vpn0(DATA_ALIAS_VA)] = pte_for_page(data_page, leaf_flags);
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
    map_data_alias();
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
    asm volatile("csrw mtvec, %0" : : "r"(rv64_sv39_data_unexpected_trap) : "memory");
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

/* Execute every RV64 integer load width through the Sv39 alias. */
static void sv39_load_widths(uint8_t *alias, uint64_t *out)
{
    uint64_t lb;
    uint64_t lbu;
    uint64_t lh;
    uint64_t lhu;
    uint64_t lw;
    uint64_t lwu;
    uint64_t ld;

    asm volatile(
        "lb %[lb], 0(%[alias])\n"
        "lbu %[lbu], 0(%[alias])\n"
        "lh %[lh], 2(%[alias])\n"
        "lhu %[lhu], 2(%[alias])\n"
        "lw %[lw], 4(%[alias])\n"
        "lwu %[lwu], 4(%[alias])\n"
        "ld %[ld], 16(%[alias])\n"
        : [lb] "=&r"(lb),
          [lbu] "=&r"(lbu),
          [lh] "=&r"(lh),
          [lhu] "=&r"(lhu),
          [lw] "=&r"(lw),
          [lwu] "=&r"(lwu),
          [ld] "=&r"(ld)
        : [alias] "r"(alias)
        : "memory");

    out[0] = lb;
    out[1] = lbu;
    out[2] = lh;
    out[3] = lhu;
    out[4] = lw;
    out[5] = lwu;
    out[6] = ld;
}

/* Execute every RV64 integer store width through the Sv39 alias. */
static void sv39_store_widths(uint8_t *alias)
{
    const uint64_t byte_value = 0xa5ull;
    const uint64_t half_value = 0xb6c7ull;
    const uint64_t word_value = 0xd8e9fa0bull;
    const uint64_t dword_value = 0x1122334455667788ull;

    asm volatile(
        "sb %[byte_value], 24(%[alias])\n"
        "sh %[half_value], 26(%[alias])\n"
        "sw %[word_value], 28(%[alias])\n"
        "sd %[dword_value], 32(%[alias])\n"
        :
        : [alias] "r"(alias),
          [byte_value] "r"(byte_value),
          [half_value] "r"(half_value),
          [word_value] "r"(word_value),
          [dword_value] "r"(dword_value)
        : "memory");
}

/* Verify that Sv39 data loads and stores use the alias mapping, not VA==PA. */
static void test_sv39_data_memory(void)
{
    uint8_t *raw = (uint8_t *)data_page;
    uint64_t loaded[7];

    for (uint32_t i = 0; i < PAGE_SIZE; i++)
    {
        raw[i] = 0;
    }

    raw[0] = 0x80u;
    raw[2] = 0x00u;
    raw[3] = 0x80u;
    raw[4] = 0x01u;
    raw[5] = 0x00u;
    raw[6] = 0x00u;
    raw[7] = 0x80u;
    data_page[2] = 0x0102030405060708ull;

    sv39_load_widths((uint8_t *)(uintptr_t)DATA_ALIAS_VA, loaded);
    sv39_store_widths((uint8_t *)(uintptr_t)DATA_ALIAS_VA);

    check(loaded[0] == 0xffffffffffffff80ull);
    check(loaded[1] == 0x80ull);
    check(loaded[2] == 0xffffffffffff8000ull);
    check(loaded[3] == 0x8000ull);
    check(loaded[4] == 0xffffffff80000001ull);
    check(loaded[5] == 0x80000001ull);
    check(loaded[6] == 0x0102030405060708ull);

    check(raw[24] == 0xa5u);
    check(raw[26] == 0xc7u && raw[27] == 0xb6u);
    check(raw[28] == 0x0bu && raw[29] == 0xfau &&
          raw[30] == 0xe9u && raw[31] == 0xd8u);
    check(data_page[4] == 0x1122334455667788ull);
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
    test_sv39_data_memory();
#endif

    return 0;
}
