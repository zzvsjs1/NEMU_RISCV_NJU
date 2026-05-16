#ifndef __RISCV32_FAST_EXEC_H__
#define __RISCV32_FAST_EXEC_H__

#include <isa-jit.h>
#include <isa.h>
#include <cpu/difftest.h>
#include <memory/host.h>
#include <memory/paddr.h>
#include <memory/vaddr.h>
#include <utils.h>
#include "../local-include/reg.h"

/*
 * This header is included into cpu-exec.c so the fast executor can be inlined
 * into the main dispatch loop. It is deliberately conservative: every shortcut
 * either proves that the access is plain PMEM with valid translation state, or
 * returns to the interpreter helpers that own the complete device/MMU semantics.
 */

/*
 * RISC-V32 Sv32 puts the address-translation mode in satp[31]. When this bit
 * is clear, virtual addresses are physical addresses in this NEMU target. The
 * lower 22 bits hold the root page-table physical page number when paging is
 * enabled.
 */
#define RV32_FAST_SATP_MODE_MASK 0x80000000u
#define RV32_FAST_SATP_PPN_MASK 0x003fffffu

/*
 * Only the permission bits needed by this fast path are named here. Other PTE
 * bits, such as user/global/accessed/dirty, are still handled by the existing
 * vaddr/paddr path whenever this simple fast check is not enough.
 */
#define RV32_FAST_PTE_V 0x001u
#define RV32_FAST_PTE_R 0x002u
#define RV32_FAST_PTE_W 0x004u
#define RV32_FAST_PTE_X 0x008u

/*
 * A small direct-mapped TLB is enough for the hot Nanos-lite/PAL working set and
 * keeps the lookup cheap: index = virtual-page-number & (size - 1). Collisions
 * simply overwrite older entries; correctness is preserved because each hit
 * also compares satp and the full virtual page number.
 */
#define RV32_FAST_TLB_SIZE 64u

#define RV32_FAST_INLINE static inline __attribute__((always_inline))

typedef struct
{
    /* satp is part of the tag so entries cannot leak between address spaces. */
    uint32_t satp;
    /* Virtual page number, excluding the 12-bit page offset. */
    uint32_t vpn;
    /* Cached R/W/X permission bits from the leaf PTE. */
    uint32_t perm;
    /* Physical base address of the translated 4 KiB page. */
    paddr_t pg_paddr;
    /*
   * Physical address of the level-0 page-table page that produced this entry.
   * Stores to that page may change the translation, so write paths use it to
   * conservatively flush the TLB.
   */
    paddr_t pt_page;
    /* Invalid entries are ignored even if the tag fields happen to match. */
    bool valid;
} rv32_fast_tlb_entry_t;

static rv32_fast_tlb_entry_t rv32_fast_tlb[RV32_FAST_TLB_SIZE];
/* Number of valid fast-TLB entries; lets Bare-mode stores skip an empty scan. */
static uint32_t rv32_fast_tlb_valid_count = 0;

/* Extract an inclusive bit range from an instruction or CSR value. */
RV32_FAST_INLINE uint32_t rv32_fast_bits(uint32_t value, int hi, int lo)
{
    return (value >> lo) & ((1u << (hi - lo + 1)) - 1u);
}

/*
 * Sign-extend a field that is already right-aligned. The xor/subtract form
 * avoids branches: flipping the sign bit moves the unsigned value around the
 * signed boundary, then subtracting the sign bit lands on the correct signed
 * result.
 */
RV32_FAST_INLINE sword_t rv32_fast_sext(uint32_t value, unsigned bits)
{
    const uint32_t sign = 1u << (bits - 1u);
    return (sword_t)((value ^ sign) - sign);
}

/* I-type immediate: instr[31:20], sign-extended to XLEN. */
RV32_FAST_INLINE sword_t rv32_fast_imm_i(uint32_t instr)
{
    return rv32_fast_sext(rv32_fast_bits(instr, 31, 20), 12);
}

/* S-type immediate: instr[31:25] forms the high bits, instr[11:7] the low bits. */
RV32_FAST_INLINE sword_t rv32_fast_imm_s(uint32_t instr)
{
    const uint32_t imm = rv32_fast_bits(instr, 11, 7) | (rv32_fast_bits(instr, 31, 25) << 5);
    return rv32_fast_sext(imm, 12);
}

/*
 * B-type branch offsets are encoded in a shuffled layout and are always
 * halfword-aligned, so bit 0 is implicitly zero. The fast path reconstructs the
 * byte offset before adding it to pc.
 */
RV32_FAST_INLINE sword_t rv32_fast_imm_b(uint32_t instr)
{
    const uint32_t imm = (rv32_fast_bits(instr, 11, 8) << 1) | (rv32_fast_bits(instr, 30, 25) << 5) | (rv32_fast_bits(instr, 7, 7) << 11) | (rv32_fast_bits(instr, 31, 31) << 12);
    return rv32_fast_sext(imm, 13);
}

/* U-type immediate is already placed in bits [31:12]. */
RV32_FAST_INLINE word_t rv32_fast_imm_u(uint32_t instr)
{
    return instr & 0xfffff000u;
}

/*
 * J-type jump offsets use the same idea as B-type offsets: the encoded pieces
 * are shuffled, bit 0 is implicit, and the final value is a signed pc-relative
 * byte offset.
 */
RV32_FAST_INLINE sword_t rv32_fast_imm_j(uint32_t instr)
{
    const uint32_t imm = (rv32_fast_bits(instr, 19, 12) << 12) | (rv32_fast_bits(instr, 20, 20) << 11) | (rv32_fast_bits(instr, 30, 21) << 1) | (rv32_fast_bits(instr, 31, 31) << 20);
    return rv32_fast_sext(imm, 21);
}

/* Preserve the RISC-V rule that writes to x0 are ignored. */
RV32_FAST_INLINE void rv32_fast_write_gpr(uint32_t rd, word_t value)
{
    if (rd != 0)
    {
        gpr(rd) = value;
    }
}

RV32_FAST_INLINE bool rv32_fast_is_naturally_aligned(vaddr_t addr, int len)
{
    return (addr & (vaddr_t)(len - 1)) == 0;
}

RV32_FAST_INLINE void rv32_fast_raise_trap(word_t cause, vaddr_t pc, word_t tval)
{
    cpu.pc = isa_raise_intr_tval(cause, pc, tval);
    difftest_skip_ref();
}

RV32_FAST_INLINE bool rv32_fast_csr_access_ok(word_t csr_addr, bool will_write)
{
    const word_t required_priv = (csr_addr >> 8) & 0x3u;
    return isCSRImplemented(csr_addr) &&
           cpu.prvi >= required_priv &&
           (!will_write || isCSRWriteable(csr_addr));
}

/*
 * Direct mode is the cheapest memory case. satp.MODE == Bare means no Sv32 page
 * walk is needed, so the fast path can treat a virtual address as a physical
 * address and only check whether it points inside PMEM before using host memory.
 */
RV32_FAST_INLINE bool rv32_fast_direct_mode()
{
    return likely((cpu.csr.satp & RV32_FAST_SATP_MODE_MASK) == 0);
}

/*
 * The fast paged path only translates accesses contained within one 4 KiB page.
 * Cross-page loads/stores are uncommon here and need two translations plus
 * careful exception/device handling, so they intentionally fall back.
 */
RV32_FAST_INLINE bool rv32_fast_cross_page(vaddr_t addr, int len)
{
    const word_t off = (word_t)(addr & PAGE_MASK);
    return off + (word_t)len > PAGE_SIZE;
}

/*
 * Verify that the whole physical byte range is backed by PMEM. Checking both
 * start and end also rejects wrap-around, because end must stay >= addr.
 */
RV32_FAST_INLINE bool rv32_fast_pmem_range(paddr_t addr, int len)
{
    const paddr_t end = addr + (paddr_t)len - 1u;
    return len > 0 && end >= addr && likely(in_pmem(addr) && in_pmem(end));
}

/*
 * Map NEMU's memory access kind to the Sv32 permission bit that must be present
 * on a leaf PTE. If a caller passes an unknown type, requiring no bit forces a
 * later conservative fallback rather than inventing new behaviour.
 */
RV32_FAST_INLINE uint32_t rv32_fast_required_perm(int type)
{
    switch (type)
    {
    case MEM_TYPE_IFETCH:
        return RV32_FAST_PTE_X;
    case MEM_TYPE_READ:
        return RV32_FAST_PTE_R;
    case MEM_TYPE_WRITE:
        return RV32_FAST_PTE_W;
    default:
        return 0;
    }
}

/*
 * Clearing the table is cheaper and less error-prone than tracking individual
 * invalidations. satp writes and suspected page-table stores are rare compared
 * with normal instruction/data accesses.
 */
RV32_FAST_INLINE void rv32_fast_tlb_flush()
{
    memset(rv32_fast_tlb, 0, sizeof(rv32_fast_tlb));
    rv32_fast_tlb_valid_count = 0;
}

/*
 * Translate a virtual address to a physical address for the fast memory path.
 *
 * The common cases are:
 *   1. Bare/direct mode: no page walk, return addr as paddr.
 *   2. TLB hit: combine cached physical page base with the 12-bit page offset.
 *   3. TLB miss: perform a minimal Sv32 two-level walk for a 4 KiB leaf page.
 *
 * Anything more subtle, such as superpages, invalid PTE combinations, MMIO, or
 * cross-page accesses, returns false so the existing NEMU memory path handles it
 * with the complete checks and side effects.
 */
RV32_FAST_INLINE bool rv32_fast_translate(vaddr_t addr, int len, int type, paddr_t *paddr)
{
    const uint32_t satp = cpu.csr.satp;

    /* In Bare mode virtual address == physical address. */

    if (rv32_fast_direct_mode())
    {
        *paddr = (paddr_t)addr;
        return true;
    }

    if (rv32_fast_cross_page(addr, len))
    {
        return false;
    }

    /*
   * The TLB is direct-mapped. Using a power-of-two size makes the modulo a cheap
   * mask, and the full tag comparison below keeps collisions correct.
   */
    const uint32_t need_perm = rv32_fast_required_perm(type);
    const uint32_t vpn = (uint32_t)(addr >> PAGE_SHIFT);
    const uint32_t idx = vpn & (RV32_FAST_TLB_SIZE - 1u);
    rv32_fast_tlb_entry_t *entry = &rv32_fast_tlb[idx];

    /*
   * A hit is valid only for the same satp, same virtual page, and an access type
   * allowed by the cached leaf permissions.
   */

    if (likely(entry->valid && entry->satp == satp && entry->vpn == vpn &&
               (entry->perm & need_perm) != 0))
    {
        *paddr = entry->pg_paddr | (paddr_t)(addr & PAGE_MASK);
        return true;
    }

    const paddr_t root =
        ((paddr_t)(satp & RV32_FAST_SATP_PPN_MASK)) << PAGE_SHIFT;
    const word_t vpn1 = (word_t)((addr >> 22) & 0x3ffu);
    const word_t vpn0 = (word_t)((addr >> 12) & 0x3ffu);
    const paddr_t pte1_addr = root + (paddr_t)(vpn1 * 4u);
    const uint32_t pte1 = (uint32_t)paddr_read(pte1_addr, 4);

    /* Invalid first-level PTEs need the full slow path for the usual handling. */

    if ((pte1 & RV32_FAST_PTE_V) == 0)
    {
        return false;
    }

    /*
   * This fast path only caches normal 4 KiB pages. A first-level leaf would be
   * an Sv32 superpage, so let the existing page-walk code deal with it.
   */
    const uint32_t pte1_rwx = pte1 & (RV32_FAST_PTE_R | RV32_FAST_PTE_W | RV32_FAST_PTE_X);

    if (pte1_rwx != 0)
    {
        return false;
    }

    const paddr_t l0_pt = ((paddr_t)(pte1 >> 10)) << PAGE_SHIFT;
    const paddr_t pte0_addr = l0_pt + (paddr_t)(vpn0 * 4u);
    const uint32_t pte0 = (uint32_t)paddr_read(pte0_addr, 4);

    /* The second-level PTE must be valid and must be a leaf. */

    if ((pte0 & RV32_FAST_PTE_V) == 0)
    {
        return false;
    }

    const uint32_t perm = pte0 & (RV32_FAST_PTE_R | RV32_FAST_PTE_W | RV32_FAST_PTE_X);

    if (perm == 0 || (perm & need_perm) == 0)
    {
        return false;
    }

    /*
   * Cache the translated physical page base, not the exact byte address. Future
   * accesses to the same 4 KiB virtual page then only need to restore the page
   * offset with bitwise OR.
   */
    const paddr_t pg_paddr = ((paddr_t)(pte0 >> 10)) << PAGE_SHIFT;
    if (!entry->valid)
    {
        rv32_fast_tlb_valid_count++;
    }

    *entry = (rv32_fast_tlb_entry_t){
        .satp = satp,
        .vpn = vpn,
        .perm = perm,
        .pg_paddr = pg_paddr,
        .pt_page = l0_pt,
        .valid = true,
    };

    *paddr = pg_paddr | (paddr_t)(addr & PAGE_MASK);
    return true;
}

/*
 * Decide whether a store might have changed a page-table page that backs a
 * cached translation. This is intentionally conservative: false positives only
 * cost a full TLB flush, while false negatives could execute with stale address
 * translations.
 */
RV32_FAST_INLINE bool rv32_fast_store_may_touch_page_table(paddr_t paddr)
{
    if (likely(rv32_fast_tlb_valid_count == 0))
    {
        return false;
    }

    const paddr_t page = paddr & ~(paddr_t)PAGE_MASK;

    /*
   * The fast TLB can hold entries for satp values that are not active right now.
   * A guest may edit an old page table while Bare mode or another address space
   * is active, then switch back later.  Scan all valid entries and compare the
   * physical page against both walk levels that can affect those entries.
   */
    for (size_t i = 0; i < RV32_FAST_TLB_SIZE; i++)
    {
        const rv32_fast_tlb_entry_t *entry = &rv32_fast_tlb[i];

        if (!entry->valid)
        {
            continue;
        }

        const paddr_t root =
            ((paddr_t)(entry->satp & RV32_FAST_SATP_PPN_MASK)) << PAGE_SHIFT;

        if (page == root || page == entry->pt_page)
        {
            return true;
        }
    }

    return false;
}

/*
 * Width-specific PMEM readers avoid the host_read(len) switch in the hottest
 * load paths. They still check the physical address against PMEM first; a false
 * result tells the caller to use the normal vaddr/paddr path, which is required
 * for MMIO and other device side effects.
 */
RV32_FAST_INLINE bool rv32_fast_read_pmem_u8(paddr_t paddr, word_t *data)
{
    /*
   * Byte loads cannot cross a PMEM boundary by width, so checking the starting
   * physical address is enough before dereferencing the host PMEM pointer.
   */

    if (!likely(in_pmem(paddr)))
    {
        return false;
    }

    *data = *(uint8_t *)guest_to_host(paddr);
    return true;
}

RV32_FAST_INLINE bool rv32_fast_read_pmem_u16(paddr_t paddr, word_t *data)
{
    /*
   * Halfword loads are used for LH/LHU. The cast deliberately lets the host do
   * the same little-endian unaligned access style already used by host_read().
   */

    if (!likely(in_pmem(paddr)))
    {
        return false;
    }

    *data = *(uint16_t *)guest_to_host(paddr);
    return true;
}

RV32_FAST_INLINE bool rv32_fast_read_pmem_u32(paddr_t paddr, word_t *data)
{
    /*
   * Word loads and instruction fetches are the most common hot-path memory
   * operations, so this removes the generic host_read(len) switch completely.
   */

    if (!likely(in_pmem(paddr)))
    {
        return false;
    }

    *data = *(uint32_t *)guest_to_host(paddr);
    return true;
}

/*
 * Paged fast accesses need both a successful Sv32 translation and a translated
 * physical range that stays fully inside PMEM. If either check fails, fallback
 * keeps the complete NEMU memory behaviour, including MMIO callbacks.
 */
RV32_FAST_INLINE bool rv32_fast_translate_pmem(vaddr_t addr, int len, int type, paddr_t *paddr)
{
    /*
   * rv32_fast_translate() may succeed for direct mode or translated mode. The
   * second predicate is still required because a valid translated physical
   * address can be MMIO, and MMIO must go through the full fallback path.
   */
    return rv32_fast_translate(addr, len, type, paddr) &&
           rv32_fast_pmem_range(*paddr, len);
}

/*
 * Width-specific virtual readers keep direct-mode accesses short while still
 * supporting translated Nanos-lite addresses through the fast TLB.
 */
RV32_FAST_INLINE bool rv32_fast_read_u8(vaddr_t addr, int type, word_t *data)
{
    /*
   * Bare mode is expected for AM benchmarks. In that case the virtual address is
   * the physical address, and the helper below only needs the PMEM bound check.
   */

    if (rv32_fast_direct_mode())
    {
        return rv32_fast_read_pmem_u8((paddr_t)addr, data);
    }

    paddr_t paddr = 0;
    return rv32_fast_translate_pmem(addr, 1, type, &paddr) &&
           rv32_fast_read_pmem_u8(paddr, data);
}

RV32_FAST_INLINE bool rv32_fast_read_u16(vaddr_t addr, int type, word_t *data)
{
    /* Same split as byte reads, specialised for the LH/LHU access width. */

    if (rv32_fast_direct_mode())
    {
        return rv32_fast_read_pmem_u16((paddr_t)addr, data);
    }

    paddr_t paddr = 0;
    return rv32_fast_translate_pmem(addr, 2, type, &paddr) &&
           rv32_fast_read_pmem_u16(paddr, data);
}

RV32_FAST_INLINE bool rv32_fast_read_u32(vaddr_t addr, int type, word_t *data)
{
    /*
   * This serves both LW and normal 32-bit RISC-V instruction fetches. Keeping it
   * width-specific avoids a switch in the common direct-PMEM case.
   */

    if (rv32_fast_direct_mode())
    {
        return rv32_fast_read_pmem_u32((paddr_t)addr, data);
    }

    paddr_t paddr = 0;
    return rv32_fast_translate_pmem(addr, 4, type, &paddr) &&
           rv32_fast_read_pmem_u32(paddr, data);
}

/*
 * Width-specific PMEM writers mirror the readers: direct host stores for normal
 * RAM, false for anything outside PMEM so device writes are not swallowed.
 */
RV32_FAST_INLINE bool rv32_fast_write_pmem_u8(paddr_t paddr, word_t data)
{
    /*
   * Store helpers truncate naturally through the target pointer type, matching
   * host_write() for SB/SH/SW without carrying a runtime length argument.
   */

    if (!likely(in_pmem(paddr)))
    {
        return false;
    }

    const bool flush_tlb = rv32_fast_store_may_touch_page_table(paddr);
    *(uint8_t *)guest_to_host(paddr) = data;
    if (unlikely(isa_jit_invalidation_active))
    {
        isa_jit_invalidate_paddr(paddr, 1);
    }

    if (unlikely(flush_tlb))
    {
        rv32_fast_tlb_flush();
    }
    return true;
}

RV32_FAST_INLINE bool rv32_fast_write_pmem_u16(paddr_t paddr, word_t data)
{
    /* Halfword store for SH; falls back before dereference if the address is MMIO. */

    if (!likely(in_pmem(paddr)))
    {
        return false;
    }

    const bool flush_tlb = rv32_fast_store_may_touch_page_table(paddr);
    *(uint16_t *)guest_to_host(paddr) = data;
    if (unlikely(isa_jit_invalidation_active))
    {
        isa_jit_invalidate_paddr(paddr, 2);
    }

    if (unlikely(flush_tlb))
    {
        rv32_fast_tlb_flush();
    }
    return true;
}

RV32_FAST_INLINE bool rv32_fast_write_pmem_u32(paddr_t paddr, word_t data)
{
    /* Word store for SW, the dominant benchmark store width. */

    if (!likely(in_pmem(paddr)))
    {
        return false;
    }

    const bool flush_tlb = rv32_fast_store_may_touch_page_table(paddr);
    *(uint32_t *)guest_to_host(paddr) = data;
    if (unlikely(isa_jit_invalidation_active))
    {
        isa_jit_invalidate_paddr(paddr, 4);
    }

    if (unlikely(flush_tlb))
    {
        rv32_fast_tlb_flush();
    }
    return true;
}

/*
 * Width-specific virtual writers keep the direct-mode path branch-light.  The
 * final PMEM writer owns JIT source invalidation and fast-TLB page-table checks,
 * so Bare and translated callers share the same post-write safety rules.
 */
RV32_FAST_INLINE bool rv32_fast_write_u8(vaddr_t addr, word_t data)
{
    /*
   * In Bare mode this is just SB to PMEM plus the same JIT/fast-TLB write
   * notification used after translated PMEM stores.
   */

    if (rv32_fast_direct_mode())
    {
        return rv32_fast_write_pmem_u8((paddr_t)addr, data);
    }

    paddr_t paddr = 0;

    if (!rv32_fast_translate_pmem(addr, 1, MEM_TYPE_WRITE, &paddr))
    {
        return false;
    }

    return rv32_fast_write_pmem_u8(paddr, data);
}

RV32_FAST_INLINE bool rv32_fast_write_u16(vaddr_t addr, word_t data)
{
    /* SH version of rv32_fast_write_u8(), with the same fallback and TLB rules. */

    if (rv32_fast_direct_mode())
    {
        return rv32_fast_write_pmem_u16((paddr_t)addr, data);
    }

    paddr_t paddr = 0;

    if (!rv32_fast_translate_pmem(addr, 2, MEM_TYPE_WRITE, &paddr))
    {
        return false;
    }

    return rv32_fast_write_pmem_u16(paddr, data);
}

RV32_FAST_INLINE bool rv32_fast_write_u32(vaddr_t addr, word_t data)
{
    /*
   * SW is important for stack traffic and structure copies. Page-table writes
   * are detected before the store, but the flush happens after the new word is
   * visible so the next walk observes the updated PTE.
   */

    if (rv32_fast_direct_mode())
    {
        return rv32_fast_write_pmem_u32((paddr_t)addr, data);
    }

    paddr_t paddr = 0;

    if (!rv32_fast_translate_pmem(addr, 4, MEM_TYPE_WRITE, &paddr))
    {
        return false;
    }

    return rv32_fast_write_pmem_u32(paddr, data);
}

/*
 * Public read wrapper used by instruction execution. It first tries the fast
 * path and falls back to vaddr_ifetch/vaddr_read when the access is MMIO,
 * cross-page, unsupported by the minimal page walk, or otherwise unsafe.
 */
RV32_FAST_INLINE word_t rv32_fast_read_or_fallback(vaddr_t addr, int len, int type)
{
    word_t data = 0;

    /*
   * len is normally a compile-time constant at each call site. After inlining,
   * GCC can keep only the matching case, which gives separate hot code for
   * byte, halfword, and word accesses without changing the public helper API.
   */
    switch (len)
    {
    case 1:
        if (rv32_fast_read_u8(addr, type, &data))
            return data;
        break;
    case 2:
        if (rv32_fast_read_u16(addr, type, &data))
            return data;
        break;
    case 4:
        if (rv32_fast_read_u32(addr, type, &data))
            return data;
        break;
    default:
        break;
    }

    return type == MEM_TYPE_IFETCH ? vaddr_ifetch(addr, len) : vaddr_read(addr, len);
}

/*
 * Public write wrapper used by store instructions. The slow fallback keeps all
 * normal NEMU behaviour for MMIO, device callbacks, and edge cases that this
 * fast PMEM-only path deliberately rejects.
 */
RV32_FAST_INLINE void rv32_fast_write_or_fallback(vaddr_t addr, int len, word_t data)
{
    bool ok = false;

    /*
   * Store widths are also constant at call sites. Unsupported widths are not
   * guessed here; they go to vaddr_write(), preserving the slow-path behaviour
   * and the central PMEM/JIT invalidation hooks used outside this fast path.
   */
    switch (len)
    {
    case 1:
        ok = rv32_fast_write_u8(addr, data);
        break;
    case 2:
        ok = rv32_fast_write_u16(addr, data);
        break;
    case 4:
        ok = rv32_fast_write_u32(addr, data);
        break;
    default:
        break;
    }

    if (!ok)
    {
        vaddr_write(addr, len, data);
        /*
     * The rejected fast path may still have written ordinary PMEM through the
     * normal memory layer.  paddr_write() notifies the JIT source/TLB hooks when
     * it sees the final physical address; the fast executor has its own small TLB,
     * so it also drops local translations before the next fast memory access.
     */
        rv32_fast_tlb_flush();
    }
}

/*
 * satp is CSR 0x180. Changing it can replace the whole active address space, so
 * cached virtual-to-physical translations become suspect and must be dropped.
 * JIT blocks are keyed by satp and re-check their physical source mapping before
 * reuse, so a normal context switch does not need to throw away the native-code
 * arena.
 */
RV32_FAST_INLINE void rv32_fast_tlb_flush_if_satp(word_t csr_addr)
{
    if (unlikely(csr_addr == 0x180u))
    {
        rv32_fast_tlb_flush();
    }
}

/*
 * The AM nemu_trap instruction is a synthetic stop point, not a standard RISC-V
 * opcode. The fast path mirrors the slow helper: report the halt pc, return
 * value in a0, and advance pc past the trap instruction.
 */
RV32_FAST_INLINE void rv32_fast_nemu_trap(vaddr_t pc)
{
    nemu_state.state = NEMU_END;
    nemu_state.halt_pc = pc;
    nemu_state.halt_ret = gpr(10);
    cpu.pc = pc + 4;
}

/*
 * Minimal MRET implementation for the fast path. It restores the previous
 * privilege level from mstatus.MPP, moves MPIE back to MIE, sets MPIE, and then
 * resumes at mepc, matching the behaviour needed by the bare-metal runtime.
 */
RV32_FAST_INLINE void rv32_fast_mret(vaddr_t pc)
{
    if (cpu.prvi != RISCV32_PRIV_M)
    {
        rv32_fast_raise_trap(RISCV32_CAUSE_ILLEGAL_INST, pc, 0);
        return;
    }

    word_t mstatus = cpu.csr.mstatus;
    word_t mpp = (mstatus >> 11) & 0x3u;
    word_t mpie = (mstatus >> 7) & 0x1u;

    mstatus &= ~((word_t)0x3u << 11);
    mstatus = (mstatus & ~((word_t)1u << 3)) | (mpie << 3);
    mstatus |= ((word_t)1u << 7);

    if (mpp != 0x3u)
    {
        mstatus &= ~((word_t)1u << 17);
    }

    cpu.csr.mstatus = mstatus;
    cpu.prvi = mpp;
    cpu.pc = cpu.csr.mepc;
}

/*
 * Fast implementation of RISC-V32 loads. Address calculation still follows the
 * ISA exactly: rs1 plus the sign-extended I-immediate. The memory helper chooses
 * fast PMEM access or the normal fallback, then the case arm applies the correct
 * sign or zero extension for LB/LH/LW/LBU/LHU.
 */
RV32_FAST_INLINE bool rv32_fast_exec_load(uint32_t instr, vaddr_t pc)
{
    const uint32_t rd = rv32_fast_bits(instr, 11, 7);
    const uint32_t funct3 = rv32_fast_bits(instr, 14, 12);
    const uint32_t rs1 = rv32_fast_bits(instr, 19, 15);
    const vaddr_t addr = gpr(rs1) + rv32_fast_imm_i(instr);

    switch (funct3)
    {
    case 0x0:
        rv32_fast_write_gpr(rd, (sword_t)(int8_t)rv32_fast_read_or_fallback(addr, 1, MEM_TYPE_READ));
        break;
    case 0x1:
        if (!rv32_fast_is_naturally_aligned(addr, 2))
        {
            rv32_fast_raise_trap(RISCV32_CAUSE_LOAD_ADDR_MISALIGNED, pc, addr);
            return true;
        }
        rv32_fast_write_gpr(rd, (sword_t)(int16_t)rv32_fast_read_or_fallback(addr, 2, MEM_TYPE_READ));
        break;
    case 0x2:
        if (!rv32_fast_is_naturally_aligned(addr, 4))
        {
            rv32_fast_raise_trap(RISCV32_CAUSE_LOAD_ADDR_MISALIGNED, pc, addr);
            return true;
        }
        rv32_fast_write_gpr(rd, rv32_fast_read_or_fallback(addr, 4, MEM_TYPE_READ));
        break;
    case 0x4:
        rv32_fast_write_gpr(rd, rv32_fast_read_or_fallback(addr, 1, MEM_TYPE_READ));
        break;
    case 0x5:
        if (!rv32_fast_is_naturally_aligned(addr, 2))
        {
            rv32_fast_raise_trap(RISCV32_CAUSE_LOAD_ADDR_MISALIGNED, pc, addr);
            return true;
        }
        rv32_fast_write_gpr(rd, rv32_fast_read_or_fallback(addr, 2, MEM_TYPE_READ));
        break;
    default:
        return false;
    }

    cpu.pc = pc + 4;
    return true;
}

/*
 * Fast implementation of stores. S-type immediates are split across the
 * instruction word, so rv32_fast_imm_s() rebuilds the signed offset before the
 * store width selects SB/SH/SW.
 */
RV32_FAST_INLINE bool rv32_fast_exec_store(uint32_t instr, vaddr_t pc)
{
    const uint32_t funct3 = rv32_fast_bits(instr, 14, 12);
    const uint32_t rs1 = rv32_fast_bits(instr, 19, 15);
    const uint32_t rs2 = rv32_fast_bits(instr, 24, 20);
    const vaddr_t addr = gpr(rs1) + rv32_fast_imm_s(instr);

    switch (funct3)
    {
    case 0x0:
        rv32_fast_write_or_fallback(addr, 1, gpr(rs2));
        break;
    case 0x1:
        if (!rv32_fast_is_naturally_aligned(addr, 2))
        {
            rv32_fast_raise_trap(RISCV32_CAUSE_STORE_ADDR_MISALIGNED, pc, addr);
            return true;
        }
        rv32_fast_write_or_fallback(addr, 2, gpr(rs2));
        break;
    case 0x2:
        if (!rv32_fast_is_naturally_aligned(addr, 4))
        {
            rv32_fast_raise_trap(RISCV32_CAUSE_STORE_ADDR_MISALIGNED, pc, addr);
            return true;
        }
        rv32_fast_write_or_fallback(addr, 4, gpr(rs2));
        break;
    default:
        return false;
    }

    cpu.pc = pc + 4;
    return true;
}

/*
 * Fast OP-IMM execution. These ALU operations have no memory or device side
 * effects, so executing them directly avoids the table-driven decoder while
 * keeping unsupported encodings on the slow path by returning false.
 */
RV32_FAST_INLINE bool rv32_fast_exec_op_imm(uint32_t instr, vaddr_t pc)
{
    const uint32_t rd = rv32_fast_bits(instr, 11, 7);
    const uint32_t funct3 = rv32_fast_bits(instr, 14, 12);
    const uint32_t rs1 = rv32_fast_bits(instr, 19, 15);
    const word_t src = gpr(rs1);
    const sword_t imm = rv32_fast_imm_i(instr);

    switch (funct3)
    {
    case 0x0:
        rv32_fast_write_gpr(rd, src + imm);
        break;
    case 0x2:
        rv32_fast_write_gpr(rd, (sword_t)src < imm);
        break;
    case 0x3:
        rv32_fast_write_gpr(rd, src < (word_t)imm);
        break;
    case 0x4:
        rv32_fast_write_gpr(rd, src ^ (word_t)imm);
        break;
    case 0x6:
        rv32_fast_write_gpr(rd, src | (word_t)imm);
        break;
    case 0x7:
        rv32_fast_write_gpr(rd, src & (word_t)imm);
        break;
    case 0x1:
        /*
       * Only shift-immediate instructions look at funct7. The other OP-IMM
       * forms, especially ADDI used by li/mv and stack adjustment, avoid this
       * extraction on their hot path.
       */

        if (rv32_fast_bits(instr, 31, 25) != 0x00)
            return false;
        rv32_fast_write_gpr(rd, src << rv32_fast_bits(instr, 24, 20));
        break;
    case 0x5:
    {
        const uint32_t funct7 = rv32_fast_bits(instr, 31, 25);

        if (funct7 == 0x00)
        {
            rv32_fast_write_gpr(rd, src >> rv32_fast_bits(instr, 24, 20));
        }
        else if (funct7 == 0x20)
        {
            rv32_fast_write_gpr(rd, (word_t)((sword_t)src >> rv32_fast_bits(instr, 24, 20)));
        }
        else
        {
            return false;
        }
    }
    break;
    default:
        return false;
    }

    cpu.pc = pc + 4;
    return true;
}

/*
 * MULHSU returns the high 32 bits of signed(lhs) * unsigned(rhs). The casts make
 * the two operand interpretations explicit before the 64-bit multiply.
 */
RV32_FAST_INLINE word_t rv32_fast_mulhsu(word_t lhs, word_t rhs)
{
    return (uint64_t)((int64_t)(sword_t)lhs * (uint64_t)rhs) >> 32;
}

/*
 * Fast OP execution covers the common integer register-register operations and
 * the RV32M multiply/divide cases used by the benchmarks. The key combines
 * funct7 and funct3 so the switch can distinguish ADD/SUB, SRL/SRA, and the M
 * extension operations without nested decoding.
 */
RV32_FAST_INLINE bool rv32_fast_exec_op(uint32_t instr, vaddr_t pc)
{
    const uint32_t rd = rv32_fast_bits(instr, 11, 7);
    const uint32_t funct3 = rv32_fast_bits(instr, 14, 12);
    const uint32_t rs1 = rv32_fast_bits(instr, 19, 15);
    const uint32_t rs2 = rv32_fast_bits(instr, 24, 20);
    const uint32_t funct7 = rv32_fast_bits(instr, 31, 25);
    const uint32_t key = (funct7 << 3) | funct3;
    const word_t lhs = gpr(rs1);
    const word_t rhs = gpr(rs2);

    switch (key)
    {
    case 0x000:
        rv32_fast_write_gpr(rd, lhs + rhs);
        break;
    case 0x100:
        rv32_fast_write_gpr(rd, lhs - rhs);
        break;
    case 0x001:
        rv32_fast_write_gpr(rd, lhs << (rhs & 0x1fu));
        break;
    case 0x002:
        rv32_fast_write_gpr(rd, (sword_t)lhs < (sword_t)rhs);
        break;
    case 0x003:
        rv32_fast_write_gpr(rd, lhs < rhs);
        break;
    case 0x004:
        rv32_fast_write_gpr(rd, lhs ^ rhs);
        break;
    case 0x005:
        rv32_fast_write_gpr(rd, lhs >> (rhs & 0x1fu));
        break;
    case 0x105:
        rv32_fast_write_gpr(rd, (word_t)((sword_t)lhs >> (rhs & 0x1fu)));
        break;
    case 0x006:
        rv32_fast_write_gpr(rd, lhs | rhs);
        break;
    case 0x007:
        rv32_fast_write_gpr(rd, lhs & rhs);
        break;
    case 0x008:
        rv32_fast_write_gpr(rd, lhs * rhs);
        break;
    case 0x009:
        rv32_fast_write_gpr(rd, ((int64_t)(sword_t)lhs * (int64_t)(sword_t)rhs) >> 32);
        break;
    case 0x00a:
        rv32_fast_write_gpr(rd, rv32_fast_mulhsu(lhs, rhs));
        break;
    case 0x00b:
        rv32_fast_write_gpr(rd, ((uint64_t)lhs * (uint64_t)rhs) >> 32);
        break;
    case 0x00c:
        if ((sword_t)rhs == 0)
            rv32_fast_write_gpr(rd, (word_t)-1);
        else if ((sword_t)lhs == (sword_t)(1ULL << 31) && (sword_t)rhs == -1)
            rv32_fast_write_gpr(rd, lhs);
        else
            rv32_fast_write_gpr(rd, (word_t)((sword_t)lhs / (sword_t)rhs));
        break;
    case 0x00d:
        rv32_fast_write_gpr(rd, rhs == 0 ? (word_t)-1 : lhs / rhs);
        break;
    case 0x00e:
        if ((sword_t)rhs == 0)
            rv32_fast_write_gpr(rd, lhs);
        else if ((sword_t)lhs == (sword_t)(1ULL << 31) && (sword_t)rhs == -1)
            rv32_fast_write_gpr(rd, 0);
        else
            rv32_fast_write_gpr(rd, (word_t)((sword_t)lhs % (sword_t)rhs));
        break;
    case 0x00f:
        rv32_fast_write_gpr(rd, rhs == 0 ? lhs : lhs % rhs);
        break;
    default:
        return false;
    }

    cpu.pc = pc + 4;
    return true;
}

/*
 * Fast branch execution compares rs1/rs2 according to funct3. When taken, the
 * sign-extended B-immediate is added to the current pc; otherwise execution
 * continues with the next 4-byte instruction.
 */
RV32_FAST_INLINE bool rv32_fast_exec_branch(uint32_t instr, vaddr_t pc)
{
    const uint32_t funct3 = rv32_fast_bits(instr, 14, 12);
    const uint32_t rs1 = rv32_fast_bits(instr, 19, 15);
    const uint32_t rs2 = rv32_fast_bits(instr, 24, 20);
    const word_t lhs = gpr(rs1);
    const word_t rhs = gpr(rs2);
    bool taken = false;

    switch (funct3)
    {
    case 0x0:
        taken = lhs == rhs;
        break;
    case 0x1:
        taken = lhs != rhs;
        break;
    case 0x4:
        taken = (sword_t)lhs < (sword_t)rhs;
        break;
    case 0x5:
        taken = (sword_t)lhs >= (sword_t)rhs;
        break;
    case 0x6:
        taken = lhs < rhs;
        break;
    case 0x7:
        taken = lhs >= rhs;
        break;
    default:
        return false;
    }

    const vaddr_t target = pc + rv32_fast_imm_b(instr);

    if (taken && ((target & 0x3u) != 0))
    {
        rv32_fast_raise_trap(RISCV32_CAUSE_INST_ADDR_MISALIGNED, pc, target);
        return true;
    }

    cpu.pc = taken ? target : pc + 4;
    return true;
}

/*
 * Fast SYSTEM execution handles the subset used by this project: ECALL, MRET,
 * and the standard CSR read/write/set/clear forms. Unsupported SYSTEM encodings
 * return false so the normal interpreter can perform complete handling.
 */
RV32_FAST_INLINE bool rv32_fast_exec_system(uint32_t instr, vaddr_t pc)
{
    const uint32_t rd = rv32_fast_bits(instr, 11, 7);
    const uint32_t funct3 = rv32_fast_bits(instr, 14, 12);
    const uint32_t rs1 = rv32_fast_bits(instr, 19, 15);
    const word_t rs1_value = gpr(rs1);
    const word_t csr_addr = rv32_fast_bits(instr, 31, 20);
    rtlreg_t *csr = NULL;
    word_t old = 0;
    bool will_write = false;

    if (funct3 == 0)
    {
        switch (instr)
        {
        case 0x00000073u:
            /*
             * The syscall ABI may use a7 as a payload register, but the
             * architectural trap cause comes from the current privilege mode.
             */
            rv32_fast_raise_trap(riscv32_ecall_cause_from_priv(cpu.prvi), pc, 0);
            return true;
        case 0x00100073u:
            rv32_fast_raise_trap(RISCV32_CAUSE_BREAKPOINT, pc, 0);
            return true;
        case 0x30200073u:
            rv32_fast_mret(pc);
            return true;
        default:
            return false;
        }
    }

    if (funct3 != 0x1 && funct3 != 0x2 && funct3 != 0x3 &&
        funct3 != 0x5 && funct3 != 0x6 && funct3 != 0x7)
    {
        return false;
    }

    will_write = funct3 == 0x1 || funct3 == 0x5 ||
                 ((funct3 == 0x2 || funct3 == 0x3 ||
                   funct3 == 0x6 || funct3 == 0x7) &&
                  rs1 != 0);

    if (!rv32_fast_csr_access_ok(csr_addr, will_write))
    {
        rv32_fast_raise_trap(RISCV32_CAUSE_ILLEGAL_INST, pc, 0);
        return true;
    }

    csr = getCSRAddress(csr_addr);

    switch (funct3)
    {
    case 0x1:
        /*
         * CSRRW with rd=x0 still writes but suppresses the architectural CSR
         * read side effects. The implemented CSRs are side-effect-free today,
         * but preserving the rule keeps this path aligned with the ISA.
         */
        if (rd != 0)
        {
            old = *csr;
            rv32_fast_write_gpr(rd, old);
        }
        *csr = rs1_value;
        rv32_fast_tlb_flush_if_satp(csr_addr);
        break;
    case 0x2:
        /* CSRRS writes only when rs1 is not x0; reads still return the old value. */
        old = *csr;
        rv32_fast_write_gpr(rd, old);

        if (rs1 != 0)
        {
            *csr = old | rs1_value;
            rv32_fast_tlb_flush_if_satp(csr_addr);
        }
        break;
    case 0x3:
        /* CSRRC clears only the bits present in rs1, again only when rs1 is not x0. */
        old = *csr;
        rv32_fast_write_gpr(rd, old);

        if (rs1 != 0)
        {
            *csr = old & ~rs1_value;
            rv32_fast_tlb_flush_if_satp(csr_addr);
        }
        break;
    case 0x5:
        /* Immediate CSR form: zimm is encoded in the rs1 field. */
        if (rd != 0)
        {
            old = *csr;
            rv32_fast_write_gpr(rd, old);
        }
        *csr = rs1;
        rv32_fast_tlb_flush_if_satp(csr_addr);
        break;
    case 0x6:
        /* CSRRSI sets bits from the immediate value when zimm is non-zero. */
        old = *csr;
        rv32_fast_write_gpr(rd, old);

        if (rs1 != 0)
        {
            *csr = old | rs1;
            rv32_fast_tlb_flush_if_satp(csr_addr);
        }
        break;
    case 0x7:
        /* CSRRCI clears bits from the immediate value when zimm is non-zero. */
        old = *csr;
        rv32_fast_write_gpr(rd, old);

        if (rs1 != 0)
        {
            *csr = old & ~(word_t)rs1;
            rv32_fast_tlb_flush_if_satp(csr_addr);
        }
        break;
    default:
        return false;
    }

    cpu.pc = pc + 4;
    return true;
}

/*
 * Execute one instruction through the fast RISC-V32 decoder. Returning true
 * means the instruction was fully handled and cpu.pc was updated. Returning
 * false hands control back to the normal interpreter for rare or unsupported
 * instructions, preserving correctness while keeping the hot path compact.
 */
RV32_FAST_INLINE bool isa_fast_exec_once()
{
    const vaddr_t pc = cpu.pc;
    const uint32_t instr = rv32_fast_read_or_fallback(pc, 4, MEM_TYPE_IFETCH);
    const uint32_t opcode = instr & 0x7fu;
    const uint32_t rd = rv32_fast_bits(instr, 11, 7);
    const uint32_t rs1 = rv32_fast_bits(instr, 19, 15);

    switch (opcode)
    {
    case 0x03:
        return rv32_fast_exec_load(instr, pc);
    case 0x13:
        return rv32_fast_exec_op_imm(instr, pc);
    case 0x17:
        rv32_fast_write_gpr(rd, pc + rv32_fast_imm_u(instr));
        cpu.pc = pc + 4;
        return true;
    case 0x23:
        return rv32_fast_exec_store(instr, pc);
    case 0x33:
        return rv32_fast_exec_op(instr, pc);
    case 0x37:
        rv32_fast_write_gpr(rd, rv32_fast_imm_u(instr));
        cpu.pc = pc + 4;
        return true;
    case 0x63:
        return rv32_fast_exec_branch(instr, pc);
    case 0x67:
        if (rv32_fast_bits(instr, 14, 12) != 0)
            return false;
        {
            const vaddr_t target = (gpr(rs1) + rv32_fast_imm_i(instr)) & ~(vaddr_t)1u;
            if ((target & 0x3u) != 0)
            {
                rv32_fast_raise_trap(RISCV32_CAUSE_INST_ADDR_MISALIGNED, pc, target);
                return true;
            }
            rv32_fast_write_gpr(rd, pc + 4);
            cpu.pc = target;
        }
        return true;
    case 0x6f:
    {
        const vaddr_t target = pc + rv32_fast_imm_j(instr);
        if ((target & 0x3u) != 0)
        {
            rv32_fast_raise_trap(RISCV32_CAUSE_INST_ADDR_MISALIGNED, pc, target);
            return true;
        }
        rv32_fast_write_gpr(rd, pc + 4);
        cpu.pc = target;
        return true;
    }
    case 0x73:
        return rv32_fast_exec_system(instr, pc);
    case 0x6b:
        rv32_fast_nemu_trap(pc);
        return true;
    default:
        return false;
    }
}

/*
 * Execute a bounded run of fast-path instructions before returning to the outer
 * CPU loop for device and interrupt accounting.  Each iteration still calls the
 * strict single-instruction executor, so traps, CSR checks, and alignment checks
 * happen at the same architectural point.  The batch stops as soon as an
 * unsupported instruction needs the interpreter, NEMU leaves RUNNING state, or
 * an interrupt has been latched.
 */
RV32_FAST_INLINE uint32_t isa_fast_exec_batch(uint32_t budget)
{
    uint32_t done = 0;

    while (done < budget)
    {
        if (!isa_fast_exec_once())
        {
            break;
        }

        done++;

        if (unlikely(nemu_state.state != NEMU_RUNNING || cpu.INTR))
        {
            break;
        }
    }

    return done;
}

#endif
