#include <isa-jit.h>
#include <isa.h>
#include <memory/host.h>
#include <memory/paddr.h>
#include <memory/vaddr.h>
#include <utils.h>

#include <stddef.h>
#include <stdlib.h>

/*
 * Minimal RISC-V64 JIT bring-up.
 *
 * The native emitter compiles RV64 integer ALU, control-flow, and guarded memory
 * instructions into x86-64 code.  Bare PMEM loads/stores use direct guards;
 * Sv39 data memory uses helper calls that reuse the interpreter MMU. CSR/trap,
 * fence, and other sensitive instructions still fall back to the interpreter.
 */

#if defined(__x86_64__) && defined(CONFIG_RV64_JIT) && \
    defined(CONFIG_TARGET_NATIVE_ELF) && !defined(CONFIG_TRACE) && \
    !defined(CONFIG_DIFFTEST) && !defined(CONFIG_WATCHPOINT) && \
    !defined(CONFIG_MTRACE) && !defined(CONFIG_FTRACE)
#define RV64_JIT_ENABLED 1
#include <sys/mman.h>
#include <unistd.h>
#else
#define RV64_JIT_ENABLED 0
#endif

#ifdef CONFIG_RV64_JIT_STATS
#define RV64_JIT_STATS 1
#else
#define RV64_JIT_STATS 0
#endif

/* RISC-V base instructions are 4 bytes here; compressed `C` is not emitted yet. */
#define RV64_INSN_SIZE 4u
/* Seven low bits select the base RISC-V opcode. */
#define RV64_OPCODE_MASK 0x7fu
/* Branch targets must be 4-byte aligned while the JIT has no compressed path. */
#define RV64_BRANCH_ALIGN_MASK 0x3u

/* RISC-V opcodes used by this first native subset. */
#define RV64_OPCODE_LOAD 0x03u
#define RV64_OPCODE_OP_IMM 0x13u
#define RV64_OPCODE_AUIPC 0x17u
#define RV64_OPCODE_OP_IMM_32 0x1bu
#define RV64_OPCODE_STORE 0x23u
#define RV64_OPCODE_OP 0x33u
#define RV64_OPCODE_LUI 0x37u
#define RV64_OPCODE_OP_32 0x3bu
#define RV64_OPCODE_BRANCH 0x63u
#define RV64_OPCODE_JALR 0x67u
#define RV64_OPCODE_JAL 0x6fu

/* Match RV32's longer block budget now that RV64 has register caching. */
#define RV64_JIT_BLOCK_MAX_INSNS 64u
/* Match the CPU loop's device polling window; native code still returns bounded work. */
#define RV64_JIT_BATCH_MAX_INSNS 65536u
/* Power-of-two direct-mapped cache size, so `(size - 1)` is a valid index mask. */
#define RV64_JIT_CACHE_SIZE 262144u
/* Keep the RV64 arena in parity with RV32 so long-running workloads reset less. */
#define RV64_JIT_CODE_SIZE (256u * 1024u * 1024u)
/* 16-byte alignment is the normal x86-64 code-entry alignment. */
#define RV64_JIT_CODE_ALIGN 16u
/* Conservative per-block free-space check for the worst native byte expansion. */
/*
 * A 64-instruction block with register spills, guarded memory, helper calls and
 * side exits can be much larger than the early minimal emitter's blocks.
 */
#define RV64_JIT_BLOCK_CODE_HEADROOM (32u * 1024u)
/*
 * Source invalidation is tracked in 128-byte chunks, matching RV32.  This is
 * fine-grained enough that normal data stores near code avoid a full cache scan.
 */
#define RV64_JIT_SOURCE_CHUNK_SHIFT 7u
#define RV64_JIT_SOURCE_CHUNK_SIZE (1u << RV64_JIT_SOURCE_CHUNK_SHIFT)
#define RV64_JIT_SOURCE_CHUNK_MASK (RV64_JIT_SOURCE_CHUNK_SIZE - 1u)
#define RV64_JIT_PMEM_CHUNK_COUNT \
    (((size_t)CONFIG_MSIZE + (size_t)RV64_JIT_SOURCE_CHUNK_SIZE - 1u) / \
     (size_t)RV64_JIT_SOURCE_CHUNK_SIZE)
/*
 * Keep the helper data TLB intentionally small: 256 direct-mapped entries cover
 * common hot pages while keeping the inline index mask to one byte of entropy.
 */
#define RV64_JIT_DATA_TLB_SIZE 256u
/*
 * Page-table dependency refs are tracked per guest PMEM page.  A store to any
 * referenced page flushes the data TLB before a stale translation can be reused.
 */
#define RV64_JIT_PMEM_PAGE_COUNT \
    (((size_t)CONFIG_MSIZE + (size_t)PAGE_SIZE - 1u) / (size_t)PAGE_SIZE)

/* RV64/Sv39 constants repeated here so the JIT helper can reject unsafe cases. */
#define RV64_JIT_SATP_MODE_SHIFT 60u
#define RV64_JIT_SATP_MODE_SV39 8u
#define RV64_JIT_SATP_PPN_MASK (((word_t)1u << 44) - 1u)
#define RV64_JIT_PTE_V ((word_t)1u << 0)
#define RV64_JIT_PTE_R ((word_t)1u << 1)
#define RV64_JIT_PTE_W ((word_t)1u << 2)
#define RV64_JIT_PTE_X ((word_t)1u << 3)
#define RV64_JIT_PTE_U ((word_t)1u << 4)
#define RV64_JIT_PTE_A ((word_t)1u << 6)
#define RV64_JIT_PTE_D ((word_t)1u << 7)
#define RV64_JIT_PTE_RWX (RV64_JIT_PTE_R | RV64_JIT_PTE_W | RV64_JIT_PTE_X)
#define RV64_JIT_PTE_NON_LEAF_RESERVED \
    (RV64_JIT_PTE_U | RV64_JIT_PTE_A | RV64_JIT_PTE_D)
#define RV64_JIT_PTE_PPN_SHIFT 10u
#define RV64_JIT_PTE_PPN_MASK (((word_t)1u << 44) - 1u)
/*
 * The RV64 JIT has no Svnapot/Svpbmt support.  Sv39 PTE bits [63:54] therefore
 * remain reserved and must fault rather than produce a cached translation.
 */
#define RV64_JIT_PTE_RESERVED_63_54_MASK (((word_t)0x3ffu) << 54)
#define RV64_JIT_MSTATUS_MPRV ((word_t)1u << 17)
#define RV64_JIT_MSTATUS_SUM ((word_t)1u << 18)
#define RV64_JIT_MSTATUS_MXR ((word_t)1u << 19)
#define RV64_JIT_MSTATUS_MPP_SHIFT 11u
#define RV64_JIT_MSTATUS_MPP_MASK ((word_t)0x3u << RV64_JIT_MSTATUS_MPP_SHIFT)
#define RV64_JIT_DATA_TLB_READ 0x1u
#define RV64_JIT_DATA_TLB_WRITE 0x2u

typedef uint32_t (*rv64_jit_entry_t)(void);

typedef struct
{
    uint8_t *start;
    uint8_t *cur;
    uint8_t *end;
} rv64_jit_writer_t;

typedef enum
{
    RV64_JIT_HREG_RBX = 0,
    RV64_JIT_HREG_R12,
    RV64_JIT_HREG_R13,
    RV64_JIT_HREG_R14,
    RV64_JIT_HREG_R15,
    RV64_JIT_HREG_COUNT,
} rv64_jit_hreg_t;

typedef struct
{
    bool valid;
    bool loaded;
    bool dirty;
    uint32_t guest_reg;
    uint32_t age;
    rv64_jit_hreg_t hreg;
} rv64_jit_reg_slot_t;

typedef struct
{
    rv64_jit_reg_slot_t slots[RV64_JIT_HREG_COUNT];
    uint32_t next_age;
} rv64_jit_reg_cache_t;

typedef struct
{
    word_t satp;
    uint64_t vpn;
    uint32_t state;
    uint32_t access;
    uint64_t pg_paddr;
    uint64_t pt_pages[3];
    uint8_t pt_page_count;
    bool valid;
} rv64_jit_data_tlb_entry_t;

typedef char rv64_jit_data_tlb_entry_size_must_be_64[sizeof(rv64_jit_data_tlb_entry_t) == 64 ? 1 : -1];
typedef char rv64_jit_pmem_mapping_must_be_page_aligned[((CONFIG_MBASE | CONFIG_MSIZE) & PAGE_MASK) == 0 ? 1 : -1];

typedef struct
{
    bool valid;
    bool translated;
    vaddr_t pc;
    word_t satp;
    uint32_t data_state;
    paddr_t paddr_start;
    uint32_t source_len;
    uint32_t insn_count;
    rv64_jit_entry_t entry;
} rv64_jit_block_t;

typedef struct
{
    uint64_t exec_requests;
    uint64_t cache_hits;
    uint64_t cache_misses;
    uint64_t unsupported_hits;
    uint64_t blocks_compiled;
    uint64_t blocks_unsupported;
    uint64_t blocks_executed;
    uint64_t compiled_insns;
    uint64_t executed_insns;
    uint64_t native_loads;
    uint64_t native_stores;
    uint64_t native_jumps;
    uint64_t native_m_ops;
    uint64_t translated_blocks;
    uint64_t reg_cache_spills;
    uint64_t native_store_continuations;
    uint64_t native_paged_loads;
    uint64_t native_paged_stores;
    uint64_t zero_side_exits;
    uint64_t data_tlb_hits;
    uint64_t data_tlb_misses;
    uint64_t data_tlb_fills;
    uint64_t data_tlb_flushes;
    uint64_t data_tlb_page_table_flushes;
    uint64_t data_tlb_direct_loads;
    uint64_t data_tlb_direct_stores;
    uint64_t inline_paged_loads;
    uint64_t inline_paged_stores;
    uint64_t inline_paged_load_hits;
    uint64_t inline_paged_store_hits;
    uint64_t invalidation_requests;
    uint64_t invalidated_blocks;
    uint64_t arena_resets;
} rv64_jit_stats_t;

typedef struct
{
    uint8_t *slow_disps[10];
    uint32_t count;
} rv64_jit_tlb_guard_patch_t;

static rv64_jit_block_t jit_cache[RV64_JIT_CACHE_SIZE];
static rv64_jit_data_tlb_entry_t jit_data_tlb[RV64_JIT_DATA_TLB_SIZE];
static uint16_t jit_data_tlb_pt_page_refs[RV64_JIT_PMEM_PAGE_COUNT];
static uint16_t jit_source_chunk_refs[RV64_JIT_PMEM_CHUNK_COUNT];
static uint8_t *jit_code = NULL;
static size_t jit_code_used = 0;
static rv64_jit_stats_t jit_stats;
#if RV64_JIT_ENABLED
static bool jit_disabled = false;
#endif
static bool jit_env_disable = false;
static bool jit_stats_enabled = false;
static bool jit_runtime_options_ready = false;
/* Current native-entry instruction budget, used by in-block chained loops. */
static volatile uint32_t jit_entry_budget = 0;
/* Extra guest instructions completed by earlier chained loop laps. */
static volatile uint32_t jit_loop_extra = 0;

/*
 * Public write-side guard. It becomes true after the native arena exists, so
 * PMEM writers know when exact physical invalidation may be needed.
 */
bool isa_jit_invalidation_active = false;

#if RV64_JIT_STATS
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
        (void)(value); \
    } while (0)
#endif

/* Extract an inclusive bit range from a 32-bit RISC-V instruction. */
static uint32_t bits(uint32_t value, int hi, int lo)
{
    /*
     * All current callers pass 0 <= lo <= hi < 32. The `(1u << width) - 1`
     * mask is therefore well-defined and keeps only the requested field.
     */
    return (value >> lo) & ((1u << (hi - lo + 1)) - 1u);
}

/* Sign-extend an instruction field whose sign bit is at width - 1. */
static int64_t sext(uint32_t value, unsigned width)
{
    /*
     * RV64 immediates in this file are at most 32 bits before extension. Shift
     * left until the field sign reaches bit 31, then rely on signed arithmetic
     * right shift to fill the high bits.
     */
    const uint32_t shift = 32u - width;
    return (int64_t)((int32_t)(value << shift) >> shift);
}

/* Decode the I-format immediate as a signed XLEN value. */
static int64_t imm_i(uint32_t instr)
{
    return sext(bits(instr, 31, 20), 12);
}

/* Decode the S-format store immediate as a signed XLEN value. */
static int64_t imm_s(uint32_t instr)
{
    const uint32_t imm = bits(instr, 11, 7) | (bits(instr, 31, 25) << 5);
    return sext(imm, 12);
}

/* Decode the B-format immediate, including the implicit low zero bit. */
static int64_t imm_b(uint32_t instr)
{
    /*
     * RISC-V scatters branch offsets as imm[12|10:5|4:1|11]. The low bit is
     * always zero because base ISA branches target halfword/word boundaries.
     */
    uint32_t imm = (bits(instr, 11, 8) << 1) |
                   (bits(instr, 30, 25) << 5) |
                   (bits(instr, 7, 7) << 11) |
                   (bits(instr, 31, 31) << 12);
    return sext(imm, 13);
}

/* Decode and sign-extend a U-format immediate for RV64 LUI/AUIPC. */
static int64_t imm_u_sext(uint32_t instr)
{
    /* `0xfffff000` keeps imm[31:12], the upper 20-bit U-type payload. */
    return (int64_t)(int32_t)(instr & 0xfffff000u);
}

/* Decode the J-format immediate, including the implicit low zero bit. */
static int64_t imm_j(uint32_t instr)
{
    /*
     * JAL scatters offsets as imm[20|10:1|11|19:12]. The low bit is implicit
     * zero, then the 21-bit value is sign-extended to XLEN.
     */
    uint32_t imm = (bits(instr, 30, 21) << 1) |
                   (bits(instr, 20, 20) << 11) |
                   (bits(instr, 19, 12) << 12) |
                   (bits(instr, 31, 31) << 20);
    return sext(imm, 21);
}

/* Return the byte offset of a guest GPR inside CPU_state. */
static uint32_t jit_gpr_offset(uint32_t reg)
{
    return (uint32_t)(offsetof(CPU_state, gpr) + reg * sizeof(cpu.gpr[0]));
}

/* Return the byte offset of the guest PC inside CPU_state. */
static uint32_t jit_pc_offset(void)
{
    return (uint32_t)offsetof(CPU_state, pc);
}

/* Read simple environment flags: unset, empty, and exactly "0" mean false. */
static bool jit_env_flag_enabled(const char *name)
{
    const char *value = getenv(name);
    return value != NULL && value[0] != '\0' &&
           !(value[0] == '0' && value[1] == '\0');
}

/* Cache runtime switches once so dispatch does not call getenv() repeatedly. */
static void jit_init_runtime_options(void)
{
    if (!jit_runtime_options_ready)
    {
        jit_env_disable = jit_env_flag_enabled("NEMU_DISABLE_JIT");
        jit_stats_enabled = jit_env_flag_enabled("NEMU_JIT_STATS");
        jit_runtime_options_ready = true;
    }
}

/* Return whether runtime configuration has disabled this binary's RV64 JIT. */
static bool jit_runtime_disabled(void)
{
    jit_init_runtime_options();
    return jit_env_disable;
}

/* Clear the RV64 JIT data TLB and its page-table dependency refcounts. */
static void jit_data_tlb_flush(void)
{
    /*
     * SFENCE.VMA and page-table writes do not need selective invalidation for
     * this first stage.  The table is small, and a full clear avoids mistakes
     * around ASID, virtual-address operands, and superpage dependency ranges.
     */
    memset(jit_data_tlb, 0, sizeof(jit_data_tlb));
    memset(jit_data_tlb_pt_page_refs, 0, sizeof(jit_data_tlb_pt_page_refs));
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
        jit_data_tlb_pt_page_refs[idx] != UINT16_MAX)
    {
        jit_data_tlb_pt_page_refs[idx]++;
    }
}

/* Drop one dependency ref for an overwritten data-TLB entry. */
static void jit_data_tlb_unref_page(paddr_t page)
{
    size_t idx = 0;

    if (jit_data_pmem_page_index(page, &idx) &&
        jit_data_tlb_pt_page_refs[idx] > 0)
    {
        jit_data_tlb_pt_page_refs[idx]--;
    }
}

/* Return whether any live data-TLB entry depends on this page-table page. */
static bool jit_data_tlb_refs_page(paddr_t page)
{
    size_t idx = 0;
    return jit_data_pmem_page_index(page, &idx) &&
           jit_data_tlb_pt_page_refs[idx] != 0;
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
static bool jit_write_may_touch_data_tlb_page_table(paddr_t addr, int len)
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
static uint32_t jit_data_tlb_state(int type)
{
    /*
     * MPRV is folded into the effective privilege.  SUM and MXR stay explicit
     * because they change whether S-mode may access U pages and whether reads
     * may use execute-only PTEs.
     */
    uint32_t state = (uint32_t)jit_data_effective_priv(type);

    if ((cpu.csr.mstatus & RV64_JIT_MSTATUS_SUM) != 0)
    {
        state |= 1u << 2;
    }

    if ((cpu.csr.mstatus & RV64_JIT_MSTATUS_MXR) != 0)
    {
        state |= 1u << 3;
    }

    return state;
}

/* Return the satp mode field used by RV64 address translation. */
static word_t jit_data_satp_mode(word_t satp)
{
    return satp >> RV64_JIT_SATP_MODE_SHIFT;
}

/* Return whether an Sv39 virtual address is canonical. */
static bool jit_data_sv39_canonical(vaddr_t vaddr)
{
    const uint64_t sign = ((uint64_t)vaddr >> 38) & 1u;
    const uint64_t high = (uint64_t)vaddr >> 39;

    return sign ? high == ((1ull << 25) - 1ull) : high == 0;
}

/* Return whether a data access stays within one 4 KiB translated page. */
static bool jit_data_cross_page(vaddr_t addr, uint32_t len)
{
    const word_t off = (word_t)(addr & PAGE_MASK);
    return len == 0 || off + (word_t)len > PAGE_SIZE;
}

/* Validate the Sv39 PTE bits that are illegal before leaf/non-leaf selection. */
static bool jit_data_pte_valid(word_t pte)
{
    return (pte & RV64_JIT_PTE_V) != 0 &&
           (pte & (RV64_JIT_PTE_R | RV64_JIT_PTE_W)) != RV64_JIT_PTE_W &&
           (pte & RV64_JIT_PTE_RESERVED_63_54_MASK) == 0;
}

/* Return whether an Sv39 PTE is a leaf rather than the next-level pointer. */
static bool jit_data_pte_leaf(word_t pte)
{
    return (pte & RV64_JIT_PTE_RWX) != 0;
}

/* Extract the physical page number encoded in an Sv39 PTE. */
static word_t jit_data_pte_ppn(word_t pte)
{
    return (pte >> RV64_JIT_PTE_PPN_SHIFT) & RV64_JIT_PTE_PPN_MASK;
}

/* Check the low PPN fields that must be zero for legal Sv39 superpages. */
static bool jit_data_superpage_aligned(word_t ppn, int level)
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

/* Compute which data access kinds are legal for this leaf and CPU state. */
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
static paddr_t jit_data_leaf_page_base(word_t ppn, const word_t vpn[3], int level)
{
    word_t pa_ppn = ppn;

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
    return (uint32_t)((vpn ^ (vpn >> 9) ^ satp ^ (satp >> 12) ^ state) &
                      (RV64_JIT_DATA_TLB_SIZE - 1u));
}

/* Fill or hit the RV64/Sv39 data TLB for ordinary translated PMEM accesses. */
static bool jit_translate_pmem(vaddr_t addr, uint32_t len, int type, paddr_t *paddr)
{
    const word_t satp = cpu.csr.satp;
    const word_t mode = jit_data_satp_mode(satp);
    const word_t priv = jit_data_effective_priv(type);

    if (mode == 0)
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

    if (!jit_data_sv39_canonical(addr) || jit_data_cross_page(addr, len))
    {
        return false;
    }

    const uint32_t need = jit_data_tlb_need(type);

    if (need == 0)
    {
        return false;
    }

    const uint64_t vpn_tag = (uint64_t)addr >> PAGE_SHIFT;
    const uint32_t state = jit_data_tlb_state(type);
    const uint32_t idx = jit_data_tlb_index(vpn_tag, satp, state);
    rv64_jit_data_tlb_entry_t *entry = &jit_data_tlb[idx];

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

    const word_t vpn[3] = {
        ((word_t)addr >> 12) & 0x1ffu,
        ((word_t)addr >> 21) & 0x1ffu,
        ((word_t)addr >> 30) & 0x1ffu,
    };
    paddr_t pt_base = (paddr_t)((satp & RV64_JIT_SATP_PPN_MASK) << PAGE_SHIFT);
    paddr_t pt_pages[3] = {0};
    uint8_t pt_page_count = 0;

    for (int level = 2; level >= 0; --level)
    {
        const paddr_t pte_addr = pt_base + (paddr_t)(vpn[level] * sizeof(uint64_t));

        if (!jit_data_pmem_range(pte_addr, sizeof(uint64_t)))
        {
            return false;
        }

        pt_pages[pt_page_count++] = pt_base;
        const word_t pte = (word_t)paddr_read(pte_addr, 8);

        if (!jit_data_pte_valid(pte))
        {
            return false;
        }

        const word_t ppn = jit_data_pte_ppn(pte);

        if (jit_data_pte_leaf(pte))
        {
            if (!jit_data_superpage_aligned(ppn, level))
            {
                return false;
            }

            const uint32_t access = jit_data_leaf_access(pte, priv);

            if ((access & need) == 0)
            {
                return false;
            }

            const paddr_t pg_paddr = jit_data_leaf_page_base(ppn, vpn, level);
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

/* Forward declaration: store helpers need source-chunk state defined below. */
static bool jit_write_may_touch_source_chunk(paddr_t addr, int len);

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

    if (jit_translate_pmem(addr, len, MEM_TYPE_READ, &paddr))
    {
        JIT_STAT_INC(data_tlb_direct_loads);
        return (uint64_t)host_read(guest_to_host(paddr), (int)len);
    }

    return (uint64_t)vaddr_read(addr, (int)len);
}

/* Load one signed byte and sign-extend it to RV64 XLEN. */
static uint64_t jit_load_i8(vaddr_t addr)
{
    return (uint64_t)(int64_t)(int8_t)jit_load_vaddr_raw(addr, 1);
}

/* Load one signed halfword and sign-extend it to RV64 XLEN. */
static uint64_t jit_load_i16(vaddr_t addr)
{
    return (uint64_t)(int64_t)(int16_t)jit_load_vaddr_raw(addr, 2);
}

/* Load one signed word and sign-extend it to RV64 XLEN. */
static uint64_t jit_load_i32(vaddr_t addr)
{
    return (uint64_t)(int64_t)(int32_t)jit_load_vaddr_raw(addr, 4);
}

/* Load one doubleword; RV64 LD already produces a full-width value. */
static uint64_t jit_load_u64(vaddr_t addr)
{
    return jit_load_vaddr_raw(addr, 8);
}

/* Load one unsigned byte and zero-extend it to RV64 XLEN. */
static uint64_t jit_load_u8(vaddr_t addr)
{
    return jit_load_vaddr_raw(addr, 1) & 0xffu;
}

/* Load one unsigned halfword and zero-extend it to RV64 XLEN. */
static uint64_t jit_load_u16(vaddr_t addr)
{
    return jit_load_vaddr_raw(addr, 2) & 0xffffu;
}

/* Load one unsigned word and zero-extend it to RV64 XLEN. */
static uint64_t jit_load_u32(vaddr_t addr)
{
    return jit_load_vaddr_raw(addr, 4) & 0xffffffffu;
}

/* Shared RV64 store helper that preserves MMIO, tracing, and invalidation. */
static void jit_store_vaddr(vaddr_t addr, uint32_t len, uint64_t data)
{
    /*
     * A data-TLB hit skips the repeated page walk but still commits through
     * paddr_write().  That keeps device boundaries, source invalidation, and
     * page-table dependency flushing under the same write-side hook used by the
     * interpreter.  Anything not proven ordinary PMEM uses vaddr_write().
     */
    paddr_t paddr = 0;

    if (jit_translate_pmem(addr, len, MEM_TYPE_WRITE, &paddr))
    {
        JIT_STAT_INC(data_tlb_direct_stores);
        paddr_write(paddr, (int)len, (word_t)data);
        return;
    }

    vaddr_write(addr, (int)len, (word_t)data);
}

/* Commit a guarded PMEM store and report whether native code may continue. */
static uint32_t jit_store_pmem_continue(paddr_t addr, uint32_t len, uint64_t data)
{
    /*
     * Source writes must leave the native block after paddr_write() because the
     * write can invalidate the block currently running.  Ordinary data writes
     * can continue: paddr_write() still owns tracing/MMIO boundaries and exact
     * invalidation, while the source-chunk pre-check decides whether continuing
     * would risk executing stale native bytes.
     */
    const bool may_touch_source = jit_write_may_touch_source_chunk(addr, (int)len);
    paddr_write(addr, (int)len, (word_t)data);
    return may_touch_source ? 0u : 1u;
}

/* Sign-extend one 32-bit W-form result to the RV64 register width. */
static uint64_t jit_sext32(uint32_t value)
{
    return (uint64_t)(int64_t)(int32_t)value;
}

/* Compute RV64M operations that are uncommon or awkward to emit inline. */
static uint64_t jit_m_result(uint64_t lhs, uint64_t rhs, uint32_t instr)
{
    const uint32_t opcode = instr & RV64_OPCODE_MASK;
    const uint32_t funct3 = bits(instr, 14, 12);
    const uint32_t key = (bits(instr, 31, 25) << 3) | funct3;

    if (opcode == RV64_OPCODE_OP)
    {
        switch (key)
        {
        case 0x009: /* MULH */
            return (uint64_t)(((__int128)(int64_t)lhs * (__int128)(int64_t)rhs) >> 64);
        case 0x00a: /* MULHSU */
            return (uint64_t)(((__int128)(int64_t)lhs * (__int128)(uint64_t)rhs) >> 64);
        case 0x00b: /* MULHU */
            return (uint64_t)(((__uint128_t)lhs * (__uint128_t)rhs) >> 64);
        case 0x00c: /* DIV */
            if (rhs == 0)
            {
                return UINT64_MAX;
            }
            if (lhs == (uint64_t)INT64_MIN && rhs == UINT64_MAX)
            {
                return lhs;
            }
            return (uint64_t)((int64_t)lhs / (int64_t)rhs);
        case 0x00d: /* DIVU */
            return rhs == 0 ? UINT64_MAX : lhs / rhs;
        case 0x00e: /* REM */
            if (rhs == 0)
            {
                return lhs;
            }
            if (lhs == (uint64_t)INT64_MIN && rhs == UINT64_MAX)
            {
                return 0;
            }
            return (uint64_t)((int64_t)lhs % (int64_t)rhs);
        case 0x00f: /* REMU */
            return rhs == 0 ? lhs : lhs % rhs;
        default:
            return 0;
        }
    }

    if (opcode == RV64_OPCODE_OP_32)
    {
        const int32_t lhs_s = (int32_t)lhs;
        const int32_t rhs_s = (int32_t)rhs;
        const uint32_t lhs_u = (uint32_t)lhs;
        const uint32_t rhs_u = (uint32_t)rhs;

        switch (key)
        {
        case 0x00c: /* DIVW */
            if (rhs_s == 0)
            {
                return UINT64_MAX;
            }
            if (lhs_s == INT32_MIN && rhs_s == -1)
            {
                return jit_sext32((uint32_t)lhs_s);
            }
            return jit_sext32((uint32_t)(lhs_s / rhs_s));
        case 0x00d: /* DIVUW */
            return rhs_u == 0 ? UINT64_MAX : jit_sext32(lhs_u / rhs_u);
        case 0x00e: /* REMW */
            if (rhs_s == 0)
            {
                return jit_sext32((uint32_t)lhs_s);
            }
            if (lhs_s == INT32_MIN && rhs_s == -1)
            {
                return 0;
            }
            return jit_sext32((uint32_t)(lhs_s % rhs_s));
        case 0x00f: /* REMUW */
            return rhs_u == 0 ? jit_sext32(lhs_u) : jit_sext32(lhs_u % rhs_u);
        default:
            return 0;
        }
    }

    return 0;
}

/* Round a code offset up to the next power-of-two alignment boundary. */
static size_t jit_align_up(size_t value, size_t align)
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

/* Add source-ref counts for the physical bytes backing one native block. */
static void jit_source_chunks_ref(paddr_t addr, uint32_t len)
{
    if (len == 0)
    {
        return;
    }

    size_t first = 0;
    size_t last = 0;

    if (!jit_paddr_to_source_chunk(addr, &first) ||
        !jit_paddr_to_source_chunk(addr + (paddr_t)len - 1u, &last))
    {
        return;
    }

    for (size_t i = first; i <= last; i++)
    {
        Assert(jit_source_chunk_refs[i] != UINT16_MAX,
               "jit: RV64 source chunk refcount overflow at %zu", i);
        jit_source_chunk_refs[i]++;
    }
}

/* Remove source-ref counts when a native block is discarded. */
static void jit_source_chunks_unref(paddr_t addr, uint32_t len)
{
    if (len == 0)
    {
        return;
    }

    size_t first = 0;
    size_t last = 0;

    if (!jit_paddr_to_source_chunk(addr, &first) ||
        !jit_paddr_to_source_chunk(addr + (paddr_t)len - 1u, &last))
    {
        return;
    }

    for (size_t i = first; i <= last; i++)
    {
        Assert(jit_source_chunk_refs[i] > 0,
               "jit: RV64 source chunk refcount underflow at %zu", i);
        jit_source_chunk_refs[i]--;
    }
}

/* Return whether a PMEM write can overlap any compiled source chunk. */
static bool jit_write_may_touch_source_chunk(paddr_t addr, int len)
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
        if (jit_source_chunk_refs[i] != 0)
        {
            return true;
        }
    }

    return false;
}

/* Release one cache slot and its source refs, if it owns source bytes. */
static void jit_block_discard(rv64_jit_block_t *block)
{
    if (block->valid && block->source_len != 0)
    {
        jit_source_chunks_unref(block->paddr_start, block->source_len);
    }

    *block = (rv64_jit_block_t){0};
}

/* Hash the current address-space tag and guest PC into the direct-mapped cache. */
static uint32_t jit_hash(vaddr_t pc, word_t satp)
{
    /*
     * `pc >> 2` drops fixed 4-byte instruction-alignment zeros. `satp >> 12`
     * mixes the PPN/ASID-like high bits with the raw CSR value. The cache size
     * is a power of two, so `size - 1` is the direct-mapped slot mask.
     */
    return (uint32_t)(((pc >> 2) ^ satp ^ (satp >> 12)) & (RV64_JIT_CACHE_SIZE - 1u));
}

/* Return the direct-mapped block-cache slot for the current PC. */
static rv64_jit_block_t *jit_cache_slot(vaddr_t pc)
{
    return &jit_cache[jit_hash(pc, cpu.csr.satp)];
}

/* Clear every published block when arena or broad machine state changes. */
static void jit_cache_clear(void)
{
    memset(jit_cache, 0, sizeof(jit_cache));
    memset(jit_source_chunk_refs, 0, sizeof(jit_source_chunk_refs));
}

/* Allocate executable memory for generated x86-64 blocks. */
static bool jit_code_init(void)
{
    if (jit_code != NULL)
    {
        return true;
    }

#if RV64_JIT_ENABLED
    if (jit_disabled)
    {
        return false;
    }

    void *mem = mmap(NULL, RV64_JIT_CODE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (mem == MAP_FAILED)
    {
        jit_disabled = true;
        Log("jit: mmap failed, disable RISC-V64 JIT");
        return false;
    }

    jit_code = (uint8_t *)mem;
    jit_code_used = 0;
    isa_jit_invalidation_active = true;
    Log("jit: RISC-V64 native code arena = %zu bytes", (size_t)RV64_JIT_CODE_SIZE);
    return true;
#else
    return false;
#endif
}

/* Reuse the executable arena after discarding every old code pointer. */
static void jit_arena_reset(void)
{
    jit_cache_clear();
    jit_code_used = 0;
    JIT_STAT_INC(arena_resets);
}

/* Emit one byte into the current native block. */
static bool emit_u8(rv64_jit_writer_t *w, uint8_t value)
{
    if (w->cur >= w->end)
    {
        return false;
    }

    *w->cur++ = value;
    return true;
}

/* Emit one little-endian 32-bit value into the current native block. */
static bool emit_u32(rv64_jit_writer_t *w, uint32_t value)
{
    for (size_t i = 0; i < sizeof(value); i++)
    {
        if (!emit_u8(w, (uint8_t)(value >> (i * 8))))
        {
            return false;
        }
    }

    return true;
}

/* Emit one little-endian 64-bit value into the current native block. */
static bool emit_u64(rv64_jit_writer_t *w, uint64_t value)
{
    for (size_t i = 0; i < sizeof(value); i++)
    {
        if (!emit_u8(w, (uint8_t)(value >> (i * 8))))
        {
            return false;
        }
    }

    return true;
}

/* Return the x86 register number backing one callee-saved cache slot. */
static uint8_t jit_hreg_x86_reg(rv64_jit_hreg_t hreg)
{
    switch (hreg)
    {
    case RV64_JIT_HREG_RBX:
        return 3;
    case RV64_JIT_HREG_R12:
        return 12;
    case RV64_JIT_HREG_R13:
        return 13;
    case RV64_JIT_HREG_R14:
        return 14;
    case RV64_JIT_HREG_R15:
        return 15;
    default:
        Assert(0, "jit: invalid RV64 host register slot %d", hreg);
    }

    return 3;
}

/* Build an x86 ModRM byte from its mode, register, and r/m fields. */
static uint8_t jit_modrm(uint8_t mod, uint8_t reg, uint8_t rm)
{
    return (uint8_t)((mod << 6) | ((reg & 7u) << 3) | (rm & 7u));
}

/* Emit a 64-bit REX.W prefix, including high-register extension bits. */
static bool emit_rex64(rv64_jit_writer_t *w, uint8_t reg, uint8_t rm)
{
    uint8_t rex = 0x48;

    if ((reg & 8u) != 0)
    {
        rex |= 0x04;
    }

    if ((rm & 8u) != 0)
    {
        rex |= 0x01;
    }

    return emit_u8(w, rex);
}

/* Save all callee-saved host registers used as guest-register cache slots. */
static bool emit_push_saved_hregs(rv64_jit_writer_t *w)
{
    /*
     * Push order is RBX, R12, R13, R14, R15.  Five 8-byte pushes also change
     * the System V entry stack from rsp%16==8 to rsp%16==0, so helper calls are
     * naturally aligned without a separate stack adjustment.
     */
    return emit_u8(w, 0x53) &&
           emit_u8(w, 0x41) && emit_u8(w, 0x54) &&
           emit_u8(w, 0x41) && emit_u8(w, 0x55) &&
           emit_u8(w, 0x41) && emit_u8(w, 0x56) &&
           emit_u8(w, 0x41) && emit_u8(w, 0x57);
}

/* Restore callee-saved cache registers in the reverse of the push order. */
static bool emit_pop_saved_hregs(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x41) && emit_u8(w, 0x5f) &&
           emit_u8(w, 0x41) && emit_u8(w, 0x5e) &&
           emit_u8(w, 0x41) && emit_u8(w, 0x5d) &&
           emit_u8(w, 0x41) && emit_u8(w, 0x5c) &&
           emit_u8(w, 0x5b);
}

/* Forward declaration: the prologue needs R10 before the grouped move helpers. */
static bool emit_movabs_r10(rv64_jit_writer_t *w, uint64_t value);

/* Emit `movabs r11, &cpu`, restoring the fixed CPU-state base register. */
static bool emit_load_cpu_base(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x49) && emit_u8(w, 0xbb) &&
           emit_u64(w, (uint64_t)(uintptr_t)&cpu);
}

/* Emit the common native-block prologue and load long-lived base registers. */
static bool emit_prologue(rv64_jit_writer_t *w)
{
    /*
     * R11 is caller-saved, so generated code can dedicate it to `CPU_state *`
     * without saving it. R10 holds the host pointer for guest physical
     * CONFIG_MBASE, letting direct PMEM loads use `[r10 + offset]` after a
     * strict in-range guard. The saved host registers are the per-block guest
     * register cache and also provide 16-byte stack alignment for helper calls.
     */
    return emit_push_saved_hregs(w) &&
           emit_load_cpu_base(w) &&
           emit_movabs_r10(w, (uint64_t)(uintptr_t)guest_to_host(CONFIG_MBASE));
}

/* Restore saved host registers and return to the C dispatcher. */
static bool emit_epilogue(rv64_jit_writer_t *w)
{
    return emit_pop_saved_hregs(w) && emit_u8(w, 0xc3);
}

/* Emit a native return with a fixed completed guest-instruction count. */
static bool emit_return_count(rv64_jit_writer_t *w, uint32_t count)
{
    return emit_u8(w, 0xb8) && emit_u32(w, count) && emit_epilogue(w);
}

/* Emit a native return when EAX already holds the completed instruction count. */
static bool emit_return_eax(rv64_jit_writer_t *w)
{
    return emit_epilogue(w);
}

/* Emit `movabs rax, imm64`, used for full-width constants and helper targets. */
static bool emit_movabs_rax(rv64_jit_writer_t *w, uint64_t value)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0xb8) && emit_u64(w, value);
}

/* Emit `movabs rdx, imm64`, used for addresses of JIT loop counters. */
static bool emit_movabs_rdx(rv64_jit_writer_t *w, uint64_t value)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0xba) && emit_u64(w, value);
}

/* Emit `movabs rcx, imm64`, used for full-width PMEM range guards. */
static bool emit_movabs_rcx(rv64_jit_writer_t *w, uint64_t value)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0xb9) && emit_u64(w, value);
}

/* Emit `movabs r10, imm64`, the fixed host PMEM base for direct loads. */
static bool emit_movabs_r10(rv64_jit_writer_t *w, uint64_t value)
{
    return emit_u8(w, 0x49) && emit_u8(w, 0xba) && emit_u64(w, value);
}

/* Emit `mov eax, [rdx]`, loading one 32-bit JIT loop counter. */
static bool emit_mov_eax_m32_rdx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x8b) && emit_u8(w, 0x02);
}

/* Emit `mov [rdx], eax`, storing one 32-bit JIT loop counter. */
static bool emit_mov_m32_rdx_eax(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x89) && emit_u8(w, 0x02);
}

/* Emit `mov ecx, eax`, copying the loop count for the budget look-ahead. */
static bool emit_mov_ecx_eax(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x89) && emit_u8(w, 0xc1);
}

/* Emit `mov eax, ecx`, restoring a saved dynamic return count. */
static bool emit_mov_eax_ecx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x89) && emit_u8(w, 0xc8);
}

/* Emit `test eax, eax`, commonly used after boolean helper returns. */
static bool emit_test_eax_eax(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x85) && emit_u8(w, 0xc0);
}

/* Emit `mov rcx, rax`, preserving a dynamic JALR target across link writes. */
static bool emit_mov_rcx_rax(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x89) && emit_u8(w, 0xc1);
}

/* Emit `mov rax, rcx`, restoring a dynamic JALR target. */
static bool emit_mov_rax_rcx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x89) && emit_u8(w, 0xc8);
}

/* Emit `mov rdx, rax`, copying a guest address for PMEM range checks. */
static bool emit_mov_rdx_rax(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x89) && emit_u8(w, 0xc2);
}

/* Emit `mov rdx, rcx`, preserving a store value as a helper argument. */
static bool emit_mov_rdx_rcx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x89) && emit_u8(w, 0xca);
}

/* Emit `mov r8, rdx`, copying a VPN or PMEM offset into an index register. */
static bool emit_mov_r8_rdx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x49) && emit_u8(w, 0x89) && emit_u8(w, 0xd0);
}

/* Emit `mov r8d, edx`, used before indexing small refcount tables. */
static bool emit_mov_r8d_edx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x41) && emit_u8(w, 0x89) && emit_u8(w, 0xd0);
}

/* Emit `mov rdi, rdx`, preparing the first helper argument from a PMEM offset. */
static bool emit_mov_rdi_rdx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x89) && emit_u8(w, 0xd7);
}

/* Emit `mov rdi, rax`, preparing the first helper argument from a guest value. */
static bool emit_mov_rdi_rax(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x89) && emit_u8(w, 0xc7);
}

/* Emit `mov rsi, rdx`, preparing the second helper argument from a guest value. */
static bool emit_mov_rsi_rdx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x89) && emit_u8(w, 0xd6);
}

/* Emit `add eax, imm32`, used for completed-loop instruction accounting. */
static bool emit_add_eax_imm32(rv64_jit_writer_t *w, uint32_t imm)
{
    return emit_u8(w, 0x05) && emit_u32(w, imm);
}

/* Emit `add ecx, imm32`, used to test whether one more loop lap fits. */
static bool emit_add_ecx_imm32(rv64_jit_writer_t *w, uint32_t imm)
{
    return emit_u8(w, 0x81) && emit_u8(w, 0xc1) && emit_u32(w, imm);
}

/* Emit `sub rdx, rcx`, converting guest address to a PMEM byte offset. */
static bool emit_sub_rdx_rcx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x29) && emit_u8(w, 0xca);
}

/* Emit `sub rdx, rax`, converting translated paddr to a PMEM byte offset. */
static bool emit_sub_rdx_rax(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x29) && emit_u8(w, 0xc2);
}

/* Emit `add rdi, rcx`, converting a PMEM offset back to a physical address. */
static bool emit_add_rdi_rcx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x01) && emit_u8(w, 0xcf);
}

/* Emit `cmp rdx, rcx`, used by unsigned PMEM range guards. */
static bool emit_cmp_rdx_rcx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x39) && emit_u8(w, 0xca);
}

/* Emit `or rdx, rax`, combining a page base with a low page offset. */
static bool emit_or_rdx_rax(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x09) && emit_u8(w, 0xc2);
}

/* Shift RDX right by an immediate count. */
static bool emit_shr_rdx_imm(rv64_jit_writer_t *w, uint8_t value)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0xc1) && emit_u8(w, 0xea) && emit_u8(w, value);
}

/* Shift R8 left by an immediate count; DTLB entries are power-of-two sized. */
static bool emit_shl_r8_imm(rv64_jit_writer_t *w, uint8_t value)
{
    return emit_u8(w, 0x49) && emit_u8(w, 0xc1) && emit_u8(w, 0xe0) && emit_u8(w, value);
}

/* Shift R8 right by an immediate count while preserving high VPN tag bits. */
static bool emit_shr_r8_imm(rv64_jit_writer_t *w, uint8_t value)
{
    return emit_u8(w, 0x49) && emit_u8(w, 0xc1) && emit_u8(w, 0xe8) && emit_u8(w, value);
}

/* Shift R8D right by an immediate count for PMEM refcount table indexes. */
static bool emit_shr_r8d_imm(rv64_jit_writer_t *w, uint8_t value)
{
    return emit_u8(w, 0x41) && emit_u8(w, 0xc1) && emit_u8(w, 0xe8) && emit_u8(w, value);
}

/* Mask R8D with an immediate, usually to keep a direct-mapped table index. */
static bool emit_and_r8d_imm(rv64_jit_writer_t *w, uint32_t value)
{
    return emit_u8(w, 0x41) && emit_u8(w, 0x81) && emit_u8(w, 0xe0) && emit_u32(w, value);
}

/* Compare R8D with an immediate, used by store source-chunk guards. */
static bool emit_cmp_r8d_imm(rv64_jit_writer_t *w, uint32_t value)
{
    return emit_u8(w, 0x41) && emit_u8(w, 0x81) && emit_u8(w, 0xf8) && emit_u32(w, value);
}

/* Add RDX to R8, producing a pointer into the direct-mapped DTLB. */
static bool emit_add_r8_rdx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x49) && emit_u8(w, 0x01) && emit_u8(w, 0xd0);
}

/* XOR RDX into R8, matching the helper TLB hash mix. */
static bool emit_xor_r8_rdx(rv64_jit_writer_t *w)
{
    /* `49 31 d0` is `xor r8, rdx`; R8 holds the evolving TLB index. */
    return emit_u8(w, 0x49) && emit_u8(w, 0x31) && emit_u8(w, 0xd0);
}

/* Compare a byte field in the R8-pointed DTLB entry with an immediate. */
static bool emit_cmp_r8b_field_imm8(rv64_jit_writer_t *w, uint32_t offset,
                                    uint8_t value)
{
    Assert(offset <= INT8_MAX, "jit: RV64 DTLB byte field offset is too large");
    return emit_u8(w, 0x41) && emit_u8(w, 0x80) &&
           emit_u8(w, 0x78) && emit_u8(w, (uint8_t)offset) && emit_u8(w, value);
}

/* Compare a qword field in the R8-pointed DTLB entry with RDX. */
static bool emit_cmp_r8q_field_rdx(rv64_jit_writer_t *w, uint32_t offset)
{
    Assert(offset <= INT8_MAX, "jit: RV64 DTLB qword field offset is too large");
    return emit_u8(w, 0x49) && emit_u8(w, 0x39) &&
           emit_u8(w, 0x50) && emit_u8(w, (uint8_t)offset);
}

/* Compare a dword field in the R8-pointed DTLB entry with an immediate. */
static bool emit_cmp_r8d_field_imm32(rv64_jit_writer_t *w, uint32_t offset,
                                     uint32_t value)
{
    Assert(offset <= INT8_MAX, "jit: RV64 DTLB dword field offset is too large");
    return emit_u8(w, 0x41) && emit_u8(w, 0x81) &&
           emit_u8(w, 0x78) && emit_u8(w, (uint8_t)offset) && emit_u32(w, value);
}

/* Test permission bits in a dword field in the R8-pointed DTLB entry. */
static bool emit_test_r8d_field_imm32(rv64_jit_writer_t *w, uint32_t offset,
                                      uint32_t value)
{
    Assert(offset <= INT8_MAX, "jit: RV64 DTLB dword field offset is too large");
    return emit_u8(w, 0x41) && emit_u8(w, 0xf7) &&
           emit_u8(w, 0x40) && emit_u8(w, (uint8_t)offset) && emit_u32(w, value);
}

/* Load a qword field from the R8-pointed DTLB entry into RDX. */
static bool emit_mov_rdx_r8q_field(rv64_jit_writer_t *w, uint32_t offset)
{
    Assert(offset <= INT8_MAX, "jit: RV64 DTLB qword field offset is too large");
    return emit_u8(w, 0x49) && emit_u8(w, 0x8b) &&
           emit_u8(w, 0x50) && emit_u8(w, (uint8_t)offset);
}

/* Compare a refcount word in the RAX-based table indexed by R8D with zero. */
static bool emit_cmp_ref_word_zero_rax_r8(rv64_jit_writer_t *w)
{
    /* `66 42 83 3c 40 00` is `cmp word ptr [rax + r8 * 2], 0`. */
    return emit_u8(w, 0x66) && emit_u8(w, 0x42) &&
           emit_u8(w, 0x83) && emit_u8(w, 0x3c) &&
           emit_u8(w, 0x40) && emit_u8(w, 0x00);
}

/* Emit `mov esi, imm32`, preparing the second helper argument. */
static bool emit_mov_esi_imm32(rv64_jit_writer_t *w, uint32_t imm)
{
    return emit_u8(w, 0xbe) && emit_u32(w, imm);
}

/* Emit `mov edx, imm32`, preparing the third helper argument. */
static bool emit_mov_edx_imm32(rv64_jit_writer_t *w, uint32_t imm)
{
    return emit_u8(w, 0xba) && emit_u32(w, imm);
}

/* Emit `cmp ecx, [rdx]`, comparing proposed work with the entry budget. */
static bool emit_cmp_ecx_m32_rdx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x3b) && emit_u8(w, 0x0a);
}

/* Return `jit_loop_extra + count` for exits from blocks with chained laps. */
static bool emit_return_loop_count(rv64_jit_writer_t *w, uint32_t count)
{
    return emit_movabs_rdx(w, (uint64_t)(uintptr_t)&jit_loop_extra) &&
           emit_mov_eax_m32_rdx(w) &&
           emit_add_eax_imm32(w, count) &&
           emit_return_eax(w);
}

/* Emit a 32-bit zeroing idiom for RAX. */
static bool emit_zero_rax(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x31) && emit_u8(w, 0xc0);
}

/* Emit `test al, imm8`, checking low address alignment bits. */
static bool emit_test_al_imm8(rv64_jit_writer_t *w, uint8_t mask)
{
    return emit_u8(w, 0xa8) && emit_u8(w, mask);
}

/* Load `cpu.gpr[reg]` into one 64-bit cached host register. */
static bool emit_load_gpr_hreg(rv64_jit_writer_t *w, rv64_jit_hreg_t hreg,
                               uint32_t reg)
{
    const uint8_t dst = jit_hreg_x86_reg(hreg);
    const uint8_t base = 11;

    /* `REX.W 8b /r` is `mov r64, qword ptr [r11 + disp32]`. */
    return emit_rex64(w, dst, base) &&
           emit_u8(w, 0x8b) &&
           emit_u8(w, jit_modrm(2, dst, base)) &&
           emit_u32(w, jit_gpr_offset(reg));
}

/* Store one cached 64-bit host register back into `cpu.gpr[reg]`. */
static bool emit_store_gpr_hreg(rv64_jit_writer_t *w, uint32_t reg,
                                rv64_jit_hreg_t hreg)
{
    const uint8_t src = jit_hreg_x86_reg(hreg);
    const uint8_t base = 11;

    /* `REX.W 89 /r` is `mov qword ptr [r11 + disp32], r64`. */
    return emit_rex64(w, src, base) &&
           emit_u8(w, 0x89) &&
           emit_u8(w, jit_modrm(2, src, base)) &&
           emit_u32(w, jit_gpr_offset(reg));
}

/* Copy one cached host-register value into RAX for generic emitters. */
static bool emit_mov_rax_hreg(rv64_jit_writer_t *w, rv64_jit_hreg_t hreg)
{
    const uint8_t src = jit_hreg_x86_reg(hreg);

    /* `mov rax, hreg` is encoded as `REX.W 89 /r` with RAX in the r/m field. */
    return emit_rex64(w, src, 0) &&
           emit_u8(w, 0x89) &&
           emit_u8(w, jit_modrm(3, src, 0));
}

/* Copy one cached host-register value into RCX for second operands. */
static bool emit_mov_rcx_hreg(rv64_jit_writer_t *w, rv64_jit_hreg_t hreg)
{
    const uint8_t src = jit_hreg_x86_reg(hreg);

    /* RCX is r/m field 1 in `mov rcx, hreg`. */
    return emit_rex64(w, src, 1) &&
           emit_u8(w, 0x89) &&
           emit_u8(w, jit_modrm(3, src, 1));
}

/* Copy one cached host-register value into RDX for helper arguments. */
static bool emit_mov_rdx_hreg(rv64_jit_writer_t *w, rv64_jit_hreg_t hreg)
{
    const uint8_t src = jit_hreg_x86_reg(hreg);

    /* RDX is r/m field 2 in `mov rdx, hreg`. */
    return emit_rex64(w, src, 2) &&
           emit_u8(w, 0x89) &&
           emit_u8(w, jit_modrm(3, src, 2));
}

/* Copy the RAX temporary result into a cached host register. */
static bool emit_mov_hreg_rax(rv64_jit_writer_t *w, rv64_jit_hreg_t hreg)
{
    const uint8_t dst = jit_hreg_x86_reg(hreg);

    /* `mov hreg, rax` is `REX.W 89 /r` with RAX in the reg field. */
    return emit_rex64(w, 0, dst) &&
           emit_u8(w, 0x89) &&
           emit_u8(w, jit_modrm(3, 0, dst));
}

/* Copy one cached host register to another. */
static bool emit_mov_hreg_hreg(rv64_jit_writer_t *w, rv64_jit_hreg_t dst,
                               rv64_jit_hreg_t src)
{
    const uint8_t dst_reg = jit_hreg_x86_reg(dst);
    const uint8_t src_reg = jit_hreg_x86_reg(src);

    if (dst == src)
    {
        return true;
    }

    /* `mov dst, src` keeps both operands in 64-bit host registers. */
    return emit_rex64(w, src_reg, dst_reg) &&
           emit_u8(w, 0x89) &&
           emit_u8(w, jit_modrm(3, src_reg, dst_reg));
}

/* Load a full-width constant into one cached host register. */
static bool emit_mov_hreg_imm64(rv64_jit_writer_t *w, rv64_jit_hreg_t hreg,
                                uint64_t value)
{
    const uint8_t dst = jit_hreg_x86_reg(hreg);

    if ((int64_t)value >= INT32_MIN && (int64_t)value <= INT32_MAX)
    {
        /*
         * `REX.W c7 /0 imm32` sign-extends a 32-bit immediate to 64 bits, which
         * is shorter than movabs and exactly matches small RV64 constants.
         */
        return emit_rex64(w, 0, dst) &&
               emit_u8(w, 0xc7) &&
               emit_u8(w, jit_modrm(3, 0, dst)) &&
               emit_u32(w, (uint32_t)value);
    }

    /* `REX.W b8+rd imm64` is the full movabs form for arbitrary RV64 values. */
    return emit_rex64(w, 0, dst) &&
           emit_u8(w, (uint8_t)(0xb8u + (dst & 7u))) &&
           emit_u64(w, value);
}

/* Initialise per-block guest-register cache metadata. */
static void jit_reg_cache_init(rv64_jit_reg_cache_t *regs)
{
    regs->next_age = 1;

    for (uint32_t i = 0; i < RV64_JIT_HREG_COUNT; i++)
    {
        regs->slots[i] = (rv64_jit_reg_slot_t){
            .valid = false,
            .loaded = false,
            .dirty = false,
            .guest_reg = 0,
            .age = 0,
            .hreg = (rv64_jit_hreg_t)i,
        };
    }
}

/* Restore compile-time cache metadata after an instruction emitter rolls back. */
static void jit_reg_cache_restore(rv64_jit_reg_cache_t *regs,
                                  const rv64_jit_reg_cache_t *snapshot)
{
    *regs = *snapshot;
}

/* Find the host-register slot currently assigned to one guest register. */
static rv64_jit_reg_slot_t *jit_reg_find(rv64_jit_reg_cache_t *regs,
                                         uint32_t reg)
{
    for (uint32_t i = 0; i < RV64_JIT_HREG_COUNT; i++)
    {
        rv64_jit_reg_slot_t *slot = &regs->slots[i];

        if (slot->valid && slot->guest_reg == reg)
        {
            return slot;
        }
    }

    return NULL;
}

/* Emit a store-back for one dirty cached slot without changing metadata. */
static bool jit_reg_emit_flush_slot(rv64_jit_writer_t *w,
                                    const rv64_jit_reg_slot_t *slot)
{
    if (!slot->valid || !slot->loaded || !slot->dirty || slot->guest_reg == 0)
    {
        return true;
    }

    return emit_store_gpr_hreg(w, slot->guest_reg, slot->hreg);
}

/* Flush one dirty slot and mark it clean once the native bytes are emitted. */
static bool jit_reg_flush_slot(rv64_jit_writer_t *w, rv64_jit_reg_slot_t *slot)
{
    if (!jit_reg_emit_flush_slot(w, slot))
    {
        return false;
    }

    slot->dirty = false;
    return true;
}

/* Flush every dirty cached guest register before helper-visible exits. */
static bool jit_reg_flush_all_dirty(rv64_jit_writer_t *w,
                                    rv64_jit_reg_cache_t *regs)
{
    for (uint32_t i = 0; i < RV64_JIT_HREG_COUNT; i++)
    {
        if (!jit_reg_flush_slot(w, &regs->slots[i]))
        {
            return false;
        }
    }

    return true;
}

/* Emit all dirty store-backs without changing the continuing path metadata. */
static bool jit_reg_emit_flush_all_dirty(rv64_jit_writer_t *w,
                                         const rv64_jit_reg_cache_t *regs)
{
    for (uint32_t i = 0; i < RV64_JIT_HREG_COUNT; i++)
    {
        if (!jit_reg_emit_flush_slot(w, &regs->slots[i]))
        {
            return false;
        }
    }

    return true;
}

/* Select a free slot or the least-recently-used slot when all are occupied. */
static rv64_jit_reg_slot_t *jit_reg_choose_slot(rv64_jit_reg_cache_t *regs)
{
    rv64_jit_reg_slot_t *oldest = &regs->slots[0];

    for (uint32_t i = 0; i < RV64_JIT_HREG_COUNT; i++)
    {
        rv64_jit_reg_slot_t *slot = &regs->slots[i];

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

/* Reserve a cache slot for one guest register, spilling the LRU victim if needed. */
static rv64_jit_reg_slot_t *jit_reg_alloc(rv64_jit_writer_t *w,
                                          rv64_jit_reg_cache_t *regs,
                                          uint32_t reg)
{
    rv64_jit_reg_slot_t *slot = jit_reg_find(regs, reg);

    if (slot != NULL)
    {
        slot->age = regs->next_age++;
        return slot;
    }

    slot = jit_reg_choose_slot(regs);
    const bool spill = slot->valid && slot->loaded && slot->dirty && slot->guest_reg != 0;

    if (!jit_reg_flush_slot(w, slot))
    {
        return NULL;
    }

    if (spill)
    {
        JIT_STAT_INC(reg_cache_spills);
    }

    slot->valid = true;
    slot->loaded = false;
    slot->dirty = false;
    slot->guest_reg = reg;
    slot->age = regs->next_age++;
    return slot;
}

/* Return a slot whose host register definitely contains the guest value. */
static rv64_jit_reg_slot_t *jit_reg_loaded_slot(rv64_jit_writer_t *w,
                                                rv64_jit_reg_cache_t *regs,
                                                uint32_t reg)
{
    rv64_jit_reg_slot_t *slot = jit_reg_alloc(w, regs, reg);

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

/* Materialise a guest register in RAX, treating x0 as constant zero. */
static bool jit_reg_read_rax(rv64_jit_writer_t *w,
                             rv64_jit_reg_cache_t *regs, uint32_t reg)
{
    if (reg == 0)
    {
        return emit_zero_rax(w);
    }

    rv64_jit_reg_slot_t *slot = jit_reg_loaded_slot(w, regs, reg);
    return slot != NULL && emit_mov_rax_hreg(w, slot->hreg);
}

/* Materialise a guest register in RCX, treating x0 as constant zero. */
static bool jit_reg_read_rcx(rv64_jit_writer_t *w,
                             rv64_jit_reg_cache_t *regs, uint32_t reg)
{
    if (reg == 0)
    {
        return emit_u8(w, 0x31) && emit_u8(w, 0xc9);
    }

    rv64_jit_reg_slot_t *slot = jit_reg_loaded_slot(w, regs, reg);
    return slot != NULL && emit_mov_rcx_hreg(w, slot->hreg);
}

/* Materialise a guest register in RDX, treating x0 as constant zero. */
static bool jit_reg_read_rdx(rv64_jit_writer_t *w,
                             rv64_jit_reg_cache_t *regs, uint32_t reg)
{
    if (reg == 0)
    {
        return emit_u8(w, 0x31) && emit_u8(w, 0xd2);
    }

    rv64_jit_reg_slot_t *slot = jit_reg_loaded_slot(w, regs, reg);
    return slot != NULL && emit_mov_rdx_hreg(w, slot->hreg);
}

/* Write the current RAX result into one guest-register cache slot. */
static bool jit_reg_write_rax(rv64_jit_writer_t *w,
                              rv64_jit_reg_cache_t *regs, uint32_t reg)
{
    if (reg == 0)
    {
        return true;
    }

    rv64_jit_reg_slot_t *slot = jit_reg_alloc(w, regs, reg);

    if (slot == NULL || !emit_mov_hreg_rax(w, slot->hreg))
    {
        return false;
    }

    slot->loaded = true;
    slot->dirty = true;
    slot->age = regs->next_age++;
    return true;
}

/* Write a constant value into one guest-register cache slot. */
static bool jit_reg_write_imm(rv64_jit_writer_t *w,
                              rv64_jit_reg_cache_t *regs, uint32_t reg,
                              uint64_t value)
{
    if (reg == 0)
    {
        return true;
    }

    rv64_jit_reg_slot_t *slot = jit_reg_alloc(w, regs, reg);

    if (slot == NULL || !emit_mov_hreg_imm64(w, slot->hreg, value))
    {
        return false;
    }

    slot->loaded = true;
    slot->dirty = true;
    slot->age = regs->next_age++;
    return true;
}

/* Copy a guest register value to another cache slot without touching memory. */
static bool jit_reg_copy(rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs,
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

    rv64_jit_reg_slot_t *src = jit_reg_loaded_slot(w, regs, src_reg);
    if (src == NULL)
    {
        return false;
    }

    if (dst_reg == src_reg)
    {
        return true;
    }

    rv64_jit_reg_slot_t *dst = jit_reg_alloc(w, regs, dst_reg);
    if (dst == NULL || !emit_mov_hreg_hreg(w, dst->hreg, src->hreg))
    {
        return false;
    }

    dst->loaded = true;
    dst->dirty = true;
    dst->age = regs->next_age++;
    return true;
}

/* Store an immediate guest PC by materialising it in RAX first. */
static bool emit_store_pc_imm(rv64_jit_writer_t *w, vaddr_t pc)
{
    return emit_movabs_rax(w, pc) &&
           /* `49 89 83 disp32` stores RAX into `cpu.pc` through R11. */
           emit_u8(w, 0x49) && emit_u8(w, 0x89) &&
           emit_u8(w, 0x83) && emit_u32(w, jit_pc_offset());
}

/* Store a dynamic guest PC already held in RAX. */
static bool emit_store_rax_pc(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x49) && emit_u8(w, 0x89) &&
           emit_u8(w, 0x83) && emit_u32(w, jit_pc_offset());
}

/* Emit `add rax, imm32`, whose immediate is sign-extended by x86-64. */
static bool emit_add_rax_imm32(rv64_jit_writer_t *w, int32_t imm)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x05) && emit_u32(w, (uint32_t)imm);
}

/* Emit `and rax, imm32`, whose immediate is sign-extended by x86-64. */
static bool emit_and_rax_imm32(rv64_jit_writer_t *w, int32_t imm)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x25) && emit_u32(w, (uint32_t)imm);
}

/* Emit one RAX op RCX 64-bit ALU instruction selected by the opcode byte. */
static bool emit_rax_rcx_alu64(rv64_jit_writer_t *w, uint8_t opcode)
{
    /*
     * Opcodes use ModRM C8 (`rax, rcx`): 01=ADD, 29=SUB, 31=XOR,
     * 09=OR and 21=AND. REX.W makes the operation full 64-bit.
     */
    return emit_u8(w, 0x48) && emit_u8(w, opcode) && emit_u8(w, 0xc8);
}

/* Emit one EAX op ECX 32-bit ALU instruction, then sign-extend to 64 bits. */
static bool emit_eax_ecx_alu32_sext(rv64_jit_writer_t *w, uint8_t opcode)
{
    /* W-form RV64 ALU operations keep low 32 bits, then CDQE sign-extends EAX. */
    return emit_u8(w, opcode) && emit_u8(w, 0xc8) &&
           emit_u8(w, 0x48) && emit_u8(w, 0x98);
}

/* Emit a 64-bit immediate shift of RAX. */
static bool emit_shift_rax_imm(rv64_jit_writer_t *w, uint8_t subop, uint8_t shamt)
{
    /* Group-2 ModRM subops are e0=SHL, e8=SHR and f8=SAR on RAX. */
    return emit_u8(w, 0x48) && emit_u8(w, 0xc1) && emit_u8(w, subop) && emit_u8(w, shamt);
}

/* Emit a 32-bit immediate shift of EAX, then sign-extend to 64 bits. */
static bool emit_shift_eax_imm_sext(rv64_jit_writer_t *w, uint8_t subop, uint8_t shamt)
{
    /* Group-2 ModRM subops are e0=SHL, e8=SHR and f8=SAR on EAX. */
    return emit_u8(w, 0xc1) && emit_u8(w, subop) && emit_u8(w, shamt) &&
           emit_u8(w, 0x48) && emit_u8(w, 0x98);
}

/* Emit a 64-bit variable shift of RAX by CL. */
static bool emit_shift_rax_cl(rv64_jit_writer_t *w, uint8_t subop)
{
    /* D3 uses CL as the variable shift count; RISC-V masks the count similarly. */
    return emit_u8(w, 0x48) && emit_u8(w, 0xd3) && emit_u8(w, subop);
}

/* Emit a 32-bit variable shift of EAX by CL, then sign-extend to 64 bits. */
static bool emit_shift_eax_cl_sext(rv64_jit_writer_t *w, uint8_t subop)
{
    /* D3 uses CL as the variable shift count; CDQE sign-extends W-form results. */
    return emit_u8(w, 0xd3) && emit_u8(w, subop) &&
           emit_u8(w, 0x48) && emit_u8(w, 0x98);
}

/* Emit `cmp rax, rcx` for signed or unsigned setcc operations. */
static bool emit_cmp_rax_rcx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x39) && emit_u8(w, 0xc8);
}

/* Emit `cmp rax, imm32`, using x86-64 sign-extension of the immediate. */
static bool emit_cmp_rax_imm32(rv64_jit_writer_t *w, int32_t imm)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x3d) && emit_u32(w, (uint32_t)imm);
}

/* Materialise a condition-code result as 0 or 1 in RAX. */
static bool emit_setcc_rax(rv64_jit_writer_t *w, uint8_t setcc_opcode)
{
    /* `0f setcc c0` writes AL, then `0f b6 c0` zero-extends AL into EAX/RAX. */
    return emit_u8(w, 0x0f) && emit_u8(w, setcc_opcode) &&
           emit_u8(w, 0xc0) &&
           emit_u8(w, 0x0f) && emit_u8(w, 0xb6) && emit_u8(w, 0xc0);
}

/* Emit a conditional branch with a rel32 placeholder and return its patch site. */
static bool emit_jcc_rel32_placeholder(rv64_jit_writer_t *w, uint8_t jcc_opcode,
                                       uint8_t **disp)
{
    /* x86 near conditional branches are `0f 8x disp32`; `disp` points at disp32. */
    if (!emit_u8(w, 0x0f) || !emit_u8(w, jcc_opcode))
    {
        return false;
    }

    *disp = w->cur;
    return emit_u32(w, 0);
}

/* Emit an unconditional `jmp rel32` and return its displacement patch site. */
static bool emit_jmp_rel32_placeholder(rv64_jit_writer_t *w, uint8_t **disp)
{
    /* `e9 disp32` jumps relative to the byte after the 32-bit displacement. */
    if (!emit_u8(w, 0xe9))
    {
        return false;
    }

    *disp = w->cur;
    return emit_u32(w, 0);
}

/* Emit `movabs rax, target; call rax` for rare helper-backed side paths. */
static bool emit_call_abs(rv64_jit_writer_t *w, uintptr_t target)
{
    return emit_movabs_rax(w, (uint64_t)target) &&
           emit_u8(w, 0xff) && emit_u8(w, 0xd0);
}

/* Emit an optional native-side increment for one 64-bit JIT stat counter. */
static bool emit_inc_jit_stat_counter(rv64_jit_writer_t *w, uint64_t *counter)
{
#if RV64_JIT_STATS
    /*
     * `48 ff 00` is `inc qword ptr [rax]`.  The helper deliberately clobbers
     * RAX; callers place it after address proof and before instructions that
     * overwrite RAX or no longer need it.
     */
    return emit_movabs_rax(w, (uint64_t)(uintptr_t)counter) &&
           emit_u8(w, 0x48) && emit_u8(w, 0xff) && emit_u8(w, 0x00);
#else
    (void)w;
    (void)counter;
    return true;
#endif
}

/* Count one runtime load that completed through the inline translated-PMEM path. */
static bool emit_inline_paged_load_hit_stats(rv64_jit_writer_t *w)
{
    return emit_inc_jit_stat_counter(w, &jit_stats.data_tlb_hits) &&
           emit_inc_jit_stat_counter(w, &jit_stats.inline_paged_load_hits);
}

/* Count one runtime store that completed through the inline translated-PMEM path. */
static bool emit_inline_paged_store_hit_stats(rv64_jit_writer_t *w)
{
    return emit_inc_jit_stat_counter(w, &jit_stats.data_tlb_hits) &&
           emit_inc_jit_stat_counter(w, &jit_stats.inline_paged_store_hits);
}

/* Patch a rel32 displacement emitted by a previous branch helper. */
static void patch_rel32(uint8_t *disp, const uint8_t *target)
{
    int64_t rel = target - (disp + 4);
    Assert(rel >= INT32_MIN && rel <= INT32_MAX, "jit: rel32 target is out of range");
    int32_t rel32 = (int32_t)rel;
    memcpy(disp, &rel32, sizeof(rel32));
}

/* Flush cached registers and side-exit so the interpreter executes this PC. */
static bool emit_interpreter_side_exit(rv64_jit_writer_t *w,
                                       rv64_jit_reg_cache_t *regs, vaddr_t pc,
                                       uint32_t completed_count,
                                       bool loop_count_needed)
{
    return jit_reg_emit_flush_all_dirty(w, regs) &&
           emit_store_pc_imm(w, pc) &&
           (loop_count_needed ? emit_return_loop_count(w, completed_count)
                              : emit_return_count(w, completed_count));
}

/* Emit the x86 load instruction matching one RV64 load funct3 field. */
static bool emit_direct_pmem_load_rax(rv64_jit_writer_t *w, uint32_t funct3)
{
    /*
     * RDX is the byte offset from CONFIG_MBASE and R10 is the host pointer for
     * CONFIG_MBASE. Signed byte/half/word forms use x86 sign-extension loads;
     * unsigned forms write EAX, which zeroes the upper half of RAX by x86-64
     * rule. LD is a plain 64-bit load.
     */
    switch (funct3)
    {
    case 0x0: /* LB: movsx rax, byte ptr [r10 + rdx]. */
        return emit_u8(w, 0x49) && emit_u8(w, 0x0f) && emit_u8(w, 0xbe) &&
               emit_u8(w, 0x04) && emit_u8(w, 0x12);
    case 0x1: /* LH: movsx rax, word ptr [r10 + rdx]. */
        return emit_u8(w, 0x49) && emit_u8(w, 0x0f) && emit_u8(w, 0xbf) &&
               emit_u8(w, 0x04) && emit_u8(w, 0x12);
    case 0x2: /* LW: movsxd rax, dword ptr [r10 + rdx]. */
        return emit_u8(w, 0x49) && emit_u8(w, 0x63) &&
               emit_u8(w, 0x04) && emit_u8(w, 0x12);
    case 0x3: /* LD: mov rax, qword ptr [r10 + rdx]. */
        return emit_u8(w, 0x49) && emit_u8(w, 0x8b) &&
               emit_u8(w, 0x04) && emit_u8(w, 0x12);
    case 0x4: /* LBU: movzx eax, byte ptr [r10 + rdx]. */
        return emit_u8(w, 0x41) && emit_u8(w, 0x0f) && emit_u8(w, 0xb6) &&
               emit_u8(w, 0x04) && emit_u8(w, 0x12);
    case 0x5: /* LHU: movzx eax, word ptr [r10 + rdx]. */
        return emit_u8(w, 0x41) && emit_u8(w, 0x0f) && emit_u8(w, 0xb7) &&
               emit_u8(w, 0x04) && emit_u8(w, 0x12);
    case 0x6: /* LWU: mov eax, dword ptr [r10 + rdx]. */
        return emit_u8(w, 0x41) && emit_u8(w, 0x8b) &&
               emit_u8(w, 0x04) && emit_u8(w, 0x12);
    default:
        return false;
    }
}

/* Emit one conservative fallback branch for an inline RV64 data-TLB guard. */
static bool emit_tlb_guard_slow_jcc(rv64_jit_writer_t *w,
                                    rv64_jit_tlb_guard_patch_t *patch,
                                    uint8_t jcc_opcode)
{
    Assert(patch->count < sizeof(patch->slow_disps) / sizeof(patch->slow_disps[0]),
           "jit: too many RV64 DTLB slow-path branches");
    return emit_jcc_rel32_placeholder(w, jcc_opcode,
                                      &patch->slow_disps[patch->count++]);
}

/* Patch every fallback branch emitted by an inline RV64 data-TLB guard. */
static void patch_tlb_guard(const rv64_jit_tlb_guard_patch_t *patch,
                            const uint8_t *slow_path)
{
    for (uint32_t i = 0; i < patch->count; i++)
    {
        patch_rel32(patch->slow_disps[i], slow_path);
    }
}

/* Emit the shared inline DTLB-hit proof for translated PMEM data accesses. */
static bool emit_paged_tlb_common_offset_rdx(rv64_jit_writer_t *w, uint32_t len,
                                            uint32_t need_access,
                                            rv64_jit_tlb_guard_patch_t *patch)
{
    Assert(len >= 1 && len <= 8, "jit: unsupported RV64 DTLB width %u", len);

    const word_t satp = cpu.csr.satp;
    const uint32_t state = jit_data_tlb_state(MEM_TYPE_READ);
    const uint32_t valid_off = (uint32_t)offsetof(rv64_jit_data_tlb_entry_t, valid);
    const uint32_t satp_off = (uint32_t)offsetof(rv64_jit_data_tlb_entry_t, satp);
    const uint32_t vpn_off = (uint32_t)offsetof(rv64_jit_data_tlb_entry_t, vpn);
    const uint32_t state_off = (uint32_t)offsetof(rv64_jit_data_tlb_entry_t, state);
    const uint32_t access_off = (uint32_t)offsetof(rv64_jit_data_tlb_entry_t, access);
    const uint32_t pg_paddr_off =
        (uint32_t)offsetof(rv64_jit_data_tlb_entry_t, pg_paddr);
    const uint8_t entry_shift = 6; /* sizeof(rv64_jit_data_tlb_entry_t) == 64. */

    /*
     * The generated proof mirrors jit_translate_pmem()'s TLB-hit half:
     *   vpn = vaddr >> 12
     *   entry = &jit_data_tlb[(vpn ^ vpn>>9 ^ satp ^ satp>>12 ^ state) & mask]
     *   require valid, exact satp, exact VPN, exact permission state and access
     *   require the byte range to stay inside the translated 4 KiB page
     *
     * RCX is reserved by the caller for the original guest address or store
     * value, so the guard uses RAX/RDX/R8 only.  Any failed guard branches to
     * the old helper path, which still owns faults, MMIO, and fresh TLB fills.
     *
     * x86 condition opcodes below are the near-Jcc low bytes: 0x84 is JE/JZ,
     * 0x85 is JNE/JNZ, and 0x87 is JA for unsigned page-offset overflow.
     */
    if (!emit_mov_rdx_rax(w) ||
        !emit_shr_rdx_imm(w, PAGE_SHIFT) ||
        !emit_mov_r8_rdx(w) ||
        !emit_shr_r8_imm(w, 9) ||
        !emit_xor_r8_rdx(w) ||
        !emit_movabs_rdx(w, satp ^ (satp >> 12) ^ state) ||
        !emit_xor_r8_rdx(w) ||
        !emit_and_r8d_imm(w, RV64_JIT_DATA_TLB_SIZE - 1u) ||
        !emit_shl_r8_imm(w, entry_shift) ||
        !emit_movabs_rdx(w, (uint64_t)(uintptr_t)jit_data_tlb) ||
        !emit_add_r8_rdx(w) ||
        !emit_cmp_r8b_field_imm8(w, valid_off, 0) ||
        !emit_tlb_guard_slow_jcc(w, patch, 0x84) ||
        !emit_movabs_rdx(w, satp) ||
        !emit_cmp_r8q_field_rdx(w, satp_off) ||
        !emit_tlb_guard_slow_jcc(w, patch, 0x85) ||
        !emit_mov_rdx_rax(w) ||
        !emit_shr_rdx_imm(w, PAGE_SHIFT) ||
        !emit_cmp_r8q_field_rdx(w, vpn_off) ||
        !emit_tlb_guard_slow_jcc(w, patch, 0x85) ||
        !emit_cmp_r8d_field_imm32(w, state_off, state) ||
        !emit_tlb_guard_slow_jcc(w, patch, 0x85) ||
        !emit_test_r8d_field_imm32(w, access_off, need_access) ||
        !emit_tlb_guard_slow_jcc(w, patch, 0x84) ||
        !emit_and_rax_imm32(w, PAGE_MASK) ||
        !emit_cmp_rax_imm32(w, PAGE_SIZE - len) ||
        !emit_tlb_guard_slow_jcc(w, patch, 0x87) ||
        !emit_mov_rdx_r8q_field(w, pg_paddr_off) ||
        !emit_or_rdx_rax(w) ||
        !emit_movabs_rax(w, (uint64_t)CONFIG_MBASE) ||
        !emit_sub_rdx_rax(w))
    {
        return false;
    }

    return true;
}

/* Emit an inline translated-PMEM load using a previously filled RV64 data TLB. */
static bool emit_paged_tlb_load_rax(rv64_jit_writer_t *w, uint32_t funct3,
                                    uint32_t len,
                                    rv64_jit_tlb_guard_patch_t *patch)
{
    /*
     * RCX must contain the original guest virtual address before entry.  The
     * common guard may clobber RAX while computing the page offset; on success
     * RDX is the byte offset from CONFIG_MBASE for emit_direct_pmem_load_rax().
     */
    return emit_paged_tlb_common_offset_rdx(w, len, RV64_JIT_DATA_TLB_READ, patch) &&
           emit_inline_paged_load_hit_stats(w) &&
           emit_direct_pmem_load_rax(w, funct3);
}

/* Emit an inline PMEM store from RCX using the selected RV64 store width. */
static bool emit_direct_pmem_store_from_rcx(rv64_jit_writer_t *w, uint32_t len)
{
    /*
     * The low part of RCX naturally supplies SB/SH/SW truncation.  SD uses the
     * full 64-bit register.  The caller has already proved that RDX is an
     * in-PMEM byte offset and that the write is not to tracked source or page
     * table bytes.
     */
    switch (len)
    {
    case 1: /* mov byte ptr [r10 + rdx], cl. */
        return emit_u8(w, 0x41) && emit_u8(w, 0x88) &&
               emit_u8(w, 0x0c) && emit_u8(w, 0x12);
    case 2: /* mov word ptr [r10 + rdx], cx. */
        return emit_u8(w, 0x66) && emit_u8(w, 0x41) &&
               emit_u8(w, 0x89) && emit_u8(w, 0x0c) && emit_u8(w, 0x12);
    case 4: /* mov dword ptr [r10 + rdx], ecx. */
        return emit_u8(w, 0x41) && emit_u8(w, 0x89) &&
               emit_u8(w, 0x0c) && emit_u8(w, 0x12);
    case 8: /* mov qword ptr [r10 + rdx], rcx. */
        return emit_u8(w, 0x49) && emit_u8(w, 0x89) &&
               emit_u8(w, 0x0c) && emit_u8(w, 0x12);
    default:
        return false;
    }
}

/* Emit guards that keep inline stores away from compiled source chunks. */
static bool emit_store_source_chunk_guard(rv64_jit_writer_t *w, uint32_t len,
                                          uint8_t **cross_chunk_disp,
                                          uint8_t **source_chunk_disp)
{
    Assert(len >= 1 && len <= 8, "jit: unsupported RV64 store width %u", len);

    /*
     * Direct inline stores only continue when they stay within one source-ref
     * chunk and that chunk currently has no compiled block references.  The
     * helper path performs exact invalidation and exits for every ambiguous
     * store, preserving self-modifying-code ordering.
     */
    return emit_mov_r8d_edx(w) &&
           emit_and_r8d_imm(w, RV64_JIT_SOURCE_CHUNK_MASK) &&
           emit_cmp_r8d_imm(w, RV64_JIT_SOURCE_CHUNK_SIZE - len) &&
           emit_jcc_rel32_placeholder(w, 0x87, cross_chunk_disp) &&
           emit_mov_r8d_edx(w) &&
           emit_shr_r8d_imm(w, RV64_JIT_SOURCE_CHUNK_SHIFT) &&
           emit_movabs_rax(w, (uint64_t)(uintptr_t)jit_source_chunk_refs) &&
           emit_cmp_ref_word_zero_rax_r8(w) &&
           emit_jcc_rel32_placeholder(w, 0x85, source_chunk_disp);
}

/* Emit a guard that keeps inline stores away from cached page-table pages. */
static bool emit_store_page_table_guard(rv64_jit_writer_t *w,
                                        uint8_t **page_table_disp)
{
    /*
     * RDX is the PMEM byte offset.  A non-zero page-table refcount means a
     * direct write could stale a data-TLB entry, so the helper must perform the
     * store, flush the DTLB through isa_jit_invalidate_paddr(), and exit.
     */
    return emit_mov_r8d_edx(w) &&
           emit_shr_r8d_imm(w, PAGE_SHIFT) &&
           emit_movabs_rax(w, (uint64_t)(uintptr_t)jit_data_tlb_pt_page_refs) &&
           emit_cmp_ref_word_zero_rax_r8(w) &&
           emit_jcc_rel32_placeholder(w, 0x85, page_table_disp);
}

/* Emit an inline translated-PMEM store address proof through the RV64 data TLB. */
static bool emit_paged_tlb_store_offset_rdx(rv64_jit_writer_t *w, uint32_t len,
                                            rv64_jit_tlb_guard_patch_t *patch)
{
    /*
     * RDI must hold the original guest virtual address and RCX the store value.
     * The common guard may clobber RAX while proving the page offset.  On
     * success RDX is the byte offset from CONFIG_MBASE for the direct store.
     */
    return emit_paged_tlb_common_offset_rdx(w, len, RV64_JIT_DATA_TLB_WRITE, patch);
}

/* Emit one helper-backed RV64 load for non-Bare address translation modes. */
static bool emit_paged_load_instr(rv64_jit_writer_t *w,
                                  rv64_jit_reg_cache_t *regs,
                                  uint32_t rd, uint32_t rs1,
                                  uint32_t funct3,
                                  int32_t imm, uint32_t len,
                                  uintptr_t helper, vaddr_t pc,
                                  uint32_t completed_count,
                                  bool loop_count_needed)
{
    uint8_t *align_slow_disp = NULL;
    uint8_t *fast_done_disp = NULL;
    uint8_t *done_disp = NULL;
    rv64_jit_tlb_guard_patch_t tlb_guard = {0};
    rv64_jit_reg_cache_t side_exit_regs;

    if (!jit_reg_read_rax(w, regs, rs1) ||
        !emit_add_rax_imm32(w, imm))
    {
        return false;
    }

    side_exit_regs = *regs;

    if (len > 1 &&
        (!emit_test_al_imm8(w, (uint8_t)(len - 1u)) ||
         /* 0x85 is x86 JNE/JNZ rel32: misaligned address falls back. */
         !emit_jcc_rel32_placeholder(w, 0x85, &align_slow_disp)))
    {
        return false;
    }

    /*
     * The inline path consumes a TLB hit without leaving generated code.  RCX
     * preserves the original guest address until every fallback branch has
     * reached the helper path; the helper remains the only place that fills the
     * TLB or reports faults/MMIO.
     */
    if (!emit_mov_rcx_rax(w) ||
        !emit_paged_tlb_load_rax(w, funct3, len, &tlb_guard))
    {
        return false;
    }

    if (!emit_jmp_rel32_placeholder(w, &fast_done_disp))
    {
        return false;
    }

    const uint8_t *slow_path = w->cur;
    patch_tlb_guard(&tlb_guard, slow_path);

    if (!emit_mov_rax_rcx(w) ||
        !jit_reg_emit_flush_all_dirty(w, regs) ||
        !emit_mov_rdi_rax(w) ||
        !emit_store_pc_imm(w, pc) ||
        !emit_call_abs(w, helper) ||
        !emit_load_cpu_base(w) ||
        !emit_movabs_r10(w, (uint64_t)(uintptr_t)guest_to_host(CONFIG_MBASE)))
    {
        return false;
    }

    patch_rel32(fast_done_disp, w->cur);

    if (!jit_reg_write_rax(w, regs, rd))
    {
        return false;
    }

    if (align_slow_disp != NULL)
    {
        /*
         * The alignment side exit is only valid before the load changes RD.
         * Fast and helper-backed success must skip it; otherwise an instruction
         * such as `ld a4, imm(a4)` would re-enter the interpreter with the
         * loaded value already in the base register.
         */
        if (!emit_jmp_rel32_placeholder(w, &done_disp))
        {
            return false;
        }

        patch_rel32(align_slow_disp, w->cur);
        if (!emit_interpreter_side_exit(w, &side_exit_regs, pc, completed_count, loop_count_needed))
        {
            return false;
        }

        patch_rel32(done_disp, w->cur);
    }

    JIT_STAT_INC(native_loads);
    JIT_STAT_INC(native_paged_loads);
    JIT_STAT_INC(inline_paged_loads);
    return true;
}

/* Emit one guarded bare-mode RV64 load that falls back before unsafe accesses. */
static bool emit_load_instr(rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs,
                            uint32_t instr, vaddr_t pc,
                            uint32_t completed_count, bool loop_count_needed)
{
    const uint32_t rd = bits(instr, 11, 7);
    const uint32_t funct3 = bits(instr, 14, 12);
    const uint32_t rs1 = bits(instr, 19, 15);
    const int32_t imm = (int32_t)imm_i(instr);
    uint32_t len = 0;
    uintptr_t helper = 0;
    uint8_t *align_slow_disp = NULL;
    uint8_t *range_slow_disp = NULL;
    uint8_t *done_disp = NULL;
    rv64_jit_reg_cache_t side_exit_regs;

    switch (funct3)
    {
    case 0x0: /* LB */
        helper = (uintptr_t)jit_load_i8;
        len = 1;
        break;
    case 0x4: /* LBU */
        helper = (uintptr_t)jit_load_u8;
        len = 1;
        break;
    case 0x1: /* LH */
        helper = (uintptr_t)jit_load_i16;
        len = 2;
        break;
    case 0x5: /* LHU */
        helper = (uintptr_t)jit_load_u16;
        len = 2;
        break;
    case 0x2: /* LW */
        helper = (uintptr_t)jit_load_i32;
        len = 4;
        break;
    case 0x6: /* LWU */
        helper = (uintptr_t)jit_load_u32;
        len = 4;
        break;
    case 0x3: /* LD */
        helper = (uintptr_t)jit_load_u64;
        len = 8;
        break;
    default:
        return false;
    }

    /*
     * The direct PMEM tier is intentionally Bare-mode only.  Non-Bare modes use
     * helper calls below, because Sv39 permission and effective-privilege checks
     * are subtler than this physical-address range proof.
     */
    if ((cpu.csr.satp >> 60) != 0)
    {
        return emit_paged_load_instr(w, regs, rd, rs1, funct3, imm, len, helper, pc,
                                     completed_count, loop_count_needed);
    }

    if (!jit_reg_read_rax(w, regs, rs1) ||
        !emit_add_rax_imm32(w, imm))
    {
        return false;
    }

    side_exit_regs = *regs;

    if (len > 1 &&
        (!emit_test_al_imm8(w, (uint8_t)(len - 1u)) ||
         !emit_jcc_rel32_placeholder(w, 0x85, &align_slow_disp)))
    {
        return false;
    }

    /*
     * Guard the complete physical byte range before touching host memory:
     *   offset = guest_addr - CONFIG_MBASE
     *   accept only offset <= CONFIG_MSIZE - len
     * Unsigned JA catches underflow, wraparound, MMIO and out-of-PMEM addresses.
     */
    if (!emit_mov_rdx_rax(w) ||
        !emit_movabs_rcx(w, (uint64_t)CONFIG_MBASE) ||
        !emit_sub_rdx_rcx(w) ||
        !emit_movabs_rcx(w, (uint64_t)CONFIG_MSIZE - len) ||
        !emit_cmp_rdx_rcx(w) ||
        !emit_jcc_rel32_placeholder(w, 0x87, &range_slow_disp) ||
        !emit_direct_pmem_load_rax(w, funct3) ||
        !jit_reg_write_rax(w, regs, rd) ||
        !emit_jmp_rel32_placeholder(w, &done_disp))
    {
        return false;
    }

    if (align_slow_disp != NULL)
    {
        patch_rel32(align_slow_disp, w->cur);
    }
    patch_rel32(range_slow_disp, w->cur);

    if (!emit_interpreter_side_exit(w, &side_exit_regs, pc, completed_count, loop_count_needed))
    {
        return false;
    }

    patch_rel32(done_disp, w->cur);
    JIT_STAT_INC(native_loads);
    return true;
}

/* Emit one helper-backed RV64 store for non-Bare address translation modes. */
static bool emit_paged_store_instr(rv64_jit_writer_t *w,
                                   rv64_jit_reg_cache_t *regs,
                                   uint32_t rs1, uint32_t rs2,
                                   int32_t imm, uint32_t len,
                                   vaddr_t pc, vaddr_t next_pc,
                                   uint32_t completed_count,
                                   bool loop_count_needed)
{
    uint8_t *align_slow_disp = NULL;
    uint8_t *cross_chunk_disp = NULL;
    uint8_t *source_chunk_disp = NULL;
    uint8_t *page_table_disp = NULL;
    uint8_t *fast_done_disp = NULL;
    uint8_t *done_disp = NULL;
    rv64_jit_tlb_guard_patch_t tlb_guard = {0};
    rv64_jit_reg_cache_t side_exit_regs;

    if (!jit_reg_flush_all_dirty(w, regs) ||
        !jit_reg_read_rax(w, regs, rs1) ||
        !emit_add_rax_imm32(w, imm))
    {
        return false;
    }

    side_exit_regs = *regs;

    if (len > 1 &&
        (!emit_test_al_imm8(w, (uint8_t)(len - 1u)) ||
         /* 0x85 is x86 JNE/JNZ rel32: misaligned address falls back. */
         !emit_jcc_rel32_placeholder(w, 0x85, &align_slow_disp)))
    {
        return false;
    }

    /*
     * A DTLB-hit store can commit inline only when the final physical bytes are
     * ordinary PMEM and are not tracked as compiled source or page-table pages.
     * Every miss or sensitive write uses the old helper-and-exit path, so stale
     * translations and self-modifying code are still observed before the next
     * native block lookup.
     */
    if (!emit_mov_rdi_rax(w) ||
        !jit_reg_read_rcx(w, regs, rs2) ||
        !emit_paged_tlb_store_offset_rdx(w, len, &tlb_guard) ||
        !emit_store_source_chunk_guard(w, len, &cross_chunk_disp,
                                       &source_chunk_disp) ||
        !emit_store_page_table_guard(w, &page_table_disp) ||
        !emit_inline_paged_store_hit_stats(w) ||
        !emit_direct_pmem_store_from_rcx(w, len) ||
        !emit_jmp_rel32_placeholder(w, &fast_done_disp))
    {
        return false;
    }

    const uint8_t *slow_path = w->cur;
    patch_tlb_guard(&tlb_guard, slow_path);
    patch_rel32(cross_chunk_disp, slow_path);
    patch_rel32(source_chunk_disp, slow_path);
    patch_rel32(page_table_disp, slow_path);

    if (!emit_mov_rdx_rcx(w) ||
        !emit_mov_esi_imm32(w, len) ||
        !emit_store_pc_imm(w, pc) ||
        !emit_call_abs(w, (uintptr_t)jit_store_vaddr) ||
        !emit_load_cpu_base(w) ||
        !emit_movabs_r10(w, (uint64_t)(uintptr_t)guest_to_host(CONFIG_MBASE)) ||
        !emit_store_pc_imm(w, next_pc) ||
        !(loop_count_needed ? emit_return_loop_count(w, completed_count + 1u)
                             : emit_return_count(w, completed_count + 1u)))
    {
        return false;
    }

    patch_rel32(fast_done_disp, w->cur);

    if (align_slow_disp != NULL)
    {
        /*
         * Only the pre-store alignment guard may enter this side exit.  A
         * successful inline store continues in native code, while a helper store
         * has already returned to the dispatcher after updating cpu.pc.
         */
        if (!emit_jmp_rel32_placeholder(w, &done_disp))
        {
            return false;
        }

        patch_rel32(align_slow_disp, w->cur);
        if (!emit_interpreter_side_exit(w, &side_exit_regs, pc, completed_count, loop_count_needed))
        {
            return false;
        }

        patch_rel32(done_disp, w->cur);
    }

    JIT_STAT_INC(native_stores);
    JIT_STAT_INC(native_paged_stores);
    JIT_STAT_INC(inline_paged_stores);
    return true;
}

/* Emit one guarded bare-mode RV64 store that commits through paddr_write(). */
static bool emit_store_instr(rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs,
                             uint32_t instr, vaddr_t pc,
                             vaddr_t next_pc, uint32_t completed_count,
                             bool loop_count_needed)
{
    const uint32_t funct3 = bits(instr, 14, 12);
    const uint32_t rs1 = bits(instr, 19, 15);
    const uint32_t rs2 = bits(instr, 24, 20);
    const int32_t imm = (int32_t)imm_s(instr);
    uint32_t len = 0;
    uint8_t *align_slow_disp = NULL;
    uint8_t *range_slow_disp = NULL;
    uint8_t *exit_disp = NULL;
    uint8_t *continue_disp = NULL;

    switch (funct3)
    {
    case 0x0: /* SB */
        len = 1;
        break;
    case 0x1: /* SH */
        len = 2;
        break;
    case 0x2: /* SW */
        len = 4;
        break;
    case 0x3: /* SD */
        len = 8;
        break;
    default:
        return false;
    }

    if ((cpu.csr.satp >> 60) != 0)
    {
        return emit_paged_store_instr(w, regs, rs1, rs2, imm, len, pc, next_pc,
                                      completed_count, loop_count_needed);
    }

    if (!jit_reg_flush_all_dirty(w, regs) ||
        !jit_reg_read_rax(w, regs, rs1) ||
        !emit_add_rax_imm32(w, imm))
    {
        return false;
    }

    if (len > 1 &&
        (!emit_test_al_imm8(w, (uint8_t)(len - 1u)) ||
         !emit_jcc_rel32_placeholder(w, 0x85, &align_slow_disp)))
    {
        return false;
    }

    if (!emit_mov_rdx_rax(w) ||
        !emit_movabs_rcx(w, (uint64_t)CONFIG_MBASE) ||
        !emit_sub_rdx_rcx(w) ||
        !emit_movabs_rcx(w, (uint64_t)CONFIG_MSIZE - len) ||
        !emit_cmp_rdx_rcx(w) ||
        !emit_jcc_rel32_placeholder(w, 0x87, &range_slow_disp))
    {
        return false;
    }

    /*
     * Convert the proven PMEM offset back to a physical address for paddr_write:
     *   RDI = offset + CONFIG_MBASE, ESI = byte width, RDX = store data.
     * The continuation helper returns zero when the write may have invalidated
     * translated source bytes.  That path exits after the store; otherwise the
     * native block reloads its base registers and keeps compiling after the
     * safe data store.
     */
    if (!emit_mov_rdi_rdx(w) ||
        !emit_movabs_rcx(w, (uint64_t)CONFIG_MBASE) ||
        !emit_add_rdi_rcx(w) ||
        !jit_reg_flush_all_dirty(w, regs) ||
        !jit_reg_read_rdx(w, regs, rs2) ||
        !emit_mov_esi_imm32(w, len) ||
        !emit_store_pc_imm(w, pc) ||
        !emit_call_abs(w, (uintptr_t)jit_store_pmem_continue) ||
        !emit_load_cpu_base(w) ||
        !emit_movabs_r10(w, (uint64_t)(uintptr_t)guest_to_host(CONFIG_MBASE)) ||
        !emit_test_eax_eax(w) ||
        /* 0x84 is x86 JE/JZ rel32: helper returned zero, so exit. */
        !emit_jcc_rel32_placeholder(w, 0x84, &exit_disp) ||
        !emit_jmp_rel32_placeholder(w, &continue_disp))
    {
        return false;
    }

    patch_rel32(exit_disp, w->cur);

    if (!emit_load_cpu_base(w) ||
        !emit_store_pc_imm(w, next_pc) ||
        !(loop_count_needed ? emit_return_loop_count(w, completed_count + 1u)
                             : emit_return_count(w, completed_count + 1u)))
    {
        return false;
    }

    if (align_slow_disp != NULL)
    {
        patch_rel32(align_slow_disp, w->cur);
    }
    patch_rel32(range_slow_disp, w->cur);

    if (!emit_interpreter_side_exit(w, regs, pc, completed_count, loop_count_needed))
    {
        return false;
    }

    patch_rel32(continue_disp, w->cur);

    JIT_STAT_INC(native_stores);
    JIT_STAT_INC(native_store_continuations);
    return true;
}

/* Emit a helper-backed RV64M operation and keep compiling after the call. */
static bool emit_m_helper(rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs,
                          uint32_t instr,
                          uint32_t rd, uint32_t rs1, uint32_t rs2)
{
    /*
     * System V arguments are RDI, RSI, RDX. The helper returns the result in
     * RAX. Because a C call may clobber caller-saved R10/R11, reload both JIT
     * base registers before storing the result or emitting later PMEM accesses.
     */
    if (!jit_reg_read_rax(w, regs, rs1) ||
        !emit_mov_rdi_rax(w) ||
        !jit_reg_read_rdx(w, regs, rs2) ||
        !emit_mov_rsi_rdx(w) ||
        !emit_mov_edx_imm32(w, instr) ||
        !emit_call_abs(w, (uintptr_t)jit_m_result) ||
        !emit_load_cpu_base(w) ||
        !emit_movabs_r10(w, (uint64_t)(uintptr_t)guest_to_host(CONFIG_MBASE)) ||
        !jit_reg_write_rax(w, regs, rd))
    {
        return false;
    }

    JIT_STAT_INC(native_m_ops);
    return true;
}

/* Emit a 64-bit RISC-V OP-IMM instruction into native code. */
static bool emit_op_imm(rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs,
                        uint32_t instr)
{
    const uint32_t rd = bits(instr, 11, 7);
    const uint32_t funct3 = bits(instr, 14, 12);
    const uint32_t rs1 = bits(instr, 19, 15);
    const int32_t imm = (int32_t)imm_i(instr);

    if (funct3 == 0x0 && imm == 0)
    {
        return jit_reg_copy(w, regs, rd, rs1);
    }

    if (!jit_reg_read_rax(w, regs, rs1))
    {
        return false;
    }

    switch (funct3)
    {
    case 0x0: /* ADDI */
        return emit_add_rax_imm32(w, imm) && jit_reg_write_rax(w, regs, rd);
    case 0x2: /* SLTI, signed compare; SETL is opcode 0x9c. */
        return emit_cmp_rax_imm32(w, imm) && emit_setcc_rax(w, 0x9c) && jit_reg_write_rax(w, regs, rd);
    case 0x3: /* SLTIU, unsigned compare; SETB is opcode 0x92. */
        return emit_cmp_rax_imm32(w, imm) && emit_setcc_rax(w, 0x92) && jit_reg_write_rax(w, regs, rd);
    case 0x4: /* XORI; `48 35 imm32` is XOR RAX, sign-extended imm32. */
        return emit_u8(w, 0x48) && emit_u8(w, 0x35) && emit_u32(w, (uint32_t)imm) && jit_reg_write_rax(w, regs, rd);
    case 0x6: /* ORI; `48 0d imm32` is OR RAX, sign-extended imm32. */
        return emit_u8(w, 0x48) && emit_u8(w, 0x0d) && emit_u32(w, (uint32_t)imm) && jit_reg_write_rax(w, regs, rd);
    case 0x7: /* ANDI; `48 25 imm32` is AND RAX, sign-extended imm32. */
        return emit_u8(w, 0x48) && emit_u8(w, 0x25) && emit_u32(w, (uint32_t)imm) && jit_reg_write_rax(w, regs, rd);
    case 0x1: /* SLLI; funct6 must be 000000 for RV64 base shifts. */
        if (bits(instr, 31, 26) != 0x00)
        {
            return false;
        }
        return emit_shift_rax_imm(w, 0xe0, (uint8_t)bits(instr, 25, 20)) && jit_reg_write_rax(w, regs, rd);
    case 0x5: /* SRLI/SRAI; funct6 selects logical versus arithmetic right shift. */
        if (bits(instr, 31, 26) == 0x00)
        {
            return emit_shift_rax_imm(w, 0xe8, (uint8_t)bits(instr, 25, 20)) && jit_reg_write_rax(w, regs, rd);
        }

        if (bits(instr, 31, 26) == 0x10)
        {
            return emit_shift_rax_imm(w, 0xf8, (uint8_t)bits(instr, 25, 20)) && jit_reg_write_rax(w, regs, rd);
        }
        return false;
    default:
        return false;
    }
}

/* Emit an RV64 OP-IMM-32 instruction and sign-extend the 32-bit result. */
static bool emit_op_imm32(rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs,
                          uint32_t instr)
{
    const uint32_t rd = bits(instr, 11, 7);
    const uint32_t funct3 = bits(instr, 14, 12);
    const uint32_t rs1 = bits(instr, 19, 15);
    const int32_t imm = (int32_t)imm_i(instr);

    if (!jit_reg_read_rax(w, regs, rs1))
    {
        return false;
    }

    switch (funct3)
    {
    case 0x0: /* ADDIW; EAX addition naturally drops to 32 bits, then CDQE. */
        return emit_u8(w, 0x05) && emit_u32(w, (uint32_t)imm) &&
               emit_u8(w, 0x48) && emit_u8(w, 0x98) &&
               jit_reg_write_rax(w, regs, rd);
    case 0x1: /* SLLIW; funct7 must be zero and shamt is five bits. */
        if (bits(instr, 31, 25) != 0x00)
        {
            return false;
        }
        return emit_shift_eax_imm_sext(w, 0xe0, (uint8_t)bits(instr, 24, 20)) && jit_reg_write_rax(w, regs, rd);
    case 0x5: /* SRLIW/SRAIW; funct7 distinguishes logical from arithmetic. */
        if (bits(instr, 31, 25) == 0x00)
        {
            return emit_shift_eax_imm_sext(w, 0xe8, (uint8_t)bits(instr, 24, 20)) && jit_reg_write_rax(w, regs, rd);
        }

        if (bits(instr, 31, 25) == 0x20)
        {
            return emit_shift_eax_imm_sext(w, 0xf8, (uint8_t)bits(instr, 24, 20)) && jit_reg_write_rax(w, regs, rd);
        }
        return false;
    default:
        return false;
    }
}

/* Emit a 64-bit RV64 OP instruction for the integer ALU subset. */
static bool emit_op(rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs,
                    uint32_t instr)
{
    const uint32_t rd = bits(instr, 11, 7);
    const uint32_t funct3 = bits(instr, 14, 12);
    const uint32_t rs1 = bits(instr, 19, 15);
    const uint32_t rs2 = bits(instr, 24, 20);
    const uint32_t key = (bits(instr, 31, 25) << 3) | funct3;

    if (!jit_reg_read_rax(w, regs, rs1) || !jit_reg_read_rcx(w, regs, rs2))
    {
        return false;
    }

    switch (key)
    {
    case 0x000: /* ADD */
        return emit_rax_rcx_alu64(w, 0x01) && jit_reg_write_rax(w, regs, rd);
    case 0x100: /* SUB */
        return emit_rax_rcx_alu64(w, 0x29) && jit_reg_write_rax(w, regs, rd);
    case 0x001: /* SLL */
        return emit_shift_rax_cl(w, 0xe0) && jit_reg_write_rax(w, regs, rd);
    case 0x002: /* SLT, signed compare; SETL is opcode 0x9c. */
        return emit_cmp_rax_rcx(w) && emit_setcc_rax(w, 0x9c) && jit_reg_write_rax(w, regs, rd);
    case 0x003: /* SLTU, unsigned compare; SETB is opcode 0x92. */
        return emit_cmp_rax_rcx(w) && emit_setcc_rax(w, 0x92) && jit_reg_write_rax(w, regs, rd);
    case 0x004: /* XOR */
        return emit_rax_rcx_alu64(w, 0x31) && jit_reg_write_rax(w, regs, rd);
    case 0x005: /* SRL */
        return emit_shift_rax_cl(w, 0xe8) && jit_reg_write_rax(w, regs, rd);
    case 0x105: /* SRA */
        return emit_shift_rax_cl(w, 0xf8) && jit_reg_write_rax(w, regs, rd);
    case 0x006: /* OR */
        return emit_rax_rcx_alu64(w, 0x09) && jit_reg_write_rax(w, regs, rd);
    case 0x007: /* AND */
        return emit_rax_rcx_alu64(w, 0x21) && jit_reg_write_rax(w, regs, rd);
    case 0x008: /* MUL; low 64 bits match x86-64 IMUL RAX, RCX. */
        JIT_STAT_INC(native_m_ops);
        return emit_u8(w, 0x48) && emit_u8(w, 0x0f) && emit_u8(w, 0xaf) && emit_u8(w, 0xc1) &&
               jit_reg_write_rax(w, regs, rd);
    case 0x009: /* MULH */
    case 0x00a: /* MULHSU */
    case 0x00b: /* MULHU */
    case 0x00c: /* DIV */
    case 0x00d: /* DIVU */
    case 0x00e: /* REM */
    case 0x00f: /* REMU */
        return emit_m_helper(w, regs, instr, rd, rs1, rs2);
    default:
        return false;
    }
}

/* Emit an RV64 OP-32 instruction and sign-extend the 32-bit result. */
static bool emit_op32(rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs,
                      uint32_t instr)
{
    const uint32_t rd = bits(instr, 11, 7);
    const uint32_t funct3 = bits(instr, 14, 12);
    const uint32_t rs1 = bits(instr, 19, 15);
    const uint32_t rs2 = bits(instr, 24, 20);
    const uint32_t key = (bits(instr, 31, 25) << 3) | funct3;

    if (!jit_reg_read_rax(w, regs, rs1) || !jit_reg_read_rcx(w, regs, rs2))
    {
        return false;
    }

    switch (key)
    {
    case 0x000: /* ADDW */
        return emit_eax_ecx_alu32_sext(w, 0x01) && jit_reg_write_rax(w, regs, rd);
    case 0x100: /* SUBW */
        return emit_eax_ecx_alu32_sext(w, 0x29) && jit_reg_write_rax(w, regs, rd);
    case 0x001: /* SLLW */
        return emit_shift_eax_cl_sext(w, 0xe0) && jit_reg_write_rax(w, regs, rd);
    case 0x005: /* SRLW */
        return emit_shift_eax_cl_sext(w, 0xe8) && jit_reg_write_rax(w, regs, rd);
    case 0x105: /* SRAW */
        return emit_shift_eax_cl_sext(w, 0xf8) && jit_reg_write_rax(w, regs, rd);
    case 0x008: /* MULW; IMUL low 32 bits, then CDQE sign-extension. */
        JIT_STAT_INC(native_m_ops);
        return emit_u8(w, 0x0f) && emit_u8(w, 0xaf) && emit_u8(w, 0xc1) &&
               emit_u8(w, 0x48) && emit_u8(w, 0x98) &&
               jit_reg_write_rax(w, regs, rd);
    case 0x00c: /* DIVW */
    case 0x00d: /* DIVUW */
    case 0x00e: /* REMW */
    case 0x00f: /* REMUW */
        return emit_m_helper(w, regs, instr, rd, rs1, rs2);
    default:
        return false;
    }
}

/* Emit the taken side of a branch that can jump back to the native loop head. */
static bool emit_branch_chain_backedge(rv64_jit_writer_t *w,
                                       rv64_jit_reg_cache_t *regs, vaddr_t target,
                                       uint32_t exit_count,
                                       const uint8_t *target_native)
{
    uint8_t *over_budget_disp = NULL;
    uint8_t *loop_disp = NULL;

    /*
     * The current lap has completed `exit_count` guest instructions at the
     * branch.  EAX becomes the total completed count including this lap.  ECX
     * then looks one more full lap ahead; only if that still fits
     * `jit_entry_budget` do we store EAX in `jit_loop_extra` and jump back to
     * the native loop body.  Otherwise, returning EAX keeps cpu_exec() budget
     * accounting exact.
     */
    if (!emit_movabs_rdx(w, (uint64_t)(uintptr_t)&jit_loop_extra) ||
        !emit_mov_eax_m32_rdx(w) ||
        !emit_add_eax_imm32(w, exit_count) ||
        !emit_mov_ecx_eax(w) ||
        !emit_add_ecx_imm32(w, exit_count) ||
        !emit_movabs_rdx(w, (uint64_t)(uintptr_t)&jit_entry_budget) ||
        !emit_cmp_ecx_m32_rdx(w) ||
        !emit_jcc_rel32_placeholder(w, 0x87, &over_budget_disp) || /* JA: unsigned proposed count > budget. */
        !jit_reg_emit_flush_all_dirty(w, regs) ||
        !emit_movabs_rdx(w, (uint64_t)(uintptr_t)&jit_loop_extra) ||
        !emit_mov_m32_rdx_eax(w) ||
        !emit_jmp_rel32_placeholder(w, &loop_disp))
    {
        return false;
    }

    patch_rel32(loop_disp, target_native);
    patch_rel32(over_budget_disp, w->cur);
    /*
     * EAX contains the completed count, but emit_store_pc_imm() uses RAX as its
     * immediate scratch register. Preserve the count in ECX across the PC store
     * and restore EAX before the native function returns.
     */
    return emit_mov_ecx_eax(w) &&
           jit_reg_emit_flush_all_dirty(w, regs) &&
           emit_store_pc_imm(w, target) &&
           emit_mov_eax_ecx(w) &&
           emit_return_eax(w);
}

/* Emit one conditional branch with a taken side exit and fall-through fast path. */
static bool emit_branch(rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs,
                        uint32_t instr, vaddr_t pc,
                        vaddr_t block_start_pc, const uint8_t *block_start_native,
                        bool loop_count_needed, bool chain_safe,
                        bool *branch_chained, uint32_t exit_count)
{
    const uint32_t funct3 = bits(instr, 14, 12);
    const uint32_t rs1 = bits(instr, 19, 15);
    const uint32_t rs2 = bits(instr, 24, 20);
    const vaddr_t target = pc + imm_b(instr);
    uint8_t inverse_jcc = 0;
    uint8_t *fallthrough_disp = NULL;

    if ((target & RV64_BRANCH_ALIGN_MASK) != 0)
    {
        return false;
    }

    switch (funct3)
    {
    case 0x0: /* BEQ: inverse JNE falls through when not equal. */
        inverse_jcc = 0x85;
        break;
    case 0x1: /* BNE: inverse JE falls through when equal. */
        inverse_jcc = 0x84;
        break;
    case 0x4: /* BLT: inverse JGE falls through for signed greater/equal. */
        inverse_jcc = 0x8d;
        break;
    case 0x5: /* BGE: inverse JL falls through for signed less-than. */
        inverse_jcc = 0x8c;
        break;
    case 0x6: /* BLTU: inverse JAE falls through for unsigned above/equal. */
        inverse_jcc = 0x83;
        break;
    case 0x7: /* BGEU: inverse JB falls through for unsigned below. */
        inverse_jcc = 0x82;
        break;
    default:
        return false;
    }

    if (!jit_reg_read_rax(w, regs, rs1) ||
        !jit_reg_read_rcx(w, regs, rs2) ||
        !emit_cmp_rax_rcx(w) ||
        !emit_jcc_rel32_placeholder(w, inverse_jcc, &fallthrough_disp))
    {
        return false;
    }

    if (chain_safe && target == block_start_pc)
    {
        if (!emit_branch_chain_backedge(w, regs, target, exit_count, block_start_native))
        {
            return false;
        }
        *branch_chained = true;
    }
    else if (!jit_reg_emit_flush_all_dirty(w, regs) ||
             !emit_store_pc_imm(w, target) ||
             !(loop_count_needed ? emit_return_loop_count(w, exit_count)
                                  : emit_return_count(w, exit_count)))
    {
        return false;
    }

    patch_rel32(fallthrough_disp, w->cur);
    return true;
}

/* Emit JAL or JALR, both of which end the current native block. */
static bool emit_jump_instr(rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs,
                            uint32_t instr, vaddr_t pc,
                            uint32_t completed_count, bool loop_count_needed)
{
    const uint32_t opcode = instr & RV64_OPCODE_MASK;
    const uint32_t rd = bits(instr, 11, 7);
    const vaddr_t link = pc + RV64_INSN_SIZE;
    uint8_t *misaligned_disp = NULL;
    rv64_jit_reg_cache_t side_exit_regs;

    if (opcode == RV64_OPCODE_JAL)
    {
        const vaddr_t target = pc + imm_j(instr);

        if ((target & RV64_BRANCH_ALIGN_MASK) != 0)
        {
            return false;
        }

        JIT_STAT_INC(native_jumps);
        return emit_movabs_rax(w, link) &&
               jit_reg_write_rax(w, regs, rd) &&
               jit_reg_flush_all_dirty(w, regs) &&
               emit_store_pc_imm(w, target) &&
               (loop_count_needed ? emit_return_loop_count(w, completed_count + 1u)
                                  : emit_return_count(w, completed_count + 1u));
    }

    if (opcode != RV64_OPCODE_JALR || bits(instr, 14, 12) != 0)
    {
        return false;
    }

    /*
     * JALR computes `(rs1 + imm) & ~1`, then checks instruction alignment after
     * clearing bit zero.  The misaligned case returns before JALR executes so
     * the interpreter raises the same trap and does not write the link register.
     */
    if (!jit_reg_read_rax(w, regs, bits(instr, 19, 15)) ||
        !emit_add_rax_imm32(w, (int32_t)imm_i(instr)) ||
        !emit_and_rax_imm32(w, -2) ||
        !emit_test_al_imm8(w, RV64_BRANCH_ALIGN_MASK) ||
        /* 0x85 is x86 JNE/JNZ rel32: target misalignment falls back. */
        !emit_jcc_rel32_placeholder(w, 0x85, &misaligned_disp))
    {
        return false;
    }

    side_exit_regs = *regs;

    if (!emit_mov_rcx_rax(w) ||
        !emit_movabs_rax(w, link) ||
        !jit_reg_write_rax(w, regs, rd) ||
        !jit_reg_flush_all_dirty(w, regs) ||
        !emit_mov_rax_rcx(w) ||
        !emit_store_rax_pc(w) ||
        !(loop_count_needed ? emit_return_loop_count(w, completed_count + 1u)
                             : emit_return_count(w, completed_count + 1u)))
    {
        return false;
    }

    patch_rel32(misaligned_disp, w->cur);
    if (!emit_interpreter_side_exit(w, &side_exit_regs, pc, completed_count, loop_count_needed))
    {
        return false;
    }

    JIT_STAT_INC(native_jumps);
    return true;
}

/* Dispatch one supported non-branch RISC-V instruction to the native emitter. */
static bool emit_instr(rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs,
                       uint32_t instr, vaddr_t pc,
                       uint32_t exit_count)
{
    const uint32_t opcode = instr & RV64_OPCODE_MASK;
    const uint32_t rd = bits(instr, 11, 7);

    switch (opcode)
    {
    case RV64_OPCODE_OP_IMM: /* ADDI/SLTI/SLTIU/XORI/ORI/ANDI/SLLI/SRLI/SRAI. */
        return emit_op_imm(w, regs, instr);
    case RV64_OPCODE_OP_IMM_32: /* ADDIW/SLLIW/SRLIW/SRAIW. */
        return emit_op_imm32(w, regs, instr);
    case RV64_OPCODE_OP: /* 64-bit register-register integer ALU subset. */
        return emit_op(w, regs, instr);
    case RV64_OPCODE_OP_32: /* W-form register-register integer ALU subset. */
        return emit_op32(w, regs, instr);
    case RV64_OPCODE_LUI: /* LUI materialises the sign-extended U immediate. */
        return jit_reg_write_imm(w, regs, rd, (uint64_t)imm_u_sext(instr));
    case RV64_OPCODE_AUIPC: /* AUIPC adds the sign-extended U immediate to PC. */
        return jit_reg_write_imm(w, regs, rd, (uint64_t)(pc + imm_u_sext(instr)));
    default:
        (void)exit_count;
        return false;
    }
}

/* Translate an instruction-fetch virtual PC and report whether paging was used. */
static bool jit_translate_ifetch_ex(vaddr_t pc, paddr_t *paddr, bool *translated)
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
        const paddr_t ret = isa_mmu_translate(pc, RV64_INSN_SIZE, MEM_TYPE_IFETCH);

        /*
         * `isa_mmu_translate()` stores the status code in the low page-offset
         * bits and the 4 KiB physical page base in the upper bits.  The JIT only
         * accepts the exact success code; page faults and cross-page compressed
         * cases stay on the interpreter path where the normal trap behaviour is
         * centralised.
         */
        if ((ret & (paddr_t)PAGE_MASK) == MEM_RET_OK)
        {
            *paddr = (ret & ~(paddr_t)PAGE_MASK) | (paddr_t)(pc & PAGE_MASK);
            *translated = true;
            return true;
        }
    }

    return false;
}

/* Translate an instruction-fetch virtual PC to its physical source address. */
static bool jit_translate_ifetch(vaddr_t pc, paddr_t *paddr)
{
    bool translated = false;
    return jit_translate_ifetch_ex(pc, paddr, &translated);
}

/* Check whether a cache slot still describes the current PC and source bytes. */
static bool jit_block_matches(const rv64_jit_block_t *block, vaddr_t pc)
{
    if (!block->valid ||
        block->pc != pc ||
        block->satp != cpu.csr.satp ||
        block->data_state != jit_data_tlb_state(MEM_TYPE_READ))
    {
        return false;
    }

    /*
     * Sv39 page tables can remap any virtual instruction inside a cached block
     * without changing satp.  Re-walking every compiled instruction is slower
     * than RV32's page-table dependency refs, but it is strict: stale native
     * code is rejected before entry even when only a later instruction's page
     * changes.
     */
    for (uint32_t off = 0; off < block->source_len; off += RV64_INSN_SIZE)
    {
        paddr_t now = 0;
        bool translated = false;

        if (!jit_translate_ifetch_ex(pc + off, &now, &translated) ||
            translated != block->translated ||
            now != block->paddr_start + (paddr_t)off)
        {
            return false;
        }
    }

    return true;
}

/* Publish a negative cache entry for a currently unsupported instruction. */
static void jit_mark_unsupported(vaddr_t pc, paddr_t paddr, bool translated)
{
    JIT_STAT_INC(blocks_unsupported);

    rv64_jit_block_t *block = jit_cache_slot(pc);
    jit_block_discard(block);
    *block = (rv64_jit_block_t){
        .valid = true,
        .translated = translated,
        .pc = pc,
        .satp = cpu.csr.satp,
        .data_state = jit_data_tlb_state(MEM_TYPE_READ),
        .paddr_start = paddr,
        .source_len = RV64_INSN_SIZE,
        .insn_count = 0,
        .entry = NULL,
    };
    /*
     * Negative cache entries need source refs too.  If self-modifying code
     * rewrites an unsupported instruction into a supported one, exact
     * invalidation must remove this marker so the JIT can compile the new bytes.
     */
    jit_source_chunks_ref(paddr, RV64_INSN_SIZE);
}

/* Return true for opcodes that can appear inside a native chained loop body. */
static bool jit_instr_can_chain_body(uint32_t instr)
{
    const uint32_t opcode = instr & RV64_OPCODE_MASK;

    switch (opcode)
    {
    case RV64_OPCODE_LOAD:
    case RV64_OPCODE_STORE:
    case RV64_OPCODE_OP_IMM:
    case RV64_OPCODE_OP_IMM_32:
    case RV64_OPCODE_OP:
    case RV64_OPCODE_OP_32:
    case RV64_OPCODE_AUIPC:
    case RV64_OPCODE_LUI:
    case RV64_OPCODE_BRANCH:
        return true;
    default:
        return false;
    }
}

/* Cheaply pre-scan whether this block has a branch back to its own start. */
static bool jit_block_has_chainable_backedge(vaddr_t pc, uint32_t max_insns,
                                             paddr_t first_paddr)
{
    vaddr_t cur_pc = pc;
    uint32_t count = 0;
    uint32_t source_len = 0;

    while (count < max_insns && count < RV64_JIT_BLOCK_MAX_INSNS)
    {
        paddr_t cur_paddr = 0;

        if (!jit_translate_ifetch(cur_pc, &cur_paddr) || !in_pmem(cur_paddr) ||
            cur_paddr != first_paddr + (paddr_t)source_len)
        {
            return false;
        }

        const uint32_t instr = (uint32_t)vaddr_ifetch(cur_pc, RV64_INSN_SIZE);
        const uint32_t opcode = instr & RV64_OPCODE_MASK;

        if (!jit_instr_can_chain_body(instr))
        {
            return false;
        }

        if (opcode == RV64_OPCODE_BRANCH && cur_pc + imm_b(instr) == pc)
        {
            return true;
        }

        cur_pc += RV64_INSN_SIZE;
        source_len += RV64_INSN_SIZE;
        count++;
    }

    return false;
}

/* Compile one straight-line block starting at the current guest PC. */
static rv64_jit_block_t *jit_compile_block(vaddr_t pc, uint32_t max_insns)
{
    if (!jit_code_init() || max_insns == 0)
    {
        return NULL;
    }

    if (jit_code_used + RV64_JIT_BLOCK_CODE_HEADROOM > RV64_JIT_CODE_SIZE)
    {
        jit_arena_reset();
    }

    jit_code_used = jit_align_up(jit_code_used, RV64_JIT_CODE_ALIGN);

    paddr_t first_paddr = 0;
    bool first_translated = false;
    if (!jit_translate_ifetch_ex(pc, &first_paddr, &first_translated) ||
        !in_pmem(first_paddr))
    {
        return NULL;
    }

    rv64_jit_writer_t w = {
        .start = jit_code + jit_code_used,
        .cur = jit_code + jit_code_used,
        .end = jit_code + RV64_JIT_CODE_SIZE,
    };
    rv64_jit_reg_cache_t regs;
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
    bool chain_safe = loop_count_needed;
    bool chained_loop = false;

    while (count < max_insns && count < RV64_JIT_BLOCK_MAX_INSNS)
    {
        paddr_t cur_paddr = 0;
        bool cur_translated = false;

        if (!jit_translate_ifetch_ex(cur_pc, &cur_paddr, &cur_translated) ||
            !in_pmem(cur_paddr) ||
            cur_translated != first_translated)
        {
            break;
        }

        if (cur_paddr != first_paddr + (paddr_t)source_len)
        {
            break;
        }

        const uint32_t instr = (uint32_t)vaddr_ifetch(cur_pc, RV64_INSN_SIZE);
        const uint32_t opcode = instr & RV64_OPCODE_MASK;
        uint8_t *instr_start = w.cur;
        rv64_jit_reg_cache_t regs_start = regs;
        bool end_block = false;

        if (opcode == RV64_OPCODE_JAL ||
            opcode == RV64_OPCODE_JALR)
        {
            if (!emit_jump_instr(&w, &regs, instr, cur_pc, count, loop_count_needed))
            {
                w.cur = instr_start;
                jit_reg_cache_restore(&regs, &regs_start);
                break;
            }
            end_block = true;
        }
        else if (opcode == RV64_OPCODE_LOAD)
        {
            /*
             * A guarded load may side-exit with zero completed instructions
             * when it is the first block instruction and the runtime address is
             * unsafe.  The dispatcher treats that as a miss-like fallback and
             * lets the interpreter execute the load.
             */
            if (!emit_load_instr(&w, &regs, instr, cur_pc, count, loop_count_needed))
            {
                w.cur = instr_start;
                jit_reg_cache_restore(&regs, &regs_start);
                break;
            }
        }
        else if (opcode == RV64_OPCODE_STORE)
        {
            /*
             * Safe PMEM data stores can continue in the native block.  Stores
             * that may fault, hit MMIO, or touch source bytes side-exit before
             * or immediately after the store so interpreter-visible ordering is
             * preserved.
             */
            if (!emit_store_instr(&w, &regs, instr, cur_pc, cur_pc + RV64_INSN_SIZE,
                                  count, loop_count_needed))
            {
                w.cur = instr_start;
                jit_reg_cache_restore(&regs, &regs_start);
                break;
            }
        }
        else if (opcode == RV64_OPCODE_BRANCH)
        {
            bool branch_chained = false;

            if (!emit_branch(&w, &regs, instr, cur_pc, pc, block_start_native,
                             loop_count_needed, chain_safe, &branch_chained,
                             count + 1u))
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
        else if (!emit_instr(&w, &regs, instr, cur_pc, count + 1u))
        {
            w.cur = instr_start;
            jit_reg_cache_restore(&regs, &regs_start);
            break;
        }

        cur_pc += RV64_INSN_SIZE;
        source_len += RV64_INSN_SIZE;
        count++;

        /*
         * A chained back-edge is both a taken-loop fast path and the natural end
         * of this native block.  Its fall-through path returns below with
         * `jit_loop_extra + count`, while taken laps jump back to
         * `block_start_native` without re-running the prologue.
         */
        if (end_block)
        {
            break;
        }
    }

    if (count == 0)
    {
        jit_mark_unsupported(pc, first_paddr, first_translated);
        return NULL;
    }

    if (!jit_reg_flush_all_dirty(&w, &regs) ||
        !emit_store_pc_imm(&w, cur_pc) ||
        !(chained_loop ? emit_return_loop_count(&w, count)
                       : emit_return_count(&w, count)))
    {
        return NULL;
    }

    __builtin___clear_cache((char *)w.start, (char *)w.cur);

    rv64_jit_block_t *block = jit_cache_slot(pc);
    jit_block_discard(block);
    jit_source_chunks_ref(first_paddr, source_len);
    *block = (rv64_jit_block_t){
        .valid = true,
        .translated = first_translated,
        .pc = pc,
        .satp = cpu.csr.satp,
        .data_state = jit_data_tlb_state(MEM_TYPE_READ),
        .paddr_start = first_paddr,
        .source_len = source_len,
        .insn_count = count,
        .entry = (rv64_jit_entry_t)w.start,
    };

    jit_code_used = (size_t)(w.cur - jit_code);
    JIT_STAT_INC(blocks_compiled);
    if (first_translated)
    {
        JIT_STAT_INC(translated_blocks);
    }
    JIT_STAT_ADD(compiled_insns, count);
    return block;
}

/* Report whether native RV64 JIT execution can be attempted in this run. */
bool isa_jit_available(void)
{
    return RV64_JIT_ENABLED && !jit_runtime_disabled();
}

/* Drop all cached native blocks and reset private RV64 JIT state. */
void isa_jit_flush_all(void)
{
    if (jit_code != NULL)
    {
        jit_arena_reset();
    }
    jit_data_tlb_flush();
}

/* Flush only the JIT's local data translations after SFENCE.VMA. */
void isa_jit_flush_data_tlb(void)
{
    jit_data_tlb_flush();
}

/* Invalidate native blocks whose physical source bytes overlap a PMEM write. */
void isa_jit_invalidate_paddr(paddr_t addr, int len)
{
    JIT_STAT_INC(invalidation_requests);

    if (len <= 0 || jit_code == NULL)
    {
        return;
    }

    if (jit_write_may_touch_data_tlb_page_table(addr, len))
    {
        JIT_STAT_INC(data_tlb_page_table_flushes);
        jit_data_tlb_flush();
    }

    if (!jit_write_may_touch_source_chunk(addr, len))
    {
        return;
    }

    for (size_t i = 0; i < RV64_JIT_CACHE_SIZE; i++)
    {
        rv64_jit_block_t *block = &jit_cache[i];

        if (!block->valid)
        {
            continue;
        }

        if (jit_ranges_overlap(block->paddr_start, block->source_len, addr, len))
        {
            jit_block_discard(block);
            JIT_STAT_INC(invalidated_blocks);
        }
    }
}

/* Execute cached or newly compiled native RV64 blocks within the given budgets. */
bool isa_jit_exec(uint64_t remaining, uint32_t device_budget, uint32_t *executed)
{
    *executed = 0;

    if (remaining == 0 || device_budget == 0 || !isa_jit_available())
    {
        return false;
    }

    JIT_STAT_INC(exec_requests);

    uint32_t batch_budget = remaining > RV64_JIT_BATCH_MAX_INSNS
                                ? RV64_JIT_BATCH_MAX_INSNS
                                : (uint32_t)remaining;

    if (batch_budget > device_budget)
    {
        batch_budget = device_budget;
    }

    uint32_t total = 0;

    while (total < batch_budget)
    {
        uint32_t remaining_budget = batch_budget - total;
        uint32_t block_budget = remaining_budget;

        if (block_budget > RV64_JIT_BLOCK_MAX_INSNS)
        {
            block_budget = RV64_JIT_BLOCK_MAX_INSNS;
        }

        rv64_jit_block_t *block = jit_cache_slot(cpu.pc);

        if (jit_block_matches(block, cpu.pc))
        {
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

        /*
         * Chained loops use these two globals as a tiny ABI between cpu_exec()
         * and generated code. `jit_entry_budget` is the maximum work this entry
         * may retire; `jit_loop_extra` starts at zero and accumulates completed
         * native loop laps before the final block exit returns the total.
         */
        jit_entry_budget = remaining_budget;
        jit_loop_extra = 0;
        const uint32_t ran = block->entry();
        if (ran == 0)
        {
            JIT_STAT_INC(zero_side_exits);
            break;
        }

        Assert(ran <= remaining_budget,
               "jit: invalid RV64 executed count %u", ran);
        JIT_STAT_INC(blocks_executed);
        JIT_STAT_ADD(executed_insns, ran);
        total += ran;
    }

    *executed = total;
    return total > 0;
}

#if RV64_JIT_STATS
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

/* Print optional RV64 JIT counters at the end of execution. */
void isa_jit_dump_stats(void)
{
    jit_init_runtime_options();

    if (jit_runtime_disabled())
    {
        Log("jit: disabled by NEMU_DISABLE_JIT=1");
        return;
    }

#if RV64_JIT_STATS
    if (!jit_stats_enabled || !RV64_JIT_ENABLED)
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
    Log("jit: invalidation requests = %" PRIu64
        ", invalidated blocks = %" PRIu64
        ", arena resets = %" PRIu64,
        jit_stats.invalidation_requests,
        jit_stats.invalidated_blocks,
        jit_stats.arena_resets);
    Log("jit: native loads = %" PRIu64,
        jit_stats.native_loads);
    Log("jit: native stores = %" PRIu64,
        jit_stats.native_stores);
    Log("jit: native jumps = %" PRIu64,
        jit_stats.native_jumps);
    Log("jit: native M ops = %" PRIu64,
        jit_stats.native_m_ops);
    Log("jit: translated blocks = %" PRIu64,
        jit_stats.translated_blocks);
    Log("jit: reg cache spills = %" PRIu64,
        jit_stats.reg_cache_spills);
    Log("jit: native store continuations = %" PRIu64,
        jit_stats.native_store_continuations);
    Log("jit: native paged loads = %" PRIu64,
        jit_stats.native_paged_loads);
    Log("jit: native paged stores = %" PRIu64,
        jit_stats.native_paged_stores);
    Log("jit: data TLB hits = %" PRIu64
        ", misses = %" PRIu64,
        jit_stats.data_tlb_hits,
        jit_stats.data_tlb_misses);
    Log("jit: data TLB fills = %" PRIu64,
        jit_stats.data_tlb_fills);
    Log("jit: data TLB flushes = %" PRIu64,
        jit_stats.data_tlb_flushes);
    Log("jit: data TLB page-table flushes = %" PRIu64,
        jit_stats.data_tlb_page_table_flushes);
    Log("jit: data TLB direct loads = %" PRIu64
        ", direct stores = %" PRIu64,
        jit_stats.data_tlb_direct_loads,
        jit_stats.data_tlb_direct_stores);
    Log("jit: inline paged loads = %" PRIu64
        ", inline paged stores = %" PRIu64,
        jit_stats.inline_paged_loads,
        jit_stats.inline_paged_stores);
    Log("jit: inline paged load hits = %" PRIu64
        ", inline paged store hits = %" PRIu64,
        jit_stats.inline_paged_load_hits,
        jit_stats.inline_paged_store_hits);
    Log("jit: zero side exits = %" PRIu64,
        jit_stats.zero_side_exits);
#else
    if (jit_stats_enabled)
    {
        Log("jit: stats requested, but this binary was built without RV64_JIT_STATS=1");
    }
#endif
}
