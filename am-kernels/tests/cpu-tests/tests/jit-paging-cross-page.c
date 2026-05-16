#include "trap.h"
#include <stdint.h>

typedef int (*generated_fn_t)(void);

/*
 * This test targets a subtle JIT cache-hit case.  The translated function starts
 * at the last 32-bit instruction slot of one virtual page, then continues on the
 * next virtual page.  The two virtual pages are initially backed by contiguous
 * physical PMEM, so an old JIT could compile one block covering both pages and
 * record one contiguous physical source range.
 */
#define PAGE_SIZE 4096u
#define WORDS_PER_PAGE (PAGE_SIZE / sizeof(uint32_t))

#define PTE_V 0x001u
#define PTE_R 0x002u
#define PTE_W 0x004u
#define PTE_X 0x008u
#define PTE_A 0x040u
#define PTE_D 0x080u

#define SATP_MODE_SV32 0x80000000u
#define MSTATUS_MPIE (1u << 7)
#define MSTATUS_MPP_MASK (3u << 11)
#define MSTATUS_MPP_S (1u << 11)

/*
 * Keep the normal program identity mapped at 0x80000000, and use the following
 * 4 MiB virtual region for the generated cross-page function.  The entry point
 * is PAGE_SIZE - 4 so instruction 0 lives in the first alias page and
 * instruction 1 lives in the second alias page.
 */
#define IDENTITY_BASE 0x80000000u
#define IDENTITY_PAGES 64u
#define ALIAS_BASE 0x80400000u
#define ALIAS_ENTRY (ALIAS_BASE + PAGE_SIZE - 4u)

static uint32_t root_pt[1024] __attribute__((aligned(PAGE_SIZE)));
static uint32_t identity_l0[1024] __attribute__((aligned(PAGE_SIZE)));
static uint32_t alias_l0[1024] __attribute__((aligned(PAGE_SIZE)));
static uint32_t code_pair_a[WORDS_PER_PAGE * 2u] __attribute__((aligned(PAGE_SIZE)));
static uint32_t code_second_b[WORDS_PER_PAGE] __attribute__((aligned(PAGE_SIZE)));

static uint32_t pte_for_page(const void *page, uint32_t flags)
{
    const uintptr_t pa = (uintptr_t)page;
    /*
   * Sv32 stores the page number in PTE bits [31:10].  The page buffers are
   * aligned, so dropping the low 12 address bits and then shifting into the PTE
   * field gives the encoded physical page number.
   */
    return (uint32_t)((pa >> 12) << 10) | flags;
}

static uint32_t addi_a0_zero_imm(uint32_t imm)
{
    /* addi a0, zero, imm: rd=x10, rs1=x0, funct3=0, opcode=OP-IMM. */
    return ((imm & 0xfffu) << 20) | (10u << 7) | 0x13u;
}

static uint32_t addi_a0_a0_imm(uint32_t imm)
{
    /* addi a0, a0, imm: rd=x10 and rs1=x10, used to make page 2 observable. */
    return ((imm & 0xfffu) << 20) | (10u << 15) | (10u << 7) | 0x13u;
}

static void clear_page_table(uint32_t *pt)
{
    for (uint32_t i = 0; i < 1024u; i++)
    {
        pt[i] = 0;
    }
}

static void map_identity_window(void)
{
    const uint32_t leaf_flags = PTE_V | PTE_R | PTE_W | PTE_X | PTE_A | PTE_D;
    for (uint32_t i = 0; i < IDENTITY_PAGES; i++)
    {
        const uintptr_t pa = (uintptr_t)IDENTITY_BASE + (uintptr_t)i * PAGE_SIZE;
        identity_l0[i] = (uint32_t)((pa >> 12) << 10) | leaf_flags;
    }
}

static void install_page_tables(void)
{
    const uint32_t table_flags = PTE_V;
    const uint32_t leaf_flags = PTE_V | PTE_R | PTE_W | PTE_X | PTE_A | PTE_D;
    const uint32_t alias_vpn0 = (ALIAS_BASE >> 12) & 0x3ffu;

    clear_page_table(root_pt);
    clear_page_table(identity_l0);
    clear_page_table(alias_l0);
    map_identity_window();

    root_pt[IDENTITY_BASE >> 22] = pte_for_page(identity_l0, table_flags);
    root_pt[ALIAS_BASE >> 22] = pte_for_page(alias_l0, table_flags);
    alias_l0[alias_vpn0] = pte_for_page(code_pair_a, leaf_flags);
    alias_l0[alias_vpn0 + 1u] =
        pte_for_page(&code_pair_a[WORDS_PER_PAGE], leaf_flags);
}

static void enable_sv32(void)
{
    const uintptr_t root_ppn = (uintptr_t)root_pt >> 12;
    const uint32_t satp = SATP_MODE_SV32 | (uint32_t)root_ppn;
    asm volatile("csrw satp, %0" : : "r"(satp) : "memory");
}

static void enter_supervisor_mode(void)
{
    uintptr_t mstatus;

    /*
     * S-mode is the architectural mode where satp translation is active for
     * instruction fetch. This keeps the cross-page JIT test valid after M-mode
     * fetches correctly ignore satp.
     */
    asm volatile(
        "csrr %[mstatus], mstatus\n"
        "li t0, %[mpp_mask]\n"
        "not t0, t0\n"
        "and %[mstatus], %[mstatus], t0\n"
        "li t0, %[mpp_s]\n"
        "or %[mstatus], %[mstatus], t0\n"
        "ori %[mstatus], %[mstatus], %[mpie]\n"
        "csrw mstatus, %[mstatus]\n"
        "la t0, 1f\n"
        "csrw mepc, t0\n"
        "mret\n"
        "1:\n"
        : [mstatus] "=&r"(mstatus)
        : [mpp_mask] "i"(MSTATUS_MPP_MASK),
          [mpp_s] "i"(MSTATUS_MPP_S),
          [mpie] "i"(MSTATUS_MPIE)
        : "t0", "memory");
}

static void prepare_generated_code(void)
{
    /*
   * First mapping:
   *   page 1 last word:  a0 = 7
   *   page 2 word 0:     a0 += 1
   *   page 2 word 1:     ret
   *
   * Remapped second page:
   *   page 2 word 0:     a0 += 2
   *   page 2 word 1:     ret
   *
   * The first instruction remains mapped to the same physical byte, so a cache
   * hit that validates only the block's first instruction would incorrectly keep
   * returning 8 after the second page is remapped.
   */
    code_pair_a[WORDS_PER_PAGE - 1u] = addi_a0_zero_imm(7);
    code_pair_a[WORDS_PER_PAGE] = addi_a0_a0_imm(1);
    code_pair_a[WORDS_PER_PAGE + 1u] = 0x00008067u;

    code_second_b[0] = addi_a0_a0_imm(2);
    code_second_b[1] = 0x00008067u;
}

int main()
{
    prepare_generated_code();
    install_page_tables();
    enable_sv32();
    enter_supervisor_mode();

    generated_fn_t fn = (generated_fn_t)(uintptr_t)ALIAS_ENTRY;

    const int first = fn();
    check(first == 8);

    alias_l0[((ALIAS_BASE >> 12) & 0x3ffu) + 1u] =
        pte_for_page(code_second_b, PTE_V | PTE_R | PTE_W | PTE_X | PTE_A | PTE_D);

    const int second = fn();
    check(second == 9);

    return 0;
}
