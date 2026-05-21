#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 32

#include <stdint.h>

#define PAGE_SIZE 4096u
#define WORDS_PER_PAGE (PAGE_SIZE / sizeof(uint32_t))

#define PTE_V 0x001u
#define PTE_R 0x002u
#define PTE_W 0x004u
#define PTE_X 0x008u
#define PTE_A 0x040u
#define PTE_D 0x080u

#define SATP_MODE_SV32 0x80000000u

#define MSTATUS_MPRV (1u << 17)
#define MSTATUS_MPP_MASK (3u << 11)
#define MSTATUS_MPP_S (1u << 11)

/*
 * Use a virtual address in the next 4 MiB region after the normal test image.
 * In M-mode with MPRV clear, satp must be inactive and this address is a direct
 * physical PMEM access. With MPRV set and MPP=S, the same explicit load must
 * use the Sv32 mapping below.
 */
#define IDENTITY_BASE 0x80000000u
#define IDENTITY_PAGES 64u
#define PROBE_VA 0x80400000u

#define DIRECT_VALUE 0x13579bdfu
#define TRANSLATED_VALUE 0x2468ace0u

static uint32_t root_pt[1024] __attribute__((aligned(PAGE_SIZE)));
static uint32_t identity_l0[1024] __attribute__((aligned(PAGE_SIZE)));
static uint32_t probe_l0[1024] __attribute__((aligned(PAGE_SIZE)));
static uint32_t translated_page[WORDS_PER_PAGE] __attribute__((aligned(PAGE_SIZE)));

static inline uintptr_t read_satp(void)
{
    uintptr_t v;
    asm volatile("csrr %0, satp" : "=r"(v));
    return v;
}

static inline void write_satp(uintptr_t v)
{
    asm volatile("csrw satp, %0" : : "r"(v) : "memory");
}

static inline uintptr_t read_mstatus(void)
{
    uintptr_t v;
    asm volatile("csrr %0, mstatus" : "=r"(v));
    return v;
}

static inline void write_mstatus(uintptr_t v)
{
    asm volatile("csrw mstatus, %0" : : "r"(v) : "memory");
}

static inline uint32_t load_word(uintptr_t addr)
{
    uint32_t v;
    asm volatile("lw %0, 0(%1)" : "=r"(v) : "r"(addr) : "memory");
    return v;
}

static uint32_t pte_for_page(const void *page, uint32_t flags)
{
    const uintptr_t pa = (uintptr_t)page;

    return (uint32_t)((pa >> 12) << 10) | flags;
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

    clear_page_table(root_pt);
    clear_page_table(identity_l0);
    clear_page_table(probe_l0);
    map_identity_window();

    root_pt[IDENTITY_BASE >> 22] = pte_for_page(identity_l0, table_flags);
    root_pt[PROBE_VA >> 22] = pte_for_page(probe_l0, table_flags);
    probe_l0[(PROBE_VA >> 12) & 0x3ffu] =
        pte_for_page(translated_page, leaf_flags);
}

static void enable_sv32(void)
{
    const uintptr_t root_ppn = (uintptr_t)root_pt >> 12;
    const uint32_t satp = SATP_MODE_SV32 | (uint32_t)root_ppn;

    write_satp(satp);
}

static void test_satp_uses_effective_privilege_for_explicit_loads(void)
{
    const uintptr_t old_satp = read_satp();
    const uintptr_t old_mstatus = read_mstatus();
    uintptr_t mstatus_mprv_s = old_mstatus;

    /*
     * The direct physical page and the Sv32 target deliberately hold different
     * words. The observed value tells us whether the load used Bare M-mode
     * addressing or S-mode translation through MPRV.
     */
    *(volatile uint32_t *)(uintptr_t)PROBE_VA = DIRECT_VALUE;
    translated_page[0] = TRANSLATED_VALUE;
    install_page_tables();
    enable_sv32();

    mstatus_mprv_s &= ~MSTATUS_MPP_MASK;
    mstatus_mprv_s |= MSTATUS_MPRV | MSTATUS_MPP_S;
    write_mstatus(mstatus_mprv_s);
    check(load_word(PROBE_VA) == TRANSLATED_VALUE);

    write_mstatus(old_mstatus & ~MSTATUS_MPRV);
    check(load_word(PROBE_VA) == DIRECT_VALUE);

    write_mstatus(old_mstatus);
    write_satp(old_satp);
}

#endif

int main(void)
{
#if defined(__riscv) && __riscv_xlen == 32
    test_satp_uses_effective_privilege_for_explicit_loads();
#endif

    return 0;
}
