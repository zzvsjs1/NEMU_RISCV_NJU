#include <isa.h>
#include <memory/paddr.h>
#include <memory/vaddr.h>

#define PTE_V ((word_t)1u << 0)
#define PTE_R ((word_t)1u << 1)
#define PTE_W ((word_t)1u << 2)
#define PTE_X ((word_t)1u << 3)
#define PTE_U ((word_t)1u << 4)
#define PTE_A ((word_t)1u << 6)
#define PTE_D ((word_t)1u << 7)
#define PTE_RWX (PTE_R | PTE_W | PTE_X)
#define PTE_PPN_SHIFT 10u

#define MSTATUS_MPRV ((word_t)1u << 17)
#define MSTATUS_SUM ((word_t)1u << 18)
#define MSTATUS_MXR ((word_t)1u << 19)
#define MSTATUS_MPP_SHIFT 11u
#define MSTATUS_MPP_MASK ((word_t)0x3u << MSTATUS_MPP_SHIFT)

#ifdef CONFIG_RV64
#define SATP_MODE_SHIFT 60u
#define SATP_MODE_MASK ((word_t)0xfu << SATP_MODE_SHIFT)
#define SATP_MODE_SV39 8u
#define SATP_PPN_MASK (((word_t)1u << 44) - 1u)
#define PTE_PPN_MASK (((word_t)1u << 44) - 1u)
#define PTE_NON_LEAF_RESERVED (PTE_U | PTE_A | PTE_D)
/*
 * This NEMU target does not implement Svnapot or Svpbmt, and the remaining
 * Sv39 PTE bits [60:54] are still reserved by the privileged spec.
 */
#define PTE_RESERVED_63_54_MASK (((word_t)0x3ffu) << 54)
#else
#define SATP_MODE_MASK 0x80000000u
#define SATP_PPN_MASK 0x003fffffu
#endif

/*
 * The vaddr layer handles one page at a time.  A zero/negative length or an
 * access that runs past PAGE_SIZE is reported specially so the caller can split
 * or reject it before this page-walk code computes a physical address.
 */
static bool is_cross_page(vaddr_t vaddr, int len)
{
    const word_t off = (word_t)(vaddr & PAGE_MASK);
    return len <= 0 || off + (word_t)len > PAGE_SIZE;
}

/*
 * Determine the privilege used for data access checks.  Instruction fetch uses
 * current privilege directly; loads/stores in M-mode can instead use MPP when
 * mstatus.MPRV is set.
 */
static word_t effective_mem_priv(int type)
{
    if (type == MEM_TYPE_IFETCH)
    {
        return cpu.prvi;
    }

    if (cpu.prvi == RISCV_PRIV_M && (cpu.csr.mstatus & MSTATUS_MPRV) != 0)
    {
        return (cpu.csr.mstatus & MSTATUS_MPP_MASK) >> MSTATUS_MPP_SHIFT;
    }

    return cpu.prvi;
}

#ifdef CONFIG_RV64

/* Extract satp.MODE from the RV64 satp layout. */
static word_t satp_mode(word_t satp)
{
    return (satp & SATP_MODE_MASK) >> SATP_MODE_SHIFT;
}

/*
 * Sv39 translation applies only when satp selects Sv39 and the effective
 * privilege is below M-mode.  M-mode without MPRV remains physical/direct.
 */
static bool sv39_active_for_access(int type)
{
    return satp_mode(cpu.csr.satp) == SATP_MODE_SV39 &&
           effective_mem_priv(type) != RISCV_PRIV_M;
}

/*
 * Sv39 virtual addresses are canonical: bits [63:39] must all copy bit 38.
 * Rejecting non-canonical addresses here produces a page-fault-style failure
 * before any page table memory is read.
 */
static bool is_sv39_canonical(vaddr_t vaddr)
{
    const uint64_t sign = ((uint64_t)vaddr >> 38) & 1u;
    const uint64_t high = (uint64_t)vaddr >> 39;

    return sign ? high == ((1ull << 25) - 1ull) : high == 0;
}

/*
 * Validate PTE bits common to leaf and non-leaf entries.  W without R is
 * reserved, V must be set, and unsupported high extension bits must be zero.
 */
static bool pte_is_valid(word_t pte)
{
    return (pte & PTE_V) != 0 &&
           (pte & (PTE_R | PTE_W)) != PTE_W &&
           (pte & PTE_RESERVED_63_54_MASK) == 0;
}

/* A PTE with any R/W/X bit set is a leaf mapping; otherwise it points lower. */
static bool pte_is_leaf(word_t pte)
{
    return (pte & PTE_RWX) != 0;
}

/* Extract the physical page number from a Sv39 PTE. */
static word_t pte_ppn(word_t pte)
{
    return (pte >> PTE_PPN_SHIFT) & PTE_PPN_MASK;
}

/*
 * Superpage leaves must have zero lower PPN fields that would be replaced by
 * VPN bits.  Level 2 is a 1 GiB page; level 1 is a 2 MiB page.
 */
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

/*
 * Check privilege against the PTE_U bit and mstatus.SUM.  S-mode may access
 * user pages for data only when SUM is set; instruction fetch from user pages
 * remains forbidden for S-mode.
 */
static bool pte_allows_priv(word_t pte, word_t priv, int type)
{
    const bool user_page = (pte & PTE_U) != 0;

    if (priv == RISCV_PRIV_U)
    {
        return user_page;
    }

    if (priv == RISCV_PRIV_S)
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

/*
 * Check access permissions after privilege is known.  A must be set for all
 * accesses, D must also be set for writes, and MXR lets reads treat executable
 * pages as readable.
 */
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

/*
 * Build the physical page base for a leaf PTE.  For superpages, low physical
 * PPN fields come from the virtual page number, which is why vpn[] is needed
 * even after the leaf has been found.
 */
static paddr_t leaf_page_base(word_t ppn, const word_t vpn[3], int level)
{
    word_t pa_ppn = ppn;

    /*
     * For a legal superpage leaf, the lower physical PPN fields come from the
     * virtual page number.  The vaddr layer adds the original page offset.
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

/*
 * Fast pre-check used by the memory layer.  It decides whether translation is
 * needed at all, leaving detailed PTE permission checks to isa_mmu_translate().
 */
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

    return effective_mem_priv(type) == RISCV_PRIV_M ? MMU_DIRECT : MMU_TRANSLATE;
}

/*
 * Walk an Sv39 page table from level 2 down to level 0.  On success, the return
 * value is the physical page base tagged with MEM_RET_OK; failures return a
 * MEM_RET_* sentinel so the vaddr layer can raise the correct fault.
 */
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

#else

/*
 * Sv32 translation applies only when satp enables paging and the effective
 * privilege is S/U.  M-mode without MPRV remains direct.
 */
static bool sv32_active_for_access(int type)
{
    if ((cpu.csr.satp & SATP_MODE_MASK) == 0)
    {
        return false;
    }

    return effective_mem_priv(type) != RISCV_PRIV_M;
}

/*
 * Fast RV32 check: decide direct versus translated access.  The detailed Sv32
 * walk below still validates PTE presence and permission bits.
 */
int isa_mmu_check(vaddr_t vaddr, int len, int type)
{
    (void)vaddr;
    (void)len;

    /*
     * Sv32 is active only when satp selects translation and the effective
     * privilege for this access is S/U.  Detailed access checks are deferred to
     * isa_mmu_translate(), matching the previous RV32 path.
     */
    return sv32_active_for_access(type) ? MMU_TRANSLATE : MMU_DIRECT;
}

/*
 * Walk the two-level Sv32 page table for a normal 4 KiB page.  This model keeps
 * superpages unsupported, so a successful level-1 entry must be a non-leaf that
 * points at the level-0 table.
 */
paddr_t isa_mmu_translate(vaddr_t vaddr, int len, int type)
{
    if (is_cross_page(vaddr, len))
    {
        return (paddr_t)MEM_RET_CROSS_PAGE;
    }

    const rtlreg_t satp = cpu.csr.satp;
    Assert(sv32_active_for_access(type), "Not in memory protection mode!");

    const paddr_t root = (paddr_t)((satp & SATP_PPN_MASK) * PAGE_SIZE);
    const word_t vpn1 = (word_t)((vaddr >> 22) & 0x3ffu);
    const word_t vpn0 = (word_t)((vaddr >> 12) & 0x3ffu);

    const paddr_t pte1_addr = root + (paddr_t)(vpn1 * 4u);
    const uint32_t pte1 = (uint32_t)paddr_read(pte1_addr, 4);

    Assert((pte1 & PTE_V) != 0, "Not a valid pte at %u", (word_t)pte1_addr);

    const uint32_t pte1_rwx = pte1 & PTE_RWX;
    /*
     * Superpages are deliberately not implemented here.  Requiring a non-leaf
     * level-1 PTE keeps every successful translation on a normal 4 KiB leaf.
     */
    Assert(pte1_rwx == 0, "super page");

    const paddr_t l0_pt = (paddr_t)(((paddr_t)(pte1 >> PTE_PPN_SHIFT)) * PAGE_SIZE);
    const paddr_t pte0_addr = l0_pt + (paddr_t)(vpn0 * 4u);
    const uint32_t pte0 = (uint32_t)paddr_read(pte0_addr, 4);

    Assert((pte0 & PTE_V) != 0, "PTE 0 invalid");

    const uint32_t pte0_rwx = pte0 & PTE_RWX;
    assert(pte0_rwx != 0);

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

    const paddr_t pg_paddr = (paddr_t)(((paddr_t)(pte0 >> PTE_PPN_SHIFT)) << PAGE_SHIFT);
    return pg_paddr | (paddr_t)MEM_RET_OK;
}

#endif
