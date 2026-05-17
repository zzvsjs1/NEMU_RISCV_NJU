#include <isa.h>
#include <memory/vaddr.h>
#include <memory/paddr.h>

#define SATP_MODE_SHIFT 60u
#define SATP_MODE_MASK ((word_t)0xfu << SATP_MODE_SHIFT)
#define SATP_MODE_SV39 8u
#define SATP_PPN_MASK (((word_t)1u << 44) - 1u)

#define PTE_V ((word_t)1u << 0)
#define PTE_R ((word_t)1u << 1)
#define PTE_W ((word_t)1u << 2)
#define PTE_X ((word_t)1u << 3)
#define PTE_U ((word_t)1u << 4)
#define PTE_A ((word_t)1u << 6)
#define PTE_D ((word_t)1u << 7)
#define PTE_RWX (PTE_R | PTE_W | PTE_X)
#define PTE_NON_LEAF_RESERVED (PTE_U | PTE_A | PTE_D)

#define PTE_PPN_SHIFT 10u
#define PTE_PPN_MASK (((word_t)1u << 44) - 1u)
/*
 * This NEMU target does not implement Svnapot or Svpbmt, and the remaining
 * Sv39 PTE bits [60:54] are still reserved by the privileged spec.  Step 3 of
 * the translation algorithm requires a page fault if any of these bits are set.
 */
#define PTE_RESERVED_63_54_MASK (((word_t)0x3ffu) << 54)

#define MSTATUS_MPRV ((word_t)1u << 17)
#define MSTATUS_SUM ((word_t)1u << 18)
#define MSTATUS_MXR ((word_t)1u << 19)
#define MSTATUS_MPP_SHIFT 11u
#define MSTATUS_MPP_MASK ((word_t)0x3u << MSTATUS_MPP_SHIFT)

static bool is_cross_page(vaddr_t vaddr, int len)
{
    const word_t off = (word_t)(vaddr & PAGE_MASK);
    return len <= 0 || off + (word_t)len > PAGE_SIZE;
}

static word_t satp_mode(word_t satp)
{
    return (satp & SATP_MODE_MASK) >> SATP_MODE_SHIFT;
}

static word_t effective_mem_priv(int type)
{
    if (type == MEM_TYPE_IFETCH)
    {
        return cpu.prvi;
    }

    if (cpu.prvi == RISCV64_PRIV_M && (cpu.csr.mstatus & MSTATUS_MPRV) != 0)
    {
        return (cpu.csr.mstatus & MSTATUS_MPP_MASK) >> MSTATUS_MPP_SHIFT;
    }

    return cpu.prvi;
}

static bool sv39_active_for_access(int type)
{
    return satp_mode(cpu.csr.satp) == SATP_MODE_SV39 &&
           effective_mem_priv(type) != RISCV64_PRIV_M;
}

static bool is_sv39_canonical(vaddr_t vaddr)
{
    const uint64_t sign = ((uint64_t)vaddr >> 38) & 1u;
    const uint64_t high = (uint64_t)vaddr >> 39;

    return sign ? high == ((1ull << 25) - 1ull) : high == 0;
}

static bool pte_is_valid(word_t pte)
{
    return (pte & PTE_V) != 0 &&
           (pte & (PTE_R | PTE_W)) != PTE_W &&
           (pte & PTE_RESERVED_63_54_MASK) == 0;
}

static bool pte_is_leaf(word_t pte)
{
    return (pte & PTE_RWX) != 0;
}

static word_t pte_ppn(word_t pte)
{
    return (pte >> PTE_PPN_SHIFT) & PTE_PPN_MASK;
}

static bool superpage_aligned(word_t ppn, int level)
{
    if (level == 2)
    {
        return (ppn & 0x3ffffu) == 0;
    }

    if (level == 1)
    {
        return (ppn & 0x1ffu) == 0;
    }

    return true;
}

static bool pte_allows_priv(word_t pte, word_t priv, int type)
{
    const bool user_page = (pte & PTE_U) != 0;

    if (priv == RISCV64_PRIV_U)
    {
        return user_page;
    }

    if (priv == RISCV64_PRIV_S)
    {
        if (!user_page)
        {
            return true;
        }

        /*
         * SUM lets S-mode load/store user pages.  It never permits S-mode
         * instruction fetch from a user page.
         */
        return type != MEM_TYPE_IFETCH && (cpu.csr.mstatus & MSTATUS_SUM) != 0;
    }

    return false;
}

static bool pte_allows_access(word_t pte, word_t priv, int type)
{
    if (!pte_allows_priv(pte, priv, type) || (pte & PTE_A) == 0)
    {
        return false;
    }

    if (type == MEM_TYPE_IFETCH)
    {
        return (pte & PTE_X) != 0;
    }

    if (type == MEM_TYPE_READ)
    {
        return (pte & PTE_R) != 0 ||
               ((cpu.csr.mstatus & MSTATUS_MXR) != 0 && (pte & PTE_X) != 0);
    }

    if (type == MEM_TYPE_WRITE)
    {
        return (pte & (PTE_W | PTE_D)) == (PTE_W | PTE_D);
    }

    return false;
}

static paddr_t leaf_page_base(word_t ppn, const word_t vpn[3], int level)
{
    word_t pa_ppn = ppn;

    /*
     * For a legal superpage leaf, the lower physical PPN fields come from the
     * virtual page number.  The return value stays 4 KiB-aligned because the
     * vaddr layer adds the original page offset afterwards.
     */
    if (level >= 1)
    {
        pa_ppn = (pa_ppn & ~0x1ffu) | vpn[0];
    }

    if (level >= 2)
    {
        pa_ppn = (pa_ppn & ~0x3ffffu) | (vpn[1] << 9) | vpn[0];
    }

    return (paddr_t)(pa_ppn << PAGE_SHIFT);
}

int isa_mmu_check(vaddr_t vaddr, int len, int type)
{
    (void)vaddr;
    (void)len;

    const word_t mode = satp_mode(cpu.csr.satp);

    if (mode == 0)
    {
        return MMU_DIRECT;
    }

    if (mode != SATP_MODE_SV39)
    {
        return MMU_FAIL;
    }

    return effective_mem_priv(type) == RISCV64_PRIV_M ? MMU_DIRECT : MMU_TRANSLATE;
}

paddr_t isa_mmu_translate(vaddr_t vaddr, int len, int type)
{
    if (!sv39_active_for_access(type) || !is_sv39_canonical(vaddr))
    {
        return (paddr_t)MEM_RET_FAIL;
    }

    if (is_cross_page(vaddr, len))
    {
        return (paddr_t)MEM_RET_CROSS_PAGE;
    }

    const word_t vpn[3] = {
        ((word_t)vaddr >> 12) & 0x1ffu,
        ((word_t)vaddr >> 21) & 0x1ffu,
        ((word_t)vaddr >> 30) & 0x1ffu,
    };
    const word_t priv = effective_mem_priv(type);
    paddr_t pt_base = (paddr_t)((cpu.csr.satp & SATP_PPN_MASK) << PAGE_SHIFT);

    for (int level = 2; level >= 0; --level)
    {
        const paddr_t pte_addr = pt_base + (paddr_t)(vpn[level] * sizeof(uint64_t));
        const word_t pte = (word_t)paddr_read(pte_addr, 8);

        if (!pte_is_valid(pte))
        {
            return (paddr_t)MEM_RET_FAIL;
        }

        const word_t ppn = pte_ppn(pte);

        if (pte_is_leaf(pte))
        {
            if (!superpage_aligned(ppn, level) || !pte_allows_access(pte, priv, type))
            {
                return (paddr_t)MEM_RET_FAIL;
            }

            return leaf_page_base(ppn, vpn, level) | (paddr_t)MEM_RET_OK;
        }

        if (level == 0 || (pte & PTE_NON_LEAF_RESERVED) != 0)
        {
            return (paddr_t)MEM_RET_FAIL;
        }

        pt_base = (paddr_t)(ppn << PAGE_SHIFT);
    }

    return (paddr_t)MEM_RET_FAIL;
}
