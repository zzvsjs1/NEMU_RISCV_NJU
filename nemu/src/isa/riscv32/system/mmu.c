#include <isa.h>
#include <memory/paddr.h>
#include <memory/vaddr.h>

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

    if (cpu.prvi == RISCV_PRIV_M && (cpu.csr.mstatus & RISCV_MSTATUS_MPRV) != 0)
    {
        return (cpu.csr.mstatus & RISCV_MSTATUS_MPP_MASK) >> RISCV_MSTATUS_MPP_SHIFT;
    }

    return cpu.prvi;
}

#ifdef CONFIG_RV64

/* Extract satp.MODE from the RV64 satp layout. */
static word_t satp_mode(word_t satp)
{
    return (satp & (word_t)RISCV64_SATP_MODE_MASK) >> RISCV64_SATP_MODE_SHIFT;
}

/*
 * Sv39 translation applies only when satp selects Sv39 and the effective
 * privilege is below M-mode.  M-mode without MPRV remains physical/direct.
 */
static bool sv39_active_for_access(int type)
{
    return satp_mode(cpu.csr.satp) == RISCV64_SATP_MODE_SV39 && effective_mem_priv(type) != RISCV_PRIV_M;
}

/*
 * Sv39 virtual addresses are canonical: bits [63:39] must all copy bit 38.
 * Rejecting non-canonical addresses here produces a page-fault-style failure
 * before any page table memory is read.
 */
static bool is_sv39_canonical(vaddr_t vaddr)
{
    const uint64_t sign = ((uint64_t)vaddr >> RISCV64_SV39_CANONICAL_SIGN_BIT) & 1u;
    const uint64_t high = (uint64_t)vaddr >> RISCV64_SV39_CANONICAL_HIGH_SHIFT;

    return sign ? high == ((UINT64_C(1) << RISCV64_SV39_CANONICAL_HIGH_BITS) - UINT64_C(1)) : high == 0;
}

/*
 * Validate PTE bits common to leaf and non-leaf entries.  W without R is
 * reserved, V must be set, and unsupported high extension bits must be zero.
 */
static bool pte_is_valid(word_t pte)
{
    return (pte & RISCV_PTE_V) != 0 && (pte & (RISCV_PTE_R | RISCV_PTE_W)) != RISCV_PTE_W && (pte & (word_t)RISCV64_PTE_UNSUPPORTED_HIGH_MASK) == 0;
}

/* A PTE with any R/W/X bit set is a leaf mapping; otherwise it points lower. */
static bool pte_is_leaf(word_t pte)
{
    return (pte & RISCV_PTE_RWX) != 0;
}

/* Extract the physical page number from a Sv39 PTE. */
static word_t pte_ppn(word_t pte)
{
    return (pte >> RISCV_PTE_PPN_SHIFT) & (word_t)RISCV64_PTE_PPN_MASK;
}

/*
 * Superpage leaves must have zero lower PPN fields that would be replaced by
 * VPN bits.  Level 2 is a 1 GiB page; level 1 is a 2 MiB page.
 */
static bool superpage_aligned(word_t ppn, int level)
{
    if (level == RISCV64_SV39_ROOT_LEVEL)
    {
        return (ppn & RISCV64_SV39_LEVEL2_LOW_PPN_MASK) == 0;
    }

    if (level == RISCV64_SV39_MEGAPAGE_LEVEL)
    {
        return (ppn & RISCV64_SV39_LEVEL1_LOW_PPN_MASK) == 0;
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
    const bool user_page = (pte & RISCV_PTE_U) != 0;

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
        return type != MEM_TYPE_IFETCH && (cpu.csr.mstatus & RISCV_MSTATUS_SUM) != 0;
    }

    return false;
}

/*
 * Check access permissions after privilege is known.  The architecture permits
 * an implementation either to update A/D in memory or to raise a page fault so
 * software can update them.  This Sv39 walker chooses the latter, Svade-style
 * policy: A must already be set for every access and D must also be set for a
 * write.  MXR separately lets a read treat an executable page as readable.
 */
static bool pte_allows_access(word_t pte, word_t priv, int type)
{
    if (!pte_allows_priv(pte, priv, type) || (pte & RISCV_PTE_A) == 0)
    {
        return false;
    }

    if (type == MEM_TYPE_IFETCH)
    {
        return (pte & RISCV_PTE_X) != 0;
    }

    if (type == MEM_TYPE_READ)
    {
        return (pte & RISCV_PTE_R) != 0 || ((cpu.csr.mstatus & RISCV_MSTATUS_MXR) != 0 && (pte & RISCV_PTE_X) != 0);
    }

    if (type == MEM_TYPE_WRITE)
    {
        return (pte & (RISCV_PTE_W | RISCV_PTE_D)) == (RISCV_PTE_W | RISCV_PTE_D);
    }

    return false;
}

/*
 * Build the physical page base for a leaf PTE.  For superpages, low physical
 * PPN fields come from the virtual page number, which is why vpn[] is needed
 * even after the leaf has been found.
 */
static paddr_t leaf_page_base(word_t ppn, const word_t vpn[RISCV64_SV39_LEVELS], int level)
{
    word_t pa_ppn = ppn;

    /*
     * For a legal superpage leaf, the lower physical PPN fields come from the
     * virtual page number.  The vaddr layer adds the original page offset.
     */
    if (level >= RISCV64_SV39_MEGAPAGE_LEVEL)
    {
        pa_ppn = (pa_ppn & ~RISCV64_SV39_LEVEL1_LOW_PPN_MASK) | vpn[RISCV64_SV39_PAGE_LEVEL];
    }

    if (level >= RISCV64_SV39_GIGAPAGE_LEVEL)
    {
        pa_ppn =
            (pa_ppn & ~RISCV64_SV39_LEVEL2_LOW_PPN_MASK) | (vpn[RISCV64_SV39_MEGAPAGE_LEVEL] << RISCV64_SV39_VPN_BITS) | vpn[RISCV64_SV39_PAGE_LEVEL];
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

    if (mode == RISCV_SATP_MODE_BARE)
    {
        return MMU_DIRECT;
    }

    if (mode != RISCV64_SATP_MODE_SV39)
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

    /*
     * Starting at the twelve-bit page offset, Sv39 divides the virtual page
     * number into three adjacent nine-bit indices.  Expressing each shift as
     * PAGE_SHIFT + level * VPN_BITS keeps the array order identical to the
     * page-table level number used by the descending walk below.
     */
    const word_t vpn[RISCV64_SV39_LEVELS] = {
        ((word_t)vaddr >> (PAGE_SHIFT + RISCV64_SV39_PAGE_LEVEL * RISCV64_SV39_VPN_BITS)) & RISCV64_SV39_VPN_MASK,
        ((word_t)vaddr >> (PAGE_SHIFT + RISCV64_SV39_MEGAPAGE_LEVEL * RISCV64_SV39_VPN_BITS)) & RISCV64_SV39_VPN_MASK,
        ((word_t)vaddr >> (PAGE_SHIFT + RISCV64_SV39_GIGAPAGE_LEVEL * RISCV64_SV39_VPN_BITS)) & RISCV64_SV39_VPN_MASK,
    };
    const word_t priv = effective_mem_priv(type);
    paddr_t pt_base = (paddr_t)((cpu.csr.satp & (word_t)RISCV64_SATP_PPN_MASK) << PAGE_SHIFT);

    for (int level = RISCV64_SV39_ROOT_LEVEL; level >= RISCV64_SV39_PAGE_LEVEL; --level)
    {
        const paddr_t pte_addr = pt_base + (paddr_t)(vpn[level] * RISCV64_SV39_PTE_BYTES);
        const word_t pte = (word_t)paddr_read(pte_addr, RISCV64_SV39_PTE_BYTES);

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

        if (level == RISCV64_SV39_PAGE_LEVEL || (pte & RISCV_PTE_NON_LEAF_RESERVED) != 0)
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
    if ((cpu.csr.satp & RISCV32_SATP_MODE_MASK) == RISCV_SATP_MODE_BARE)
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

    /*
     * Sv32 places a twelve-bit page offset below two ten-bit VPN fields.
     * vpn[0] begins at PAGE_SHIFT and vpn[1] begins one VPN field above it.
     * Multiplying an index by the four-byte PTE size selects its entry without
     * embedding `22`, `0x3ff`, or `4` independently in the walk.
     */
    const paddr_t root = (paddr_t)((satp & RISCV32_SATP_PPN_MASK) * PAGE_SIZE);
    const word_t vpn1 = (word_t)((vaddr >> (PAGE_SHIFT + RISCV32_SV32_ROOT_LEVEL * RISCV32_SV32_VPN_BITS)) & RISCV32_SV32_VPN_MASK);
    const word_t vpn0 = (word_t)((vaddr >> PAGE_SHIFT) & RISCV32_SV32_VPN_MASK);

    const paddr_t pte1_addr = root + (paddr_t)(vpn1 * RISCV32_SV32_PTE_BYTES);
    const uint32_t pte1 = (uint32_t)paddr_read(pte1_addr, RISCV32_SV32_PTE_BYTES);

    Assert((pte1 & RISCV_PTE_V) != 0, "Not a valid pte at %u", (word_t)pte1_addr);

    const uint32_t pte1_rwx = pte1 & RISCV_PTE_RWX;
    /*
     * Superpages are deliberately not implemented here.  Requiring a non-leaf
     * level-1 PTE keeps every successful translation on a normal 4 KiB leaf.
     */
    Assert(pte1_rwx == 0, "super page");

    const paddr_t l0_pt = (paddr_t)(((paddr_t)(pte1 >> RISCV_PTE_PPN_SHIFT)) * PAGE_SIZE);
    const paddr_t pte0_addr = l0_pt + (paddr_t)(vpn0 * RISCV32_SV32_PTE_BYTES);
    const uint32_t pte0 = (uint32_t)paddr_read(pte0_addr, RISCV32_SV32_PTE_BYTES);

    Assert((pte0 & RISCV_PTE_V) != 0, "PTE 0 invalid");

    const uint32_t pte0_rwx = pte0 & RISCV_PTE_RWX;
    assert(pte0_rwx != 0);

    if (type == MEM_TYPE_IFETCH)
    {
        assert((pte0 & RISCV_PTE_X) != 0);
    }
    else if (type == MEM_TYPE_READ)
    {
        assert((pte0 & RISCV_PTE_R) != 0);
    }
    else if (type == MEM_TYPE_WRITE)
    {
        assert((pte0 & RISCV_PTE_W) != 0);
    }

    const paddr_t pg_paddr = (paddr_t)(((paddr_t)(pte0 >> RISCV_PTE_PPN_SHIFT)) << PAGE_SHIFT);
    return pg_paddr | (paddr_t)MEM_RET_OK;
}

#endif
