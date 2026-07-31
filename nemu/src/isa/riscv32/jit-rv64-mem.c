#include <generated/autoconf.h>

#ifdef CONFIG_RV64

#include "jit-rv64-internal.h"

/*
 * RV64 JIT memory layer: Sv39 translation, data TLBs, instruction-fetch
 * dependency tracking, PMEM source-byte tracking and helper memory operations.
 */
/* Advance the generation that protects translated instruction-fetch mappings. */
void rv64_jit_ifetch_generation_bump(void)
{
    Assert(rv64_jit_ifetch_generation != UINT64_MAX,
           "jit: RV64 ifetch generation overflow");
    rv64_jit_ifetch_generation++;
    JIT_STAT_INC(ifetch_generation_bumps);
}

/* Clear the RV64 JIT data TLB and its page-table dependency refcounts. */
void rv64_jit_data_tlb_flush(void)
{
    /*
     * SFENCE.VMA and page-table writes do not need selective invalidation for
     * this first stage.  The table is small, and a full clear avoids mistakes
     * around ASID, virtual-address operands, and superpage dependency ranges.
     */
    memset(rv64_jit_data_tlb, 0, sizeof(rv64_jit_data_tlb));
    memset(rv64_jit_data_tlb_pt_page_refs, 0, sizeof(rv64_jit_data_tlb_pt_page_refs));
    JIT_STAT_INC(data_tlb_flushes);
}

/* Check that a complete physical byte range is ordinary guest PMEM. */
static bool jit_data_pmem_range(paddr_t addr, uint32_t len)
{
    if (len == 0)
    {
        return false;
    }

    const paddr_t end = addr + (paddr_t)len - 1u;
    return end >= addr && likely(in_pmem(addr) && in_pmem(end));
}

/* Convert a PMEM page base into the dependency-ref array index. */
static bool jit_data_pmem_page_index(paddr_t page, size_t *idx)
{
    const paddr_t base = (paddr_t)CONFIG_MBASE;

    if (page < base || page >= base + (paddr_t)CONFIG_MSIZE)
    {
        return false;
    }

    *idx = (size_t)((page - base) >> PAGE_SHIFT);
    return *idx < RV64_JIT_PMEM_PAGE_COUNT;
}

/* Record that one data-TLB entry depends on a physical page-table page. */
static void jit_data_tlb_ref_page(paddr_t page)
{
    size_t idx = 0;

    if (jit_data_pmem_page_index(page, &idx) &&
        rv64_jit_data_tlb_pt_page_refs[idx] != UINT16_MAX)
    {
        rv64_jit_data_tlb_pt_page_refs[idx]++;
    }
}

/* Record that one translated block depends on an instruction page-table page. */
static void jit_ifetch_ref_page(paddr_t page)
{
    size_t idx = 0;

    if (jit_data_pmem_page_index(page, &idx))
    {
        Assert(rv64_jit_ifetch_pt_page_refs[idx] != UINT32_MAX,
               "jit: RV64 ifetch page-table refcount overflow");
        rv64_jit_ifetch_pt_page_refs[idx]++;
    }
}

/* Drop one translated-block dependency on an instruction page-table page. */
static void jit_ifetch_unref_page(paddr_t page)
{
    size_t idx = 0;

    if (jit_data_pmem_page_index(page, &idx))
    {
        Assert(rv64_jit_ifetch_pt_page_refs[idx] > 0,
               "jit: RV64 ifetch page-table refcount underflow");
        rv64_jit_ifetch_pt_page_refs[idx]--;
    }
}

/* Drop one dependency ref for an overwritten data-TLB entry. */
static void jit_data_tlb_unref_page(paddr_t page)
{
    size_t idx = 0;

    if (jit_data_pmem_page_index(page, &idx) &&
        rv64_jit_data_tlb_pt_page_refs[idx] > 0)
    {
        rv64_jit_data_tlb_pt_page_refs[idx]--;
    }
}

/* Return whether any live data-TLB entry depends on this page-table page. */
static bool jit_data_tlb_refs_page(paddr_t page)
{
    size_t idx = 0;
    return jit_data_pmem_page_index(page, &idx) &&
           rv64_jit_data_tlb_pt_page_refs[idx] != 0;
}

/* Return whether a translated block depends on this page-table page. */
static bool jit_ifetch_refs_page(paddr_t page)
{
    size_t idx = 0;
    return jit_data_pmem_page_index(page, &idx) &&
           rv64_jit_ifetch_pt_page_refs[idx] != 0;
}

/* Remove page-table dependency refs owned by one direct-mapped TLB slot. */
static void jit_data_tlb_unref_entry(rv64_jit_data_tlb_entry_t *entry)
{
    if (!entry->valid)
    {
        return;
    }

    for (uint32_t i = 0; i < entry->pt_page_count; i++)
    {
        jit_data_tlb_unref_page((paddr_t)entry->pt_pages[i]);
    }
}

/* Return whether a PMEM write may have changed a page table used by the TLB. */
bool rv64_jit_write_may_touch_data_tlb_page_table(paddr_t addr, int len)
{
    /*
     * The data TLB is tagged by satp and effective privilege state, but old
     * entries can survive after the guest temporarily leaves an address space.
     * Track dependencies physically, so editing an old root or leaf table page
     * invalidates entries before the guest can switch back to that satp value.
     */
    if (len <= 0)
    {
        return false;
    }

    const paddr_t end = addr + (paddr_t)len - 1u;

    if (end < addr)
    {
        return true;
    }

    for (paddr_t page = addr & ~(paddr_t)PAGE_MASK;
         page <= (end & ~(paddr_t)PAGE_MASK);
         page += PAGE_SIZE)
    {
        if (jit_data_tlb_refs_page(page))
        {
            return true;
        }

        if (page > (paddr_t)-1 - PAGE_SIZE)
        {
            break;
        }
    }

    return false;
}

/* Return whether a PMEM write may have changed an ifetch page table. */
bool rv64_jit_write_may_touch_ifetch_page_table(paddr_t addr, int len)
{
    if (len <= 0)
    {
        return false;
    }

    const paddr_t end = addr + (paddr_t)len - 1u;

    if (end < addr)
    {
        return true;
    }

    for (paddr_t page = addr & ~(paddr_t)PAGE_MASK;
         page <= (end & ~(paddr_t)PAGE_MASK);
         page += PAGE_SIZE)
    {
        if (jit_ifetch_refs_page(page))
        {
            return true;
        }

        if (page > (paddr_t)-1 - PAGE_SIZE)
        {
            break;
        }
    }

    return false;
}

/*
 * Data translation and TLB design.
 *
 * The fast path distinguishes three cases.  M-mode bare accesses are physical
 * and may become direct PMEM loads/stores after alignment and range checks.
 * Sv39 accesses are walked by `jit_translate_pmem()` using the same effective
 * privilege rules as the architecture: MPRV can lower data privilege, SUM/MXR
 * affect supervisor reads, and A/D/U/R/W/X bits must permit the access.  MMIO,
 * faults, non-canonical addresses, cross-page accesses and uncertain reserved
 * encodings all return to the normal vaddr helper.
 *
 * Successful Sv39 PMEM translations can fill a tiny direct-mapped data TLB.
 * Each entry is tagged by `satp`, VPN, access type and the compact permission
 * state.  The entry also remembers page-table pages touched by the walk; stores
 * into those pages flush the data TLB so native code cannot reuse a stale
 * translation after `sfence.vma`-like effects or explicit page-table writes.
 */
/* Return the privilege level that the architecture uses for this data access. */
static word_t jit_data_effective_priv(int type)
{
    if (type != MEM_TYPE_IFETCH &&
        cpu.prvi == RISCV64_PRIV_M &&
        (cpu.csr.mstatus & RV64_JIT_MSTATUS_MPRV) != 0)
    {
        return (cpu.csr.mstatus & RV64_JIT_MSTATUS_MPP_MASK) >>
               RV64_JIT_MSTATUS_MPP_SHIFT;
    }

    return cpu.prvi;
}

/* Compact the permission-relevant state into a TLB tag. */
uint32_t rv64_jit_data_tlb_state(int type)
{
    /*
     * MPRV is folded into the effective privilege.  SUM and MXR stay explicit
     * because they change whether S-mode may access U pages and whether reads
     * may use execute-only PTEs. The low state bits store effective privilege;
     * the named high bits record the relevant mstatus controls.
     */
    uint32_t state =
        (uint32_t)jit_data_effective_priv(type) & RV64_JIT_DATA_TLB_STATE_PRIV_MASK;

    if ((cpu.csr.mstatus & RV64_JIT_MSTATUS_SUM) != 0)
    {
        state |= RV64_JIT_DATA_TLB_STATE_SUM;
    }

    if ((cpu.csr.mstatus & RV64_JIT_MSTATUS_MXR) != 0)
    {
        state |= RV64_JIT_DATA_TLB_STATE_MXR;
    }

    return state;
}

/* Compact the state that changes instruction-fetch translation/protection. */
uint32_t rv64_jit_ifetch_state(void)
{
    /*
     * MPRV, SUM, and MXR are data-access controls.  Instruction fetch only uses
     * the current architectural privilege; satp is already stored separately.
     */
    return (uint32_t)cpu.prvi;
}

/* Return the satp mode field used by RV64 address translation. */
static word_t jit_sv39_satp_mode(word_t satp)
{
    return satp >> RV64_JIT_SATP_MODE_SHIFT;
}

/* Return whether an Sv39 virtual address is canonical. */
static bool jit_sv39_canonical(vaddr_t vaddr)
{
    const uint64_t sign =
        ((uint64_t)vaddr >> RV64_JIT_SV39_CANONICAL_SIGN_BIT) & 1u;
    const uint64_t high =
        (uint64_t)vaddr >> RV64_JIT_SV39_CANONICAL_HIGH_SHIFT;

    return sign
               ? high == ((1ull << RV64_JIT_SV39_CANONICAL_HIGH_BITS) - 1ull)
               : high == 0;
}

/* Return whether an access is zero-length or crosses a 4 KiB translated page. */
static bool jit_sv39_cross_page(vaddr_t addr, uint32_t len)
{
    const word_t off = (word_t)(addr & PAGE_MASK);
    return len == 0 || off + (word_t)len > PAGE_SIZE;
}

/* Validate the Sv39 PTE bits that are illegal before leaf/non-leaf selection. */
static bool jit_sv39_pte_valid(word_t pte)
{
    return (pte & RV64_JIT_PTE_V) != 0 &&
           (pte & (RV64_JIT_PTE_R | RV64_JIT_PTE_W)) != RV64_JIT_PTE_W &&
           (pte & RV64_JIT_PTE_UNSUPPORTED_HIGH_MASK) == 0;
}

/* Return whether an Sv39 PTE is a leaf rather than the next-level pointer. */
static bool jit_sv39_pte_leaf(word_t pte)
{
    return (pte & RV64_JIT_PTE_RWX) != 0;
}

/* Extract the physical page number encoded in an Sv39 PTE. */
static word_t jit_sv39_pte_ppn(word_t pte)
{
    return (pte >> RV64_JIT_PTE_PPN_SHIFT) & RV64_JIT_PTE_PPN_MASK;
}

/* Check the low PPN fields that must be zero for legal Sv39 superpages. */
static bool jit_sv39_superpage_aligned(word_t ppn, int level)
{
    if (level == 2)
    {
        return (ppn & RV64_JIT_SV39_LEVEL2_LOW_PPN_MASK) == 0;
    }

    if (level == 1)
    {
        return (ppn & RV64_JIT_SV39_LEVEL1_LOW_PPN_MASK) == 0;
    }

    return true;
}

/* Return whether the leaf PTE permits the effective privilege to touch it. */
static bool jit_data_pte_allows_priv(word_t pte, word_t priv)
{
    const bool user_page = (pte & RV64_JIT_PTE_U) != 0;

    if (priv == RISCV64_PRIV_U)
    {
        return user_page;
    }

    if (priv == RISCV64_PRIV_S)
    {
        return !user_page || (cpu.csr.mstatus & RV64_JIT_MSTATUS_SUM) != 0;
    }

    return false;
}

/*
 * Compute which data access kinds are legal for this leaf and CPU state.
 *
 * The privileged architecture permits either faulting on clear A/D bits
 * (Svade behaviour) or updating them when the relevant extension is absent.
 * NEMU's current architectural walker faults when A is clear, or when D is
 * clear for a store.  The JIT deliberately mirrors that reference behaviour;
 * it must be changed together with system/mmu.c if NEMU later implements
 * hardware-style A/D updates.
 */
static uint32_t jit_data_leaf_access(word_t pte, word_t priv)
{
    if (!jit_data_pte_allows_priv(pte, priv) ||
        (pte & RV64_JIT_PTE_A) == 0)
    {
        return 0;
    }

    uint32_t access = 0;

    if ((pte & RV64_JIT_PTE_R) != 0 ||
        ((cpu.csr.mstatus & RV64_JIT_MSTATUS_MXR) != 0 &&
         (pte & RV64_JIT_PTE_X) != 0))
    {
        access |= RV64_JIT_DATA_TLB_READ;
    }

    if ((pte & (RV64_JIT_PTE_W | RV64_JIT_PTE_D)) ==
        (RV64_JIT_PTE_W | RV64_JIT_PTE_D))
    {
        access |= RV64_JIT_DATA_TLB_WRITE;
    }

    return access;
}

/* Combine a leaf PPN with lower VPN fields for 1 GiB/2 MiB Sv39 leaves. */
static paddr_t jit_sv39_leaf_page_base(word_t ppn,
                                       const word_t vpn[RV64_JIT_SV39_LEVELS],
                                       int level)
{
    word_t pa_ppn = ppn;

    if (level >= 1)
    {
        pa_ppn = (pa_ppn & ~RV64_JIT_SV39_LEVEL1_LOW_PPN_MASK) | vpn[0];
    }

    if (level >= 2)
    {
        pa_ppn = (pa_ppn & ~RV64_JIT_SV39_LEVEL2_LOW_PPN_MASK) |
                 (vpn[1] << RV64_JIT_SV39_VPN_BITS) | vpn[0];
    }

    return (paddr_t)(pa_ppn << PAGE_SHIFT);
}

/* Map an access type to the access bit stored in a JIT data-TLB entry. */
static uint32_t jit_data_tlb_need(int type)
{
    if (type == MEM_TYPE_READ)
    {
        return RV64_JIT_DATA_TLB_READ;
    }

    if (type == MEM_TYPE_WRITE)
    {
        return RV64_JIT_DATA_TLB_WRITE;
    }

    return 0;
}

/* Hash a 4 KiB virtual page and translation state into the direct-mapped TLB. */
static uint32_t jit_data_tlb_index(uint64_t vpn, word_t satp, uint32_t state)
{
    /*
     * The low VPN bits give locality, while shifted VPN/satp bits reduce simple
     * collisions between neighbouring pages and reused address spaces.
     */
    return (uint32_t)((vpn ^ (vpn >> RV64_JIT_DATA_TLB_VPN_MIX_SHIFT) ^
                       satp ^ (satp >> RV64_JIT_DATA_TLB_SATP_MIX_SHIFT) ^
                       state) &
                      (RV64_JIT_DATA_TLB_SIZE - 1u));
}

/* Fill or hit the RV64/Sv39 data TLB for ordinary translated PMEM accesses. */
static bool jit_translate_pmem(vaddr_t addr, uint32_t len, int type, paddr_t *paddr)
{
    const word_t satp = cpu.csr.satp;
    const word_t mode = jit_sv39_satp_mode(satp);
    const word_t priv = jit_data_effective_priv(type);

    if (mode == RV64_JIT_SATP_MODE_BARE)
    {
        const paddr_t direct = (paddr_t)addr;

        if (!jit_data_pmem_range(direct, len))
        {
            return false;
        }

        *paddr = direct;
        return true;
    }

    if (mode != RV64_JIT_SATP_MODE_SV39)
    {
        return false;
    }

    if (priv == RISCV64_PRIV_M)
    {
        const paddr_t direct = (paddr_t)addr;

        if (!jit_data_pmem_range(direct, len))
        {
            return false;
        }

        *paddr = direct;
        return true;
    }

    if (!jit_sv39_canonical(addr) || jit_sv39_cross_page(addr, len))
    {
        return false;
    }

    const uint32_t need = jit_data_tlb_need(type);

    if (need == 0)
    {
        return false;
    }

    const uint64_t vpn_tag = (uint64_t)addr >> PAGE_SHIFT;
    const uint32_t state = rv64_jit_data_tlb_state(type);
    const uint32_t idx = jit_data_tlb_index(vpn_tag, satp, state);
    rv64_jit_data_tlb_entry_t *entry = &rv64_jit_data_tlb[idx];

    if (likely(entry->valid &&
               entry->satp == satp &&
               entry->vpn == vpn_tag &&
               entry->state == state &&
               (entry->access & need) != 0))
    {
        const paddr_t translated =
            (paddr_t)entry->pg_paddr | (paddr_t)(addr & PAGE_MASK);

        if (!jit_data_pmem_range(translated, len))
        {
            return false;
        }

        JIT_STAT_INC(data_tlb_hits);
        *paddr = translated;
        return true;
    }

    JIT_STAT_INC(data_tlb_misses);

    const word_t vpn[RV64_JIT_SV39_LEVELS] = {
        ((word_t)addr >> RV64_JIT_SV39_VPN_SHIFT(0)) &
            RV64_JIT_SV39_VPN_MASK,
        ((word_t)addr >> RV64_JIT_SV39_VPN_SHIFT(1)) &
            RV64_JIT_SV39_VPN_MASK,
        ((word_t)addr >> RV64_JIT_SV39_VPN_SHIFT(2)) &
            RV64_JIT_SV39_VPN_MASK,
    };
    paddr_t pt_base = (paddr_t)((satp & RV64_JIT_SATP_PPN_MASK) << PAGE_SHIFT);
    paddr_t pt_pages[RV64_JIT_SV39_LEVELS] = {0};
    uint8_t pt_page_count = 0;

    for (int level = (int)RV64_JIT_SV39_LEVELS - 1; level >= 0; --level)
    {
        const paddr_t pte_addr =
            pt_base + (paddr_t)(vpn[level] * RV64_JIT_PTE_SIZE);

        if (!jit_data_pmem_range(pte_addr, RV64_JIT_PTE_SIZE))
        {
            return false;
        }

        pt_pages[pt_page_count++] = pt_base;
        const word_t pte = (word_t)paddr_read(pte_addr, RV64_JIT_PTE_SIZE);

        if (!jit_sv39_pte_valid(pte))
        {
            return false;
        }

        const word_t ppn = jit_sv39_pte_ppn(pte);

        if (jit_sv39_pte_leaf(pte))
        {
            if (!jit_sv39_superpage_aligned(ppn, level))
            {
                return false;
            }

            const uint32_t access = jit_data_leaf_access(pte, priv);

            if ((access & need) == 0)
            {
                return false;
            }

            const paddr_t pg_paddr = jit_sv39_leaf_page_base(ppn, vpn, level);
            const paddr_t translated = pg_paddr | (paddr_t)(addr & PAGE_MASK);

            if (!jit_data_pmem_range(translated, len))
            {
                return false;
            }

            jit_data_tlb_unref_entry(entry);
            *entry = (rv64_jit_data_tlb_entry_t){
                .satp = satp,
                .vpn = vpn_tag,
                .state = state,
                .access = access,
                .pg_paddr = pg_paddr,
                .pt_page_count = pt_page_count,
                .valid = true,
            };

            for (uint32_t i = 0; i < pt_page_count; i++)
            {
                entry->pt_pages[i] = pt_pages[i];
                jit_data_tlb_ref_page(pt_pages[i]);
            }

            JIT_STAT_INC(data_tlb_fills);
            *paddr = translated;
            return true;
        }

        if (level == 0 || (pte & RV64_JIT_PTE_NON_LEAF_RESERVED) != 0)
        {
            return false;
        }

        pt_base = (paddr_t)(ppn << PAGE_SHIFT);
    }

    return false;
}

/*
 * Warm one JIT data-TLB entry without performing the guest memory operation.
 *
 * Native FP-memory slow edges use this only before returning to the dispatcher
 * at the unexecuted instruction. A valid ordinary-PMEM translation may
 * therefore be reused by the retrying native entry, while MMIO, faults,
 * non-canonical addresses, and cross-page accesses remain completely owned by
 * the architectural fallback path.
 */
bool rv64_jit_data_tlb_probe_read(vaddr_t addr, uint32_t len)
{
    paddr_t ignored = 0;
    return jit_translate_pmem(addr, len, MEM_TYPE_READ, &ignored);
}

bool rv64_jit_data_tlb_probe_write(vaddr_t addr, uint32_t len)
{
    paddr_t ignored = 0;
    return jit_translate_pmem(addr, len, MEM_TYPE_WRITE, &ignored);
}

/* Forward declaration: store helpers need source-chunk state defined below. */
bool rv64_jit_write_may_touch_source_chunk(paddr_t addr, int len);

/* Shared RV64 load helper that delegates translation and faults to vaddr_read(). */
static uint64_t jit_load_vaddr_raw(vaddr_t addr, uint32_t len)
{
    /*
     * The JIT data TLB only accepts cases where a strict Sv39 walk proves that
     * the final physical byte range is ordinary PMEM.  MMIO, faulting,
     * cross-page, and otherwise ambiguous accesses fall back to vaddr_read(),
     * which remains the architectural reference for visible failure behaviour.
     */
    paddr_t paddr = 0;

    JIT_STAT_INC(helper_load_count);

    if (jit_translate_pmem(addr, len, MEM_TYPE_READ, &paddr))
    {
        JIT_STAT_INC(data_tlb_direct_loads);
        return (uint64_t)host_read(guest_to_host(paddr), (int)len);
    }

    return (uint64_t)vaddr_read(addr, (int)len);
}

/*
 * Read an aligned Bare-mode address which generated code has already proved is
 * outside PMEM. Enter at the physical layer to avoid repeating translation and
 * virtual-address checks, while retaining MMIO routing, callbacks, tracing,
 * bounds checks, and invalid-address behaviour.
 */
static uint64_t jit_load_paddr_raw(paddr_t addr, uint32_t len)
{
    JIT_STAT_INC(helper_load_count);
    JIT_STAT_INC(bare_mmio_load_calls);
    return (uint64_t)paddr_read(addr, (int)len);
}

/* Load one signed byte and sign-extend it to RV64 XLEN. */
uint64_t rv64_jit_load_i8(vaddr_t addr)
{
    return (uint64_t)(int64_t)(int8_t)jit_load_vaddr_raw(addr, 1);
}

/* Load one signed halfword and sign-extend it to RV64 XLEN. */
uint64_t rv64_jit_load_i16(vaddr_t addr)
{
    return (uint64_t)(int64_t)(int16_t)jit_load_vaddr_raw(addr, 2);
}

/* Load one signed word and sign-extend it to RV64 XLEN. */
uint64_t rv64_jit_load_i32(vaddr_t addr)
{
    return (uint64_t)(int64_t)(int32_t)jit_load_vaddr_raw(addr, 4);
}

/* Load one doubleword; RV64 LD already produces a full-width value. */
uint64_t rv64_jit_load_u64(vaddr_t addr)
{
    return jit_load_vaddr_raw(addr, 8);
}

/* Load one unsigned byte and zero-extend it to RV64 XLEN. */
uint64_t rv64_jit_load_u8(vaddr_t addr)
{
    return jit_load_vaddr_raw(addr, 1) & 0xffu;
}

/* Load one unsigned halfword and zero-extend it to RV64 XLEN. */
uint64_t rv64_jit_load_u16(vaddr_t addr)
{
    return jit_load_vaddr_raw(addr, 2) & 0xffffu;
}

/* Load one unsigned word and zero-extend it to RV64 XLEN. */
uint64_t rv64_jit_load_u32(vaddr_t addr)
{
    return jit_load_vaddr_raw(addr, 4) & UINT32_MAX;
}

/* Load one signed Bare-mode MMIO byte and sign-extend it to RV64 XLEN. */
uint64_t rv64_jit_load_bare_i8(paddr_t addr)
{
    return (uint64_t)(int64_t)(int8_t)jit_load_paddr_raw(addr, 1);
}

/* Load one signed Bare-mode MMIO halfword and sign-extend it to RV64 XLEN. */
uint64_t rv64_jit_load_bare_i16(paddr_t addr)
{
    return (uint64_t)(int64_t)(int16_t)jit_load_paddr_raw(addr, 2);
}

/* Load one signed Bare-mode MMIO word and sign-extend it to RV64 XLEN. */
uint64_t rv64_jit_load_bare_i32(paddr_t addr)
{
    return (uint64_t)(int64_t)(int32_t)jit_load_paddr_raw(addr, 4);
}

/* Load one full-width Bare-mode MMIO doubleword. */
uint64_t rv64_jit_load_bare_u64(paddr_t addr)
{
    return jit_load_paddr_raw(addr, 8);
}

/* Load one unsigned Bare-mode MMIO byte and zero-extend it to RV64 XLEN. */
uint64_t rv64_jit_load_bare_u8(paddr_t addr)
{
    return jit_load_paddr_raw(addr, 1) & 0xffu;
}

/* Load one unsigned Bare-mode MMIO halfword and zero-extend it to RV64 XLEN. */
uint64_t rv64_jit_load_bare_u16(paddr_t addr)
{
    return jit_load_paddr_raw(addr, 2) & 0xffffu;
}

/* Load one unsigned Bare-mode MMIO word and zero-extend it to RV64 XLEN. */
uint64_t rv64_jit_load_bare_u32(paddr_t addr)
{
    return jit_load_paddr_raw(addr, 4) & UINT32_MAX;
}

/* Commit a proven PMEM store and invalidate only when the bytes are sensitive. */
static uint32_t jit_store_pmem_direct_continue(paddr_t addr, uint32_t len,
                                               uint64_t data)
{
    const bool touch_source = rv64_jit_write_may_touch_source_chunk(addr, (int)len);
    const bool touch_page_table =
        rv64_jit_write_may_touch_data_tlb_page_table(addr, (int)len) ||
        rv64_jit_write_may_touch_ifetch_page_table(addr, (int)len);

    /*
     * Ordinary data stores do not need the full write helper. The JIT has
     * already proved that this is PMEM, and tracing is not enabled for native
     * JIT builds. Sensitive writes still run the exact invalidation path after
     * the new bytes are visible, preserving the write-side ordering needed by
     * self-modifying code and page-table edits.
     */
    host_write(guest_to_host(addr), (int)len, (word_t)data);

    if (touch_source || touch_page_table)
    {
        isa_jit_invalidate_paddr(addr, (int)len);
    }

    return (touch_source || touch_page_table)
               ? RV64_JIT_STORE_MUST_EXIT
               : RV64_JIT_STORE_MAY_CONTINUE;
}

/*
 * Shared translated-store helper.
 *
 * A proven PMEM translation can refill the DTLB, commit the store, and let the
 * current native block continue when no source or translation dependency was
 * touched. MMIO, faults, and sensitive writes retain the dispatcher boundary.
 */
uint32_t rv64_jit_store_vaddr_continue(vaddr_t addr, uint32_t len,
                                       uint64_t data)
{
    /*
     * A data-TLB hit skips the repeated page walk and commits through the
     * direct PMEM helper above. That helper invalidates only when the physical
     * bytes are tracked as source or page-table state. Anything not proven
     * ordinary PMEM uses vaddr_write().
     */
    paddr_t paddr = 0;

    JIT_STAT_INC(helper_store_count);

    if (jit_translate_pmem(addr, len, MEM_TYPE_WRITE, &paddr))
    {
        JIT_STAT_INC(data_tlb_direct_stores);
        const uint32_t result =
            jit_store_pmem_direct_continue(paddr, len, data);

        if (result == RV64_JIT_STORE_MAY_CONTINUE)
        {
            JIT_STAT_INC(paged_store_helper_continuations);
        }

        return result;
    }

    vaddr_write(addr, (int)len, (word_t)data);
    return RV64_JIT_STORE_MUST_EXIT;
}

#if RV64_JIT_STATS
/*
 * Return two deterministic boundary phases for the focused statistics test.
 *
 * Production builds compile this hook out. The diagnostic build pays one
 * cached environment lookup and injects only when explicitly requested:
 * The first call simulates callback-driven DMA invalidating the currently
 * executing Bare block, the second requests the same outer CPU boundary as a
 * device which makes an interrupt pending, and the fixture's final helper call
 * resets the complete native arena. Delaying that reset preserves the earlier
 * route-cache measurements while still proving that generated code exits.
 */
static uint32_t jit_test_mmio_boundary_phase(void)
{
    static bool initialised = false;
    static bool enabled = false;
    static uint32_t call_count = 0;

    if (!initialised)
    {
        const char *value =
            getenv("NEMU_RV64_JIT_TEST_MMIO_BOUNDARIES");
        enabled = value != NULL && value[0] != '\0' &&
                  !(value[0] == '0' && value[1] == '\0');
        initialised = true;
    }

    if (!enabled)
    {
        return 0;
    }

    call_count++;

    if (call_count == 1u)
    {
        return 1u;
    }

    if (call_count == 2u)
    {
        return 3u;
    }

    if (call_count == 24u)
    {
        return 2u;
    }

    return 0;
}
#endif

/*
 * Commit one guarded bare-address store, then report whether native execution
 * may continue. The generated range guard normally sends only MMIO here, but
 * using paddr_write() preserves the complete architectural path if that guard
 * is ever broadened.
 */
uint32_t rv64_jit_store_bare_continue(paddr_t addr, uint32_t len,
                                      uint64_t data)
{
    const uint64_t ifetch_generation_before =
        rv64_jit_ifetch_generation;
    const uint64_t native_cache_epoch_before =
        rv64_jit_native_cache_epoch;
    const bool interrupt_was_pending = cpu.INTR;

    JIT_STAT_INC(helper_store_count);
    JIT_STAT_INC(bare_mmio_store_calls);
    paddr_write(addr, (int)len, (word_t)data);

#if RV64_JIT_STATS
    const uint32_t test_boundary_phase =
        jit_test_mmio_boundary_phase();

    if (test_boundary_phase == 1u)
    {
        /*
         * Generated stores publish their current PC before entering this
         * helper. In this Bare-only fixture it is also the source paddr, so the
         * invalidation precisely reproduces an MMIO callback whose DMA target
         * overlaps the active native block.
         */
        isa_jit_invalidate_paddr((paddr_t)cpu.pc, RV64_INSN_SIZE);
    }
    else if (test_boundary_phase == 2u)
    {
        rv64_jit_arena_reset();
    }

    const bool test_cpu_boundary = test_boundary_phase == 3u;
    if (test_cpu_boundary)
    {
        cpu.INTR = true;
    }
#else
    const bool test_cpu_boundary = false;
#endif

    const bool cpu_boundary =
        test_cpu_boundary ||
        nemu_state.state != NEMU_RUNNING ||
        (!interrupt_was_pending && cpu.INTR);
    const bool invalidated_native_state =
        rv64_jit_ifetch_generation != ifetch_generation_before ||
        rv64_jit_native_cache_epoch != native_cache_epoch_before;

    if (invalidated_native_state)
    {
        JIT_STAT_INC(bare_mmio_store_invalidation_exits);
    }

    if (cpu_boundary)
    {
        /*
         * Returning from generated code is only a JIT-dispatch boundary.
         * Request an outer CPU-loop boundary as well so interrupts and stop
         * states are observed before another native block can execute.
         */
        rv64_jit_cpu_boundary_requested = true;
    }

    if (cpu_boundary || invalidated_native_state)
    {
        JIT_STAT_INC(bare_mmio_store_boundary_exits);
        return RV64_JIT_STORE_MUST_EXIT;
    }

    JIT_STAT_INC(bare_mmio_store_continuations);
    return RV64_JIT_STORE_MAY_CONTINUE;
}

/* Commit a guarded PMEM store and report whether native code may continue. */
uint32_t rv64_jit_store_pmem_continue(paddr_t addr, uint32_t len, uint64_t data)
{
    /*
     * Source writes must leave the native block after the direct PMEM helper,
     * because the write can invalidate the block currently running. Ordinary
     * data writes can continue: the pre-check decided that no source bytes or
     * cached page-table dependencies are touched.
     */
    JIT_STAT_INC(helper_store_count);

    return jit_store_pmem_direct_continue(addr, len, data);
}

/*
 * Source invalidation model.
 *
 * A native block is valid only while no physical source byte or referenced
 * ifetch page-table page has been modified since compilation.  The JIT does not
 * keep a second byte snapshot.  Instead, every CPU, helper, DMA, and device PMEM
 * writer must call isa_jit_invalidate_paddr(); that global write-side invariant
 * is what prevents stale native instructions from running.
 *
 * Source bytes are grouped into 128-byte PMEM chunks.  Each block publishes
 * reverse links from those chunks to its cache slot, allowing a normal store or
 * DMA write to discard affected blocks without scanning the whole cache in the
 * common case.  The full scan fallback is retained for oversized ranges and
 * defensive overflow handling.
 */
/* Round a code offset up to the next power-of-two alignment boundary. */
size_t rv64_jit_align_up(size_t value, size_t align)
{
    return (value + align - 1u) & ~(align - 1u);
}

/* Return true when two half-open physical ranges overlap. */
static bool jit_ranges_overlap(paddr_t a, uint32_t a_len, paddr_t b, int b_len)
{
    if (a_len == 0 || b_len <= 0)
    {
        return false;
    }

    const paddr_t max_address = (paddr_t)-1;
    if (a > max_address - (paddr_t)a_len ||
        b > max_address - (paddr_t)b_len)
    {
        /* A wrapped caller range is malformed; conservatively report overlap. */
        return true;
    }

    const paddr_t a_end = a + (paddr_t)a_len;
    const paddr_t b_end = b + (paddr_t)b_len;
    return a < b_end && b < a_end;
}

/* Convert a PMEM physical address to its source-ref chunk index. */
static bool jit_paddr_to_source_chunk(paddr_t addr, size_t *chunk)
{
    if (!in_pmem(addr))
    {
        return false;
    }

    *chunk = (size_t)((addr - (paddr_t)CONFIG_MBASE) >> RV64_JIT_SOURCE_CHUNK_SHIFT);
    return *chunk < RV64_JIT_PMEM_CHUNK_COUNT;
}

/* Convert one physical source range to the chunk range that covers it. */
bool rv64_jit_source_chunk_range(paddr_t addr, uint32_t len,
                                 size_t *first, size_t *last)
{
    if (len == 0)
    {
        return false;
    }

    const paddr_t end = addr + (paddr_t)len - 1u;
    return end >= addr &&
           jit_paddr_to_source_chunk(addr, first) &&
           jit_paddr_to_source_chunk(end, last);
}

/* Append one instruction's physical bytes to the current source-segment list. */
bool rv64_jit_source_builder_append(rv64_jit_source_builder_t *source,
                                    paddr_t paddr, uint32_t len)
{
    if (len == 0)
    {
        return false;
    }

    const uint32_t source_offset = source->source_len;

    if (source->segment_count != 0)
    {
        rv64_jit_source_segment_t *last =
            &source->segments[source->segment_count - 1u];

        if (last->source_offset + last->len == source_offset &&
            last->paddr_start + (paddr_t)last->len == paddr)
        {
            last->len += len;
            source->source_len += len;
            return true;
        }
    }

    if (source->segment_count >= RV64_JIT_BLOCK_MAX_SOURCE_SEGMENTS)
    {
        return false;
    }

    source->segments[source->segment_count++] = (rv64_jit_source_segment_t){
        .paddr_start = paddr,
        .source_offset = source_offset,
        .len = len,
    };
    source->source_len += len;
    return true;
}

/* Append one unique ifetch page-table page to a block-local dependency list. */
static bool jit_ifetch_ref_builder_append(rv64_jit_ifetch_ref_builder_t *refs,
                                          paddr_t page)
{
    for (uint32_t i = 0; i < refs->count; i++)
    {
        if (refs->pages[i] == page)
        {
            return true;
        }
    }

    if (refs->count >= RV64_JIT_BLOCK_MAX_IFETCH_PT_PAGES)
    {
        return false;
    }

    refs->pages[refs->count++] = page;
    return true;
}

/* Publish ifetch page-table refs owned by one translated native block. */
void rv64_jit_ifetch_refs_ref(const rv64_jit_block_t *block)
{
    for (uint32_t i = 0; i < block->ifetch_pt_page_count; i++)
    {
        jit_ifetch_ref_page(block->ifetch_pt_pages[i]);
    }
}

/* Release ifetch page-table refs owned by one translated native block. */
static void jit_ifetch_refs_unref(const rv64_jit_block_t *block)
{
    for (uint32_t i = 0; i < block->ifetch_pt_page_count; i++)
    {
        jit_ifetch_unref_page(block->ifetch_pt_pages[i]);
    }
}

/* Replace a live block's ifetch dependency pages after successful revalidation. */
static void jit_ifetch_refs_replace(rv64_jit_block_t *block,
                                    const rv64_jit_ifetch_ref_builder_t *refs)
{
    jit_ifetch_refs_unref(block);
    block->ifetch_pt_page_count = refs->count;
    memcpy(block->ifetch_pt_pages, refs->pages,
           refs->count * sizeof(refs->pages[0]));
    rv64_jit_ifetch_refs_ref(block);
}

/* Find the physical source byte expected at one virtual block offset. */
static bool jit_block_source_paddr_at(const rv64_jit_block_t *block,
                                      uint32_t source_offset, paddr_t *paddr)
{
    for (uint32_t i = 0; i < block->source_segment_count; i++)
    {
        const rv64_jit_source_segment_t *segment = &block->source_segments[i];

        if (source_offset >= segment->source_offset &&
            source_offset < segment->source_offset + segment->len)
        {
            *paddr = segment->paddr_start +
                     (paddr_t)(source_offset - segment->source_offset);
            return true;
        }
    }

    return false;
}

/* Return whether a PMEM write overlaps any physical segment of one block. */
bool rv64_jit_block_source_overlaps(const rv64_jit_block_t *block,
                                    paddr_t addr, int len)
{
    for (uint32_t i = 0; i < block->source_segment_count; i++)
    {
        const rv64_jit_source_segment_t *segment = &block->source_segments[i];

        if (jit_ranges_overlap(segment->paddr_start, segment->len, addr, len))
        {
            return true;
        }
    }

    return false;
}

/* Return whether an earlier segment in this block already covers a chunk. */
static bool jit_block_source_chunk_seen_before(const rv64_jit_block_t *block,
                                               uint32_t segment_idx,
                                               uint32_t chunk)
{
    for (uint32_t i = 0; i < segment_idx; i++)
    {
        const rv64_jit_source_segment_t *segment = &block->source_segments[i];
        size_t first = 0;
        size_t last = 0;

        if (rv64_jit_source_chunk_range(segment->paddr_start, segment->len,
                                        &first, &last) &&
            chunk >= first && chunk <= last)
        {
            return true;
        }
    }

    return false;
}

/*
 * Nodes below this frontier have been claimed in the current arena lifetime.
 * Index zero remains the null sentinel; discarded nodes are recycled through
 * rv64_jit_source_link_free_head before the frontier advances further.
 */
static uint32_t jit_source_link_next_unused = 1u;

/*
 * Validate the static-storage state used by the first executable arena.
 * Chunk heads and node storage are zero-initialised by C before NEMU starts,
 * so walking the multi-million-node pool here would only manufacture a free
 * list that the lazy frontier can represent with this single scalar.
 */
void rv64_jit_source_reverse_map_init(void)
{
    Assert(rv64_jit_source_link_free_head == RV64_JIT_SOURCE_LINK_NULL &&
               jit_source_link_next_unused == 1u,
           "jit: RV64 source reverse map initialised after node allocation");
}

/* Discard every published root and rewind the lazy per-arena allocators. */
void rv64_jit_source_reverse_map_reset(void)
{
    memset(rv64_jit_source_chunk_heads, 0, sizeof(rv64_jit_source_chunk_heads));
    rv64_jit_source_link_free_head = RV64_JIT_SOURCE_LINK_NULL;
    jit_source_link_next_unused = 1u;
}

/* Allocate a recycled node first, then claim untouched pool storage lazily. */
static uint32_t jit_source_link_alloc(void)
{
    uint32_t node = RV64_JIT_SOURCE_LINK_NULL;

    if (rv64_jit_source_link_free_head != RV64_JIT_SOURCE_LINK_NULL)
    {
        node = rv64_jit_source_link_free_head;
        Assert(node < jit_source_link_next_unused,
               "jit: invalid recycled RV64 source reverse-map node %u", node);
        rv64_jit_source_link_free_head = rv64_jit_source_links[node].next;
        JIT_STAT_INC(source_link_recycled_allocations);
    }
    else
    {
        Assert(jit_source_link_next_unused < RV64_JIT_SOURCE_LINK_COUNT,
               "jit: RV64 source reverse-map node pool exhausted");
        node = jit_source_link_next_unused++;
        JIT_STAT_INC(source_link_sequential_allocations);
    }

    /* A post-reset frontier claim may contain an old arena's stale link. */
    rv64_jit_source_links[node].next = RV64_JIT_SOURCE_LINK_NULL;
    return node;
}

/* Return one reverse source-map node to the free list. */
static void jit_source_link_free(uint32_t node)
{
    Assert(node != RV64_JIT_SOURCE_LINK_NULL &&
               node < jit_source_link_next_unused,
           "jit: invalid RV64 source reverse-map node %u", node);

    rv64_jit_source_links[node].block_index = 0;
    rv64_jit_source_links[node].next = rv64_jit_source_link_free_head;
    rv64_jit_source_link_free_head = node;
}

/* Return the direct-mapped cache index for one block pointer. */
static uint32_t jit_block_index(const rv64_jit_block_t *block)
{
    const uintptr_t block_addr = (uintptr_t)block;
    const uintptr_t cache_start = (uintptr_t)rv64_jit_cache;
    const uintptr_t cache_end = (uintptr_t)(rv64_jit_cache + RV64_JIT_CACHE_SIZE);

    Assert(block_addr >= cache_start && block_addr < cache_end,
           "jit: RV64 block pointer outside cache");
    return (uint32_t)(block - rv64_jit_cache);
}

/* Add one block to every source chunk it references. */
void rv64_jit_source_reverse_map_add(rv64_jit_block_t *block)
{
    if (block->source_segment_count == 0)
    {
        return;
    }

    const uint32_t block_index = jit_block_index(block);

    for (uint32_t i = 0; i < block->source_segment_count; i++)
    {
        rv64_jit_source_segment_t *segment = &block->source_segments[i];
        size_t first = 0;
        size_t last = 0;

        if (!rv64_jit_source_chunk_range(segment->paddr_start, segment->len, &first, &last))
        {
            continue;
        }

        segment->source_chunk_first = (uint32_t)first;
        segment->source_chunk_last = (uint32_t)last;

        for (size_t chunk = first; chunk <= last; chunk++)
        {
            if (jit_block_source_chunk_seen_before(block, i, (uint32_t)chunk))
            {
                continue;
            }

            const uint32_t node = jit_source_link_alloc();
            rv64_jit_source_links[node].block_index = block_index;
            rv64_jit_source_links[node].next = rv64_jit_source_chunk_heads[chunk];
            rv64_jit_source_chunk_heads[chunk] = node;
        }
    }
}

/* Remove one block from every reverse source-chunk list it references. */
static void jit_source_reverse_map_remove(const rv64_jit_block_t *block)
{
    if (block->source_segment_count == 0)
    {
        return;
    }

    const uint32_t block_index = jit_block_index(block);

    for (uint32_t i = 0; i < block->source_segment_count; i++)
    {
        const rv64_jit_source_segment_t *segment = &block->source_segments[i];
        const uint32_t first = segment->source_chunk_first;
        const uint32_t last = segment->source_chunk_last;

        if (first >= RV64_JIT_PMEM_CHUNK_COUNT || last >= RV64_JIT_PMEM_CHUNK_COUNT)
        {
            continue;
        }

        for (uint32_t chunk = first; chunk <= last; chunk++)
        {
            if (jit_block_source_chunk_seen_before(block, i, chunk))
            {
                continue;
            }

            uint32_t *link = &rv64_jit_source_chunk_heads[chunk];

            while (*link != RV64_JIT_SOURCE_LINK_NULL)
            {
                const uint32_t node = *link;

                if (rv64_jit_source_links[node].block_index == block_index)
                {
                    *link = rv64_jit_source_links[node].next;
                    jit_source_link_free(node);
                    break;
                }

                link = &rv64_jit_source_links[node].next;
            }
        }
    }
}

/* Add source-ref counts for the physical bytes backing one native block. */
void rv64_jit_source_chunks_ref(const rv64_jit_block_t *block)
{
    for (uint32_t segment_idx = 0; segment_idx < block->source_segment_count;
         segment_idx++)
    {
        const rv64_jit_source_segment_t *segment = &block->source_segments[segment_idx];
        size_t first = 0;
        size_t last = 0;

        if (!rv64_jit_source_chunk_range(segment->paddr_start, segment->len, &first, &last))
        {
            continue;
        }

        for (size_t i = first; i <= last; i++)
        {
            if (jit_block_source_chunk_seen_before(block, segment_idx, (uint32_t)i))
            {
                continue;
            }

            Assert(rv64_jit_source_chunk_refs[i] != UINT32_MAX,
                   "jit: RV64 source chunk refcount overflow at %zu", i);
            rv64_jit_source_chunk_refs[i]++;
        }
    }
}

/* Remove source-ref counts when a native block is discarded. */
static void jit_source_chunks_unref(const rv64_jit_block_t *block)
{
    for (uint32_t segment_idx = 0; segment_idx < block->source_segment_count;
         segment_idx++)
    {
        const rv64_jit_source_segment_t *segment = &block->source_segments[segment_idx];
        size_t first = 0;
        size_t last = 0;

        if (!rv64_jit_source_chunk_range(segment->paddr_start, segment->len, &first, &last))
        {
            continue;
        }

        for (size_t i = first; i <= last; i++)
        {
            if (jit_block_source_chunk_seen_before(block, segment_idx, (uint32_t)i))
            {
                continue;
            }

            Assert(rv64_jit_source_chunk_refs[i] > 0,
                   "jit: RV64 source chunk refcount underflow at %zu", i);
            rv64_jit_source_chunk_refs[i]--;
        }
    }
}

/* Return whether a PMEM write can overlap any compiled source chunk. */
bool rv64_jit_write_may_touch_source_chunk(paddr_t addr, int len)
{
    if (len <= 0)
    {
        return false;
    }

    if (!in_pmem_range(addr, len))
    {
        /*
         * Ambiguous ranges stay conservative.  Device/DMA paths are rare here,
         * and a full scan is still correct when a range cannot be chunked.
         */
        return true;
    }

    size_t first = 0;
    size_t last = 0;

    if (!jit_paddr_to_source_chunk(addr, &first) ||
        !jit_paddr_to_source_chunk(addr + (paddr_t)len - 1u, &last))
    {
        return true;
    }

    for (size_t i = first; i <= last; i++)
    {
        if (rv64_jit_source_chunk_refs[i] != 0)
        {
            return true;
        }
    }

    return false;
}

/* Release one cache slot and its source refs, if it owns source bytes. */
void rv64_jit_block_discard(rv64_jit_block_t *block)
{
    const bool was_valid = block->valid;

    /*
     * Native selectors must stop reaching this generation before its metadata
     * or arena bytes can be cleared. Source-owned records are removed from
     * their target-slot lists by the same lifecycle hook.
     */
    rv64_jit_links_block_discard(block);

    if (was_valid)
    {
        /*
         * Make lazy PIC readers reject this publication before dependency
         * metadata is released or its arena bytes can later be recycled.
         */
        block->valid = false;
        block->generation = 0;

        if (block->source_segment_count != 0)
        {
            jit_source_reverse_map_remove(block);
            jit_source_chunks_unref(block);
        }

        jit_ifetch_refs_unref(block);
    }

    *block = (rv64_jit_block_t){0};
}

/*
 * Block validation and cache matching.
 *
 * Cache lookup is deliberately only a hint.  A slot match re-checks the guest
 * PC, `satp`, ifetch privilege, translation generation, physical source mapping,
 * and page-table dependencies before native code runs.  Actual source-byte
 * changes are handled earlier by the mandatory write-side invalidation hook
 * described above.  Unsupported slots are negative cache entries, preventing
 * repeated compilation attempts for the same fallback instruction.
 */
/* Return whether this leaf PTE permits instruction fetch at the current priv. */
static bool jit_ifetch_leaf_allows(word_t pte)
{
    const bool user_page = (pte & RV64_JIT_PTE_U) != 0;

    if ((pte & (RV64_JIT_PTE_A | RV64_JIT_PTE_X)) !=
        (RV64_JIT_PTE_A | RV64_JIT_PTE_X))
    {
        return false;
    }

    if (cpu.prvi == RISCV64_PRIV_U)
    {
        return user_page;
    }

    if (cpu.prvi == RISCV64_PRIV_S)
    {
        /*
         * SUM affects S-mode data access only.  S-mode instruction fetch from a
         * user page remains illegal, matching the reference Sv39 walker.
         */
        return !user_page;
    }

    return false;
}

/* Translate an instruction fetch, optionally collecting page-table deps. */
bool rv64_jit_translate_ifetch_collect(vaddr_t pc, paddr_t *paddr,
                                       bool *translated,
                                       rv64_jit_ifetch_ref_builder_t *refs)
{
    /* Only 32-bit base instructions are compiled; compressed fetch is fallback. */
    const int mmu = isa_mmu_check(pc, RV64_INSN_SIZE, MEM_TYPE_IFETCH);

    if (mmu == MMU_DIRECT)
    {
        *paddr = (paddr_t)pc;
        *translated = false;
        return true;
    }

    if (mmu == MMU_TRANSLATE)
    {
        if (!jit_sv39_canonical(pc) ||
            jit_sv39_cross_page(pc, RV64_INSN_SIZE))
        {
            return false;
        }

        const word_t vpn[RV64_JIT_SV39_LEVELS] = {
            ((word_t)pc >> RV64_JIT_SV39_VPN_SHIFT(0)) &
                RV64_JIT_SV39_VPN_MASK,
            ((word_t)pc >> RV64_JIT_SV39_VPN_SHIFT(1)) &
                RV64_JIT_SV39_VPN_MASK,
            ((word_t)pc >> RV64_JIT_SV39_VPN_SHIFT(2)) &
                RV64_JIT_SV39_VPN_MASK,
        };
        paddr_t pt_base =
            (paddr_t)((cpu.csr.satp & RV64_JIT_SATP_PPN_MASK) << PAGE_SHIFT);

        for (int level = (int)RV64_JIT_SV39_LEVELS - 1; level >= 0; --level)
        {
            const paddr_t pte_addr =
                pt_base + (paddr_t)(vpn[level] * RV64_JIT_PTE_SIZE);

            if (!jit_data_pmem_range(pte_addr, RV64_JIT_PTE_SIZE) ||
                (refs != NULL &&
                 !jit_ifetch_ref_builder_append(refs, pt_base)))
            {
                return false;
            }

            const word_t pte = (word_t)paddr_read(pte_addr, RV64_JIT_PTE_SIZE);

            if (!jit_sv39_pte_valid(pte))
            {
                return false;
            }

            const word_t ppn = jit_sv39_pte_ppn(pte);

            if (jit_sv39_pte_leaf(pte))
            {
                if (!jit_sv39_superpage_aligned(ppn, level) ||
                    !jit_ifetch_leaf_allows(pte))
                {
                    return false;
                }

                *paddr = jit_sv39_leaf_page_base(ppn, vpn, level) |
                         (paddr_t)(pc & PAGE_MASK);
                *translated = true;
                return true;
            }

            if (level == 0 || (pte & RV64_JIT_PTE_NON_LEAF_RESERVED) != 0)
            {
                return false;
            }

            pt_base = (paddr_t)(ppn << PAGE_SHIFT);
        }
    }

    return false;
}

/* Translate an instruction-fetch virtual PC and report whether paging was used. */
bool rv64_jit_translate_ifetch_ex(vaddr_t pc, paddr_t *paddr, bool *translated)
{
    return rv64_jit_translate_ifetch_collect(pc, paddr, translated, NULL);
}

/* Check whether a cache slot still describes the current PC and source bytes. */
bool rv64_jit_block_matches(rv64_jit_block_t *block, vaddr_t pc)
{
    if (!block->valid ||
        block->pc != pc ||
        block->satp != cpu.csr.satp ||
        block->ifetch_state != rv64_jit_ifetch_state())
    {
        return false;
    }

    if (block->uses_data_state &&
        block->data_state != rv64_jit_data_tlb_state(MEM_TYPE_READ))
    {
        return false;
    }

    if (block->translated)
    {
        if (block->ifetch_generation == rv64_jit_ifetch_generation)
        {
            JIT_STAT_INC(ifetch_generation_fast_hits);
            return true;
        }

        JIT_STAT_INC(ifetch_generation_revalidations);

        /*
         * Writes and SFENCE.VMA conservatively bump the global ifetch
         * generation.  Only after such a bump do we re-translate the virtual
         * pages touched by this block and refresh its generation if they still
         * resolve to the same physical source addresses.  Byte changes do not
         * reach this path because write-side invalidation discards the block.
         */
        uint32_t offset = 0;
        rv64_jit_ifetch_ref_builder_t refs = {0};

        while (offset < block->source_len)
        {
            const vaddr_t check_pc = pc + (vaddr_t)offset;
            paddr_t expected = 0;
            paddr_t now = 0;
            bool translated = false;

            if (!jit_block_source_paddr_at(block, offset, &expected) ||
                !rv64_jit_translate_ifetch_collect(check_pc, &now, &translated, &refs) ||
                !translated ||
                now != expected)
            {
                return false;
            }

            const uint32_t page_left =
                PAGE_SIZE - (uint32_t)(check_pc & PAGE_MASK);
            const uint32_t remaining = block->source_len - offset;
            offset += page_left < remaining ? page_left : remaining;
        }

        jit_ifetch_refs_replace(block, &refs);
        block->ifetch_generation = rv64_jit_ifetch_generation;
    }

    return true;
}

/* Publish a negative cache entry for a currently unsupported instruction. */
void rv64_jit_mark_unsupported(vaddr_t pc, paddr_t paddr, bool translated)
{
    JIT_STAT_INC(blocks_unsupported);

    rv64_jit_ifetch_ref_builder_t refs = {0};

    if (translated)
    {
        paddr_t checked_paddr = 0;
        bool checked_translated = false;

        if (!rv64_jit_translate_ifetch_collect(pc, &checked_paddr,
                                               &checked_translated, &refs) ||
            !checked_translated ||
            checked_paddr != paddr)
        {
            return;
        }
    }

    rv64_jit_block_t *block = rv64_jit_cache_slot(pc);
    rv64_jit_block_discard(block);
    *block = (rv64_jit_block_t){
        .valid = true,
        .translated = translated,
        .uses_data_state = false,
        .pc = pc,
        .satp = cpu.csr.satp,
        .ifetch_state = rv64_jit_ifetch_state(),
        .data_state = rv64_jit_data_tlb_state(MEM_TYPE_READ),
        .ifetch_generation = rv64_jit_ifetch_generation,
        .paddr_start = paddr,
        .source_len = RV64_INSN_SIZE,
        .source_segment_count = 1,
        .ifetch_pt_page_count = translated ? refs.count : 0,
        .source_segments = {
            {
                .paddr_start = paddr,
                .source_offset = 0,
                .len = RV64_INSN_SIZE,
            },
        },
        .insn_count = 0,
        .entry = NULL,
        .body_entry = NULL,
    };
    memcpy(block->ifetch_pt_pages, refs.pages,
           block->ifetch_pt_page_count * sizeof(block->ifetch_pt_pages[0]));
    /*
     * Negative cache entries need source refs too.  If self-modifying code
     * rewrites an unsupported instruction into a supported one, exact
     * invalidation must remove this marker so the JIT can compile the new bytes.
     */
    rv64_jit_ifetch_refs_ref(block);
    rv64_jit_source_chunks_ref(block);
    rv64_jit_source_reverse_map_add(block);
}

#endif /* CONFIG_RV64 */
