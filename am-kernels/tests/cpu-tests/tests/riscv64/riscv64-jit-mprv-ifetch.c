#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 64

#include <stdint.h>

typedef int (*generated_fn_t)(void);

/*
 * MPRV changes explicit load/store privilege, but it must not change
 * instruction-fetch identity.  The first call compiles a direct M-mode block at
 * code_page_m while MPRV makes data accesses look like S-mode.  The second call
 * uses the same numeric PC in real S-mode, where Sv39 maps it to code_page_s.
 * A block cache keyed only by data privilege can wrongly reuse the direct block.
 */
#define PAGE_SIZE 4096ull
#define WORDS_PER_PAGE (PAGE_SIZE / sizeof(uint32_t))

#define PTE_V 0x001ull
#define PTE_R 0x002ull
#define PTE_W 0x004ull
#define PTE_X 0x008ull
#define PTE_A 0x040ull
#define PTE_D 0x080ull

#define SATP_MODE_SV39 (8ull << 60)
#define MSTATUS_MPRV (1ull << 17)
#define MSTATUS_MPP_MASK (3ull << 11)
#define MSTATUS_MPP_MPIE_MASK (MSTATUS_MPP_MASK | (1ull << 7))
#define MSTATUS_MPP_S (1ull << 11)

#define IDENTITY_BASE 0x80000000ull
#define IDENTITY_PAGES 32768ull
#define IDENTITY_L1_ENTRIES (IDENTITY_PAGES / 512ull)

static uint64_t root_pt[512] __attribute__((aligned(PAGE_SIZE)));
static uint64_t identity_l1[512] __attribute__((aligned(PAGE_SIZE)));
static uint64_t identity_l0[IDENTITY_L1_ENTRIES][512] __attribute__((aligned(PAGE_SIZE)));
static uint32_t code_page_m[WORDS_PER_PAGE] __attribute__((aligned(PAGE_SIZE)));
static uint32_t code_page_s[WORDS_PER_PAGE] __attribute__((aligned(PAGE_SIZE)));

asm(".globl rv64_jit_mprv_ifetch_unexpected_trap\n"
    "rv64_jit_mprv_ifetch_unexpected_trap:\n"
    "  csrr a0, mcause\n"
    "  addi a0, a0, 16\n"
    "  .word 0x0000006b\n");

extern void rv64_jit_mprv_ifetch_unexpected_trap(void);

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

static void map_code_page_m_va_to_s_page(void)
{
    const uintptr_t va = (uintptr_t)code_page_m;
    const uint64_t l1 = vpn1(va) - vpn1(IDENTITY_BASE);

    identity_l0[l1][vpn0(va)] = pte_for_page(code_page_s, PTE_V | PTE_R | PTE_X | PTE_A);
}

static void install_page_tables(void)
{
    clear_page_table(root_pt);
    clear_page_table(identity_l1);
    for (uint64_t i = 0; i < IDENTITY_L1_ENTRIES; i++)
    {
        clear_page_table(identity_l0[i]);
    }

    map_identity_window();
    map_code_page_m_va_to_s_page();
    root_pt[vpn2(IDENTITY_BASE)] = pte_for_page(identity_l1, PTE_V);
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
                 : "r"(rv64_jit_mprv_ifetch_unexpected_trap)
                 : "memory");
}

static void set_mprv_data_as_supervisor(void)
{
    uintptr_t mstatus;

    asm volatile(
        "csrr %[mstatus], mstatus\n"
        "li t0, %[mpp_mask]\n"
        "not t0, t0\n"
        "and %[mstatus], %[mstatus], t0\n"
        "li t0, %[mprv_mpp_s]\n"
        "or %[mstatus], %[mstatus], t0\n"
        "csrw mstatus, %[mstatus]\n"
        : [mstatus] "=&r"(mstatus)
        : [mpp_mask] "i"(MSTATUS_MPP_MASK),
          [mprv_mpp_s] "i"(MSTATUS_MPRV | MSTATUS_MPP_S)
        : "t0", "memory");
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
    code_page_m[0] = addi_a0_zero_imm(7);
    code_page_m[1] = 0x00008067u; /* ret is jalr zero, 0(ra). */
    code_page_s[0] = addi_a0_zero_imm(9);
    code_page_s[1] = 0x00008067u; /* ret is jalr zero, 0(ra). */
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

    generated_fn_t fn = (generated_fn_t)(uintptr_t)code_page_m;

    set_mprv_data_as_supervisor();
    const int m_mode_result = fn();
    check(m_mode_result == 7);

    enter_supervisor_mode();
    const int s_mode_result = fn();
    check(s_mode_result == 9);
#endif

    return 0;
}
