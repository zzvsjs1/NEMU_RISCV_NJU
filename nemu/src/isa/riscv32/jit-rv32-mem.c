#include <generated/autoconf.h>

#ifndef CONFIG_RV64

#include "jit-rv32-internal.h"

/*
 * RV32 JIT memory layer: Sv32 helper translations, page-table dependencies,
 * generated-code load/store helpers, and compiled-source chunk ownership.
 */

void rv32_jit_tlb_flush(void)
{
    memset(rv32_jit_tlb, 0, sizeof(rv32_jit_tlb));
    memset(rv32_jit_tlb_pt_page_refs, 0, sizeof(rv32_jit_tlb_pt_page_refs));
}

static bool jit_pmem_page_index(paddr_t page, size_t *idx)
{
    const paddr_t base = (paddr_t)CONFIG_MBASE;

    if (page < base || page >= base + (paddr_t)CONFIG_MSIZE)
    {
        return false;
    }

    *idx = (size_t)((page - base) >> PAGE_SHIFT);
    return *idx < RV32_JIT_PMEM_PAGE_COUNT;
}

static void jit_tlb_ref_page(paddr_t page)
{
    size_t idx = 0;

    if (jit_pmem_page_index(page, &idx) && rv32_jit_tlb_pt_page_refs[idx] != UINT16_MAX)
    {
        rv32_jit_tlb_pt_page_refs[idx]++;
    }
}

static void jit_tlb_unref_page(paddr_t page)
{
    size_t idx = 0;

    if (jit_pmem_page_index(page, &idx) && rv32_jit_tlb_pt_page_refs[idx] > 0)
    {
        rv32_jit_tlb_pt_page_refs[idx]--;
    }
}

static bool jit_tlb_refs_page(paddr_t page)
{
    size_t idx = 0;
    return jit_pmem_page_index(page, &idx) && rv32_jit_tlb_pt_page_refs[idx] != 0;
}

bool rv32_jit_write_may_touch_page_table(paddr_t addr, int len)
{
    /*
     * The JIT TLB is tagged by satp, but entries can outlive the current satp
     * value.  For example, the guest may switch to Bare mode, edit the old page
     * table, and later switch back to the same satp.  Therefore this check is
     * purely physical: any write to a PMEM page referenced by a cached walk drops
     * all local JIT translations.
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

    for (paddr_t page = addr & ~(paddr_t)PAGE_MASK; page <= (end & ~(paddr_t)PAGE_MASK); page += PAGE_SIZE)
    {
        if (jit_tlb_refs_page(page))
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

static bool jit_cross_page(vaddr_t addr, uint32_t len)
{
    const word_t off = (word_t)(addr & PAGE_MASK);
    return off + (word_t)len > PAGE_SIZE;
}

static bool jit_pmem_range(paddr_t addr, uint32_t len)
{
    const paddr_t end = addr + (paddr_t)len - 1u;
    return len > 0 && end >= addr && likely(in_pmem(addr) && in_pmem(end));
}

static uint32_t jit_required_perm(int type)
{
    switch (type)
    {
    case MEM_TYPE_IFETCH:
        return RV32_JIT_PTE_X;
    case MEM_TYPE_READ:
        return RV32_JIT_PTE_R;
    case MEM_TYPE_WRITE:
        return RV32_JIT_PTE_W;
    default:
        return 0;
    }
}

/*
 * Translate a Sv32 virtual address to ordinary PMEM for JIT helper accesses.
 *
 * This deliberately keeps a small translation cache local to the JIT helper
 * path so generated code does not need to inline page walks.  A
 * false result is not an error; it only means the existing vaddr path must handle
 * the edge case, such as MMIO, cross-page accesses, invalid PTEs, or superpages.
 * Permission checks intentionally match the simplified interpreter MMU in
 * system/mmu.c: valid 4 KiB leaves with the required R/W/X bit can use the fast
 * path; privilege-sensitive rules such as U/SUM/MXR and accessed/dirty-bit
 * management are not implemented by the current interpreter MMU.
 */
static bool jit_translate_pmem(vaddr_t addr, uint32_t len, int type, paddr_t *paddr)
{
    const uint32_t satp = cpu.csr.satp;

    if ((satp & RV32_JIT_SATP_MODE_MASK) == RISCV_SATP_MODE_BARE)
    {
        const paddr_t direct = (paddr_t)addr;

        if (!jit_pmem_range(direct, len))
        {
            return false;
        }

        *paddr = direct;
        return true;
    }

    if (len == 0 || jit_cross_page(addr, len))
    {
        return false;
    }

    const uint32_t need_perm = jit_required_perm(type);
    const uint32_t vpn = (uint32_t)(addr >> PAGE_SHIFT);
    const uint32_t idx = vpn & (RV32_JIT_TLB_SIZE - 1u);
    rv32_jit_tlb_entry_t *entry = &rv32_jit_tlb[idx];

    if (likely(entry->valid && entry->satp == satp && entry->vpn == vpn && (entry->perm & need_perm) != 0))
    {
        const paddr_t translated = entry->pg_paddr | (paddr_t)(addr & PAGE_MASK);

        if (!jit_pmem_range(translated, len))
        {
            return false;
        }

        *paddr = translated;
        return true;
    }

    const paddr_t root = ((paddr_t)(satp & RV32_JIT_SATP_PPN_MASK)) << PAGE_SHIFT;
    const word_t vpn1 = (word_t)((addr >> (PAGE_SHIFT + RISCV32_SV32_ROOT_LEVEL * RISCV32_SV32_VPN_BITS)) & RISCV32_SV32_VPN_MASK);
    const word_t vpn0 = (word_t)((addr >> PAGE_SHIFT) & RISCV32_SV32_VPN_MASK);
    const paddr_t pte1_addr = root + (paddr_t)(vpn1 * RISCV32_SV32_PTE_BYTES);
    const uint32_t pte1 = (uint32_t)paddr_read(pte1_addr, RISCV32_SV32_PTE_BYTES);

    if ((pte1 & RV32_JIT_PTE_V) == 0)
    {
        return false;
    }

    /*
     * Nanos-lite uses normal 4 KiB leaves for this workload.  Superpages are left
     * to the full MMU path, which already owns those less common checks.
     */
    const uint32_t pte1_rwx = pte1 & (RV32_JIT_PTE_R | RV32_JIT_PTE_W | RV32_JIT_PTE_X);

    if (pte1_rwx != 0)
    {
        return false;
    }

    const paddr_t l0_pt = ((paddr_t)(pte1 >> RISCV_PTE_PPN_SHIFT)) << PAGE_SHIFT;
    const paddr_t pte0_addr = l0_pt + (paddr_t)(vpn0 * RISCV32_SV32_PTE_BYTES);
    const uint32_t pte0 = (uint32_t)paddr_read(pte0_addr, RISCV32_SV32_PTE_BYTES);

    if ((pte0 & RV32_JIT_PTE_V) == 0)
    {
        return false;
    }

    const uint32_t perm = pte0 & (RV32_JIT_PTE_R | RV32_JIT_PTE_W | RV32_JIT_PTE_X);

    if (perm == 0 || (perm & need_perm) == 0)
    {
        return false;
    }

    const paddr_t pg_paddr = ((paddr_t)(pte0 >> RISCV_PTE_PPN_SHIFT)) << PAGE_SHIFT;

    if (!jit_pmem_range(pg_paddr | (paddr_t)(addr & PAGE_MASK), len))
    {
        return false;
    }

    if (entry->valid)
    {
        const paddr_t old_root = ((paddr_t)(entry->satp & RV32_JIT_SATP_PPN_MASK)) << PAGE_SHIFT;
        jit_tlb_unref_page(old_root);
        jit_tlb_unref_page(entry->pt_page);
    }

    *entry = (rv32_jit_tlb_entry_t){
        .satp = satp,
        .vpn = vpn,
        .perm = perm,
        .pg_paddr = pg_paddr,
        .pt_page = l0_pt,
        .valid = true,
    };
    jit_tlb_ref_page(root);
    jit_tlb_ref_page(l0_pt);
    *paddr = pg_paddr | (paddr_t)(addr & PAGE_MASK);
    return true;
}

/*
 * Shared load helper for generated code.
 *
 * The generated block passes a guest virtual address and byte width. This
 * helper takes a direct PMEM shortcut only when it can prove the normal memory
 * path would be an ordinary RAM read; otherwise it calls vaddr_read().
 */
static uint32_t jit_load_raw(vaddr_t addr, uint32_t len)
{
    JIT_STAT_INC(helper_loads);

    /*
     * The direct helper path is still semantically a memory access by the guest:
     * it is allowed only after Bare or simple Sv32 translation proves the final
     * physical byte range is ordinary PMEM. Devices, cross-page accesses, and
     * exception-sensitive cases delegate to vaddr_read().
     */
    paddr_t paddr = 0;
    uint32_t value = 0;

    if (jit_translate_pmem(addr, len, MEM_TYPE_READ, &paddr))
    {
        JIT_STAT_INC(helper_load_direct);
        value = (uint32_t)host_read(guest_to_host(paddr), (int)len);
    }
    else
    {
        JIT_STAT_INC(helper_load_slow);
        value = vaddr_read(addr, (int)len);
    }

    return value;
}

/* Load one signed byte and extend it to the RV32 register width. */
uint32_t rv32_jit_load_i8(vaddr_t addr)
{
    return (uint32_t)(int32_t)(int8_t)jit_load_raw(addr, 1);
}

/* Load one signed halfword and extend it to the RV32 register width. */
uint32_t rv32_jit_load_i16(vaddr_t addr)
{
    return (uint32_t)(int32_t)(int16_t)jit_load_raw(addr, 2);
}

/* Load one 32-bit word; no extension is needed for RV32. */
uint32_t rv32_jit_load_u32(vaddr_t addr)
{
    return jit_load_raw(addr, 4);
}

/* Load one unsigned byte and zero-extend it to the RV32 register width. */
uint32_t rv32_jit_load_u8(vaddr_t addr)
{
    return jit_load_raw(addr, 1);
}

/* Load one unsigned halfword and zero-extend it to the RV32 register width. */
uint32_t rv32_jit_load_u16(vaddr_t addr)
{
    return jit_load_raw(addr, 2);
}

/*
 * Shared store helper for generated code.
 *
 * Returns non-zero only when the caller may continue executing the current
 * native block.  That is safe for ordinary translated PMEM data stores whose
 * physical page is not a page table and whose bytes are not compiled source.
 * MMIO, page-table writes, and self-modifying-code cases still force an exit so
 * the dispatcher observes the changed machine state before more translated code
 * runs.
 */
static uint32_t jit_store_raw_continue(vaddr_t addr, uint32_t len, uint32_t data)
{
    JIT_STAT_INC(helper_stores);

    paddr_t paddr = 0;

    if (jit_translate_pmem(addr, len, MEM_TYPE_WRITE, &paddr))
    {
        JIT_STAT_INC(helper_store_direct);
        const bool flush_tlb = rv32_jit_write_may_touch_page_table(paddr, (int)len);
        const bool touch_source = rv32_jit_write_may_touch_source_chunk(paddr, (int)len);
        host_write(guest_to_host(paddr), (int)len, data);

        if (touch_source || flush_tlb)
        {
            isa_jit_invalidate_paddr(paddr, (int)len);
        }

        return !flush_tlb && !touch_source;
    }

    JIT_STAT_INC(helper_store_slow);
    vaddr_write(addr, (int)len, data);
    /*
     * A failed local translation can still write PMEM through the normal memory
     * subsystem, for example on a cross-page or otherwise unsupported Sv32 case.
     * paddr_write() performs exact source invalidation and page-table detection
     * when it sees the final physical address.  Flush the small local JIT TLB as a
     * second conservative barrier before this native block exits.
     */
    rv32_jit_tlb_flush();
    return 0;
}

/*
 * Exiting store helper used by conservative paths.  It shares the fast PMEM
 * implementation above, but ignores the continuation flag because the emitted
 * code has already decided to leave the native block after this helper call.
 */
static void jit_store_raw(vaddr_t addr, uint32_t len, uint32_t data)
{
    (void)jit_store_raw_continue(addr, len, data);
}

/* Store the low byte of `data` to a guest address. */
void rv32_jit_store_u8(vaddr_t addr, uint32_t data)
{
    jit_store_raw(addr, 1, data);
}

/* Store the low halfword of `data` to a guest address. */
void rv32_jit_store_u16(vaddr_t addr, uint32_t data)
{
    jit_store_raw(addr, 2, data);
}

/* Store all 32 bits of `data` to a guest address. */
void rv32_jit_store_u32(vaddr_t addr, uint32_t data)
{
    jit_store_raw(addr, 4, data);
}

uint32_t rv32_jit_store_u8_continue(vaddr_t addr, uint32_t data)
{
    return jit_store_raw_continue(addr, 1, data);
}

uint32_t rv32_jit_store_u16_continue(vaddr_t addr, uint32_t data)
{
    return jit_store_raw_continue(addr, 2, data);
}

uint32_t rv32_jit_store_u32_continue(vaddr_t addr, uint32_t data)
{
    return jit_store_raw_continue(addr, 4, data);
}

/*
 * Convert a PMEM physical address into a source-refcount chunk index.
 *
 * The index is measured from CONFIG_MBASE and uses 128-byte chunks, so normal
 * data stores near code do not unnecessarily invalidate entire 4 KiB pages.
 */
static bool jit_pmem_source_chunk_index(paddr_t addr, size_t *idx)
{
    if (!in_pmem(addr))
    {
        return false;
    }

    *idx = ((size_t)(addr - (paddr_t)CONFIG_MBASE)) >> RV32_JIT_SOURCE_CHUNK_SHIFT;
    return *idx < RV32_JIT_PMEM_CHUNK_COUNT;
}

/* Add one owning compiled block reference to every source chunk in the range. */
void rv32_jit_source_chunks_ref(paddr_t addr, uint32_t len)
{
    /*
     * Refcounts are per PMEM chunk, not per cache slot. Multiple blocks may cover
     * the same source bytes through different PCs or satp values, so a chunk is
     * considered interesting until the last owning block is discarded.
     */

    if (len == 0)
    {
        return;
    }

    size_t first = 0;
    size_t last = 0;
    const paddr_t end = addr + (paddr_t)len - 1u;

    if (end < addr || !jit_pmem_source_chunk_index(addr, &first) || !jit_pmem_source_chunk_index(end, &last))
    {
        return;
    }

    for (size_t i = first; i <= last; i++)
    {
        Assert(rv32_jit_source_chunk_refs[i] != UINT16_MAX, "jit: too many source blocks in PMEM source chunk %zu", i);
        rv32_jit_source_chunk_refs[i]++;
    }
}

/* Remove one owning compiled block reference from every source chunk in range. */
void rv32_jit_source_chunks_unref(paddr_t addr, uint32_t len)
{
    if (len == 0)
    {
        return;
    }

    size_t first = 0;
    size_t last = 0;
    const paddr_t end = addr + (paddr_t)len - 1u;

    if (end < addr || !jit_pmem_source_chunk_index(addr, &first) || !jit_pmem_source_chunk_index(end, &last))
    {
        return;
    }

    for (size_t i = first; i <= last; i++)
    {
        Assert(rv32_jit_source_chunk_refs[i] > 0, "jit: source chunk refcount underflow on PMEM source chunk %zu", i);
        rv32_jit_source_chunk_refs[i]--;
    }
}

/*
 * Quickly decide whether a physical write might overlap compiled source bytes.
 *
 * False means no source chunk has a refcount and invalidation can be skipped.
 * True means "scan exact blocks"; it includes ambiguous wrap or boundary cases.
 */
bool rv32_jit_write_may_touch_source_chunk(paddr_t addr, int len)
{
    /*
     * This is a fast pre-filter before scanning every cache entry. Returning true
     * for ambiguous ranges is acceptable because it only costs extra invalidation
     * work; returning false for real source bytes would be a stale-code bug.
     */

    if (len <= 0)
    {
        return false;
    }

    const paddr_t pmem_start = (paddr_t)CONFIG_MBASE;
    const paddr_t pmem_end = (paddr_t)CONFIG_MBASE + (paddr_t)CONFIG_MSIZE - 1u;
    paddr_t start = addr;
    paddr_t end = addr + (paddr_t)len - 1u;

    if (end < start)
    {
        return true;
    }

    if (end < pmem_start || start > pmem_end)
    {
        return false;
    }

    if (start < pmem_start)
    {
        start = pmem_start;
    }

    if (end > pmem_end)
    {
        end = pmem_end;
    }

    size_t first = 0;
    size_t last = 0;

    if (!jit_pmem_source_chunk_index(start, &first) || !jit_pmem_source_chunk_index(end, &last))
    {
        return true;
    }

    for (size_t i = first; i <= last; i++)
    {
        if (rv32_jit_source_chunk_refs[i] != 0)
        {
            return true;
        }
    }

    return false;
}

/* Drop one cache slot and release the source-chunk references it owns. */
void rv32_jit_block_discard(rv32_jit_block_t *block)
{
    if (!block->valid)
    {
        return;
    }

    /*
     * Only compiled blocks own source chunks. Unsupported markers have entry ==
     * NULL and therefore no refcount to release, even though they still carry a
     * source address for cache matching.
     */

    if (block->entry != NULL && block->source_len != 0)
    {
        rv32_jit_source_chunks_unref(block->paddr_start, block->source_len);
    }

    block->valid = false;
    block->entry = NULL;
    block->source_len = 0;
    block->insn_count = 0;
}

#endif /* !CONFIG_RV64 */
