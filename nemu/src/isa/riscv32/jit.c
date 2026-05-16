#include <isa-jit.h>
#include <isa.h>
#include <memory/host.h>
#include <memory/paddr.h>
#include <memory/vaddr.h>
#include <utils.h>
#include "local-include/reg.h"

#include <stddef.h>
#include <stdlib.h>

/*
 * RISC-V32 JIT design overview
 * ----------------------------
 *
 * This file translates short RV32I/RV32M basic blocks into x86-64 machine code.
 * The JIT is intentionally an optimisation layer around the existing NEMU
 * interpreter semantics, not a separate CPU model. Whenever a case may involve
 * guest-visible side effects that are hard to prove locally, the emitted code
 * falls back to the normal memory or execution helpers.
 *
 * The execution path has five main stages:
 *
 * 1. Availability check:
 *    `isa_jit_available()` enables the JIT only on x86-64 native ELF builds
 *    with tracing, watchpoints, memory/function tracing, and DiffTest disabled.
 *    Those features need per-instruction interpreter hooks, so the JIT stays
 *    out of their way.
 *
 * 2. Dispatch:
 *    `isa_jit_exec()` is called by the CPU loop with a guest-instruction budget.
 *    It looks up a cache entry by `(pc, satp)`, compiles a block on a miss, then
 *    calls the block's native entry point. The native block returns the number
 *    of guest instructions that completed, which keeps device polling and timer
 *    work bounded.
 *
 * 3. Translation:
 *    `jit_compile_block()` decodes one RV32 instruction at a time. Straight-line
 *    ALU, load, store, branch, and jump cases are emitted directly. Unsupported
 *    instructions are marked with a negative cache entry so the same slow path is
 *    not repeatedly recompiled.
 *
 * 4. Register caching:
 *    Five callee-saved host registers (`rbx`, `r12`, `r13`, `r14`, `r15`) cache
 *    hot guest registers inside one translated block. A dirty cached register is
 *    flushed to `cpu.gpr[]` before any helper call or block exit that can observe
 *    full CPU state.
 *
 * 5. Memory safety and invalidation:
 *    Direct PMEM access is used only when the JIT can prove that the guest
 *    access maps to ordinary RAM and does not require MMIO or complex exception
 *    behaviour. Bare mode can be checked directly; simple Sv32 leaf translations
 *    may use the local JIT TLB. Each compiled block records the physical PMEM
 *    bytes that supplied its source instructions. Writes from the interpreter,
 *    JIT helpers, disk DMA, or full flushes invalidate overlapping native blocks
 *    before stale code can run.
 *
 * The comments below are deliberately explicit about small arithmetic steps.
 * For example, sign extension and relative-branch patching are written out
 * because off-by-one or wrong-origin mistakes in a JIT are difficult to debug.
 *
 * ISA/EEI policy note:
 * The JIT must stay behind the same architectural boundary as the interpreter.
 * Instructions whose trap behaviour depends on runtime addresses, such as
 * scalar memory accesses and JALR, deliberately fall back to the strict fast or
 * interpreter path unless the native emitter can prove the same trap ordering.
 */

#if defined(__x86_64__) && defined(CONFIG_RV32_JIT) && \
    defined(CONFIG_TARGET_NATIVE_ELF) && !defined(CONFIG_TRACE) && \
    !defined(CONFIG_DIFFTEST) && !defined(CONFIG_WATCHPOINT) && \
    !defined(CONFIG_MTRACE) && !defined(CONFIG_FTRACE)
#define RV32_JIT_ENABLED 1
#include <sys/mman.h>
#include <unistd.h>
#else
#define RV32_JIT_ENABLED 0
#endif

#define RV32_JIT_BLOCK_MAX_INSNS 32u
/*
 * Match the CPU device polling interval so one isa_jit_exec() call can consume
 * many short cached blocks before returning.  The cap is still bounded and
 * device_update() remains time-gated to TIMER_HZ, so this removes avoidable
 * dispatcher churn without letting native code run without limits.
 */
#define RV32_JIT_BATCH_MAX_INSNS 65536u
#define RV32_JIT_CACHE_SIZE 262144u
#define RV32_JIT_CODE_SIZE (256u * 1024u * 1024u)
#define RV32_JIT_CODE_ALIGN 16u
/*
 * Store continuation needs to know whether a write can touch translated source
 * bytes.  A whole 4 KiB page is too coarse for small AM images, where .text,
 * .rodata, .data and .bss can share one page.  Track source ownership in
 * 128-byte chunks instead: this is still cheap, but it avoids forcing normal
 * data stores in the same page as code through the helper-and-exit path.
 */
#define RV32_JIT_SOURCE_CHUNK_SHIFT 7u
#define RV32_JIT_SOURCE_CHUNK_SIZE (1u << RV32_JIT_SOURCE_CHUNK_SHIFT)
#define RV32_JIT_SOURCE_CHUNK_MASK (RV32_JIT_SOURCE_CHUNK_SIZE - 1u)
#define RV32_JIT_PMEM_CHUNK_COUNT \
    (((size_t)CONFIG_MSIZE + (size_t)RV32_JIT_SOURCE_CHUNK_SIZE - 1u) / \
     (size_t)RV32_JIT_SOURCE_CHUNK_SIZE)
#define RV32_JIT_SATP_MODE_MASK 0x80000000u
#define RV32_JIT_SATP_PPN_MASK 0x003fffffu
#define RV32_JIT_PTE_V 0x001u
#define RV32_JIT_PTE_R 0x002u
#define RV32_JIT_PTE_W 0x004u
#define RV32_JIT_PTE_X 0x008u
#define RV32_JIT_TLB_SIZE 128u
#define RV32_JIT_PMEM_PAGE_COUNT \
    (((size_t)CONFIG_MSIZE + (size_t)PAGE_SIZE - 1u) / (size_t)PAGE_SIZE)
#ifdef CONFIG_RV32_JIT_STATS
#define RV32_JIT_STATS 1
#else
#define RV32_JIT_STATS 0
#endif

typedef uint32_t (*rv32_jit_entry_t)(void);

typedef struct
{
    /* satp separates address spaces that reuse the same virtual page number. */
    uint32_t satp;
    /* Virtual page number, excluding the 12-bit page offset. */
    uint32_t vpn;
    /* Cached R/W/X permission bits from the Sv32 leaf PTE. */
    uint32_t perm;
    /* Physical base address of the translated 4 KiB page. */
    paddr_t pg_paddr;
    /* Level-0 page-table page used by the walk; stores here may stale the entry. */
    paddr_t pt_page;
    bool valid;
    /*
   * Keep each entry at 32 bytes so generated x86 can compute
   * `&jit_tlb[index]` with one shift instead of an imul.
   */
    uint8_t pad[11];
} rv32_jit_tlb_entry_t;

typedef char rv32_jit_tlb_entry_size_must_be_32[sizeof(rv32_jit_tlb_entry_t) == 32 ? 1 : -1];
typedef char rv32_jit_pmem_mapping_must_be_page_aligned[((CONFIG_MBASE | CONFIG_MSIZE) & PAGE_MASK) == 0 ? 1 : -1];

typedef struct
{
    /* First byte of the native code being emitted for the current block. */
    uint8_t *start;
    /* Next free byte in the code arena; every emit helper advances this pointer. */
    uint8_t *cur;
    /* One-past-last byte available to this writer; emit helpers fail at this bound. */
    uint8_t *end;
} rv32_jit_writer_t;

typedef struct
{
    /* True when this cache slot contains either native code or an unsupported marker. */
    bool valid;
    /* Guest virtual PC that starts this translated block. */
    vaddr_t pc;
    /* Address-space tag; Sv32 can map the same virtual PC to different PMEM bytes. */
    word_t satp;
    /* Physical address of the first guest instruction byte translated by this slot. */
    paddr_t paddr_start;
    /* Number of contiguous source bytes covered by this block, normally 4 * insns. */
    uint32_t source_len;
    /* Guest instruction count completed when `entry` returns normally. */
    uint32_t insn_count;
    /*
   * Native function pointer. NULL with valid == true is a negative cache marker:
   * the instruction is unsupported by this JIT and should use the interpreter.
   */
    rv32_jit_entry_t entry;
} rv32_jit_block_t;

/* Direct-mapped block cache indexed by a hash of guest PC and satp. */
static rv32_jit_block_t jit_cache[RV32_JIT_CACHE_SIZE];
/* Small translated-PMEM cache used by JIT memory helpers in Sv32 mode. */
static rv32_jit_tlb_entry_t jit_tlb[RV32_JIT_TLB_SIZE];
/*
 * Refcount PMEM pages that are currently used as page-table pages by cached JIT
 * TLB entries.  Stores can then test one indexed counter instead of scanning all
 * TLB entries, which matters because FCEUX performs huge numbers of stores.
 */
static uint16_t jit_tlb_pt_page_refs[RV32_JIT_PMEM_PAGE_COUNT];
/*
 * Refcount per 128-byte PMEM source chunk. A non-zero value means at least one
 * native block was compiled from bytes in that chunk, so stores there may need
 * exact cache invalidation.
 */
static uint16_t jit_source_chunk_refs[RV32_JIT_PMEM_CHUNK_COUNT];
/* Executable arena allocated with mmap(); emitted blocks live here. */
static uint8_t *jit_code = NULL;
/* Number of bytes already used in `jit_code`, rounded up before each block. */
static size_t jit_code_used = 0;
/* Sticky flag set when executable memory allocation fails. */
static bool jit_disabled = false;
/* Cached value of the runtime `NEMU_DISABLE_JIT` environment switch. */
static bool jit_env_disable = false;
/* True after runtime environment switches have been read once. */
static bool jit_runtime_options_ready = false;
/* Cached value of the runtime `NEMU_JIT_STATS` environment switch. */
static bool jit_stats_enabled = false;
/* Current native-entry instruction budget, used by in-block chained loops. */
static volatile uint32_t jit_entry_budget = 0;
/* Extra instructions completed by chained loop laps before the final exit. */
static volatile uint32_t jit_loop_extra = 0;

#if RV32_JIT_STATS
typedef struct
{
    /* Number of times the CPU loop asked the JIT to execute at least one block. */
    uint64_t exec_requests;
    /* Cache lookup found a valid slot for the current PC/satp tag. */
    uint64_t cache_hits;
    /* Cache lookup missed and the compiler had to try building a block. */
    uint64_t cache_misses;
    /* Valid negative-cache slots that redirected execution to the interpreter. */
    uint64_t unsupported_hits;
    /* Native blocks that were actually entered and returned normally. */
    uint64_t blocks_executed;
    /* Guest instruction count reported by executed native blocks. */
    uint64_t executed_insns;

    /* Number of block compilation attempts. */
    uint64_t compile_requests;
    /* Number of successful native blocks published to the cache. */
    uint64_t blocks_compiled;
    /* Guest instruction count represented by successfully compiled blocks. */
    uint64_t compiled_insns;
    /* Number of negative-cache markers created for unsupported instructions. */
    uint64_t blocks_unsupported;
    /* Number of times the executable arena was cleared and reused. */
    uint64_t arena_resets;

    /* Number of physical invalidation requests from stores, DMA, or full flushes. */
    uint64_t invalidation_requests;
    /* Invalidation requests skipped because no source chunk refcount was present. */
    uint64_t invalidation_page_skips;
    /* Cached native blocks discarded because their source bytes overlapped a write. */
    uint64_t invalidated_blocks;

    /* Helper load calls made from native code. */
    uint64_t helper_loads;
    /* Helper loads that could still use proven ordinary PMEM access. */
    uint64_t helper_load_direct;
    /* Helper loads that delegated to the full virtual-memory/device path. */
    uint64_t helper_load_slow;
    /* Helper store calls made from native code. */
    uint64_t helper_stores;
    /* Helper stores that wrote proven ordinary PMEM directly. */
    uint64_t helper_store_direct;
    /* Helper stores that delegated to the full virtual-memory/device path. */
    uint64_t helper_store_slow;
    /* Complex RV32M operation helper calls not emitted directly in native code. */
    uint64_t helper_complex_ops;
} rv32_jit_stats_t;

static rv32_jit_stats_t jit_stats;

#define JIT_STAT_INC(field) \
    do \
    { \
        jit_stats.field++; \
    } while (0)

#define JIT_STAT_ADD(field, value) \
    do \
    { \
        jit_stats.field += (value); \
    } while (0)
#else
#define JIT_STAT_INC(field) \
    do \
    { \
    } while (0)
#define JIT_STAT_ADD(field, value) \
    do \
    { \
    } while (0)
#endif

/* Return inclusive bit range [hi:lo] from a 32-bit instruction or value. */
static uint32_t bits(uint32_t value, int hi, int lo)
{
    return (value >> lo) & ((1u << (hi - lo + 1)) - 1u);
}

/*
 * Sign-extend a right-aligned immediate field. The xor/subtract form is a
 * branch-free way to move the unsigned field into the signed RV32 value range.
 */
static int32_t sext(uint32_t value, unsigned width)
{
    const uint32_t sign = 1u << (width - 1u);
    return (int32_t)((value ^ sign) - sign);
}

/* Decode an I-type immediate, used by loads, OP-IMM, JALR, and CSR forms. */
static int32_t imm_i(uint32_t instr)
{
    return sext(bits(instr, 31, 20), 12);
}

/* Decode an S-type store immediate from its split high/low instruction fields. */
static int32_t imm_s(uint32_t instr)
{
    return sext(bits(instr, 11, 7) | (bits(instr, 31, 25) << 5), 12);
}

/* Decode a B-type branch byte offset; bit 0 is implicit because branches align. */
static int32_t imm_b(uint32_t instr)
{
    const uint32_t imm = (bits(instr, 11, 8) << 1) | (bits(instr, 30, 25) << 5) | (bits(instr, 7, 7) << 11) | (bits(instr, 31, 31) << 12);
    return sext(imm, 13);
}

/* Decode a U-type immediate; it already occupies instruction bits [31:12]. */
static uint32_t imm_u(uint32_t instr)
{
    return instr & 0xfffff000u;
}

/* Decode a J-type jump byte offset from its shuffled instruction fields. */
static int32_t imm_j(uint32_t instr)
{
    const uint32_t imm = (bits(instr, 19, 12) << 12) | (bits(instr, 20, 20) << 11) | (bits(instr, 30, 21) << 1) | (bits(instr, 31, 31) << 20);
    return sext(imm, 21);
}

static void jit_tlb_flush(void)
{
    memset(jit_tlb, 0, sizeof(jit_tlb));
    memset(jit_tlb_pt_page_refs, 0, sizeof(jit_tlb_pt_page_refs));
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

    if (jit_pmem_page_index(page, &idx) &&
        jit_tlb_pt_page_refs[idx] != UINT16_MAX)
    {
        jit_tlb_pt_page_refs[idx]++;
    }
}

static void jit_tlb_unref_page(paddr_t page)
{
    size_t idx = 0;

    if (jit_pmem_page_index(page, &idx) && jit_tlb_pt_page_refs[idx] > 0)
    {
        jit_tlb_pt_page_refs[idx]--;
    }
}

static bool jit_tlb_refs_page(paddr_t page)
{
    size_t idx = 0;
    return jit_pmem_page_index(page, &idx) && jit_tlb_pt_page_refs[idx] != 0;
}

static bool jit_write_may_touch_page_table(paddr_t addr, int len)
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

    for (paddr_t page = addr & ~(paddr_t)PAGE_MASK;
         page <= (end & ~(paddr_t)PAGE_MASK);
         page += PAGE_SIZE)
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
 * This deliberately mirrors the fast executor's small TLB, but it stays local to
 * the JIT helper path so generated code does not need to inline page walks.  A
 * false result is not an error; it only means the existing vaddr path must handle
 * the edge case, such as MMIO, cross-page accesses, invalid PTEs, or superpages.
 * Permission checks intentionally match the simplified interpreter MMU in
 * system/mmu.c: valid 4 KiB leaves with the required R/W/X bit can use the fast
 * path; privilege-sensitive rules such as U/SUM/MXR and accessed/dirty-bit
 * management are not implemented by this teaching MMU yet.
 */
static bool jit_translate_pmem(vaddr_t addr, uint32_t len, int type, paddr_t *paddr)
{
    const uint32_t satp = cpu.csr.satp;

    if ((satp & RV32_JIT_SATP_MODE_MASK) == 0)
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
    rv32_jit_tlb_entry_t *entry = &jit_tlb[idx];

    if (likely(entry->valid && entry->satp == satp && entry->vpn == vpn &&
               (entry->perm & need_perm) != 0))
    {
        const paddr_t translated = entry->pg_paddr | (paddr_t)(addr & PAGE_MASK);

        if (!jit_pmem_range(translated, len))
        {
            return false;
        }
        *paddr = translated;
        return true;
    }

    const paddr_t root =
        ((paddr_t)(satp & RV32_JIT_SATP_PPN_MASK)) << PAGE_SHIFT;
    const word_t vpn1 = (word_t)((addr >> 22) & 0x3ffu);
    const word_t vpn0 = (word_t)((addr >> 12) & 0x3ffu);
    const paddr_t pte1_addr = root + (paddr_t)(vpn1 * 4u);
    const uint32_t pte1 = (uint32_t)paddr_read(pte1_addr, 4);

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

    const paddr_t l0_pt = ((paddr_t)(pte1 >> 10)) << PAGE_SHIFT;
    const paddr_t pte0_addr = l0_pt + (paddr_t)(vpn0 * 4u);
    const uint32_t pte0 = (uint32_t)paddr_read(pte0_addr, 4);

    if ((pte0 & RV32_JIT_PTE_V) == 0)
    {
        return false;
    }

    const uint32_t perm = pte0 & (RV32_JIT_PTE_R | RV32_JIT_PTE_W | RV32_JIT_PTE_X);

    if (perm == 0 || (perm & need_perm) == 0)
    {
        return false;
    }

    const paddr_t pg_paddr = ((paddr_t)(pte0 >> 10)) << PAGE_SHIFT;

    if (!jit_pmem_range(pg_paddr | (paddr_t)(addr & PAGE_MASK), len))
    {
        return false;
    }

    if (entry->valid)
    {
        const paddr_t old_root =
            ((paddr_t)(entry->satp & RV32_JIT_SATP_PPN_MASK)) << PAGE_SHIFT;
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
 * Read a simple boolean environment flag.
 *
 * Empty, missing, and exactly "0" mean false; any other non-empty value means
 * true. This keeps runtime switches easy to use from shell commands.
 */
static bool jit_env_flag_enabled(const char *name)
{
    const char *value = getenv(name);
    return value != NULL && value[0] != '\0' &&
           !(value[0] == '0' && value[1] == '\0');
}

/* Cache runtime environment switches once so hot dispatch does not call getenv(). */
static void jit_init_runtime_options(void)
{
    if (!jit_runtime_options_ready)
    {
        jit_env_disable = jit_env_flag_enabled("NEMU_DISABLE_JIT");
        jit_stats_enabled = jit_env_flag_enabled("NEMU_JIT_STATS");
        jit_runtime_options_ready = true;
    }
}

/* Report whether `NEMU_DISABLE_JIT` disabled native execution for this run. */
static bool jit_runtime_disabled(void)
{
    jit_init_runtime_options();
    return jit_env_disable;
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
static uint32_t jit_load_i8(vaddr_t addr)
{
    return (uint32_t)(int32_t)(int8_t)jit_load_raw(addr, 1);
}

/* Load one signed halfword and extend it to the RV32 register width. */
static uint32_t jit_load_i16(vaddr_t addr)
{
    return (uint32_t)(int32_t)(int16_t)jit_load_raw(addr, 2);
}

/* Load one 32-bit word; no extension is needed for RV32. */
static uint32_t jit_load_u32(vaddr_t addr)
{
    return jit_load_raw(addr, 4);
}

/* Load one unsigned byte and zero-extend it to the RV32 register width. */
static uint32_t jit_load_u8(vaddr_t addr)
{
    return jit_load_raw(addr, 1);
}

/* Load one unsigned halfword and zero-extend it to the RV32 register width. */
static uint32_t jit_load_u16(vaddr_t addr)
{
    return jit_load_raw(addr, 2);
}

static bool jit_write_may_touch_source_chunk(paddr_t addr, int len);

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
        const bool flush_tlb = jit_write_may_touch_page_table(paddr, (int)len);
        const bool touch_source = jit_write_may_touch_source_chunk(paddr, (int)len);
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
    jit_tlb_flush();
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
static void jit_store_u8(vaddr_t addr, uint32_t data)
{
    jit_store_raw(addr, 1, data);
}

/* Store the low halfword of `data` to a guest address. */
static void jit_store_u16(vaddr_t addr, uint32_t data)
{
    jit_store_raw(addr, 2, data);
}

/* Store all 32 bits of `data` to a guest address. */
static void jit_store_u32(vaddr_t addr, uint32_t data)
{
    jit_store_raw(addr, 4, data);
}

static uint32_t jit_store_u8_continue(vaddr_t addr, uint32_t data)
{
    return jit_store_raw_continue(addr, 1, data);
}

static uint32_t jit_store_u16_continue(vaddr_t addr, uint32_t data)
{
    return jit_store_raw_continue(addr, 2, data);
}

static uint32_t jit_store_u32_continue(vaddr_t addr, uint32_t data)
{
    return jit_store_raw_continue(addr, 4, data);
}

/*
 * Execute RV32M operations that are uncommon or awkward to emit inline.
 *
 * The helper decodes the already-fetched OP instruction, reads the architectural
 * registers from `cpu.gpr[]`, applies exact RISC-V divide/remainder edge cases,
 * writes rd when it is not x0, and returns the value for callers that also want
 * to seed the register cache from EAX after the helper call.
 */
static uint32_t jit_op_complex(uint32_t instr)
{
    JIT_STAT_INC(helper_complex_ops);

    const uint32_t rd = bits(instr, 11, 7);
    const uint32_t funct3 = bits(instr, 14, 12);
    const uint32_t rs1 = bits(instr, 19, 15);
    const uint32_t rs2 = bits(instr, 24, 20);
    const uint32_t funct7 = bits(instr, 31, 25);
    const uint32_t key = (funct7 << 3) | funct3;
    const uint32_t lhs = gpr(rs1);
    const uint32_t rhs = gpr(rs2);
    uint32_t out = 0;

    switch (key)
    {
    case 0x009:
        out = (uint32_t)(((int64_t)(int32_t)lhs *
                          (int64_t)(int32_t)rhs) >>
                         32);
        break;
    case 0x00a:
        /*
       * MULHSU is signed(rs1) * unsigned(rs2). The product still fits in a
       * signed 64-bit value because both operands are 32-bit wide.
       */
        out = (uint32_t)(((int64_t)(int32_t)lhs *
                          (int64_t)(uint64_t)rhs) >>
                         32);
        break;
    case 0x00b:
        out = (uint32_t)(((uint64_t)lhs * (uint64_t)rhs) >> 32);
        break;
    case 0x00c:
        out = (rhs == 0) ? UINT32_MAX : ((int32_t)lhs == INT32_MIN && (int32_t)rhs == -1 ? lhs : (uint32_t)((int32_t)lhs / (int32_t)rhs));
        break;
    case 0x00d:
        out = rhs == 0 ? UINT32_MAX : lhs / rhs;
        break;
    case 0x00e:
        out = (rhs == 0) ? lhs : ((int32_t)lhs == INT32_MIN && (int32_t)rhs == -1 ? 0 : (uint32_t)((int32_t)lhs % (int32_t)rhs));
        break;
    case 0x00f:
        out = rhs == 0 ? lhs : lhs % rhs;
        break;
    default:
        panic("jit: unsupported complex OP instruction 0x%08x", instr);
    }

    if (rd != 0)
    {
        gpr(rd) = out;
    }

    return out;
}

/* Round `value` up to the next `align` boundary; align is a power of two here. */
static size_t jit_align_up(size_t value, size_t align)
{
    return (value + align - 1u) & ~(align - 1u);
}

/* Hash guest PC and address-space tag into the direct-mapped block cache. */
static uint32_t jit_hash(vaddr_t pc, word_t satp)
{
    return ((pc >> 2) ^ satp ^ (satp >> 12)) & (RV32_JIT_CACHE_SIZE - 1u);
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

    *idx = ((size_t)(addr - (paddr_t)CONFIG_MBASE)) >>
           RV32_JIT_SOURCE_CHUNK_SHIFT;
    return *idx < RV32_JIT_PMEM_CHUNK_COUNT;
}

/* Add one owning compiled block reference to every source chunk in the range. */
static void jit_source_chunks_ref(paddr_t addr, uint32_t len)
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

    if (end < addr || !jit_pmem_source_chunk_index(addr, &first) ||
        !jit_pmem_source_chunk_index(end, &last))
    {
        return;
    }

    for (size_t i = first; i <= last; i++)
    {
        Assert(jit_source_chunk_refs[i] != UINT16_MAX,
               "jit: too many source blocks in PMEM source chunk %zu", i);
        jit_source_chunk_refs[i]++;
    }
}

/* Remove one owning compiled block reference from every source chunk in range. */
static void jit_source_chunks_unref(paddr_t addr, uint32_t len)
{
    if (len == 0)
    {
        return;
    }

    size_t first = 0;
    size_t last = 0;
    const paddr_t end = addr + (paddr_t)len - 1u;

    if (end < addr || !jit_pmem_source_chunk_index(addr, &first) ||
        !jit_pmem_source_chunk_index(end, &last))
    {
        return;
    }

    for (size_t i = first; i <= last; i++)
    {
        Assert(jit_source_chunk_refs[i] > 0,
               "jit: source chunk refcount underflow on PMEM source chunk %zu", i);
        jit_source_chunk_refs[i]--;
    }
}

/*
 * Quickly decide whether a physical write might overlap compiled source bytes.
 *
 * False means no source chunk has a refcount and invalidation can be skipped.
 * True means "scan exact blocks"; it includes ambiguous wrap or boundary cases.
 */
static bool jit_write_may_touch_source_chunk(paddr_t addr, int len)
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

    if (!jit_pmem_source_chunk_index(start, &first) ||
        !jit_pmem_source_chunk_index(end, &last))
    {
        return true;
    }

    for (size_t i = first; i <= last; i++)
    {
        if (jit_source_chunk_refs[i] != 0)
        {
            return true;
        }
    }

    return false;
}

/* Drop one cache slot and release the source-chunk references it owns. */
static void jit_block_discard(rv32_jit_block_t *block)
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
        jit_source_chunks_unref(block->paddr_start, block->source_len);
    }

    block->valid = false;
    block->entry = NULL;
    block->source_len = 0;
    block->insn_count = 0;
}

/* Clear every block cache slot and reset all source-chunk refcounts together. */
static void jit_cache_clear(void)
{
    memset(jit_cache, 0, sizeof(jit_cache));
    memset(jit_source_chunk_refs, 0, sizeof(jit_source_chunk_refs));
}

/*
 * Allocate the executable code arena on first use.
 *
 * The arena is RWX because this small teaching JIT emits bytes directly and then
 * calls them. If allocation fails, the sticky disabled flag avoids repeated mmap
 * attempts and execution falls back to the interpreter.
 */
static bool jit_code_init(void)
{
#if RV32_JIT_ENABLED
    if (jit_code != NULL)
    {
        return true;
    }

    void *mem = mmap(NULL, RV32_JIT_CODE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (mem == MAP_FAILED)
    {
        jit_disabled = true;
        Log("jit: mmap failed, disable RISC-V32 JIT");
        return false;
    }

    jit_code = mem;
    jit_code_used = 0;
    jit_cache_clear();
    Log("jit: RISC-V32 x86-64 code cache enabled, size = %u bytes",
        RV32_JIT_CODE_SIZE);
    return true;
#else
    return false;
#endif
}

/* Reuse the code arena from byte zero and forget all cached native blocks. */
static void jit_arena_reset(void)
{
    JIT_STAT_INC(arena_resets);
    jit_code_used = 0;
    jit_cache_clear();
}

/* Emit one raw x86-64 byte into the current writer. */
static bool emit_u8(rv32_jit_writer_t *w, uint8_t value)
{
    /*
   * All x86-64 emitters are written as boolean builders. A false result means
   * "do not publish this block"; callers either roll back to a known boundary or
   * abandon the translation before the cache entry becomes executable.
   */

    if (w->cur >= w->end)
    {
        return false;
    }

    *w->cur++ = value;
    return true;
}

/* Emit a 32-bit little-endian immediate or displacement. */
static bool emit_u32(rv32_jit_writer_t *w, uint32_t value)
{
    if ((size_t)(w->end - w->cur) < sizeof(value))
    {
        return false;
    }

    memcpy(w->cur, &value, sizeof(value));
    w->cur += sizeof(value);
    return true;
}

/* Emit a 64-bit little-endian immediate, mainly for movabs addresses. */
static bool emit_u64(rv32_jit_writer_t *w, uint64_t value)
{
    if ((size_t)(w->end - w->cur) < sizeof(value))
    {
        return false;
    }

    memcpy(w->cur, &value, sizeof(value));
    w->cur += sizeof(value);
    return true;
}

/* Emit `movabs r11, imm64`; r11 is this JIT's CPU-state base register. */
static bool emit_movabs_r11(rv32_jit_writer_t *w, uint64_t value)
{
    /* movabs r11, imm64 */
    return emit_u8(w, 0x49) && emit_u8(w, 0xbb) && emit_u64(w, value);
}

/* Load the address of global `cpu` into r11 for later `[r11 + offset]` access. */
static bool emit_load_cpu_base(rv32_jit_writer_t *w)
{
    /*
   * Generated blocks keep &cpu in r11 across straight-line code. Helper calls
   * use the host ABI and may clobber caller-saved registers, so call sites
   * reload r11 before continuing to access guest state.
   */
    return emit_movabs_r11(w, (uint64_t)(uintptr_t)&cpu);
}

/* Store a known immediate guest PC into `cpu.pc`. */
static bool emit_set_pc_imm(rv32_jit_writer_t *w, vaddr_t pc)
{
    const uint32_t off = (uint32_t)offsetof(CPU_state, pc);

    /* mov dword ptr [r11 + pc_off], imm32 */
    return emit_u8(w, 0x41) && emit_u8(w, 0xc7) && emit_u8(w, 0x83) && emit_u32(w, off) && emit_u32(w, pc);
}

/* Put a 32-bit immediate result into EAX, the normal temporary result register. */
static bool emit_mov_eax_imm(rv32_jit_writer_t *w, uint32_t value)
{
    return emit_u8(w, 0xb8) && emit_u32(w, value);
}

/* Add an RV32 immediate or address offset to EAX, using the short form when safe. */
static bool emit_add_eax_imm(rv32_jit_writer_t *w, uint32_t value)
{
    const int32_t signed_value = (int32_t)value;

    if (signed_value == 0)
    {
        return true;
    }

    if (signed_value >= INT8_MIN && signed_value <= INT8_MAX)
    {
        /* add eax, imm8; x86 sign-extends imm8, which matches RV32 immediates. */
        return emit_u8(w, 0x83) && emit_u8(w, 0xc0) && emit_u8(w, (uint8_t)signed_value);
    }

    return emit_u8(w, 0x05) && emit_u32(w, value);
}

/* Add an immediate to ECX, used by generated loop-budget checks. */
static bool emit_add_ecx_imm(rv32_jit_writer_t *w, uint32_t value)
{
    const int32_t signed_value = (int32_t)value;

    if (signed_value == 0)
    {
        return true;
    }

    if (signed_value >= INT8_MIN && signed_value <= INT8_MAX)
    {
        return emit_u8(w, 0x83) && emit_u8(w, 0xc1) && emit_u8(w, (uint8_t)signed_value);
    }

    return emit_u8(w, 0x81) && emit_u8(w, 0xc1) && emit_u32(w, value);
}

/* Compare EAX with an immediate so a following setcc/jcc can consume the flags. */
static bool emit_cmp_eax_imm(rv32_jit_writer_t *w, uint32_t value)
{
    return emit_u8(w, 0x3d) && emit_u32(w, value);
}

/* Compare EAX with ECX for register-register branches and SLT-style results. */
static bool emit_cmp_eax_ecx(rv32_jit_writer_t *w)
{
    return emit_u8(w, 0x39) && emit_u8(w, 0xc8);
}

/* Compare ECX with a sign-extended 8-bit immediate, used by RV32M guards. */
static bool emit_cmp_ecx_imm8(rv32_jit_writer_t *w, uint8_t value)
{
    return emit_u8(w, 0x83) && emit_u8(w, 0xf9) && emit_u8(w, value);
}

/* Test whether ECX is zero without modifying ECX. */
static bool emit_test_ecx_ecx(rv32_jit_writer_t *w)
{
    return emit_u8(w, 0x85) && emit_u8(w, 0xc9);
}

/* Test whether EAX is zero without modifying the helper return value. */
static bool emit_test_eax_eax(rv32_jit_writer_t *w)
{
    return emit_u8(w, 0x85) && emit_u8(w, 0xc0);
}

/* Save a guest virtual address from EAX into ECX before inline guards clobber it. */
static bool emit_mov_ecx_eax(rv32_jit_writer_t *w)
{
    return emit_u8(w, 0x89) && emit_u8(w, 0xc1);
}

/* Load one 32-bit value through RDX into EAX. */
static bool emit_mov_eax_m32_rdx(rv32_jit_writer_t *w)
{
    return emit_u8(w, 0x8b) && emit_u8(w, 0x02);
}

/* Store EAX through RDX. */
static bool emit_mov_m32_rdx_eax(rv32_jit_writer_t *w)
{
    return emit_u8(w, 0x89) && emit_u8(w, 0x02);
}

/* Compare ECX against one 32-bit value loaded through RDX. */
static bool emit_cmp_ecx_m32_rdx(rv32_jit_writer_t *w)
{
    return emit_u8(w, 0x3b) && emit_u8(w, 0x0a);
}

/* Restore a saved guest virtual address from ECX into EAX for helper fallback. */
static bool emit_mov_eax_ecx(rv32_jit_writer_t *w)
{
    return emit_u8(w, 0x89) && emit_u8(w, 0xc8);
}

/* Save a store guest virtual address from EAX into EDI for helper fallback. */
static bool emit_mov_edi_eax(rv32_jit_writer_t *w)
{
    return emit_u8(w, 0x89) && emit_u8(w, 0xc7);
}

/* Copy EAX into EDX for address arithmetic. */
static bool emit_mov_edx_eax(rv32_jit_writer_t *w)
{
    return emit_u8(w, 0x89) && emit_u8(w, 0xc2);
}

/* Shift EDX right by an immediate count. */
static bool emit_shr_edx_imm(rv32_jit_writer_t *w, uint8_t value)
{
    return emit_u8(w, 0xc1) && emit_u8(w, 0xea) && emit_u8(w, value);
}

/* Mask EAX with an immediate, used to keep the 4 KiB page offset. */
static bool emit_and_eax_imm(rv32_jit_writer_t *w, uint32_t value)
{
    return emit_u8(w, 0x25) && emit_u32(w, value);
}

/* OR EAX into EDX, combining a translated page base with the page offset. */
static bool emit_or_edx_eax(rv32_jit_writer_t *w)
{
    return emit_u8(w, 0x09) && emit_u8(w, 0xc2);
}

/* Subtract an immediate from EDX, normally CONFIG_MBASE from a physical address. */
static bool emit_sub_edx_imm(rv32_jit_writer_t *w, uint32_t value)
{
    return emit_u8(w, 0x81) && emit_u8(w, 0xea) && emit_u32(w, value);
}

/* Shift R8 left by an immediate count; R8 holds a JIT TLB entry offset. */
static bool emit_shl_r8_imm(rv32_jit_writer_t *w, uint8_t value)
{
    return emit_u8(w, 0x49) && emit_u8(w, 0xc1) && emit_u8(w, 0xe0) && emit_u8(w, value);
}

/* Emit `movabs rdx, imm64` for global table addresses. */
static bool emit_movabs_rdx(rv32_jit_writer_t *w, uint64_t value)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0xba) && emit_u64(w, value);
}

/* Emit `movabs rax, imm64` when a guard needs an untracked table base. */
static bool emit_movabs_rax(rv32_jit_writer_t *w, uint64_t value)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0xb8) && emit_u64(w, value);
}

/* Add RDX to R8, producing a pointer into the JIT TLB. */
static bool emit_add_r8_rdx(rv32_jit_writer_t *w)
{
    return emit_u8(w, 0x49) && emit_u8(w, 0x01) && emit_u8(w, 0xd0);
}

/* Clear EDX before unsigned x86 DIV, which consumes EDX:EAX as the dividend. */
static bool emit_xor_edx_edx(rv32_jit_writer_t *w)
{
    return emit_u8(w, 0x31) && emit_u8(w, 0xd2);
}

/* Sign-extend EAX into EDX:EAX before signed x86 IDIV. */
static bool emit_cdq(rv32_jit_writer_t *w)
{
    return emit_u8(w, 0x99);
}

/* Copy EDX into EAX, used for high multiply halves and remainders. */
static bool emit_mov_eax_edx(rv32_jit_writer_t *w)
{
    return emit_u8(w, 0x89) && emit_u8(w, 0xd0);
}

/* Emit unsigned multiply of EAX by ECX, producing EDX:EAX. */
static bool emit_mul_ecx(rv32_jit_writer_t *w)
{
    /* Unsigned edx:eax = eax * ecx. */
    return emit_u8(w, 0xf7) && emit_u8(w, 0xe1);
}

/* Emit signed multiply of EAX by ECX, producing EDX:EAX. */
static bool emit_imul_ecx(rv32_jit_writer_t *w)
{
    /* Signed edx:eax = eax * ecx. */
    return emit_u8(w, 0xf7) && emit_u8(w, 0xe9);
}

/* Emit unsigned divide of EDX:EAX by ECX after caller has guarded ECX != 0. */
static bool emit_div_ecx(rv32_jit_writer_t *w)
{
    /* Unsigned edx:eax / ecx, quotient in eax, remainder in edx. */
    return emit_u8(w, 0xf7) && emit_u8(w, 0xf1);
}

/* Emit signed divide of EDX:EAX by ECX after caller has guarded x86 trap cases. */
static bool emit_idiv_ecx(rv32_jit_writer_t *w)
{
    /* Signed edx:eax / ecx, quotient in eax, remainder in edx. */
    return emit_u8(w, 0xf7) && emit_u8(w, 0xf9);
}

/* Convert a condition-code result into RV32 boolean 0/1 in EAX. */
static bool emit_setcc_eax(rv32_jit_writer_t *w, uint8_t setcc_opcode)
{
    /* setcc al; movzx eax, al */
    return emit_u8(w, 0x0f) && emit_u8(w, setcc_opcode) && emit_u8(w, 0xc0) && emit_u8(w, 0x0f) && emit_u8(w, 0xb6) && emit_u8(w, 0xc0);
}

/* Emit a conditional rel32 branch and return the displacement byte location. */
static bool emit_jcc_rel32_placeholder(rv32_jit_writer_t *w, uint8_t jcc_opcode,
                                       uint8_t **disp)
{
    if (!emit_u8(w, 0x0f) || !emit_u8(w, jcc_opcode))
    {
        return false;
    }

    *disp = w->cur;
    return emit_u32(w, 0);
}

/* Emit an unconditional rel32 jump and return the displacement byte location. */
static bool emit_jmp_rel32_placeholder(rv32_jit_writer_t *w, uint8_t **disp)
{
    if (!emit_u8(w, 0xe9))
    {
        return false;
    }

    *disp = w->cur;
    return emit_u32(w, 0);
}

/* Patch a previously emitted rel32 displacement to jump to `target`. */
static void patch_rel32(uint8_t *disp, const uint8_t *target)
{
    /*
   * x86 relative branches are measured from the byte after the displacement.
   * The code arena is small enough that rel32 should always be sufficient; the
   * assertion catches accidental jumps outside the emitted block.
   */
    const int64_t rel = target - (disp + 4);
    Assert(rel >= INT32_MIN && rel <= INT32_MAX,
           "jit: x86 branch displacement out of range");
    const int32_t rel32 = (int32_t)rel;
    memcpy(disp, &rel32, sizeof(rel32));
}

/* Emit an absolute call through RAX, suitable for C helper function addresses. */
static bool emit_call_abs(rv32_jit_writer_t *w, uintptr_t func)
{
    /* movabs rax, func; call rax */
    return emit_u8(w, 0x48) && emit_u8(w, 0xb8) && emit_u64(w, (uint64_t)func) && emit_u8(w, 0xff) && emit_u8(w, 0xd0);
}

typedef struct
{
    /* Displacement of the slow-path jump emitted when satp is not Bare mode. */
    uint8_t *satp_slow_disp;
    /* Displacement of the slow-path jump emitted when the PMEM range check fails. */
    uint8_t *range_slow_disp;
} rv32_jit_pmem_guard_patch_t;

typedef struct
{
    /*
   * Paged-load guards have several independent reasons to give up: missing TLB
   * entry, different address space, permission miss, cross-page access, or an
   * unexpected PMEM range.  Keep the branch displacement list compact so the
   * caller can patch every conservative fallback to the same helper path.
   */
    uint8_t *slow_disps[8];
    uint32_t count;
} rv32_jit_tlb_load_patch_t;

typedef enum
{
    /* Callee-saved host register used as a guest-register cache slot. */
    RV32_JIT_HREG_RBX = 0,
    /* Callee-saved host register used as a guest-register cache slot. */
    RV32_JIT_HREG_R12,
    /* Callee-saved host register used as a guest-register cache slot. */
    RV32_JIT_HREG_R13,
    /* Callee-saved host register used as a guest-register cache slot. */
    RV32_JIT_HREG_R14,
    /* Callee-saved host register used as a guest-register cache slot. */
    RV32_JIT_HREG_R15,
    /* Number of host registers reserved for guest-register caching. */
    RV32_JIT_HREG_COUNT,
} rv32_jit_hreg_t;

typedef struct
{
    /* This slot currently represents `guest_reg`; false means it can be reused. */
    bool valid;
    /* The host register contains a real value; false means it is only reserved. */
    bool loaded;
    /* The host value differs from cpu.gpr[guest_reg] and must be flushed on exit. */
    bool dirty;
    /* Guest architectural register index stored in this slot. */
    uint32_t guest_reg;
    /* Monotonic use age for simple least-recently-used replacement. */
    uint32_t age;
    /* Which callee-saved host register backs this slot. */
    rv32_jit_hreg_t hreg;
} rv32_jit_reg_slot_t;

typedef struct
{
    /* Fixed set of host-register cache slots available within one native block. */
    rv32_jit_reg_slot_t slots[RV32_JIT_HREG_COUNT];
    /* Next age number assigned when a slot is touched. */
    uint32_t next_age;
    /* True once r9 has been loaded with `jit_source_chunk_refs` in this block. */
    bool source_refs_loaded;
} rv32_jit_reg_cache_t;

/* Map one JIT host-register enum value to the x86 register number encoding. */
static uint8_t jit_hreg_x86_reg(rv32_jit_hreg_t hreg)
{
    switch (hreg)
    {
    case RV32_JIT_HREG_RBX:
        return 3;
    case RV32_JIT_HREG_R12:
        return 12;
    case RV32_JIT_HREG_R13:
        return 13;
    case RV32_JIT_HREG_R14:
        return 14;
    case RV32_JIT_HREG_R15:
        return 15;
    default:
        Assert(0, "jit: invalid host register slot %d", hreg);
    }
    return 3;
}

/* Build an x86 ModRM byte from its three logical fields. */
static uint8_t jit_modrm(uint8_t mod, uint8_t reg, uint8_t rm)
{
    return (uint8_t)((mod << 6) | ((reg & 7u) << 3) | (rm & 7u));
}

/* Emit a REX prefix only when a 32-bit instruction references r8-r15. */
static bool emit_rex32_if_needed(rv32_jit_writer_t *w, uint8_t reg, uint8_t rm)
{
    uint8_t rex = 0x40;

    if ((reg & 8u) != 0)
    {
        rex |= 0x04;
    }

    if ((rm & 8u) != 0)
    {
        rex |= 0x01;
    }

    return rex == 0x40 || emit_u8(w, rex);
}

/* Save all callee-saved host registers that this JIT uses as cache slots. */
static bool emit_push_saved_hregs(rv32_jit_writer_t *w)
{
    return emit_u8(w, 0x53) && emit_u8(w, 0x41) && emit_u8(w, 0x54) && emit_u8(w, 0x41) && emit_u8(w, 0x55) && emit_u8(w, 0x41) && emit_u8(w, 0x56) && emit_u8(w, 0x41) && emit_u8(w, 0x57);
}

/* Restore host registers in the opposite order of emit_push_saved_hregs(). */
static bool emit_pop_saved_hregs(rv32_jit_writer_t *w)
{
    return emit_u8(w, 0x41) && emit_u8(w, 0x5f) && emit_u8(w, 0x41) && emit_u8(w, 0x5e) && emit_u8(w, 0x41) && emit_u8(w, 0x5d) && emit_u8(w, 0x41) && emit_u8(w, 0x5c) && emit_u8(w, 0x5b);
}

/* Load `cpu.gpr[reg]` into one cached host register. */
static bool emit_load_gpr_hreg(rv32_jit_writer_t *w, rv32_jit_hreg_t hreg,
                               uint32_t reg)
{
    const uint32_t off = (uint32_t)offsetof(CPU_state, gpr) + reg * sizeof(cpu.gpr[0]);
    const uint8_t dst = jit_hreg_x86_reg(hreg);
    const uint8_t base = 11;

    /* mov hreg32, dword ptr [r11 + off] */
    return emit_rex32_if_needed(w, dst, base) && emit_u8(w, 0x8b) && emit_u8(w, jit_modrm(2, dst, base)) && emit_u32(w, off);
}

/* Store one cached host register back into `cpu.gpr[reg]`. */
static bool emit_store_gpr_hreg(rv32_jit_writer_t *w, uint32_t reg,
                                rv32_jit_hreg_t hreg)
{
    const uint32_t off = (uint32_t)offsetof(CPU_state, gpr) + reg * sizeof(cpu.gpr[0]);
    const uint8_t src = jit_hreg_x86_reg(hreg);
    const uint8_t base = 11;

    /* mov dword ptr [r11 + off], hreg32 */
    return emit_rex32_if_needed(w, src, base) && emit_u8(w, 0x89) && emit_u8(w, jit_modrm(2, src, base)) && emit_u32(w, off);
}

/* Copy a cached host-register value into EAX for generic emitters. */
static bool emit_mov_eax_hreg(rv32_jit_writer_t *w, rv32_jit_hreg_t hreg)
{
    const uint8_t src = jit_hreg_x86_reg(hreg);

    /* mov eax, hreg32 */
    return emit_rex32_if_needed(w, src, 0) && emit_u8(w, 0x89) && emit_u8(w, jit_modrm(3, src, 0));
}

/* Copy a cached host-register value into ECX, often the second ALU operand. */
static bool emit_mov_ecx_hreg(rv32_jit_writer_t *w, rv32_jit_hreg_t hreg)
{
    const uint8_t src = jit_hreg_x86_reg(hreg);

    /* mov ecx, hreg32 */
    return emit_rex32_if_needed(w, src, 1) && emit_u8(w, 0x89) && emit_u8(w, jit_modrm(3, src, 1));
}

/* Copy the EAX temporary result into a cached host register. */
static bool emit_mov_hreg_eax(rv32_jit_writer_t *w, rv32_jit_hreg_t hreg)
{
    const uint8_t dst = jit_hreg_x86_reg(hreg);

    /* mov hreg32, eax */
    return emit_rex32_if_needed(w, 0, dst) && emit_u8(w, 0x89) && emit_u8(w, jit_modrm(3, 0, dst));
}

/* Copy one cached host register to another when rd and rs are different. */
static bool emit_mov_hreg_hreg(rv32_jit_writer_t *w, rv32_jit_hreg_t dst,
                               rv32_jit_hreg_t src)
{
    const uint8_t dst_reg = jit_hreg_x86_reg(dst);
    const uint8_t src_reg = jit_hreg_x86_reg(src);

    if (dst == src)
    {
        return true;
    }

    /* mov dst32, src32 */
    return emit_rex32_if_needed(w, src_reg, dst_reg) && emit_u8(w, 0x89) && emit_u8(w, jit_modrm(3, src_reg, dst_reg));
}

/* Load a constant guest-register value into a cached host register. */
static bool emit_mov_hreg_imm(rv32_jit_writer_t *w, rv32_jit_hreg_t hreg,
                              uint32_t value)
{
    const uint8_t dst = jit_hreg_x86_reg(hreg);

    /* mov hreg32, imm32 */
    return emit_rex32_if_needed(w, 0, dst) && emit_u8(w, 0xc7) && emit_u8(w, jit_modrm(3, 0, dst)) && emit_u32(w, value);
}

/* Copy ECX into a cached host register; retained for future ECX-result emitters. */
static bool __attribute__((unused)) emit_mov_hreg_ecx(rv32_jit_writer_t *w,
                                                      rv32_jit_hreg_t hreg)
{
    const uint8_t dst = jit_hreg_x86_reg(hreg);

    /* mov hreg32, ecx */
    return emit_rex32_if_needed(w, 1, dst) && emit_u8(w, 0x89) && emit_u8(w, jit_modrm(3, 1, dst));
}

/* Initialise the per-block guest-register cache before emitting instructions. */
static void jit_reg_cache_init(rv32_jit_reg_cache_t *regs)
{
    regs->next_age = 1;
    regs->source_refs_loaded = false;
    for (uint32_t i = 0; i < RV32_JIT_HREG_COUNT; i++)
    {
        regs->slots[i] = (rv32_jit_reg_slot_t){
            .valid = false,
            .loaded = false,
            .dirty = false,
            .guest_reg = 0,
            .age = 0,
            .hreg = (rv32_jit_hreg_t)i,
        };
    }
}

/* Roll back compile-time register-cache metadata after a failed emitter. */
static void jit_reg_cache_restore(rv32_jit_reg_cache_t *regs,
                                  const rv32_jit_reg_cache_t *snapshot)
{
    *regs = *snapshot;
}

/* Find the host-register cache slot currently assigned to one guest register. */
static rv32_jit_reg_slot_t *jit_reg_find(rv32_jit_reg_cache_t *regs,
                                         uint32_t reg)
{
    for (uint32_t i = 0; i < RV32_JIT_HREG_COUNT; i++)
    {
        rv32_jit_reg_slot_t *slot = &regs->slots[i];

        if (slot->valid && slot->guest_reg == reg)
        {
            return slot;
        }
    }

    return NULL;
}

/* Emit a store-back for one dirty slot without mutating compile-time metadata. */
static bool jit_reg_emit_flush_slot(rv32_jit_writer_t *w,
                                    const rv32_jit_reg_slot_t *slot)
{
    if (!slot->valid || !slot->loaded || !slot->dirty || slot->guest_reg == 0)
    {
        return true;
    }

    return emit_store_gpr_hreg(w, slot->guest_reg, slot->hreg);
}

/* Flush one slot and mark it clean once the store-back bytes are emitted. */
static bool jit_reg_flush_slot(rv32_jit_writer_t *w, rv32_jit_reg_slot_t *slot)
{
    if (!jit_reg_emit_flush_slot(w, slot))
    {
        return false;
    }

    slot->dirty = false;
    return true;
}

/* Emit store-backs for all dirty slots while leaving their dirty bits unchanged. */
static bool __attribute__((unused)) jit_reg_emit_flush_all_dirty(
    rv32_jit_writer_t *w, const rv32_jit_reg_cache_t *regs)
{
    for (uint32_t i = 0; i < RV32_JIT_HREG_COUNT; i++)
    {
        if (!jit_reg_emit_flush_slot(w, &regs->slots[i]))
        {
            return false;
        }
    }

    return true;
}

/* Flush all dirty guest-register cache slots before helper calls or block exit. */
static bool jit_reg_flush_all_dirty(rv32_jit_writer_t *w,
                                    rv32_jit_reg_cache_t *regs)
{
    for (uint32_t i = 0; i < RV32_JIT_HREG_COUNT; i++)
    {
        if (!jit_reg_flush_slot(w, &regs->slots[i]))
        {
            return false;
        }
    }

    return true;
}

/* Forget all cached guest-register mappings after a helper may have changed CPU state. */
static void jit_reg_invalidate_all(rv32_jit_reg_cache_t *regs)
{
    for (uint32_t i = 0; i < RV32_JIT_HREG_COUNT; i++)
    {
        regs->slots[i].valid = false;
        regs->slots[i].loaded = false;
        regs->slots[i].dirty = false;
        regs->slots[i].guest_reg = 0;
        regs->slots[i].age = 0;
    }
}

/* Select a free slot, or the least-recently-used slot when all are occupied. */
static rv32_jit_reg_slot_t *jit_reg_choose_slot(rv32_jit_reg_cache_t *regs)
{
    rv32_jit_reg_slot_t *oldest = &regs->slots[0];
    for (uint32_t i = 0; i < RV32_JIT_HREG_COUNT; i++)
    {
        rv32_jit_reg_slot_t *slot = &regs->slots[i];

        if (!slot->valid)
        {
            return slot;
        }

        if (slot->age < oldest->age)
        {
            oldest = slot;
        }
    }

    return oldest;
}

/* Reserve a host-register cache slot for a guest register, flushing if replaced. */
static rv32_jit_reg_slot_t *jit_reg_alloc(rv32_jit_writer_t *w,
                                          rv32_jit_reg_cache_t *regs, uint32_t reg)
{
    rv32_jit_reg_slot_t *slot = jit_reg_find(regs, reg);

    if (slot != NULL)
    {
        slot->age = regs->next_age++;
        return slot;
    }

    slot = jit_reg_choose_slot(regs);

    if (!jit_reg_flush_slot(w, slot))
    {
        return NULL;
    }

    slot->valid = true;
    slot->loaded = false;
    slot->dirty = false;
    slot->guest_reg = reg;
    slot->age = regs->next_age++;
    return slot;
}

/* Materialise a guest register in EAX, loading it into the cache if needed. */
static bool jit_reg_read_eax(rv32_jit_writer_t *w,
                             rv32_jit_reg_cache_t *regs, uint32_t reg)
{
    if (reg == 0)
    {
        return emit_mov_eax_imm(w, 0);
    }

    rv32_jit_reg_slot_t *slot = jit_reg_alloc(w, regs, reg);

    if (slot == NULL)
    {
        return false;
    }

    if (!slot->loaded)
    {
        if (!emit_load_gpr_hreg(w, slot->hreg, reg))
        {
            return false;
        }
        slot->loaded = true;
    }

    slot->age = regs->next_age++;
    return emit_mov_eax_hreg(w, slot->hreg);
}

/* Materialise a guest register in ECX, loading it into the cache if needed. */
static bool jit_reg_read_ecx(rv32_jit_writer_t *w,
                             rv32_jit_reg_cache_t *regs, uint32_t reg)
{
    if (reg == 0)
    {
        return emit_u8(w, 0x31) && emit_u8(w, 0xc9);
    }

    rv32_jit_reg_slot_t *slot = jit_reg_alloc(w, regs, reg);

    if (slot == NULL)
    {
        return false;
    }

    if (!slot->loaded)
    {
        if (!emit_load_gpr_hreg(w, slot->hreg, reg))
        {
            return false;
        }
        slot->loaded = true;
    }

    slot->age = regs->next_age++;
    return emit_mov_ecx_hreg(w, slot->hreg);
}

/* Write the current EAX result to a guest register cache slot. */
static bool jit_reg_write_eax(rv32_jit_writer_t *w,
                              rv32_jit_reg_cache_t *regs, uint32_t reg)
{
    if (reg == 0)
    {
        return true;
    }

    rv32_jit_reg_slot_t *slot = jit_reg_alloc(w, regs, reg);

    if (slot == NULL)
    {
        return false;
    }

    if (!emit_mov_hreg_eax(w, slot->hreg))
    {
        return false;
    }

    slot->loaded = true;
    slot->dirty = true;
    slot->age = regs->next_age++;
    return true;
}

/* Write a compile-time constant value to a guest register cache slot. */
static bool jit_reg_write_imm(rv32_jit_writer_t *w,
                              rv32_jit_reg_cache_t *regs, uint32_t reg, uint32_t value)
{
    if (reg == 0)
    {
        return true;
    }

    rv32_jit_reg_slot_t *slot = jit_reg_alloc(w, regs, reg);

    if (slot == NULL)
    {
        return false;
    }

    if (!emit_mov_hreg_imm(w, slot->hreg, value))
    {
        return false;
    }

    slot->loaded = true;
    slot->dirty = true;
    slot->age = regs->next_age++;
    return true;
}

/* Return a cache slot whose host register definitely contains the guest value. */
static rv32_jit_reg_slot_t *jit_reg_loaded_slot(rv32_jit_writer_t *w,
                                                rv32_jit_reg_cache_t *regs, uint32_t reg)
{
    rv32_jit_reg_slot_t *slot = jit_reg_alloc(w, regs, reg);

    if (slot == NULL)
    {
        return NULL;
    }

    if (!slot->loaded)
    {
        if (!emit_load_gpr_hreg(w, slot->hreg, reg))
        {
            return NULL;
        }
        slot->loaded = true;
    }

    slot->age = regs->next_age++;
    return slot;
}

/* Mark a slot as containing a new value that must eventually be written back. */
static void jit_reg_mark_hreg_dirty(rv32_jit_reg_cache_t *regs,
                                    rv32_jit_reg_slot_t *slot)
{
    slot->loaded = true;
    slot->dirty = true;
    slot->age = regs->next_age++;
}

/* Emit a two-register x86 ALU operation directly between cached host registers. */
static bool emit_hreg_binop_hreg(rv32_jit_writer_t *w, uint8_t opcode,
                                 rv32_jit_hreg_t dst, rv32_jit_hreg_t src)
{
    const uint8_t dst_reg = jit_hreg_x86_reg(dst);
    const uint8_t src_reg = jit_hreg_x86_reg(src);

    /* opcode dst, src */
    return emit_rex32_if_needed(w, src_reg, dst_reg) && emit_u8(w, opcode) && emit_u8(w, jit_modrm(3, src_reg, dst_reg));
}

/* Emit an x86 ALU immediate operation against a cached host register. */
static bool emit_hreg_alu_imm(rv32_jit_writer_t *w, rv32_jit_hreg_t hreg,
                              uint8_t subop, uint32_t imm)
{
    const uint8_t dst = jit_hreg_x86_reg(hreg);
    const int32_t simm = (int32_t)imm;

    if (simm >= INT8_MIN && simm <= INT8_MAX)
    {
        /* 83 /subop ib sign-extends the immediate, matching these RV32 values. */
        return emit_rex32_if_needed(w, subop, dst) && emit_u8(w, 0x83) && emit_u8(w, jit_modrm(3, subop, dst)) && emit_u8(w, (uint8_t)simm);
    }

    /* 81 /subop id against the cached host register. */
    return emit_rex32_if_needed(w, subop, dst) && emit_u8(w, 0x81) && emit_u8(w, jit_modrm(3, subop, dst)) && emit_u32(w, imm);
}

/* Emit an x86 shift by immediate against a cached host register. */
static bool emit_hreg_shift_imm(rv32_jit_writer_t *w, rv32_jit_hreg_t hreg,
                                uint8_t subop, uint8_t amount)
{
    const uint8_t dst = jit_hreg_x86_reg(hreg);
    return emit_rex32_if_needed(w, subop, dst) && emit_u8(w, 0xc1) && emit_u8(w, jit_modrm(3, subop, dst)) && emit_u8(w, amount);
}

/* Emit an x86 shift by CL against a cached host register. */
static bool emit_hreg_shift_cl(rv32_jit_writer_t *w, rv32_jit_hreg_t hreg,
                               uint8_t subop)
{
    const uint8_t dst = jit_hreg_x86_reg(hreg);
    return emit_rex32_if_needed(w, subop, dst) && emit_u8(w, 0xd3) && emit_u8(w, jit_modrm(3, subop, dst));
}

/* Apply an immediate ALU operation in place to a cached guest register. */
static bool jit_reg_apply_imm(rv32_jit_writer_t *w,
                              rv32_jit_reg_cache_t *regs, uint32_t reg, uint8_t subop, uint32_t imm)
{
    rv32_jit_reg_slot_t *slot = jit_reg_loaded_slot(w, regs, reg);

    if (slot == NULL)
    {
        return false;
    }

    if (!emit_hreg_alu_imm(w, slot->hreg, subop, imm))
    {
        return false;
    }

    jit_reg_mark_hreg_dirty(regs, slot);
    return true;
}

/* Apply an immediate shift in place to a cached guest register. */
static bool jit_reg_apply_shift_imm(rv32_jit_writer_t *w,
                                    rv32_jit_reg_cache_t *regs, uint32_t reg, uint8_t subop, uint8_t amount)
{
    rv32_jit_reg_slot_t *slot = jit_reg_loaded_slot(w, regs, reg);

    if (slot == NULL)
    {
        return false;
    }

    if (!emit_hreg_shift_imm(w, slot->hreg, subop, amount))
    {
        return false;
    }

    jit_reg_mark_hreg_dirty(regs, slot);
    return true;
}

/* Apply a register-register ALU operation in place to a cached destination. */
static bool jit_reg_apply_reg(rv32_jit_writer_t *w,
                              rv32_jit_reg_cache_t *regs, uint32_t dst_reg, uint32_t src_reg,
                              uint8_t opcode)
{
    rv32_jit_reg_slot_t *dst = jit_reg_loaded_slot(w, regs, dst_reg);

    if (dst == NULL)
    {
        return false;
    }

    rv32_jit_reg_slot_t *src = jit_reg_loaded_slot(w, regs, src_reg);

    if (src == NULL)
    {
        return false;
    }

    if (!emit_hreg_binop_hreg(w, opcode, dst->hreg, src->hreg))
    {
        return false;
    }

    jit_reg_mark_hreg_dirty(regs, dst);
    return true;
}

/* Copy one guest register value to another using the host-register cache. */
static bool jit_reg_copy(rv32_jit_writer_t *w, rv32_jit_reg_cache_t *regs,
                         uint32_t dst_reg, uint32_t src_reg)
{
    if (dst_reg == 0)
    {
        return true;
    }

    if (src_reg == 0)
    {
        return jit_reg_write_imm(w, regs, dst_reg, 0);
    }

    rv32_jit_reg_slot_t *src = jit_reg_loaded_slot(w, regs, src_reg);

    if (src == NULL)
    {
        return false;
    }

    if (dst_reg == src_reg)
    {
        return true;
    }

    rv32_jit_reg_slot_t *dst = jit_reg_alloc(w, regs, dst_reg);

    if (dst == NULL)
    {
        return false;
    }

    if (!emit_mov_hreg_hreg(w, dst->hreg, src->hreg))
    {
        return false;
    }

    jit_reg_mark_hreg_dirty(regs, dst);
    return true;
}

/* Apply a register-count shift in place, using ECX for the x86 CL count. */
static bool jit_reg_apply_shift_reg(rv32_jit_writer_t *w,
                                    rv32_jit_reg_cache_t *regs, uint32_t dst_reg, uint32_t src_reg,
                                    uint8_t subop)
{
    rv32_jit_reg_slot_t *dst = jit_reg_loaded_slot(w, regs, dst_reg);

    if (dst == NULL || !jit_reg_read_ecx(w, regs, src_reg))
    {
        return false;
    }

    if (!emit_hreg_shift_cl(w, dst->hreg, subop))
    {
        return false;
    }

    jit_reg_mark_hreg_dirty(regs, dst);
    return true;
}

/* Emit MULH or MULHU by taking the high 32 bits from x86 EDX. */
static bool emit_rv32_mul_high(rv32_jit_writer_t *w,
                               rv32_jit_reg_cache_t *regs, uint32_t rd, bool is_signed)
{
    return (is_signed ? emit_imul_ecx(w) : emit_mul_ecx(w)) && emit_mov_eax_edx(w) && jit_reg_write_eax(w, regs, rd);
}

/* Emit RV32 DIVU, including the defined divide-by-zero all-ones result. */
static bool emit_rv32_divu(rv32_jit_writer_t *w,
                           rv32_jit_reg_cache_t *regs, uint32_t rd)
{
    uint8_t *zero_disp = NULL;
    uint8_t *done_disp = NULL;

    /*
   * RISC-V division by zero is not a trap: DIVU returns all ones. x86 DIV would
   * fault, so emit an explicit zero-divisor side exit around the native divide.
   */

    if (!emit_test_ecx_ecx(w) ||
        !emit_jcc_rel32_placeholder(w, 0x84, &zero_disp) ||
        !emit_xor_edx_edx(w) ||
        !emit_div_ecx(w) ||
        !emit_jmp_rel32_placeholder(w, &done_disp))
    {
        return false;
    }

    patch_rel32(zero_disp, w->cur);

    if (!emit_mov_eax_imm(w, UINT32_MAX))
    {
        return false;
    }

    patch_rel32(done_disp, w->cur);
    return jit_reg_write_eax(w, regs, rd);
}

/* Emit RV32 REMU, including the defined divide-by-zero dividend result. */
static bool emit_rv32_remu(rv32_jit_writer_t *w,
                           rv32_jit_reg_cache_t *regs, uint32_t rd)
{
    uint8_t *done_disp = NULL;

    /*
   * REMU by zero returns the original dividend. EAX already contains rs1, so
   * the zero-divisor branch can skip the native divide and keep EAX unchanged.
   */

    if (!emit_test_ecx_ecx(w) ||
        !emit_jcc_rel32_placeholder(w, 0x84, &done_disp) ||
        !emit_xor_edx_edx(w) ||
        !emit_div_ecx(w) ||
        !emit_mov_eax_edx(w))
    {
        return false;
    }

    patch_rel32(done_disp, w->cur);
    return jit_reg_write_eax(w, regs, rd);
}

/* Emit RV32 DIV, guarding both x86 signed-divide trap cases first. */
static bool emit_rv32_div(rv32_jit_writer_t *w,
                          rv32_jit_reg_cache_t *regs, uint32_t rd)
{
    uint8_t *zero_disp = NULL;
    uint8_t *normal_disp = NULL;
    uint8_t *overflow_disp = NULL;
    uint8_t *normal_done_disp = NULL;
    uint8_t *zero_done_disp = NULL;

    /*
   * x86 IDIV traps on zero divisors and on INT_MIN / -1. RISC-V defines both
   * cases, so guard them before using the native signed divide.
   */

    if (!emit_test_ecx_ecx(w) ||
        !emit_jcc_rel32_placeholder(w, 0x84, &zero_disp) ||
        !emit_cmp_eax_imm(w, 0x80000000u) ||
        !emit_jcc_rel32_placeholder(w, 0x85, &normal_disp) ||
        !emit_cmp_ecx_imm8(w, 0xff) ||
        !emit_jcc_rel32_placeholder(w, 0x84, &overflow_disp))
    {
        return false;
    }

    patch_rel32(normal_disp, w->cur);

    if (!emit_cdq(w) ||
        !emit_idiv_ecx(w) ||
        !emit_jmp_rel32_placeholder(w, &normal_done_disp))
    {
        return false;
    }

    patch_rel32(zero_disp, w->cur);

    if (!emit_mov_eax_imm(w, UINT32_MAX) ||
        !emit_jmp_rel32_placeholder(w, &zero_done_disp))
    {
        return false;
    }

    patch_rel32(overflow_disp, w->cur);

    if (!emit_mov_eax_imm(w, 0x80000000u))
    {
        return false;
    }

    patch_rel32(normal_done_disp, w->cur);
    patch_rel32(zero_done_disp, w->cur);
    return jit_reg_write_eax(w, regs, rd);
}

/* Emit RV32 REM, including zero-divisor and INT_MIN / -1 edge cases. */
static bool emit_rv32_rem(rv32_jit_writer_t *w,
                          rv32_jit_reg_cache_t *regs, uint32_t rd)
{
    uint8_t *zero_disp = NULL;
    uint8_t *normal_disp = NULL;
    uint8_t *overflow_disp = NULL;
    uint8_t *normal_done_disp = NULL;
    uint8_t *zero_done_disp = NULL;

    if (!emit_test_ecx_ecx(w) ||
        !emit_jcc_rel32_placeholder(w, 0x84, &zero_disp) ||
        !emit_cmp_eax_imm(w, 0x80000000u) ||
        !emit_jcc_rel32_placeholder(w, 0x85, &normal_disp) ||
        !emit_cmp_ecx_imm8(w, 0xff) ||
        !emit_jcc_rel32_placeholder(w, 0x84, &overflow_disp))
    {
        return false;
    }

    patch_rel32(normal_disp, w->cur);

    if (!emit_cdq(w) ||
        !emit_idiv_ecx(w) ||
        !emit_mov_eax_edx(w) ||
        !emit_jmp_rel32_placeholder(w, &normal_done_disp))
    {
        return false;
    }

    patch_rel32(zero_disp, w->cur);

    if (!emit_jmp_rel32_placeholder(w, &zero_done_disp))
    {
        return false;
    }

    patch_rel32(overflow_disp, w->cur);

    if (!emit_mov_eax_imm(w, 0))
    {
        return false;
    }

    patch_rel32(normal_done_disp, w->cur);
    patch_rel32(zero_done_disp, w->cur);
    return jit_reg_write_eax(w, regs, rd);
}

/* Emit `movabs r9, imm64`; r9 holds the source-chunk refcount table base. */
static bool emit_movabs_r9(rv32_jit_writer_t *w, uint64_t value)
{
    /* movabs r9, imm64 */
    return emit_u8(w, 0x49) && emit_u8(w, 0xb9) && emit_u64(w, value);
}

/* Emit `movabs r10, imm64`; r10 holds the host PMEM base pointer. */
static bool emit_movabs_r10(rv32_jit_writer_t *w, uint64_t value)
{
    /* movabs r10, imm64 */
    return emit_u8(w, 0x49) && emit_u8(w, 0xba) && emit_u64(w, value);
}

/* Load r10 with the host pointer corresponding to guest physical CONFIG_MBASE. */
static bool emit_load_pmem_base(rv32_jit_writer_t *w)
{
    /*
   * Direct-PMEM fast paths are common enough that loading this once per native
   * block is cheaper than repeating a movabs before every translated load or
   * store. r10 is caller-saved, so helper calls that rejoin the block reload it.
   */
    return emit_movabs_r10(w, (uint64_t)(uintptr_t)guest_to_host(CONFIG_MBASE));
}

/* Load r9 with the source-chunk refcount table base for store guards. */
static bool emit_load_source_refs_base(rv32_jit_writer_t *w)
{
    /*
   * r9 holds the source-chunk reference table for direct stores.  It is loaded
   * lazily because blocks with no stores do not need it, but once a store guard
   * has needed the table, later stores in the same straight-line block can reuse
   * the base instead of paying another movabs.
   */
    return emit_movabs_r9(w, (uint64_t)(uintptr_t)jit_source_chunk_refs);
}

/* Lazily load r9 once when the first direct-store source guard needs it. */
static bool jit_reg_ensure_source_refs_base(rv32_jit_writer_t *w,
                                            rv32_jit_reg_cache_t *regs)
{
    if (regs->source_refs_loaded)
    {
        return true;
    }

    regs->source_refs_loaded = true;
    return emit_load_source_refs_base(w);
}

/* Emit LEA that computes a PMEM offset in EDX from the guest address in EAX. */
static bool emit_lea_edx_eax_imm(rv32_jit_writer_t *w, uint32_t value)
{
    /*
   * lea edx, [rax + disp32] computes the low RV32 address bits in one
   * instruction. With disp32 = -CONFIG_MBASE it replaces mov edx,eax; sub edx,
   * CONFIG_MBASE in the direct-PMEM guard.
   */
    return emit_u8(w, 0x8d) && emit_u8(w, 0x90) && emit_u32(w, value);
}

/* Copy the PMEM offset from EDX to R8D for source-chunk calculations. */
static bool emit_mov_r8d_edx(rv32_jit_writer_t *w)
{
    return emit_u8(w, 0x41) && emit_u8(w, 0x89) && emit_u8(w, 0xd0);
}

/* Compare the computed PMEM offset in EDX with an immediate bound. */
static bool emit_cmp_edx_imm(rv32_jit_writer_t *w, uint32_t value)
{
    return emit_u8(w, 0x81) && emit_u8(w, 0xfa) && emit_u32(w, value);
}

/* Mask R8D with an immediate, used to test offset within a source chunk. */
static bool emit_and_r8d_imm(rv32_jit_writer_t *w, uint32_t value)
{
    return emit_u8(w, 0x41) && emit_u8(w, 0x81) && emit_u8(w, 0xe0) && emit_u32(w, value);
}

/* Compare R8D with an immediate during store source-chunk checks. */
static bool emit_cmp_r8d_imm(rv32_jit_writer_t *w, uint32_t value)
{
    return emit_u8(w, 0x41) && emit_u8(w, 0x81) && emit_u8(w, 0xf8) && emit_u32(w, value);
}

/* Shift R8D right to convert a PMEM byte offset into a chunk index. */
static bool emit_shr_r8d_imm(rv32_jit_writer_t *w, uint8_t value)
{
    return emit_u8(w, 0x41) && emit_u8(w, 0xc1) && emit_u8(w, 0xe8) && emit_u8(w, value);
}

/* Compare a byte field in the R8-pointed JIT TLB entry with an immediate. */
static bool emit_cmp_r8b_field_imm8(rv32_jit_writer_t *w, uint32_t offset,
                                    uint8_t value)
{
    Assert(offset <= INT8_MAX, "jit: TLB byte field offset is too large");
    return emit_u8(w, 0x41) && emit_u8(w, 0x80) && emit_u8(w, 0x78) && emit_u8(w, (uint8_t)offset) && emit_u8(w, value);
}

/* Compare a dword field in the R8-pointed JIT TLB entry with an immediate. */
static bool emit_cmp_r8d_field_imm32(rv32_jit_writer_t *w, uint32_t offset,
                                     uint32_t value)
{
    Assert(offset <= INT8_MAX, "jit: TLB dword field offset is too large");
    return emit_u8(w, 0x41) && emit_u8(w, 0x81) && emit_u8(w, 0x78) && emit_u8(w, (uint8_t)offset) && emit_u32(w, value);
}

/* Compare a dword field in the R8-pointed JIT TLB entry with EDX. */
static bool emit_cmp_r8d_field_edx(rv32_jit_writer_t *w, uint32_t offset)
{
    Assert(offset <= INT8_MAX, "jit: TLB dword field offset is too large");
    return emit_u8(w, 0x41) && emit_u8(w, 0x39) && emit_u8(w, 0x50) && emit_u8(w, (uint8_t)offset);
}

/* Test permission bits in a dword field in the R8-pointed JIT TLB entry. */
static bool emit_test_r8d_field_imm32(rv32_jit_writer_t *w, uint32_t offset,
                                      uint32_t value)
{
    Assert(offset <= INT8_MAX, "jit: TLB dword field offset is too large");
    return emit_u8(w, 0x41) && emit_u8(w, 0xf7) && emit_u8(w, 0x40) && emit_u8(w, (uint8_t)offset) && emit_u32(w, value);
}

/* Load a dword field from the R8-pointed JIT TLB entry into EDX. */
static bool emit_mov_edx_r8d_field(rv32_jit_writer_t *w, uint32_t offset)
{
    Assert(offset <= INT8_MAX, "jit: TLB dword field offset is too large");
    return emit_u8(w, 0x41) && emit_u8(w, 0x8b) && emit_u8(w, 0x50) && emit_u8(w, (uint8_t)offset);
}

/* Compare `jit_source_chunk_refs[r8d]` with zero inside generated code. */
static bool emit_cmp_source_chunk_ref_zero(rv32_jit_writer_t *w)
{
    /* cmp word ptr [r9 + r8 * 2], 0 */
    return emit_u8(w, 0x66) && emit_u8(w, 0x43) && emit_u8(w, 0x83) && emit_u8(w, 0x3c) && emit_u8(w, 0x41) && emit_u8(w, 0x00);
}

/* Compare `jit_tlb_pt_page_refs[r8d]` with zero inside generated code. */
static bool emit_cmp_pt_page_ref_zero(rv32_jit_writer_t *w)
{
    /*
   * Use RAX as an untracked table base so this guard does not disturb the lazy
   * R9 source-ref base used by store source-chunk checks elsewhere in the block.
   */
    return emit_movabs_rax(w, (uint64_t)(uintptr_t)jit_tlb_pt_page_refs)
           /* cmp word ptr [rax + r8 * 2], 0 */
           && emit_u8(w, 0x66) && emit_u8(w, 0x42) && emit_u8(w, 0x83) && emit_u8(w, 0x3c) && emit_u8(w, 0x40) && emit_u8(w, 0x00);
}

/*
 * Emit the generated-code guard for an inline PMEM access.
 *
 * Input: EAX contains the guest virtual address. Output on the fast path: EDX
 * contains the byte offset from CONFIG_MBASE. Slow-path branch placeholders are
 * recorded in `patch` so the caller can patch them after emitting the helper
 * path.
 */
static bool emit_direct_pmem_guard(rv32_jit_writer_t *w, uint32_t len,
                                   rv32_jit_pmem_guard_patch_t *patch)
{
    Assert(len >= 1 && len <= 4, "jit: unsupported direct PMEM width %u", len);

    /*
   * Keep the guard stricter than paddr_read(): it only accepts a complete
   * in-PMEM byte range. Any boundary, MMIO, paging, or wraparound case falls
   * back to the existing helper path.
   *
   * Blocks are tagged by satp and `jit_block_matches()` rejects a cached block
   * if satp changes. A block compiled in Bare mode can therefore omit the
   * runtime satp reload on every memory access; translated-mode blocks still
   * jump straight to the helper path.
   */

    if ((cpu.csr.satp & 0x80000000u) != 0)
    {
        if (!emit_jmp_rel32_placeholder(w, &patch->satp_slow_disp))
        {
            return false;
        }
    }

    return emit_lea_edx_eax_imm(w, 0u - (uint32_t)CONFIG_MBASE) && emit_cmp_edx_imm(w, (uint32_t)CONFIG_MSIZE - len) && emit_jcc_rel32_placeholder(w, 0x87, &patch->range_slow_disp);
}

/* Patch every slow-path branch emitted by emit_direct_pmem_guard(). */
static void patch_direct_pmem_guard(const rv32_jit_pmem_guard_patch_t *patch,
                                    const uint8_t *slow_path)
{
    if (patch->satp_slow_disp != NULL)
    {
        patch_rel32(patch->satp_slow_disp, slow_path);
    }
    patch_rel32(patch->range_slow_disp, slow_path);
}

/* Emit the inline PMEM load variant selected by the RV32 load funct3 field. */
static bool emit_direct_pmem_load_eax(rv32_jit_writer_t *w, uint32_t funct3)
{
    /*
   * EDX is the PMEM offset produced by emit_direct_pmem_guard().  The native
   * loads below mirror the RV32 load family exactly: byte/halfword signedness is
   * encoded in the x86 instruction, while LW naturally writes a 32-bit result.
   */
    switch (funct3)
    {
    case 0x0:
        /* movsx eax, byte ptr [r10 + rdx] */
        return emit_u8(w, 0x41) && emit_u8(w, 0x0f) && emit_u8(w, 0xbe) && emit_u8(w, 0x04) && emit_u8(w, 0x12);
    case 0x1:
        /* movsx eax, word ptr [r10 + rdx] */
        return emit_u8(w, 0x41) && emit_u8(w, 0x0f) && emit_u8(w, 0xbf) && emit_u8(w, 0x04) && emit_u8(w, 0x12);
    case 0x2:
        /* mov eax, dword ptr [r10 + rdx] */
        return emit_u8(w, 0x41) && emit_u8(w, 0x8b) && emit_u8(w, 0x04) && emit_u8(w, 0x12);
    case 0x4:
        /* movzx eax, byte ptr [r10 + rdx] */
        return emit_u8(w, 0x41) && emit_u8(w, 0x0f) && emit_u8(w, 0xb6) && emit_u8(w, 0x04) && emit_u8(w, 0x12);
    case 0x5:
        /* movzx eax, word ptr [r10 + rdx] */
        return emit_u8(w, 0x41) && emit_u8(w, 0x0f) && emit_u8(w, 0xb7) && emit_u8(w, 0x04) && emit_u8(w, 0x12);
    default:
        return false;
    }
}

/* Emit one conservative fallback branch for the inline Sv32 load guard. */
static bool emit_tlb_load_slow_jcc(rv32_jit_writer_t *w,
                                   rv32_jit_tlb_load_patch_t *patch, uint8_t jcc_opcode)
{
    Assert(patch->count < sizeof(patch->slow_disps) / sizeof(patch->slow_disps[0]),
           "jit: too many paged-load slow-path branches");
    return emit_jcc_rel32_placeholder(w, jcc_opcode,
                                      &patch->slow_disps[patch->count++]);
}

/* Patch every fallback branch emitted by emit_paged_tlb_load_eax(). */
static void patch_tlb_load_guard(const rv32_jit_tlb_load_patch_t *patch,
                                 const uint8_t *slow_path)
{
    for (uint32_t i = 0; i < patch->count; i++)
    {
        patch_rel32(patch->slow_disps[i], slow_path);
    }
}

/*
 * Emit an inline Sv32 TLB-hit load.
 *
 * Input: EAX contains the guest virtual address.  On success, EAX contains the
 * loaded RV32 value.  ECX preserves the original guest address for every slow
 * branch, so the helper fallback can receive the exact same argument it did
 * before this fast path existed.
 */
static bool emit_paged_tlb_load_eax(rv32_jit_writer_t *w, uint32_t funct3,
                                    uint32_t len, rv32_jit_tlb_load_patch_t *patch)
{
    Assert(len >= 1 && len <= 4, "jit: unsupported paged load width %u", len);

    const uint32_t satp = cpu.csr.satp;
    const uint32_t valid_off = (uint32_t)offsetof(rv32_jit_tlb_entry_t, valid);
    const uint32_t satp_off = (uint32_t)offsetof(rv32_jit_tlb_entry_t, satp);
    const uint32_t vpn_off = (uint32_t)offsetof(rv32_jit_tlb_entry_t, vpn);
    const uint32_t perm_off = (uint32_t)offsetof(rv32_jit_tlb_entry_t, perm);
    const uint32_t pg_paddr_off =
        (uint32_t)offsetof(rv32_jit_tlb_entry_t, pg_paddr);

    /*
   * The index calculation is:
   *   vpn = vaddr >> 12
   *   entry = &jit_tlb[vpn & (RV32_JIT_TLB_SIZE - 1)]
   * The 32-byte entry size lets the generated code use a shift rather than a
   * host multiply.  If the C struct layout changes, the typedef assertion near
   * rv32_jit_tlb_entry_t fails at build time.
   */

    if (!emit_mov_ecx_eax(w) ||
        !emit_mov_edx_eax(w) ||
        !emit_shr_edx_imm(w, PAGE_SHIFT) ||
        !emit_mov_r8d_edx(w) ||
        !emit_and_r8d_imm(w, RV32_JIT_TLB_SIZE - 1u) ||
        !emit_shl_r8_imm(w, 5) ||
        !emit_movabs_rdx(w, (uint64_t)(uintptr_t)jit_tlb) ||
        !emit_add_r8_rdx(w))
    {
        return false;
    }

    /*
   * Recompute VPN after loading the table base into RDX.  The generated block is
   * already tagged by satp, but checking the entry's satp as well protects the
   * direct-mapped JIT TLB from stale entries after address-space reuse.
   */
    return emit_mov_edx_eax(w) &&
           emit_shr_edx_imm(w, PAGE_SHIFT) &&
           emit_cmp_r8b_field_imm8(w, valid_off, 0) &&
           emit_tlb_load_slow_jcc(w, patch, 0x84) &&
           emit_cmp_r8d_field_imm32(w, satp_off, satp) &&
           emit_tlb_load_slow_jcc(w, patch, 0x85) &&
           emit_cmp_r8d_field_edx(w, vpn_off) &&
           emit_tlb_load_slow_jcc(w, patch, 0x85) &&
           emit_test_r8d_field_imm32(w, perm_off, RV32_JIT_PTE_R) &&
           emit_tlb_load_slow_jcc(w, patch, 0x84) &&
           emit_and_eax_imm(w, PAGE_MASK) &&
           emit_cmp_eax_imm(w, PAGE_SIZE - len) &&
           emit_tlb_load_slow_jcc(w, patch, 0x87) &&
           emit_mov_edx_r8d_field(w, pg_paddr_off) &&
           emit_or_edx_eax(w) &&
           emit_sub_edx_imm(w, (uint32_t)CONFIG_MBASE) &&
           emit_direct_pmem_load_eax(w, funct3);
}

/*
 * Emit an inline Sv32 TLB-hit store address translation.
 *
 * Input: EAX contains the guest virtual address and ECX contains the store
 * value.  On success, EDX contains the PMEM byte offset for `[r10 + rdx]`, and
 * ECX is still the store value.  EDI keeps the original guest address for the
 * helper fallback, because EAX is free for guard table bases after translation.
 */
static bool emit_paged_tlb_store_offset_edx(rv32_jit_writer_t *w, uint32_t len,
                                            rv32_jit_tlb_load_patch_t *patch)
{
    Assert(len >= 1 && len <= 4, "jit: unsupported paged store width %u", len);

    const uint32_t satp = cpu.csr.satp;
    const uint32_t valid_off = (uint32_t)offsetof(rv32_jit_tlb_entry_t, valid);
    const uint32_t satp_off = (uint32_t)offsetof(rv32_jit_tlb_entry_t, satp);
    const uint32_t vpn_off = (uint32_t)offsetof(rv32_jit_tlb_entry_t, vpn);
    const uint32_t perm_off = (uint32_t)offsetof(rv32_jit_tlb_entry_t, perm);
    const uint32_t pg_paddr_off =
        (uint32_t)offsetof(rv32_jit_tlb_entry_t, pg_paddr);

    if (!emit_mov_edi_eax(w) ||
        !emit_mov_edx_eax(w) ||
        !emit_shr_edx_imm(w, PAGE_SHIFT) ||
        !emit_mov_r8d_edx(w) ||
        !emit_and_r8d_imm(w, RV32_JIT_TLB_SIZE - 1u) ||
        !emit_shl_r8_imm(w, 5) ||
        !emit_movabs_rdx(w, (uint64_t)(uintptr_t)jit_tlb) ||
        !emit_add_r8_rdx(w))
    {
        return false;
    }

    return emit_mov_edx_eax(w) &&
           emit_shr_edx_imm(w, PAGE_SHIFT) &&
           emit_cmp_r8b_field_imm8(w, valid_off, 0) &&
           emit_tlb_load_slow_jcc(w, patch, 0x84) &&
           emit_cmp_r8d_field_imm32(w, satp_off, satp) &&
           emit_tlb_load_slow_jcc(w, patch, 0x85) &&
           emit_cmp_r8d_field_edx(w, vpn_off) &&
           emit_tlb_load_slow_jcc(w, patch, 0x85) &&
           emit_test_r8d_field_imm32(w, perm_off, RV32_JIT_PTE_W) &&
           emit_tlb_load_slow_jcc(w, patch, 0x84) &&
           emit_and_eax_imm(w, PAGE_MASK) &&
           emit_cmp_eax_imm(w, PAGE_SIZE - len) &&
           emit_tlb_load_slow_jcc(w, patch, 0x87) &&
           emit_mov_edx_r8d_field(w, pg_paddr_off) &&
           emit_or_edx_eax(w) &&
           emit_sub_edx_imm(w, (uint32_t)CONFIG_MBASE);
}

/* Emit an inline PMEM store from ECX using the selected byte width. */
static bool emit_direct_pmem_store_from_ecx(rv32_jit_writer_t *w, uint32_t len)
{
    /*
   * Stores use the low part of ECX so SB/SH truncate in the same way host_write()
   * does. The caller has already proved the final address is an in-PMEM byte
   * offset and checked source-code/page-table refs before taking this
   * continuation path.  That proof can come from Bare mode or from an Sv32 JIT
   * TLB hit.
   */
    switch (len)
    {
    case 1:
        /* mov byte ptr [r10 + rdx], cl */
        return emit_u8(w, 0x41) && emit_u8(w, 0x88) && emit_u8(w, 0x0c) && emit_u8(w, 0x12);
    case 2:
        /* mov word ptr [r10 + rdx], cx */
        return emit_u8(w, 0x66) && emit_u8(w, 0x41) && emit_u8(w, 0x89) && emit_u8(w, 0x0c) && emit_u8(w, 0x12);
    case 4:
        /* mov dword ptr [r10 + rdx], ecx */
        return emit_u8(w, 0x41) && emit_u8(w, 0x89) && emit_u8(w, 0x0c) && emit_u8(w, 0x12);
    default:
        return false;
    }
}

/*
 * Emit guards that decide whether an inline PMEM store can continue in-block.
 *
 * A direct store is safe to continue only when it stays within one source chunk
 * and that chunk has no compiled-code references. Otherwise the store must go
 * through the helper so exact invalidation happens before the next fetch.
 */
static bool emit_store_source_chunk_guard(rv32_jit_writer_t *w,
                                          rv32_jit_reg_cache_t *regs, uint32_t len, uint8_t **cross_chunk_disp,
                                          uint8_t **source_chunk_disp)
{
    Assert(len >= 1 && len <= 4, "jit: unsupported direct store width %u", len);

    /*
   * Direct continuing stores only handle one source-tracking chunk. Crossing a
   * chunk boundary is rare for byte/halfword/word stores, and the helper path
   * remains the conservative choice because it can perform exact invalidation
   * and return to cpu_exec() before the next guest fetch.
   */
    return emit_mov_r8d_edx(w) && emit_and_r8d_imm(w, RV32_JIT_SOURCE_CHUNK_MASK) && emit_cmp_r8d_imm(w, RV32_JIT_SOURCE_CHUNK_SIZE - len) && emit_jcc_rel32_placeholder(w, 0x87, cross_chunk_disp) && emit_mov_r8d_edx(w) && emit_shr_r8d_imm(w, RV32_JIT_SOURCE_CHUNK_SHIFT) && jit_reg_ensure_source_refs_base(w, regs) && emit_cmp_source_chunk_ref_zero(w) && emit_jcc_rel32_placeholder(w, 0x85, source_chunk_disp);
}

/* Emit a guard that keeps inline stores away from cached page-table pages. */
static bool emit_store_page_table_guard(rv32_jit_writer_t *w,
                                        uint8_t **page_table_disp)
{
    /*
   * EDX is a byte offset from CONFIG_MBASE.  Dividing by 4096 gives the PMEM
   * page index used by jit_tlb_pt_page_refs[].  A non-zero refcount means a
   * store could stale a JIT TLB entry, so the helper must perform the write,
   * flush the JIT TLB, and leave the native block.
   */
    return emit_mov_r8d_edx(w) && emit_shr_r8d_imm(w, PAGE_SHIFT) && emit_cmp_pt_page_ref_zero(w) && emit_jcc_rel32_placeholder(w, 0x85, page_table_disp);
}

/* Emit the common native-block prologue and load long-lived base registers. */
static bool emit_prologue(rv32_jit_writer_t *w)
{
    /*
   * System V enters generated code with rsp % 16 == 8. Five callee-saved
   * pushes align the stack before helper calls and provide the guest register
   * cache slots.
   */
    return emit_push_saved_hregs(w) && emit_load_cpu_base(w) && emit_load_pmem_base(w);
}

/* Emit the common native-block epilogue and return completed guest insn count. */
static bool emit_epilogue_return_count(rv32_jit_writer_t *w, uint32_t count)
{
    /* mov eax, count; pop saved cache registers; ret */
    return emit_u8(w, 0xb8) && emit_u32(w, count) && emit_pop_saved_hregs(w) && emit_u8(w, 0xc3);
}

/* Emit the common epilogue when EAX already holds the dynamic return count. */
static bool emit_epilogue_return_eax(rv32_jit_writer_t *w)
{
    return emit_pop_saved_hregs(w) && emit_u8(w, 0xc3);
}

/* Return `jit_loop_extra + count` for exits from blocks with chained laps. */
static bool emit_epilogue_return_loop_count(rv32_jit_writer_t *w, uint32_t count)
{
    return emit_movabs_rdx(w, (uint64_t)(uintptr_t)&jit_loop_extra) &&
           emit_mov_eax_m32_rdx(w) &&
           emit_add_eax_imm(w, count) &&
           emit_epilogue_return_eax(w);
}

/*
 * Translate one RV32 load instruction.
 *
 * The fast path performs direct PMEM loads inside the native block when Bare
 * mode or a simple Sv32 JIT TLB hit proves the final physical range.  The slow
 * path flushes dirty registers, sets cpu.pc to the load instruction, calls the
 * typed load helper, and reloads base registers that helper calls may clobber.
 */
static bool __attribute__((unused)) emit_load_instr(rv32_jit_writer_t *w,
                                                    rv32_jit_reg_cache_t *regs,
                                                    uint32_t instr, vaddr_t cur_pc)
{
    const uint32_t rd = bits(instr, 11, 7);
    const uint32_t funct3 = bits(instr, 14, 12);
    const uint32_t rs1 = bits(instr, 19, 15);

    uintptr_t helper = 0;
    uint32_t len = 0;
    switch (funct3)
    {
    case 0x0:
        helper = (uintptr_t)jit_load_i8;
        len = 1;
        break;
    case 0x1:
        helper = (uintptr_t)jit_load_i16;
        len = 2;
        break;
    case 0x2:
        helper = (uintptr_t)jit_load_u32;
        len = 4;
        break;
    case 0x4:
        helper = (uintptr_t)jit_load_u8;
        len = 1;
        break;
    case 0x5:
        helper = (uintptr_t)jit_load_u16;
        len = 2;
        break;
    default:
        return false;
    }

    if ((cpu.csr.satp & 0x80000000u) != 0)
    {
        rv32_jit_tlb_load_patch_t tlb_guard = {0};
        uint8_t *done_disp = NULL;

        if (!jit_reg_read_eax(w, regs, rs1) ||
            !emit_add_eax_imm(w, (uint32_t)imm_i(instr)) ||
            !emit_paged_tlb_load_eax(w, funct3, len, &tlb_guard) ||
            !emit_jmp_rel32_placeholder(w, &done_disp))
        {
            return false;
        }

        const uint8_t *slow_path = w->cur;
        patch_tlb_load_guard(&tlb_guard, slow_path);
        /*
     * The inline guard saves the full guest virtual address in ECX before it
     * masks EAX down to a page offset. Restore EAX so the old helper path keeps
     * the same argument and fault/MMIO behaviour as before.
     */

        if (!emit_mov_eax_ecx(w) ||
            !jit_reg_emit_flush_all_dirty(w, regs) ||
            !emit_set_pc_imm(w, cur_pc) ||
            !emit_u8(w, 0x89) || !emit_u8(w, 0xc7) ||
            !emit_call_abs(w, helper) ||
            !emit_load_cpu_base(w) ||
            !emit_load_pmem_base(w) ||
            (regs->source_refs_loaded && !emit_load_source_refs_base(w)))
        {
            return false;
        }

        patch_rel32(done_disp, w->cur);
        return jit_reg_write_eax(w, regs, rd);
    }

    rv32_jit_pmem_guard_patch_t guard = {0};
    uint8_t *done_disp = NULL;

    if (!jit_reg_read_eax(w, regs, rs1) ||
        !emit_add_eax_imm(w, (uint32_t)imm_i(instr)) ||
        !emit_direct_pmem_guard(w, len, &guard) ||
        !emit_direct_pmem_load_eax(w, funct3) ||
        !emit_jmp_rel32_placeholder(w, &done_disp))
    {
        return false;
    }

    const uint8_t *slow_path = w->cur;
    patch_direct_pmem_guard(&guard, slow_path);
    /*
   * The slow helper may enter the normal vaddr path, which can report MMIO,
   * translation, or bounds failures using cpu.pc. EAX still holds the guest
   * address here, so writing cpu.pc first does not disturb the helper argument.
   */

    if (!jit_reg_emit_flush_all_dirty(w, regs) ||
        !emit_set_pc_imm(w, cur_pc) ||
        !emit_u8(w, 0x89) || !emit_u8(w, 0xc7) ||
        !emit_call_abs(w, helper) ||
        !emit_load_cpu_base(w) ||
        !emit_load_pmem_base(w) ||
        (regs->source_refs_loaded && !emit_load_source_refs_base(w)))
    {
        return false;
    }

    patch_rel32(done_disp, w->cur);
    return jit_reg_write_eax(w, regs, rd);
}

/*
 * Translate one RV32 store instruction.
 *
 * Plain PMEM data stores can continue in the native block. Stores that may hit
 * MMIO, source bytes, or page-table pages call the store helper and then leave
 * the block, so the dispatcher observes any invalidation before the next block.
 */
static bool __attribute__((unused)) emit_store_instr(rv32_jit_writer_t *w,
                                                     rv32_jit_reg_cache_t *regs,
                                                     uint32_t instr, vaddr_t cur_pc,
                                                     vaddr_t next_pc, uint32_t exit_count)
{
    const uint32_t funct3 = bits(instr, 14, 12);
    const uint32_t rs1 = bits(instr, 19, 15);
    const uint32_t rs2 = bits(instr, 24, 20);

    uintptr_t helper = 0;
    uintptr_t continue_helper = 0;
    uint32_t len = 0;
    switch (funct3)
    {
    case 0x0:
        helper = (uintptr_t)jit_store_u8;
        continue_helper = (uintptr_t)jit_store_u8_continue;
        len = 1;
        break;
    case 0x1:
        helper = (uintptr_t)jit_store_u16;
        continue_helper = (uintptr_t)jit_store_u16_continue;
        len = 2;
        break;
    case 0x2:
        helper = (uintptr_t)jit_store_u32;
        continue_helper = (uintptr_t)jit_store_u32_continue;
        len = 4;
        break;
    default:
        return false;
    }

    if ((cpu.csr.satp & 0x80000000u) != 0)
    {
        rv32_jit_tlb_load_patch_t tlb_guard = {0};
        uint8_t *cross_chunk_disp = NULL;
        uint8_t *source_chunk_disp = NULL;
        uint8_t *page_table_disp = NULL;
        uint8_t *exit_disp = NULL;
        uint8_t *fast_done_disp = NULL;
        uint8_t *helper_done_disp = NULL;
        /*
     * Paged-mode stores first try the same translated-PMEM TLB hit that the C
     * helper would use.  Inline continuation is allowed only for ordinary data
     * pages: source-code writes and page-table writes still go through the
     * helper and then exit so invalidation is observed before the next fetch.
     */

        if (!jit_reg_read_eax(w, regs, rs1) ||
            !emit_add_eax_imm(w, (uint32_t)imm_s(instr)) ||
            !jit_reg_read_ecx(w, regs, rs2) ||
            !emit_paged_tlb_store_offset_edx(w, len, &tlb_guard) ||
            !emit_store_source_chunk_guard(w, regs, len, &cross_chunk_disp,
                                           &source_chunk_disp) ||
            !emit_store_page_table_guard(w, &page_table_disp) ||
            !emit_direct_pmem_store_from_ecx(w, len) ||
            !emit_jmp_rel32_placeholder(w, &fast_done_disp))
        {
            return false;
        }

        const uint8_t *slow_path = w->cur;
        patch_tlb_load_guard(&tlb_guard, slow_path);
        patch_rel32(cross_chunk_disp, slow_path);
        patch_rel32(source_chunk_disp, slow_path);
        patch_rel32(page_table_disp, slow_path);

        if (!jit_reg_emit_flush_all_dirty(w, regs) ||
            !emit_set_pc_imm(w, cur_pc) ||
            !emit_u8(w, 0x89) || !emit_u8(w, 0xce) ||
            !emit_call_abs(w, continue_helper) ||
            !emit_load_cpu_base(w) ||
            !emit_test_eax_eax(w) ||
            !emit_jcc_rel32_placeholder(w, 0x84, &exit_disp) ||
            !emit_load_pmem_base(w) ||
            (regs->source_refs_loaded && !emit_load_source_refs_base(w)) ||
            !emit_jmp_rel32_placeholder(w, &helper_done_disp))
        {
            return false;
        }

        patch_rel32(exit_disp, w->cur);

        if (!emit_set_pc_imm(w, next_pc) ||
            !emit_epilogue_return_count(w, exit_count))
        {
            return false;
        }

        patch_rel32(fast_done_disp, w->cur);
        patch_rel32(helper_done_disp, w->cur);
        return true;
    }

    rv32_jit_pmem_guard_patch_t guard = {0};
    uint8_t *cross_chunk_disp = NULL;
    uint8_t *source_chunk_disp = NULL;
    uint8_t *page_table_disp = NULL;
    uint8_t *done_disp = NULL;
    /*
   * Stores have two native continuations. Plain PMEM data stores commit inline
   * and continue in the same block; stores that might touch translated source
   * bytes divert to the helper, which invalidates by physical address and exits
   * before the dispatcher performs the next block lookup.
   */

    if (!jit_reg_read_eax(w, regs, rs1) ||
        !emit_add_eax_imm(w, (uint32_t)imm_s(instr)) ||
        !jit_reg_read_ecx(w, regs, rs2) ||
        !emit_direct_pmem_guard(w, len, &guard) ||
        !emit_store_source_chunk_guard(w, regs, len, &cross_chunk_disp,
                                       &source_chunk_disp) ||
        !emit_store_page_table_guard(w, &page_table_disp) ||
        !emit_direct_pmem_store_from_ecx(w, len) ||
        !emit_jmp_rel32_placeholder(w, &done_disp))
    {
        return false;
    }

    const uint8_t *slow_path = w->cur;
    patch_direct_pmem_guard(&guard, slow_path);
    patch_rel32(cross_chunk_disp, slow_path);
    patch_rel32(source_chunk_disp, slow_path);
    patch_rel32(page_table_disp, slow_path);

    /*
   * The helper path handles MMIO, paging, cross-chunk direct stores, and source
   * code invalidation. Set cpu.pc to the store itself before the call so faults
   * and MMIO diagnostics identify the correct guest instruction. After a
   * successful helper return, advance cpu.pc and leave the native block; the JIT
   * dispatcher may run another block, but it will start from the post-store PC.
   */

    if (!jit_reg_emit_flush_all_dirty(w, regs) ||
        !emit_set_pc_imm(w, cur_pc) ||
        !emit_u8(w, 0x89) || !emit_u8(w, 0xc7) ||
        !emit_u8(w, 0x89) || !emit_u8(w, 0xce) ||
        !emit_call_abs(w, helper) ||
        !emit_load_cpu_base(w) ||
        !emit_set_pc_imm(w, next_pc) ||
        !emit_epilogue_return_count(w, exit_count))
    {
        return false;
    }

    patch_rel32(done_disp, w->cur);
    return true;
}

/* Dispatch a decoded LOAD or STORE opcode to its specialised emitter. */
static bool emit_load_store_instr(rv32_jit_writer_t *w,
                                  rv32_jit_reg_cache_t *regs, uint32_t instr, vaddr_t cur_pc,
                                  uint32_t exit_count)
{
    (void)w;
    (void)regs;
    (void)instr;
    (void)cur_pc;
    (void)exit_count;

    /*
     * Strict RISC-V visible traps require load/store alignment and page-fault
     * decisions before any destination register or memory side effect.  The
     * existing native memory emitters predate that policy and include direct
     * PMEM paths, so keep memory instructions on the strict non-JIT path until
     * those emitters grow equivalent runtime guards.
     */
    return false;
}

/* Return true for RV32 instructions that terminate a straight-line block. */
static bool jit_instr_is_control_flow(uint32_t instr)
{
    const uint32_t opcode = instr & 0x7fu;
    return opcode == 0x63 || opcode == 0x6f || opcode == 0x67;
}

/* Return true for instructions that can stay inside a chained loop body. */
static bool jit_instr_can_chain_body(uint32_t instr)
{
    const uint32_t opcode = instr & 0x7fu;

    switch (opcode)
    {
    case 0x13: /* OP-IMM */
    case 0x17: /* AUIPC */
    case 0x33: /* OP */
    case 0x37: /* LUI */
    case 0x63: /* BRANCH */
        return true;
    default:
        return false;
    }
}

/* Translate one conditional branch, keeping fall-through in the same block. */
static bool emit_branch_chain_backedge(rv32_jit_writer_t *w,
                                       rv32_jit_reg_cache_t *regs,
                                       vaddr_t target, uint32_t exit_count,
                                       const uint8_t *target_native)
{
    uint8_t *over_budget_disp = NULL;
    uint8_t *loop_disp = NULL;

    /*
     * The taken branch has already completed `exit_count` guest instructions from
     * the native loop head. Chain only when another full lap fits the current
     * cpu_exec() budget; otherwise return to the dispatcher at the branch target.
     */
    if (!emit_movabs_rdx(w, (uint64_t)(uintptr_t)&jit_loop_extra) ||
        !emit_mov_eax_m32_rdx(w) ||
        !emit_add_eax_imm(w, exit_count) ||
        !emit_mov_ecx_eax(w) ||
        !emit_add_ecx_imm(w, exit_count) ||
        !emit_movabs_rdx(w, (uint64_t)(uintptr_t)&jit_entry_budget) ||
        !emit_cmp_ecx_m32_rdx(w) ||
        !emit_jcc_rel32_placeholder(w, 0x87, &over_budget_disp) ||
        !emit_movabs_rdx(w, (uint64_t)(uintptr_t)&jit_loop_extra) ||
        !emit_mov_m32_rdx_eax(w) ||
        !jit_reg_emit_flush_all_dirty(w, regs) ||
        !emit_jmp_rel32_placeholder(w, &loop_disp))
    {
        return false;
    }

    patch_rel32(loop_disp, target_native);
    patch_rel32(over_budget_disp, w->cur);

    return jit_reg_emit_flush_all_dirty(w, regs) &&
           emit_set_pc_imm(w, target) &&
           emit_epilogue_return_eax(w);
}

static bool emit_branch_instr(rv32_jit_writer_t *w, rv32_jit_reg_cache_t *regs,
                              uint32_t instr, vaddr_t pc, vaddr_t block_start_pc,
                              const uint8_t *block_start_native,
                              bool loop_count_needed, bool chain_safe,
                              bool *branch_chained,
                              uint32_t exit_count)
{
    const uint32_t funct3 = bits(instr, 14, 12);
    const uint32_t rs1 = bits(instr, 19, 15);
    const uint32_t rs2 = bits(instr, 24, 20);
    uint8_t jcc = 0;

    switch (funct3)
    {
    case 0x0:
        jcc = 0x84;
        break; /* JE  */
    case 0x1:
        jcc = 0x85;
        break; /* JNE */
    case 0x4:
        jcc = 0x8c;
        break; /* JL, signed */
    case 0x5:
        jcc = 0x8d;
        break; /* JGE, signed */
    case 0x6:
        jcc = 0x82;
        break; /* JB, unsigned */
    case 0x7:
        jcc = 0x83;
        break; /* JAE, unsigned */
    default:
        return false;
    }

    uint8_t *fallthrough_disp = NULL;
    const vaddr_t target = pc + imm_b(instr);

    if ((target & 0x3u) != 0)
    {
        return false;
    }

    /*
     * Conditional branches are the first control-flow case that can keep useful
     * cached registers alive. The untaken path stays in this native block, while
     * the taken path materialises the same register state into cpu.gpr[] and
     * returns to the dispatcher at the branch target.
     */

    if (!jit_reg_read_eax(w, regs, rs1) || !jit_reg_read_ecx(w, regs, rs2) ||
        !emit_cmp_eax_ecx(w) ||
        !emit_jcc_rel32_placeholder(w, (uint8_t)(jcc ^ 1u),
                                    &fallthrough_disp))
    {
        return false;
    }

    if (chain_safe && target == block_start_pc)
    {
        if (!emit_branch_chain_backedge(w, regs, target, exit_count,
                                        block_start_native))
        {
            return false;
        }
        *branch_chained = true;
    }
    else if (!jit_reg_emit_flush_all_dirty(w, regs) ||
             !emit_set_pc_imm(w, target) ||
             !(loop_count_needed
                   ? emit_epilogue_return_loop_count(w, exit_count)
                   : emit_epilogue_return_count(w, exit_count)))
    {
        return false;
    }

    patch_rel32(fallthrough_disp, w->cur);
    return true;
}

/* Translate JAL and JALR control flow instructions that always end the block. */
static bool emit_control_flow_instr(rv32_jit_writer_t *w,
                                    rv32_jit_reg_cache_t *regs, uint32_t instr, vaddr_t pc)
{
    const uint32_t opcode = instr & 0x7fu;
    const uint32_t rd = bits(instr, 11, 7);
    const uint32_t funct3 = bits(instr, 14, 12);

    if (opcode == 0x63)
    {
        return false;
    }

    if (opcode == 0x6f)
    {
        const vaddr_t target = pc + imm_j(instr);

        if ((target & 0x3u) != 0)
        {
            return false;
        }

        return emit_mov_eax_imm(w, pc + 4u) && jit_reg_write_eax(w, regs, rd) && jit_reg_emit_flush_all_dirty(w, regs) && emit_set_pc_imm(w, target);
    }

    if (opcode == 0x67 && funct3 == 0)
    {
        /*
         * JALR's misaligned-target trap is data-dependent. Falling back keeps
         * the required "trap before link write" ordering without inlining a
         * second trap path into native code.
         */
        return false;
    }

    return false;
}

/*
 * Translate RV32 integer ALU instructions.
 *
 * This emitter handles LUI, AUIPC, OP-IMM, OP, and common RV32M operations. It
 * first tries cache-friendly forms that update guest-register slots directly,
 * then falls back to EAX/ECX temporary sequences for less convenient cases.
 */
static bool emit_alu_instr(rv32_jit_writer_t *w, rv32_jit_reg_cache_t *regs,
                           uint32_t instr, vaddr_t cur_pc)
{
    const uint32_t opcode = instr & 0x7fu;
    const uint32_t rd = bits(instr, 11, 7);
    const uint32_t funct3 = bits(instr, 14, 12);
    const uint32_t rs1 = bits(instr, 19, 15);
    const uint32_t rs2 = bits(instr, 24, 20);
    const uint32_t funct7 = bits(instr, 31, 25);

    if (opcode == 0x37)
    {
        /* LUI places the U-immediate directly in rd. */
        return jit_reg_write_imm(w, regs, rd, imm_u(instr));
    }

    if (opcode == 0x17)
    {
        return jit_reg_write_imm(w, regs, rd, cur_pc + imm_u(instr));
    }

    if (opcode == 0x13)
    {
        const uint32_t imm = (uint32_t)imm_i(instr);

        if (rs1 == 0)
        {
            switch (funct3)
            {
            case 0x0:
                return jit_reg_write_imm(w, regs, rd, imm);
            case 0x1:
                if (bits(instr, 31, 25) != 0x00)
                {
                    return false;
                }
                return jit_reg_write_imm(w, regs, rd, 0);
            case 0x2:
                return jit_reg_write_imm(w, regs, rd, (int32_t)0 < imm_i(instr));
            case 0x3:
                return jit_reg_write_imm(w, regs, rd, imm != 0);
            case 0x4:
                return jit_reg_write_imm(w, regs, rd, imm);
            case 0x5:
                if (bits(instr, 31, 25) == 0x00 || bits(instr, 31, 25) == 0x20)
                {
                    return jit_reg_write_imm(w, regs, rd, 0);
                }
                return false;
            case 0x6:
                return jit_reg_write_imm(w, regs, rd, imm);
            case 0x7:
                return jit_reg_write_imm(w, regs, rd, 0);
            default:
                return false;
            }
        }

        if (rd != 0 && rd == rs1)
        {
            switch (funct3)
            {
            case 0x0:
                return imm == 0 ? true : jit_reg_apply_imm(w, regs, rd, 0, imm);
            case 0x1:
                if (bits(instr, 31, 25) != 0x00)
                {
                    return false;
                }
                return jit_reg_apply_shift_imm(w, regs, rd, 4,
                                               (uint8_t)bits(instr, 24, 20));
            case 0x4:
                return imm == 0 ? true : jit_reg_apply_imm(w, regs, rd, 6, imm);
            case 0x5:
                if (bits(instr, 31, 25) == 0x00)
                {
                    return jit_reg_apply_shift_imm(w, regs, rd, 5,
                                                   (uint8_t)bits(instr, 24, 20));
                }

                if (bits(instr, 31, 25) == 0x20)
                {
                    return jit_reg_apply_shift_imm(w, regs, rd, 7,
                                                   (uint8_t)bits(instr, 24, 20));
                }
                return false;
            case 0x6:
                return imm == 0 ? true : jit_reg_apply_imm(w, regs, rd, 1, imm);
            case 0x7:
                return jit_reg_apply_imm(w, regs, rd, 4, imm);
            default:
                break;
            }
        }

        if (rd != 0)
        {
            const uint8_t shamt = (uint8_t)bits(instr, 24, 20);

            /*
       * The compiler emits many OP-IMM instructions as copies or as a simple
       * transformation of one live value into a different destination register.
       * Keep those inside the guest-register cache instead of bouncing through
       * eax and then copying back to a cache slot.
       */
            switch (funct3)
            {
            case 0x0:
                return jit_reg_copy(w, regs, rd, rs1) &&
                       (imm == 0 || jit_reg_apply_imm(w, regs, rd, 0, imm));
            case 0x1:
                if (bits(instr, 31, 25) != 0x00)
                {
                    return false;
                }
                return jit_reg_copy(w, regs, rd, rs1) &&
                       (shamt == 0 || jit_reg_apply_shift_imm(w, regs, rd, 4, shamt));
            case 0x4:
                return jit_reg_copy(w, regs, rd, rs1) &&
                       (imm == 0 || jit_reg_apply_imm(w, regs, rd, 6, imm));
            case 0x5:
                if (bits(instr, 31, 25) == 0x00)
                {
                    return jit_reg_copy(w, regs, rd, rs1) &&
                           (shamt == 0 ||
                            jit_reg_apply_shift_imm(w, regs, rd, 5, shamt));
                }

                if (bits(instr, 31, 25) == 0x20)
                {
                    return jit_reg_copy(w, regs, rd, rs1) &&
                           (shamt == 0 ||
                            jit_reg_apply_shift_imm(w, regs, rd, 7, shamt));
                }
                return false;
            case 0x6:
                return jit_reg_copy(w, regs, rd, rs1) &&
                       (imm == 0 || jit_reg_apply_imm(w, regs, rd, 1, imm));
            case 0x7:
                if (imm == 0)
                {
                    return jit_reg_write_imm(w, regs, rd, 0);
                }
                return jit_reg_copy(w, regs, rd, rs1) &&
                       (imm == UINT32_MAX || jit_reg_apply_imm(w, regs, rd, 4, imm));
            default:
                break;
            }
        }

        if (!jit_reg_read_eax(w, regs, rs1))
        {
            return false;
        }

        switch (funct3)
        {
        case 0x0:
            return emit_add_eax_imm(w, imm) && jit_reg_write_eax(w, regs, rd);
        case 0x1:
            if (bits(instr, 31, 25) != 0x00)
            {
                return false;
            }
            return emit_u8(w, 0xc1) && emit_u8(w, 0xe0) && emit_u8(w, bits(instr, 24, 20)) && jit_reg_write_eax(w, regs, rd);
        case 0x2:
            return emit_cmp_eax_imm(w, imm) && emit_setcc_eax(w, 0x9c) && jit_reg_write_eax(w, regs, rd);
        case 0x3:
            return emit_cmp_eax_imm(w, imm) && emit_setcc_eax(w, 0x92) && jit_reg_write_eax(w, regs, rd);
        case 0x4:
            return emit_u8(w, 0x35) && emit_u32(w, imm) && jit_reg_write_eax(w, regs, rd);
        case 0x5:
            if (bits(instr, 31, 25) == 0x00)
            {
                return emit_u8(w, 0xc1) && emit_u8(w, 0xe8) && emit_u8(w, bits(instr, 24, 20)) && jit_reg_write_eax(w, regs, rd);
            }

            if (bits(instr, 31, 25) == 0x20)
            {
                return emit_u8(w, 0xc1) && emit_u8(w, 0xf8) && emit_u8(w, bits(instr, 24, 20)) && jit_reg_write_eax(w, regs, rd);
            }
            return false;
        case 0x6:
            return emit_u8(w, 0x0d) && emit_u32(w, imm) && jit_reg_write_eax(w, regs, rd);
        case 0x7:
            return emit_u8(w, 0x25) && emit_u32(w, imm) && jit_reg_write_eax(w, regs, rd);
        default:
            return false;
        }
    }

    if (opcode == 0x33)
    {
        const uint32_t key = (funct7 << 3) | funct3;

        if (rd != 0)
        {
            switch (key)
            {
            case 0x000:
                if (rd == rs1 && rs2 != 0)
                {
                    return jit_reg_apply_reg(w, regs, rd, rs2, 0x01);
                }

                if (rd == rs2 && rs1 != 0)
                {
                    return jit_reg_apply_reg(w, regs, rd, rs1, 0x01);
                }
                break;
            case 0x100:
                if (rd == rs1 && rs2 != 0)
                {
                    return jit_reg_apply_reg(w, regs, rd, rs2, 0x29);
                }
                break;
            case 0x001:
                if (rd == rs1)
                {
                    return jit_reg_apply_shift_reg(w, regs, rd, rs2, 4);
                }
                break;
            case 0x004:
                if (rd == rs1 && rs2 != 0)
                {
                    return jit_reg_apply_reg(w, regs, rd, rs2, 0x31);
                }

                if (rd == rs2 && rs1 != 0)
                {
                    return jit_reg_apply_reg(w, regs, rd, rs1, 0x31);
                }
                break;
            case 0x005:
                if (rd == rs1)
                {
                    return jit_reg_apply_shift_reg(w, regs, rd, rs2, 5);
                }
                break;
            case 0x105:
                if (rd == rs1)
                {
                    return jit_reg_apply_shift_reg(w, regs, rd, rs2, 7);
                }
                break;
            case 0x006:
                if (rd == rs1 && rs2 != 0)
                {
                    return jit_reg_apply_reg(w, regs, rd, rs2, 0x09);
                }

                if (rd == rs2 && rs1 != 0)
                {
                    return jit_reg_apply_reg(w, regs, rd, rs1, 0x09);
                }
                break;
            case 0x007:
                if (rd == rs1 && rs2 != 0)
                {
                    return jit_reg_apply_reg(w, regs, rd, rs2, 0x21);
                }

                if (rd == rs2 && rs1 != 0)
                {
                    return jit_reg_apply_reg(w, regs, rd, rs1, 0x21);
                }
                break;
            default:
                break;
            }

            /*
       * If rd is a third guest register, start by copying rs1 into rd and then
       * apply the second operand in place. This emits one cached-register move
       * plus the ALU operation, avoiding the old sequence
       *   cached rs1 -> eax -> ALU -> cached rd.
       * The rd != rs2 condition is important for shifts and subtraction because
       * overwriting rd would otherwise destroy the still-needed source value.
       */
            switch (key)
            {
            case 0x000:
                if (rs1 == 0)
                {
                    return jit_reg_copy(w, regs, rd, rs2);
                }

                if (rs2 == 0)
                {
                    return jit_reg_copy(w, regs, rd, rs1);
                }

                if (rd != rs1 && rd != rs2)
                {
                    return jit_reg_copy(w, regs, rd, rs1) &&
                           jit_reg_apply_reg(w, regs, rd, rs2, 0x01);
                }
                break;
            case 0x100:
                if (rs1 == rs2 || rs2 == 0)
                {
                    return rs1 == rs2 ? jit_reg_write_imm(w, regs, rd, 0) : jit_reg_copy(w, regs, rd, rs1);
                }

                if (rd != rs1 && rd != rs2)
                {
                    return jit_reg_copy(w, regs, rd, rs1) &&
                           jit_reg_apply_reg(w, regs, rd, rs2, 0x29);
                }
                break;
            case 0x001:
                if (rs1 == 0)
                {
                    return jit_reg_write_imm(w, regs, rd, 0);
                }

                if (rs2 == 0)
                {
                    return jit_reg_copy(w, regs, rd, rs1);
                }

                if (rd != rs1 && rd != rs2)
                {
                    return jit_reg_copy(w, regs, rd, rs1) &&
                           jit_reg_apply_shift_reg(w, regs, rd, rs2, 4);
                }
                break;
            case 0x002:
            case 0x003:
                if (rs1 == rs2)
                {
                    return jit_reg_write_imm(w, regs, rd, 0);
                }
                break;
            case 0x004:
                if (rs1 == rs2)
                {
                    return jit_reg_write_imm(w, regs, rd, 0);
                }

                if (rs1 == 0)
                {
                    return jit_reg_copy(w, regs, rd, rs2);
                }

                if (rs2 == 0)
                {
                    return jit_reg_copy(w, regs, rd, rs1);
                }

                if (rd != rs1 && rd != rs2)
                {
                    return jit_reg_copy(w, regs, rd, rs1) &&
                           jit_reg_apply_reg(w, regs, rd, rs2, 0x31);
                }
                break;
            case 0x005:
            case 0x105:
                if (rs1 == 0)
                {
                    return jit_reg_write_imm(w, regs, rd, 0);
                }

                if (rs2 == 0)
                {
                    return jit_reg_copy(w, regs, rd, rs1);
                }

                if (rd != rs1 && rd != rs2)
                {
                    return jit_reg_copy(w, regs, rd, rs1) &&
                           jit_reg_apply_shift_reg(w, regs, rd, rs2,
                                                   key == 0x005 ? 5 : 7);
                }
                break;
            case 0x006:
                if (rs1 == 0)
                {
                    return jit_reg_copy(w, regs, rd, rs2);
                }

                if (rs2 == 0)
                {
                    return jit_reg_copy(w, regs, rd, rs1);
                }

                if (rd != rs1 && rd != rs2)
                {
                    return jit_reg_copy(w, regs, rd, rs1) &&
                           jit_reg_apply_reg(w, regs, rd, rs2, 0x09);
                }
                break;
            case 0x007:
                if (rs1 == 0 || rs2 == 0)
                {
                    return jit_reg_write_imm(w, regs, rd, 0);
                }

                if (rd != rs1 && rd != rs2)
                {
                    return jit_reg_copy(w, regs, rd, rs1) &&
                           jit_reg_apply_reg(w, regs, rd, rs2, 0x21);
                }
                break;
            default:
                break;
            }
        }

        if (!jit_reg_read_eax(w, regs, rs1) ||
            !jit_reg_read_ecx(w, regs, rs2))
        {
            return false;
        }

        switch (key)
        {
        case 0x000:
            return emit_u8(w, 0x01) && emit_u8(w, 0xc8) && jit_reg_write_eax(w, regs, rd);
        case 0x100:
            return emit_u8(w, 0x29) && emit_u8(w, 0xc8) && jit_reg_write_eax(w, regs, rd);
        case 0x001:
            return emit_u8(w, 0xd3) && emit_u8(w, 0xe0) && jit_reg_write_eax(w, regs, rd);
        case 0x002:
            return emit_cmp_eax_ecx(w) && emit_setcc_eax(w, 0x9c) && jit_reg_write_eax(w, regs, rd);
        case 0x003:
            return emit_cmp_eax_ecx(w) && emit_setcc_eax(w, 0x92) && jit_reg_write_eax(w, regs, rd);
        case 0x004:
            return emit_u8(w, 0x31) && emit_u8(w, 0xc8) && jit_reg_write_eax(w, regs, rd);
        case 0x005:
            return emit_u8(w, 0xd3) && emit_u8(w, 0xe8) && jit_reg_write_eax(w, regs, rd);
        case 0x105:
            return emit_u8(w, 0xd3) && emit_u8(w, 0xf8) && jit_reg_write_eax(w, regs, rd);
        case 0x006:
            return emit_u8(w, 0x09) && emit_u8(w, 0xc8) && jit_reg_write_eax(w, regs, rd);
        case 0x007:
            return emit_u8(w, 0x21) && emit_u8(w, 0xc8) && jit_reg_write_eax(w, regs, rd);
        case 0x008:
            return emit_u8(w, 0x0f) && emit_u8(w, 0xaf) && emit_u8(w, 0xc1) && jit_reg_write_eax(w, regs, rd);
        case 0x009:
            return emit_rv32_mul_high(w, regs, rd, true);
        case 0x00b:
            return emit_rv32_mul_high(w, regs, rd, false);
        case 0x00c:
            return emit_rv32_div(w, regs, rd);
        case 0x00d:
            return emit_rv32_divu(w, regs, rd);
        case 0x00e:
            return emit_rv32_rem(w, regs, rd);
        case 0x00f:
            return emit_rv32_remu(w, regs, rd);
        case 0x00a:
            return jit_reg_flush_all_dirty(w, regs) && emit_u8(w, 0xbf) && emit_u32(w, instr) && emit_call_abs(w, (uintptr_t)jit_op_complex) && emit_load_cpu_base(w) && emit_load_pmem_base(w) && (!regs->source_refs_loaded || emit_load_source_refs_base(w)) && (jit_reg_invalidate_all(regs), true) && jit_reg_write_eax(w, regs, rd);
        default:
            return false;
        }
    }

    return false;
}

/* Public hook: report whether native RISC-V32 JIT execution can be attempted. */
bool isa_jit_available(void)
{
    return RV32_JIT_ENABLED && !jit_runtime_disabled();
}

/* Public hook: discard all native blocks after broad CPU or address-space change. */
void isa_jit_flush_all(void)
{
    /*
   * A full flush drops every piece of JIT-owned state: native code, source refs,
   * and local Sv32 translations.  Snapshot restore is the clearest example: PMEM
   * and CSRs may both change while old (pc, satp) tags still look plausible.
   */

    if (jit_code != NULL)
    {
        jit_arena_reset();
    }
    jit_tlb_flush();
}

/* Public hook: react to PMEM writes that can stale native code or JIT translations. */
void isa_jit_invalidate_paddr(paddr_t addr, int len)
{
    JIT_STAT_INC(invalidation_requests);

    /*
   * Physical writes are the common point shared by interpreter stores, JIT helper
   * stores, fast-exec stores, and devices.  Two independent JIT caches can become
   * stale here:
   *
   *   1. native blocks translated from overwritten instruction bytes;
   *   2. local Sv32 translations whose root or level-0 PTE page was modified.
   *
   * The source-code check below uses the half-open interval [addr, addr + len).
   */

    if (len <= 0)
    {
        return;
    }

    if (jit_write_may_touch_page_table(addr, len))
    {
        jit_tlb_flush();
    }

    if (!jit_write_may_touch_source_chunk(addr, len))
    {
        JIT_STAT_INC(invalidation_page_skips);
        return;
    }

    const paddr_t end = addr + (paddr_t)len;
    for (size_t i = 0; i < RV32_JIT_CACHE_SIZE; i++)
    {
        rv32_jit_block_t *block = &jit_cache[i];

        if (!block->valid)
        {
            continue;
        }

        const paddr_t block_end = block->paddr_start + block->source_len;

        if (addr < block_end && end > block->paddr_start)
        {
            JIT_STAT_INC(invalidated_blocks);
            jit_block_discard(block);
        }
    }
}

/*
 * Translate an instruction-fetch virtual PC to the physical source byte address.
 *
 * Blocks are invalidated by physical PMEM writes, so every cache entry records
 * the physical bytes that backed its translated guest instructions.
 */
static bool jit_translate_ifetch(vaddr_t pc, paddr_t *paddr)
{
    const int mmu = isa_mmu_check(pc, 4, MEM_TYPE_IFETCH);

    if (mmu == MMU_DIRECT)
    {
        *paddr = (paddr_t)pc;
        return true;
    }

    if (mmu == MMU_TRANSLATE)
    {
        const paddr_t ret = isa_mmu_translate(pc, 4, MEM_TYPE_IFETCH);

        if ((ret & (paddr_t)PAGE_MASK) == MEM_RET_OK)
        {
            *paddr = (ret & ~(paddr_t)PAGE_MASK) | (paddr_t)(pc & PAGE_MASK);
            return true;
        }
    }

    return false;
}

/* Check whether a cache slot is still valid for the current PC, satp, and mapping. */
static bool jit_block_matches(const rv32_jit_block_t *block, vaddr_t pc)
{
    /*
   * Cheap tag checks come first. Unsupported markers also pass this test when
   * their PC and satp still match; the caller will see entry == NULL and fall
   * back without trying to execute native code.
   */

    if (!block->valid || block->pc != pc || block->satp != cpu.csr.satp)
    {
        return false;
    }

    /*
   * satp alone is not enough in paged mode: a guest can rewrite page tables so
   * the same virtual PC points at different physical source bytes. Re-translate
   * the first instruction before trusting cached native code.
   */

    if ((cpu.csr.satp & 0x80000000u) != 0)
    {
        paddr_t now = 0;
        /*
     * A translation failure is treated as a cache miss. That keeps the JIT out
     * of cases where the normal interpreter path needs to raise or report the
     * underlying memory problem.
     */

        if (!jit_translate_ifetch(pc, &now) || now != block->paddr_start)
        {
            return false;
        }
    }

    return true;
}

/* Return the direct-mapped cache slot for the current PC and CPU satp tag. */
static rv32_jit_block_t *jit_cache_slot(vaddr_t pc)
{
    return &jit_cache[jit_hash(pc, cpu.csr.satp)];
}

/* Publish a negative cache entry for an instruction this JIT cannot translate. */
static void jit_mark_unsupported(vaddr_t pc, paddr_t paddr, uint32_t source_len)
{
    JIT_STAT_INC(blocks_unsupported);

    /*
   * Negative cache entries still include satp and the first physical source
   * address so paged-mode lookups can reject them after a remap. They do not
   * own source-chunk refs because no native code can become stale; at worst, a
   * later overwrite keeps this PC on the interpreter path until normal slot
   * eviction or a full flush gives compilation another chance.
   *
   * The trade-off is deliberate: unsupported code is correctness-neutral because
   * it falls back immediately, while source-refcounting it would make every data
   * write near that instruction pay invalidation cost for no executable block.
   */
    rv32_jit_block_t *block = jit_cache_slot(pc);
    jit_block_discard(block);
    *block = (rv32_jit_block_t){
        .valid = true,
        .pc = pc,
        .satp = cpu.csr.satp,
        .paddr_start = paddr,
        .source_len = source_len,
        .insn_count = 0,
        .entry = NULL,
    };
}

/* Cheaply pre-scan whether this block can use loop-aware branch exits. */
static bool jit_block_has_chainable_backedge(vaddr_t pc, uint32_t max_insns,
                                             paddr_t first_paddr)
{
    vaddr_t cur_pc = pc;
    uint32_t count = 0;
    uint32_t source_len = 0;

    while (count < max_insns && count < RV32_JIT_BLOCK_MAX_INSNS)
    {
        if (count != 0 && (cpu.csr.satp & RV32_JIT_SATP_MODE_MASK) != 0 &&
            ((cur_pc ^ pc) & ~(vaddr_t)PAGE_MASK) != 0)
        {
            return false;
        }

        paddr_t cur_paddr = 0;
        if (!jit_translate_ifetch(cur_pc, &cur_paddr) || !in_pmem(cur_paddr) ||
            cur_paddr != first_paddr + (paddr_t)source_len)
        {
            return false;
        }

        const uint32_t instr = vaddr_ifetch(cur_pc, 4);
        const uint32_t opcode = instr & 0x7fu;

        if (!jit_instr_can_chain_body(instr))
        {
            return false;
        }

        if (opcode == 0x63 && cur_pc + imm_b(instr) == pc)
        {
            return true;
        }

        cur_pc += 4;
        source_len += 4;
        count++;
    }

    return false;
}

/*
 * Compile a native block starting at `pc`.
 *
 * The block is limited by the caller's execution budget, the maximum native
 * block length, unsupported instructions, control flow, and physical source
 * contiguity. It returns the published cache entry on success, or NULL when the
 * interpreter should execute the current instruction.
 */
static rv32_jit_block_t *jit_compile_block(vaddr_t pc, uint32_t max_insns)
{
    JIT_STAT_INC(compile_requests);

    if (!jit_code_init() || max_insns == 0)
    {
        return NULL;
    }

    if (jit_code_used + 4096u > RV32_JIT_CODE_SIZE)
    {
        jit_arena_reset();
    }
    jit_code_used = jit_align_up(jit_code_used, RV32_JIT_CODE_ALIGN);

    paddr_t first_paddr = 0;

    if (!jit_translate_ifetch(pc, &first_paddr) || !in_pmem(first_paddr))
    {
        return NULL;
    }

    rv32_jit_writer_t w = {
        .start = jit_code + jit_code_used,
        .cur = jit_code + jit_code_used,
        .end = jit_code + RV32_JIT_CODE_SIZE,
    };
    rv32_jit_reg_cache_t regs;
    jit_reg_cache_init(&regs);

    if (!emit_prologue(&w))
    {
        return NULL;
    }

    const bool loop_count_needed =
        jit_block_has_chainable_backedge(pc, max_insns, first_paddr);
    const uint8_t *block_start_native = w.cur;
    vaddr_t cur_pc = pc;
    uint32_t count = 0;
    uint32_t source_len = 0;
    bool block_sets_pc = false;
    bool chain_safe = loop_count_needed;
    bool chained_loop = false;

    while (count < max_insns && count < RV32_JIT_BLOCK_MAX_INSNS)
    {
        /*
     * In paged mode a cache hit revalidates the first instruction mapping before
     * entering native code.  Keeping one native block within one virtual page
     * makes that one mapping check sufficient: the rest of the block cannot move
     * to a different physical page unless the first mapping also changes.  Bare
     * mode has no virtual remapping, so it can still use the physical-contiguity
     * check below to span normal PMEM bytes.
     */

        if (count != 0 && (cpu.csr.satp & RV32_JIT_SATP_MODE_MASK) != 0 &&
            ((cur_pc ^ pc) & ~(vaddr_t)PAGE_MASK) != 0)
        {
            break;
        }

        /*
     * Re-translate every guest instruction, even inside one block. This keeps
     * the block metadata honest across page boundaries and avoids assuming that
     * adjacent virtual PCs are adjacent physical bytes.
     */
        paddr_t cur_paddr = 0;

        if (!jit_translate_ifetch(cur_pc, &cur_paddr) || !in_pmem(cur_paddr))
        {
            break;
        }

        /*
     * Source invalidation records one physical byte range. Stop if virtual
     * aliases make the next guest instruction non-contiguous in PMEM.
     */

        if (cur_paddr != first_paddr + (paddr_t)source_len)
        {
            break;
        }

        const uint32_t instr = vaddr_ifetch(cur_pc, 4);
        uint8_t *instr_start = w.cur;
        /*
     * Native bytes and compile-time register-cache metadata describe the same
     * partial instruction, so both must roll back together if emission fails.
     */
        rv32_jit_reg_cache_t regs_start = regs;
        bool end_block = false;
        const uint32_t opcode = instr & 0x7fu;

        if (opcode == 0x63)
        {
            bool branch_chained = false;

            if (!emit_branch_instr(&w, &regs, instr, cur_pc, pc,
                                   block_start_native, loop_count_needed, chain_safe,
                                   &branch_chained, count + 1u))
            {
                w.cur = instr_start;
                jit_reg_cache_restore(&regs, &regs_start);
                break;
            }
            if (branch_chained)
            {
                chained_loop = true;
                end_block = true;
            }
        }
        else if (jit_instr_is_control_flow(instr))
        {
            if (!emit_control_flow_instr(&w, &regs, instr, cur_pc))
            {
                w.cur = instr_start;
                jit_reg_cache_restore(&regs, &regs_start);
                break;
            }
            block_sets_pc = true;
            end_block = true;
        }
        else if (!emit_alu_instr(&w, &regs, instr, cur_pc))
        {
            w.cur = instr_start;
            jit_reg_cache_restore(&regs, &regs_start);

            if (!emit_load_store_instr(&w, &regs, instr, cur_pc, count + 1u))
            {
                /*
         * Emitters may fail after writing a prefix of an x86 instruction. Roll
         * back to the last complete native instruction before falling back.
         */
                w.cur = instr_start;
                jit_reg_cache_restore(&regs, &regs_start);
                break;
            }
            chain_safe = false;
        }

        cur_pc += 4;
        source_len += 4;
        count++;

        if (end_block)
        {
            break;
        }
    }

    if (count == 0)
    {
        jit_mark_unsupported(pc, first_paddr, 4);
        return NULL;
    }

    if ((!block_sets_pc && !jit_reg_flush_all_dirty(&w, &regs)) ||
        (!block_sets_pc && !emit_set_pc_imm(&w, cur_pc)) ||
        !(chained_loop ? emit_epilogue_return_loop_count(&w, count)
                       : emit_epilogue_return_count(&w, count)))
    {
        return NULL;
    }

    __builtin___clear_cache((char *)w.start, (char *)w.cur);

    /*
   * Publish the block only after the instruction cache has been synchronised
   * and the old slot's source refs have been released. From this point onward,
   * writes to the recorded PMEM chunks must be able to find this block.
   */
    rv32_jit_block_t *block = jit_cache_slot(pc);
    jit_block_discard(block);
    jit_source_chunks_ref(first_paddr, source_len);
    JIT_STAT_INC(blocks_compiled);
    JIT_STAT_ADD(compiled_insns, count);
    *block = (rv32_jit_block_t){
        .valid = true,
        .pc = pc,
        .satp = cpu.csr.satp,
        .paddr_start = first_paddr,
        .source_len = source_len,
        .insn_count = count,
        .entry = (rv32_jit_entry_t)w.start,
    };

    jit_code_used = (size_t)(w.cur - jit_code);
    return block;
}

/*
 * Public hook: execute cached or newly compiled native blocks.
 *
 * `remaining` is the CPU loop's instruction budget and `device_budget` is the
 * maximum number of instructions before the next device update. The function
 * writes the actual completed count to `*executed` and returns true only when at
 * least one guest instruction ran in native code.
 */
bool isa_jit_exec(uint64_t remaining, uint32_t device_budget, uint32_t *executed)
{
    *executed = 0;

    if (remaining == 0 || device_budget == 0)
    {
        return false;
    }

    /*
   * cpu_exec() already asks isa_jit_available() before entering its hot loop.
   * Keep the repeated block-dispatch path cheap, but still handle direct calls
   * before initialisation.
   */

    if (jit_code == NULL && !isa_jit_available())
    {
        return false;
    }

    JIT_STAT_INC(exec_requests);

    uint32_t batch_budget = remaining > RV32_JIT_BATCH_MAX_INSNS
                                ? RV32_JIT_BATCH_MAX_INSNS
                                : (uint32_t)remaining;

    if (batch_budget > device_budget)
    {
        batch_budget = device_budget;
    }

    uint32_t total = 0;
    while (total < batch_budget)
    {
        /*
     * Each native block reports how many guest instructions it completed. The
     * dispatcher uses that count, rather than assuming a fixed block length, so
     * helper exits and control-flow terminators keep device timing bounded.
     */
        uint32_t remaining_budget = batch_budget - total;
        uint32_t block_budget = remaining_budget;

        if (block_budget > RV32_JIT_BLOCK_MAX_INSNS)
        {
            block_budget = RV32_JIT_BLOCK_MAX_INSNS;
        }

        rv32_jit_block_t *block = jit_cache_slot(cpu.pc);

        if (jit_block_matches(block, cpu.pc))
        {
            /*
       * A valid longer block is useful cache state. If the current batch budget
       * cannot run it, return to cpu_exec() rather than replacing it with a
       * shorter budget-limited variant that would hurt later hot executions.
       */

            if (block->entry != NULL && block->insn_count > block_budget)
            {
                break;
            }
            JIT_STAT_INC(cache_hits);
        }
        else
        {
            JIT_STAT_INC(cache_misses);
            block = jit_compile_block(cpu.pc, block_budget);
        }

        if (block == NULL || !block->valid || block->entry == NULL)
        {
            if (block != NULL && block->valid && block->entry == NULL)
            {
                JIT_STAT_INC(unsupported_hits);
            }
            break;
        }

        jit_entry_budget = remaining_budget;
        jit_loop_extra = 0;
        const uint32_t ran = block->entry();
        Assert(ran > 0 && ran <= remaining_budget,
               "jit: invalid executed count %u", ran);
        JIT_STAT_INC(blocks_executed);
        JIT_STAT_ADD(executed_insns, ran);
        total += ran;
    }

    *executed = total;
    return total > 0;
}

#if RV32_JIT_STATS
/* Compute a rounded fixed-point ratio with two decimal digits. */
static uint64_t jit_ratio_x100(uint64_t numerator, uint64_t denominator)
{
    if (denominator == 0)
    {
        return 0;
    }
    return (numerator * 100u + denominator / 2u) / denominator;
}

/* Compute a rounded fixed-point percentage with two decimal digits. */
static uint64_t jit_percent_x100(uint64_t numerator, uint64_t denominator)
{
    if (denominator == 0)
    {
        return 0;
    }
    return (numerator * 10000u + denominator / 2u) / denominator;
}
#endif

/* Public hook: print optional JIT statistics at the end of execution. */
void isa_jit_dump_stats(void)
{
    jit_init_runtime_options();

    if (jit_runtime_disabled())
    {
        Log("jit: disabled by NEMU_DISABLE_JIT=1");
        return;
    }

#if RV32_JIT_STATS
    if (!jit_stats_enabled || !RV32_JIT_ENABLED ||
        (jit_code == NULL && jit_stats.exec_requests == 0))
    {
        return;
    }

    const uint64_t cache_total = jit_stats.cache_hits + jit_stats.cache_misses;
    const uint64_t cache_hit_pct =
        jit_percent_x100(jit_stats.cache_hits, cache_total);
    const uint64_t avg_compile_len =
        jit_ratio_x100(jit_stats.compiled_insns, jit_stats.blocks_compiled);
    const uint64_t avg_exec_len =
        jit_ratio_x100(jit_stats.executed_insns, jit_stats.blocks_executed);
    const uint64_t load_direct_pct =
        jit_percent_x100(jit_stats.helper_load_direct, jit_stats.helper_loads);
    const uint64_t store_direct_pct =
        jit_percent_x100(jit_stats.helper_store_direct, jit_stats.helper_stores);

    Log("jit: exec requests = %" PRIu64
        ", cache hits = %" PRIu64
        ", misses = %" PRIu64
        ", hit rate = %" PRIu64 ".%02" PRIu64 "%%",
        jit_stats.exec_requests,
        jit_stats.cache_hits,
        jit_stats.cache_misses,
        cache_hit_pct / 100u,
        cache_hit_pct % 100u);
    Log("jit: compiled blocks = %" PRIu64
        ", unsupported blocks = %" PRIu64
        ", avg compiled length = %" PRIu64 ".%02" PRIu64 " insn",
        jit_stats.blocks_compiled,
        jit_stats.blocks_unsupported,
        avg_compile_len / 100u,
        avg_compile_len % 100u);
    Log("jit: executed blocks = %" PRIu64
        ", JIT instructions = %" PRIu64
        ", avg executed block = %" PRIu64 ".%02" PRIu64 " insn"
        ", unsupported hits = %" PRIu64,
        jit_stats.blocks_executed,
        jit_stats.executed_insns,
        avg_exec_len / 100u,
        avg_exec_len % 100u,
        jit_stats.unsupported_hits);
    Log("jit: helper loads = %" PRIu64
        " (%" PRIu64 ".%02" PRIu64 "%% direct PMEM), stores = %" PRIu64
        " (%" PRIu64 ".%02" PRIu64 "%% direct PMEM), complex ops = %" PRIu64,
        jit_stats.helper_loads,
        load_direct_pct / 100u,
        load_direct_pct % 100u,
        jit_stats.helper_stores,
        store_direct_pct / 100u,
        store_direct_pct % 100u,
        jit_stats.helper_complex_ops);
    Log("jit: invalidation requests = %" PRIu64
        ", page-filter skips = %" PRIu64
        ", invalidated blocks = %" PRIu64
        ", arena resets = %" PRIu64,
        jit_stats.invalidation_requests,
        jit_stats.invalidation_page_skips,
        jit_stats.invalidated_blocks,
        jit_stats.arena_resets);
#else
    if (jit_stats_enabled)
    {
        Log("jit: stats requested, but this binary was built without RV32_JIT_STATS=1");
    }
#endif
}
