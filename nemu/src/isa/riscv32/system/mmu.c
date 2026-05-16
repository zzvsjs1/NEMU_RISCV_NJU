#include "common.h"
#include "debug.h"
#include <isa.h>
#include <memory/paddr.h>
#include <memory/vaddr.h>
#include <stdbool.h>

/* Sv32 PTE bits */
#define PTE_V 0x001u
#define PTE_R 0x002u
#define PTE_W 0x004u
#define PTE_X 0x008u

/* satp fields for Sv32 */
#define SATP_MODE_MASK 0x80000000u
#define SATP_PPN_MASK 0x003FFFFFu // low 22 bits

#define MSTATUS_MPRV ((word_t)1u << 17)
#define MSTATUS_MPP_SHIFT 11u
#define MSTATUS_MPP_MASK ((word_t)0x3u << MSTATUS_MPP_SHIFT)

static bool is_cross_page(vaddr_t vaddr, int len)
{
    const word_t off = (word_t)(vaddr & PAGE_MASK);
    return off + (word_t)len > PAGE_SIZE;
}

static word_t riscv32_effective_mem_priv(int type)
{
    if (type == MEM_TYPE_IFETCH)
    {
        return cpu.prvi;
    }

    if (cpu.prvi == RISCV32_PRIV_M && (cpu.csr.mstatus & MSTATUS_MPRV) != 0)
    {
        return (cpu.csr.mstatus & MSTATUS_MPP_MASK) >> MSTATUS_MPP_SHIFT;
    }

    return cpu.prvi;
}

static bool riscv32_translation_active(int type)
{
    if ((cpu.csr.satp & SATP_MODE_MASK) == 0)
    {
        return false;
    }

    return riscv32_effective_mem_priv(type) != RISCV32_PRIV_M;
}

int isa_mmu_check(vaddr_t vaddr, int len, int type)
{
    /*
     * Sv32 is active only when satp selects translation and the effective
     * privilege for this access is S/U.  Detailed access checks are deferred to
     * isa_mmu_translate(), so callers get a cheap direct/translate decision
     * before doing any page-table walks.
     */
    return riscv32_translation_active(type) ? MMU_TRANSLATE : MMU_DIRECT;
}

paddr_t isa_mmu_translate(vaddr_t vaddr, int len, int type)
{
    /*
     * The current memory helpers handle one translated page per access.  Report
     * a cross-page status instead of partially walking two leaves; the vaddr
     * layer owns deciding whether that case is supported.
     */

    if (is_cross_page(vaddr, len))
    {
        return (paddr_t)MEM_RET_CROSS_PAGE;
    }

    const rtlreg_t satp = cpu.csr.satp;
    Assert(riscv32_translation_active(type), "Not in memory protection mode!");

    // root page directory physical address
    // mul by 4096 (<< 12)
    // Let a be satp.ppn × PAGESIZE, and let i=LEVELS - 1. (For Sv32, PAGESIZE=2**12 and LEVELS=2.) The
    // satp register must be active, i.e., the effective privilege mode must be S-mode or U-mode.
    const paddr_t root = (paddr_t)((satp & SATP_PPN_MASK) * PAGE_SIZE);

    /*
     * Sv32 has two 10-bit VPN indexes.  vpn1 selects a level-1 PTE from the root
     * page table in satp.ppn, then vpn0 selects the leaf PTE from that next
     * level page table.
     */
    const word_t vpn1 = (word_t)((vaddr >> 22) & 0x3FFu);
    const word_t vpn0 = (word_t)((vaddr >> 12) & 0x3FFu);

    paddr_t pte1_addr = root + (paddr_t)(vpn1 * 4u);
    uint32_t pte1 = (uint32_t)paddr_read(pte1_addr, 4);

    Assert((pte1 & PTE_V) != 0, "Not a valid pte at %u", (word_t)pte1_addr);

    uint32_t pte1_rwx = pte1 & (PTE_R | PTE_W | PTE_X);
    /*
     * Superpages are deliberately not implemented here.  Requiring a non-leaf
     * level-1 PTE keeps every successful translation on a normal 4 KiB leaf,
     * which matches the packed return format used by vaddr.c.
     */
    Assert(pte1_rwx == 0, "super page");

    // next level page table base
    // ppn * PAGE_SIZE
    paddr_t l0_pt = (paddr_t)(((paddr_t)(pte1 >> 10)) * PAGE_SIZE);

    // level-0 PTE
    paddr_t pte0_addr = l0_pt + (paddr_t)(vpn0 * 4u);
    uint32_t pte0 = (uint32_t)paddr_read(pte0_addr, 4);

    // if (!(pte0 & PTE_V))
    // {
    //     printf("MMU: vaddr=0x%08x len=%d type=%d satp=0x%08x\n",
    //         (uint32_t)vaddr, len, type, (uint32_t)cpu.csr.satp);
    //     printf("MMU: vpn1=%u vpn0=%u root=0x%08x pte1_addr=0x%08x pte1=0x%08x\n",
    //         vpn1, vpn0, (uint32_t)root, (uint32_t)pte1_addr, pte1);
    //     printf("MMU: l0_base=0x%08x pte0_addr=0x%08x pte0=0x%08x\n",
    //         (uint32_t)l0_pt, (uint32_t)pte0_addr, pte0);
    // }

    Assert((pte0 & PTE_V) != 0, "PTE 0 invalid");

    uint32_t pte0_rwx = pte0 & (PTE_R | PTE_W | PTE_X);
    assert(pte0_rwx != 0); // must be leaf at level 0 in our assumption

    if (type == MEM_TYPE_IFETCH)
    {
        assert((pte0 & PTE_X) != 0);
    }
    else if (type == MEM_TYPE_READ)
    {
        assert((pte0 & PTE_R) != 0);
    }
    else if (type == MEM_TYPE_WRITE)
    {
        assert((pte0 & PTE_W) != 0);
    }

    /*
     * Return only the physical page base plus MEM_RET_OK.  The original page
     * offset is ORed in by vaddr_read/write/ifetch after they have checked the
     * status bits.
     */
    const paddr_t pg_paddr = (paddr_t)(((paddr_t)(pte0 >> 10)) << 12);
    return pg_paddr | (paddr_t)MEM_RET_OK;
}
