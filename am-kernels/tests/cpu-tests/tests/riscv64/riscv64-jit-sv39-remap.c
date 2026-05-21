#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 64

#include <stdint.h>

typedef int (*generated_fn_t)(void);

/*
 * This is the RV64/Sv39 version of the JIT remap regression.  The same virtual
 * function address is first mapped to code_page_a, then remapped to code_page_b
 * while satp stays unchanged.  A JIT cache hit must therefore re-check the
 * translated physical source address, not only the virtual PC and satp value.
 */
#define PAGE_SIZE 4096ull
#define WORDS_PER_PAGE (PAGE_SIZE / sizeof(uint32_t))

/* Sv39 PTE bits used by this test; the bit positions are architectural. */
#define PTE_V 0x001ull
#define PTE_R 0x002ull
#define PTE_W 0x004ull
#define PTE_X 0x008ull
#define PTE_A 0x040ull
#define PTE_D 0x080ull

/* satp.MODE=8 selects Sv39 on RV64, stored in bits [63:60]. */
#define SATP_MODE_SV39 (8ull << 60)
#define MSTATUS_MPP_MASK (3ull << 11)
#define MSTATUS_MPP_MPIE_MASK (MSTATUS_MPP_MASK | (1ull << 7))
#define MSTATUS_MPP_S (1ull << 11)

/*
 * CPU tests are linked at the normal NEMU PMEM base.  One Sv39 root entry covers
 * a 1 GiB region, and each level-1 entry covers a 2 MiB region.  Mapping the
 * full 128 MiB NEMU PMEM range keeps code, stack, page tables, generated
 * buffers, and any AM runtime data reachable after S-mode instruction fetch
 * starts using satp.
 */
#define IDENTITY_BASE 0x80000000ull
#define IDENTITY_PAGES 32768ull
#define IDENTITY_L1_ENTRIES (IDENTITY_PAGES / 512ull)
#define ALIAS_VA 0x80400000ull

/*
 * Sv39 uses 512 entries at each page-table level because each VPN field is nine
 * bits wide.  Every table and generated code page is 4 KiB-aligned because PTEs
 * store physical page numbers rather than byte addresses.
 */
static uint64_t root_pt[512] __attribute__((aligned(PAGE_SIZE)));
static uint64_t identity_l1[512] __attribute__((aligned(PAGE_SIZE)));
static uint64_t identity_l0[IDENTITY_L1_ENTRIES][512] __attribute__((aligned(PAGE_SIZE)));
static uint64_t alias_l0[512] __attribute__((aligned(PAGE_SIZE)));
static uint32_t code_page_a[WORDS_PER_PAGE] __attribute__((aligned(PAGE_SIZE)));
static uint32_t code_page_b[WORDS_PER_PAGE] __attribute__((aligned(PAGE_SIZE)));

/*
 * Unexpected traps are test failures.  Encoding mcause into the bad-trap code
 * makes paging setup mistakes visible instead of falling through an unset mtvec.
 */
asm(".globl rv64_sv39_unexpected_trap\n"
    "rv64_sv39_unexpected_trap:\n"
    "  csrr a0, mcause\n"
    "  addi a0, a0, 16\n"
    "  .word 0x0000006b\n");

extern void rv64_sv39_unexpected_trap(void);

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

    /*
     * PTE bits [53:10] hold PPN[2:0].  The page pointer is already aligned, so
     * dropping the 12 page-offset bits leaves the physical page number and
     * shifting left by 10 places it in the PTE.
     */
    return ((uint64_t)(pa >> 12) << 10) | flags;
}

/* Encode the two-instruction function body: addi a0, zero, imm. */
static uint32_t addi_a0_zero_imm(uint32_t imm)
{
    /*
     * OP-IMM opcode is 0x13, rd=a0 is x10, rs1=x0, funct3=0.  The immediate is
     * deliberately small so the 12-bit signed I-format field is unambiguous.
     */
    return ((imm & 0xfffu) << 20) | (10u << 7) | 0x13u;
}

/* Clear one 4 KiB page table before installing only the mappings we need. */
static void clear_page_table(uint64_t *pt)
{
    for (uint32_t i = 0; i < 512u; i++)
    {
        pt[i] = 0;
    }
}

/* Identity-map the low part of the test image with R/W/X permissions. */
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

/* Install a minimal three-level Sv39 tree for identity code and the alias VA. */
static void install_page_tables(void)
{
    const uint64_t table_flags = PTE_V;
    const uint64_t execute_flags = PTE_V | PTE_R | PTE_X | PTE_A;

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
            (uintptr_t)((vpn1(ALIAS_VA) * 512ull + i) * PAGE_SIZE + IDENTITY_BASE);
        alias_l0[i] = ((uint64_t)(pa >> 12) << 10) |
                      (PTE_V | PTE_R | PTE_W | PTE_X | PTE_A | PTE_D);
    }
    identity_l1[vpn1(ALIAS_VA)] = pte_for_page(alias_l0, table_flags);
    alias_l0[vpn0(ALIAS_VA)] = pte_for_page(code_page_a, execute_flags);
}

/* Enable Sv39 while still in M-mode; translation becomes active after mret. */
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
    asm volatile("csrw mtvec, %0" : : "r"(rv64_sv39_unexpected_trap) : "memory");
}

/* Enter S-mode so satp controls instruction fetch according to the spec. */
static void enter_supervisor_mode(void)
{
    uintptr_t mstatus;

    /*
     * Clear MPIE while setting MPP=S.  The test has no interrupt handler for
     * normal timer work, and address-translation correctness does not depend on
     * enabling interrupts during the short S-mode window.
     */
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

/* Build two physical code pages with identical shape but different results. */
static void prepare_generated_code(void)
{
    code_page_a[0] = addi_a0_zero_imm(7);
    code_page_a[1] = 0x00008067u; /* ret is jalr zero, 0(ra). */
    code_page_b[0] = addi_a0_zero_imm(9);
    code_page_b[1] = 0x00008067u; /* ret is jalr zero, 0(ra). */
}

#endif

/* Keep the source buildable for non-RV64 targets while testing RV64 JITs. */
int main(void)
{
#if defined(__riscv) && __riscv_xlen == 64
    prepare_generated_code();
    install_page_tables();
    install_unexpected_trap_handler();
    enable_sv39();
    sfence_vma_all();
    enter_supervisor_mode();

    generated_fn_t fn = (generated_fn_t)(uintptr_t)ALIAS_VA;

    const int first = fn();
    check(first == 7);

    /*
     * Remap only the leaf PTE.  If a cached native block validates only PC/satp,
     * the second call will incorrectly keep executing the old physical page.
     */
    alias_l0[vpn0(ALIAS_VA)] =
        pte_for_page(code_page_b, PTE_V | PTE_R | PTE_X | PTE_A);
    sfence_vma_all();

    const int second = fn();
    check(second == 9);
#endif

    return 0;
}
