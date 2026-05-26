#include <generated/autoconf.h>

/*
 * RISC-V32 and RISC-V64 share this directory through the riscv64 -> riscv32
 * symlink. Keep this single compiled entry point, and include the XLEN-specific
 * implementation from files that are not discovered as standalone C sources.
 */
#ifdef CONFIG_RV64

#include <isa-jit.h>
#include <isa.h>
#include <memory/host.h>
#include <memory/paddr.h>
#include <memory/vaddr.h>
#include <utils.h>

#include <inttypes.h>
#include <stddef.h>
#include <stdlib.h>

/*
 * RISC-V64 x86-64 JIT design.
 *
 * This file accelerates a conservative subset of the RV64 direct interpreter.
 * The interpreter in `inst.c` remains the architectural reference: every case
 * that is hard to prove safe in native code falls back before the instruction
 * can partially commit.  The JIT is therefore an optional fast path, not a
 * second specification of the machine.
 *
 * Control flow through the JIT has five stages:
 *
 *   1. Availability and runtime gates
 *      `isa_jit_available()` checks compile-time options, host architecture,
 *      executable arena allocation, and environment flags.  Tracing,
 *      watchpoints, memory/function tracing, and DiffTest disable this path
 *      because they require per-instruction interpreter hooks.
 *
 *   2. Block lookup and context matching
 *      `isa_jit_exec()` hashes the guest PC, `satp`, and fetch privilege into a
 *      direct-mapped block cache.  `jit_block_matches()` revalidates the cached
 *      block against the current instruction-fetch translation state, source
 *      bytes, and page-table dependencies before entering native code.
 *
 *   3. Native block compilation
 *      `jit_compile_block()` walks up to a bounded number of 32-bit RV64
 *      instructions, records the physical source bytes behind those virtual
 *      fetches, and emits x86-64 into a single executable arena.  Unsupported
 *      instructions stop compilation.  If at least one instruction was emitted,
 *      the native block returns an executed-instruction count and leaves the
 *      next guest PC in `cpu.pc`.
 *
 *   4. Native execution and side exits
 *      Generated code keeps hot guest registers in callee-saved host registers.
 *      Before any helper call, block exit, or interpreter side exit, dirty
 *      cached registers are written back to `CPU_state`.  Side exits always set
 *      `cpu.pc` to the instruction that the interpreter must execute next and
 *      return the number of already completed guest instructions, preserving
 *      cpu_exec() budget accounting and device polling.
 *
 *   5. Invalidation and dependency tracking
 *      Compiled source bytes are tracked by physical PMEM chunks.  Instruction
 *      fetch translations and data-TLB entries also track page-table pages.
 *      Writes to compiled source or relevant page tables invalidate blocks or
 *      flush translation caches before stale native code can run again.
 *
 * Memory handling is intentionally tiered:
 *
 *   - Bare-mode PMEM loads/stores use inline range and alignment guards, then
 *     read/write host memory directly.
 *   - Bare-mode MMIO or out-of-range accesses call the normal vaddr helpers.
 *   - Sv39 loads/stores can use an inline direct-mapped data-TLB hit when the
 *     permission state, `satp`, VPN, page offset, and PMEM range all match.
 *   - Any TLB miss, cross-page access, page fault, MMIO case, source-code write,
 *     page-table write, or uncertain permission case returns to helper code.
 *
 * Direct links are another optional optimisation.  A block ending in a known
 * target can jump to another block's body only after checking that the target
 * slot is still valid for the same PC, `satp`, fetch privilege, data privilege
 * state when required, and instruction-fetch generation.  A miss returns to the
 * C dispatcher, which performs full revalidation or recompilation.
 *
 * The function comments below describe the local invariant each helper protects.
 * Keep that style when adding emitters: the important part is not the x86 byte
 * sequence alone, but the RISC-V architectural condition that must be true
 * before those bytes can execute.
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

/* Keep 64 as the old basic-block threshold; longer native regions are traces. */
#define RV64_JIT_BLOCK_MAX_INSNS 64u
/* First trace stage: one fall-through superblock with side exits. */
#define RV64_JIT_TRACE_MAX_INSNS 256u
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
 * A trace-length native region with register spills, guarded memory, helper
 * calls and side exits can be much larger than early minimal-emitter blocks.
 */
#define RV64_JIT_BLOCK_CODE_HEADROOM (128u * 1024u)
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
 * A 64-instruction 32-bit block covers at most 256 bytes, so it can cross at
 * most one 4 KiB virtual page boundary.  Keep the formula explicit rather than
 * hard-coding two segments.
 */
#define RV64_JIT_BLOCK_MAX_SOURCE_SEGMENTS \
    (((RV64_JIT_TRACE_MAX_INSNS * RV64_INSN_SIZE) + PAGE_SIZE - 1u) / \
         PAGE_SIZE + \
     1u)
#define RV64_JIT_BLOCK_MAX_IFETCH_PT_PAGES \
    (RV64_JIT_BLOCK_MAX_SOURCE_SEGMENTS * 3u)
#define RV64_JIT_BLOCK_MAX_SOURCE_CHUNKS \
    (((RV64_JIT_TRACE_MAX_INSNS * RV64_INSN_SIZE) + \
      RV64_JIT_SOURCE_CHUNK_SIZE - 1u) / \
         RV64_JIT_SOURCE_CHUNK_SIZE + \
     RV64_JIT_BLOCK_MAX_SOURCE_SEGMENTS)
#define RV64_JIT_SOURCE_LINK_NULL 0u
#define RV64_JIT_SOURCE_LINK_COUNT \
    (RV64_JIT_CACHE_SIZE * RV64_JIT_BLOCK_MAX_SOURCE_CHUNKS + 1u)
/* Guard failures in one direct-link exit all jump to the same miss path. */
#define RV64_JIT_DIRECT_LINK_MISS_PATCHES 10u
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

/*
 * Native block data model.
 *
 * `entry` is the public entry point used by the C dispatcher.  It includes the
 * prologue, saved-register setup and final epilogue.  `body_entry` skips the
 * prologue and points at the first translated guest instruction, so another
 * already-running native block can jump there after proving that the target
 * slot still describes the same architectural context.
 *
 * `translated` records whether the block was fetched through Sv39 rather than
 * bare PMEM.  Translated fetches must keep the page-table dependency list and
 * the `ifetch_generation` value valid.  `uses_data_state` is separate because
 * a block may be fetched physically but still contain translated data accesses
 * when MPRV or lower privilege is active.
 *
 * Source segments record the physical bytes behind the guest PCs.  They are
 * used both to compare current bytes against the compiled copy and to link the
 * block into the reverse invalidation map.  Page-table references are stored as
 * PMEM page numbers, because any store into one of those pages can change a
 * translation even when the instruction bytes themselves are untouched.
 */
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
    RV64_JIT_HREG_RBP,
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

typedef struct
{
    paddr_t paddr_start;
    uint32_t source_offset;
    uint32_t len;
    uint32_t source_chunk_first;
    uint32_t source_chunk_last;
} rv64_jit_source_segment_t;

typedef struct
{
    rv64_jit_source_segment_t segments[RV64_JIT_BLOCK_MAX_SOURCE_SEGMENTS];
    uint32_t segment_count;
    uint32_t source_len;
} rv64_jit_source_builder_t;

typedef struct
{
    paddr_t pages[RV64_JIT_BLOCK_MAX_IFETCH_PT_PAGES];
    uint32_t count;
} rv64_jit_ifetch_ref_builder_t;

typedef char rv64_jit_data_tlb_entry_size_must_be_64[sizeof(rv64_jit_data_tlb_entry_t) == 64 ? 1 : -1];
typedef char rv64_jit_pmem_mapping_must_be_page_aligned[((CONFIG_MBASE | CONFIG_MSIZE) & PAGE_MASK) == 0 ? 1 : -1];

typedef struct
{
    bool valid;
    bool translated;
    bool uses_data_state;
    vaddr_t pc;
    word_t satp;
    uint32_t ifetch_state;
    uint32_t data_state;
    uint64_t ifetch_generation;
    paddr_t paddr_start;
    uint32_t source_len;
    uint32_t source_segment_count;
    rv64_jit_source_segment_t source_segments[RV64_JIT_BLOCK_MAX_SOURCE_SEGMENTS];
    uint32_t insn_count;
    rv64_jit_entry_t entry;
    rv64_jit_entry_t body_entry;
    uint32_t ifetch_pt_page_count;
    paddr_t ifetch_pt_pages[RV64_JIT_BLOCK_MAX_IFETCH_PT_PAGES];
} rv64_jit_block_t;

typedef enum
{
    RV64_JIT_BLOCK_END_BUDGET,
    RV64_JIT_BLOCK_END_JUMP,
    RV64_JIT_BLOCK_END_CHAINED_LOOP,
    RV64_JIT_BLOCK_END_SOURCE_BOUNDARY,
    RV64_JIT_BLOCK_END_UNSUPPORTED_AFTER_PREFIX,
    RV64_JIT_BLOCK_END_COUNT,
} rv64_jit_block_end_reason_t;

typedef enum
{
    RV64_JIT_SIDE_EXIT_LOAD_GUARD,
    RV64_JIT_SIDE_EXIT_STORE_GUARD,
    RV64_JIT_SIDE_EXIT_STORE_SOURCE,
    RV64_JIT_SIDE_EXIT_PAGED_STORE_HELPER,
    RV64_JIT_SIDE_EXIT_BRANCH_TAKEN,
    RV64_JIT_SIDE_EXIT_CHAINED_OVER_BUDGET,
    RV64_JIT_SIDE_EXIT_JALR_MISALIGNED,
    RV64_JIT_SIDE_EXIT_COUNT,
} rv64_jit_side_exit_reason_t;

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
    uint64_t translated_cross_page_blocks;
    uint64_t segmented_source_blocks;
    uint64_t trace_blocks;
    uint64_t trace_insns;
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
    uint64_t unsupported_by_opcode[RV64_OPCODE_MASK + 1u];
    uint64_t block_end_by_reason[RV64_JIT_BLOCK_END_COUNT];
    uint64_t side_exit_by_reason[RV64_JIT_SIDE_EXIT_COUNT];
    uint64_t helper_load_count;
    uint64_t helper_store_count;
    uint64_t direct_link_taken_count;
    uint64_t direct_link_miss_count;
    uint64_t direct_branch_link_taken_count;
    uint64_t direct_guarded_link_taken_count;
    uint64_t ifetch_generation_fast_hits;
    uint64_t ifetch_generation_revalidations;
    uint64_t ifetch_generation_bumps;
    uint64_t source_reverse_invalidations;
    uint64_t source_full_invalidation_scans;
    uint64_t invalidation_requests;
    uint64_t invalidated_blocks;
    uint64_t arena_resets;
} rv64_jit_stats_t;

typedef struct
{
    uint32_t block_index;
    uint32_t next;
} rv64_jit_source_link_t;

typedef struct
{
    uint8_t *slow_disps[10];
    uint32_t count;
} rv64_jit_tlb_guard_patch_t;

static rv64_jit_block_t jit_cache[RV64_JIT_CACHE_SIZE];
static rv64_jit_data_tlb_entry_t jit_data_tlb[RV64_JIT_DATA_TLB_SIZE];
static uint16_t jit_data_tlb_pt_page_refs[RV64_JIT_PMEM_PAGE_COUNT];
static uint16_t jit_ifetch_pt_page_refs[RV64_JIT_PMEM_PAGE_COUNT];
static uint16_t jit_source_chunk_refs[RV64_JIT_PMEM_CHUNK_COUNT];
static uint32_t jit_source_chunk_heads[RV64_JIT_PMEM_CHUNK_COUNT];
static rv64_jit_source_link_t jit_source_links[RV64_JIT_SOURCE_LINK_COUNT];
static uint32_t jit_source_link_free_head = RV64_JIT_SOURCE_LINK_NULL;
static uint8_t *jit_code = NULL;
static size_t jit_code_used = 0;
static rv64_jit_stats_t jit_stats;
static uint64_t jit_ifetch_generation = 1;
#if RV64_JIT_ENABLED
static bool jit_disabled = false;
#endif
static bool jit_env_disable = false;
static bool jit_env_disable_direct_link = false;
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

/* Record why one candidate instruction could not be emitted by this JIT. */
static void jit_stat_unsupported_opcode(uint32_t instr)
{
    const uint32_t opcode = instr & RV64_OPCODE_MASK;
    JIT_STAT_INC(unsupported_by_opcode[opcode]);
#if !RV64_JIT_STATS
    (void)opcode;
#endif
}

/* Record the reason a compiled native block stopped growing. */
static void jit_stat_block_end(rv64_jit_block_end_reason_t reason)
{
    JIT_STAT_INC(block_end_by_reason[reason]);
#if !RV64_JIT_STATS
    (void)reason;
#endif
}

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
        jit_env_disable_direct_link =
            jit_env_flag_enabled("NEMU_DISABLE_RV64_JIT_DIRECT_LINK");
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

/* Return whether cross-block direct links should be emitted for this process. */
static bool jit_direct_link_enabled(void)
{
    jit_init_runtime_options();
    return !jit_env_disable_direct_link;
}

/* Advance the generation that protects translated instruction-fetch mappings. */
static void jit_ifetch_generation_bump(void)
{
    Assert(jit_ifetch_generation != UINT64_MAX,
           "jit: RV64 ifetch generation overflow");
    jit_ifetch_generation++;
    JIT_STAT_INC(ifetch_generation_bumps);
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

/* Record that one translated block depends on an instruction page-table page. */
static void jit_ifetch_ref_page(paddr_t page)
{
    size_t idx = 0;

    if (jit_data_pmem_page_index(page, &idx))
    {
        Assert(jit_ifetch_pt_page_refs[idx] != UINT16_MAX,
               "jit: RV64 ifetch page-table refcount overflow");
        jit_ifetch_pt_page_refs[idx]++;
    }
}

/* Drop one translated-block dependency on an instruction page-table page. */
static void jit_ifetch_unref_page(paddr_t page)
{
    size_t idx = 0;

    if (jit_data_pmem_page_index(page, &idx))
    {
        Assert(jit_ifetch_pt_page_refs[idx] > 0,
               "jit: RV64 ifetch page-table refcount underflow");
        jit_ifetch_pt_page_refs[idx]--;
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

/* Return whether a translated block depends on this page-table page. */
static bool jit_ifetch_refs_page(paddr_t page)
{
    size_t idx = 0;
    return jit_data_pmem_page_index(page, &idx) &&
           jit_ifetch_pt_page_refs[idx] != 0;
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

/* Return whether a PMEM write may have changed an ifetch page table. */
static bool jit_write_may_touch_ifetch_page_table(paddr_t addr, int len)
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

/* Compact the state that changes instruction-fetch translation/protection. */
static uint32_t jit_ifetch_state(void)
{
    /*
     * MPRV, SUM, and MXR are data-access controls.  Instruction fetch only uses
     * the current architectural privilege; satp is already stored separately.
     */
    return (uint32_t)cpu.prvi;
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

    JIT_STAT_INC(helper_load_count);

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

/* Commit a proven PMEM store and invalidate only when the bytes are sensitive. */
static uint32_t jit_store_pmem_direct_continue(paddr_t addr, uint32_t len,
                                               uint64_t data)
{
    const bool touch_source = jit_write_may_touch_source_chunk(addr, (int)len);
    const bool touch_page_table =
        jit_write_may_touch_data_tlb_page_table(addr, (int)len) ||
        jit_write_may_touch_ifetch_page_table(addr, (int)len);

    /*
     * Ordinary data stores do not need paddr_write()'s global invalidation hook.
     * The JIT has already proved that this is PMEM, and tracing is not enabled
     * for native JIT builds.  Sensitive writes still go through the exact
     * invalidation path after the new bytes are visible, matching paddr_write()
     * ordering for self-modifying code and page-table edits.
     */
    host_write(guest_to_host(addr), (int)len, (word_t)data);

    if (touch_source || touch_page_table)
    {
        isa_jit_invalidate_paddr(addr, (int)len);
    }

    return (touch_source || touch_page_table) ? 0u : 1u;
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

    JIT_STAT_INC(helper_store_count);

    if (jit_translate_pmem(addr, len, MEM_TYPE_WRITE, &paddr))
    {
        JIT_STAT_INC(data_tlb_direct_stores);
        (void)jit_store_pmem_direct_continue(paddr, len, data);
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
    JIT_STAT_INC(helper_store_count);

    return jit_store_pmem_direct_continue(addr, len, data);
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

/*
 * Source invalidation model.
 *
 * A native block is valid only while every physical source byte still matches
 * the bytes seen during compilation and every referenced ifetch page-table page
 * is unchanged.  Source bytes are grouped into 128-byte PMEM chunks.  Each
 * block publishes reverse links from those chunks to its cache slot, allowing a
 * normal store or DMA write to discard affected blocks without scanning the
 * whole cache in the common case.  The full scan fallback is still kept for
 * oversized ranges and defensive overflow handling.
 */
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

/* Convert one physical source range to the chunk range that covers it. */
static bool jit_source_chunk_range(paddr_t addr, uint32_t len,
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
static bool jit_source_builder_append(rv64_jit_source_builder_t *source,
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
static void jit_ifetch_refs_ref(const rv64_jit_block_t *block)
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
    jit_ifetch_refs_ref(block);
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
static bool jit_block_source_overlaps(const rv64_jit_block_t *block,
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

        if (jit_source_chunk_range(segment->paddr_start, segment->len,
                                   &first, &last) &&
            chunk >= first && chunk <= last)
        {
            return true;
        }
    }

    return false;
}

/* Rebuild the free list for reverse source-chunk map nodes. */
static void jit_source_reverse_map_reset(void)
{
    memset(jit_source_chunk_heads, 0, sizeof(jit_source_chunk_heads));

    for (uint32_t i = 1; i < RV64_JIT_SOURCE_LINK_COUNT - 1u; i++)
    {
        jit_source_links[i].next = i + 1u;
        jit_source_links[i].block_index = 0;
    }

    jit_source_links[RV64_JIT_SOURCE_LINK_COUNT - 1u].next = RV64_JIT_SOURCE_LINK_NULL;
    jit_source_links[RV64_JIT_SOURCE_LINK_COUNT - 1u].block_index = 0;
    jit_source_link_free_head = 1u;
}

/* Allocate one node from the fixed reverse source map pool. */
static uint32_t jit_source_link_alloc(void)
{
    Assert(jit_source_link_free_head != RV64_JIT_SOURCE_LINK_NULL,
           "jit: RV64 source reverse-map node pool exhausted");

    const uint32_t node = jit_source_link_free_head;
    jit_source_link_free_head = jit_source_links[node].next;
    jit_source_links[node].next = RV64_JIT_SOURCE_LINK_NULL;
    return node;
}

/* Return one reverse source-map node to the free list. */
static void jit_source_link_free(uint32_t node)
{
    Assert(node != RV64_JIT_SOURCE_LINK_NULL &&
               node < RV64_JIT_SOURCE_LINK_COUNT,
           "jit: invalid RV64 source reverse-map node %u", node);

    jit_source_links[node].block_index = 0;
    jit_source_links[node].next = jit_source_link_free_head;
    jit_source_link_free_head = node;
}

/* Return the direct-mapped cache index for one block pointer. */
static uint32_t jit_block_index(const rv64_jit_block_t *block)
{
    const uintptr_t block_addr = (uintptr_t)block;
    const uintptr_t cache_start = (uintptr_t)jit_cache;
    const uintptr_t cache_end = (uintptr_t)(jit_cache + RV64_JIT_CACHE_SIZE);

    Assert(block_addr >= cache_start && block_addr < cache_end,
           "jit: RV64 block pointer outside cache");
    return (uint32_t)(block - jit_cache);
}

/* Add one block to every source chunk it references. */
static void jit_source_reverse_map_add(rv64_jit_block_t *block)
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

        if (!jit_source_chunk_range(segment->paddr_start, segment->len, &first, &last))
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
            jit_source_links[node].block_index = block_index;
            jit_source_links[node].next = jit_source_chunk_heads[chunk];
            jit_source_chunk_heads[chunk] = node;
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

            uint32_t *link = &jit_source_chunk_heads[chunk];

            while (*link != RV64_JIT_SOURCE_LINK_NULL)
            {
                const uint32_t node = *link;

                if (jit_source_links[node].block_index == block_index)
                {
                    *link = jit_source_links[node].next;
                    jit_source_link_free(node);
                    break;
                }

                link = &jit_source_links[node].next;
            }
        }
    }
}

/* Add source-ref counts for the physical bytes backing one native block. */
static void jit_source_chunks_ref(const rv64_jit_block_t *block)
{
    for (uint32_t segment_idx = 0; segment_idx < block->source_segment_count;
         segment_idx++)
    {
        const rv64_jit_source_segment_t *segment = &block->source_segments[segment_idx];
        size_t first = 0;
        size_t last = 0;

        if (!jit_source_chunk_range(segment->paddr_start, segment->len, &first, &last))
        {
            continue;
        }

        for (size_t i = first; i <= last; i++)
        {
            if (jit_block_source_chunk_seen_before(block, segment_idx, (uint32_t)i))
            {
                continue;
            }

            Assert(jit_source_chunk_refs[i] != UINT16_MAX,
                   "jit: RV64 source chunk refcount overflow at %zu", i);
            jit_source_chunk_refs[i]++;
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

        if (!jit_source_chunk_range(segment->paddr_start, segment->len, &first, &last))
        {
            continue;
        }

        for (size_t i = first; i <= last; i++)
        {
            if (jit_block_source_chunk_seen_before(block, segment_idx, (uint32_t)i))
            {
                continue;
            }

            Assert(jit_source_chunk_refs[i] > 0,
                   "jit: RV64 source chunk refcount underflow at %zu", i);
            jit_source_chunk_refs[i]--;
        }
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
    if (block->valid)
    {
        if (block->source_segment_count != 0)
        {
            jit_source_reverse_map_remove(block);
            jit_source_chunks_unref(block);
        }

        jit_ifetch_refs_unref(block);
    }

    *block = (rv64_jit_block_t){0};
}

/* Hash one fetch context and guest PC into the direct-mapped cache. */
static uint32_t jit_hash_context(vaddr_t pc, word_t satp, uint32_t ifetch_state)
{
    /*
     * `pc >> 2` drops fixed 4-byte instruction-alignment zeros. `satp >> 12`
     * mixes the PPN/ASID-like high bits with the raw CSR value.  Include the
     * fetch privilege so M/S/U entries for the same PC do not evict each other.
     */
    return (uint32_t)(((pc >> 2) ^ satp ^ (satp >> 12) ^ ifetch_state) &
                      (RV64_JIT_CACHE_SIZE - 1u));
}

/* Hash the current fetch context and guest PC into the direct-mapped cache. */
static uint32_t jit_hash(vaddr_t pc, word_t satp)
{
    return jit_hash_context(pc, satp, jit_ifetch_state());
}

/* Return the cache slot for a PC under an already-known fetch context. */
static rv64_jit_block_t *jit_cache_slot_context(vaddr_t pc, word_t satp,
                                                uint32_t ifetch_state)
{
    return &jit_cache[jit_hash_context(pc, satp, ifetch_state)];
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
    memset(jit_ifetch_pt_page_refs, 0, sizeof(jit_ifetch_pt_page_refs));
    jit_source_reverse_map_reset();
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
    jit_source_reverse_map_reset();
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

/*
 * x86-64 emitter ABI.
 *
 * Generated code is a normal C-callable function returning a 32-bit retired
 * guest-instruction count in EAX.  The prologue saves RBX, RBP and R12-R15 so
 * they can cache guest registers for the whole block.  R11 always points at the
 * global `CPU_state`, and R10 holds the PMEM host base for direct memory
 * operations.  RAX, RCX, RDX and R8 are scratch unless a helper-specific comment
 * says otherwise.
 *
 * The extra 8-byte stack adjustment keeps the System V stack aligned before
 * helper calls.  Before any helper call, native block exit or interpreter side
 * exit, dirty cached guest registers must be flushed so the C code observes a
 * complete architectural state.
 */
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
    case RV64_JIT_HREG_RBP:
        return 5;
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

/* Emit a REX prefix only when a 32-bit instruction references r8-r15. */
static bool emit_rex32_if_needed(rv64_jit_writer_t *w, uint8_t reg, uint8_t rm)
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

/* Save all callee-saved host registers used as guest-register cache slots. */
static bool emit_push_saved_hregs(rv64_jit_writer_t *w)
{
    /*
     * Push order is RBX, RBP, R12, R13, R14, R15.  Six pushes would leave the
     * System V entry stack at rsp%16==8, so an extra 8-byte pad keeps helper
     * calls 16-byte aligned.
     */
    return emit_u8(w, 0x53) &&
           emit_u8(w, 0x55) &&
           emit_u8(w, 0x41) && emit_u8(w, 0x54) &&
           emit_u8(w, 0x41) && emit_u8(w, 0x55) &&
           emit_u8(w, 0x41) && emit_u8(w, 0x56) &&
           emit_u8(w, 0x41) && emit_u8(w, 0x57) &&
           /* sub rsp, 8 */
           emit_u8(w, 0x48) && emit_u8(w, 0x83) &&
           emit_u8(w, 0xec) && emit_u8(w, 0x08);
}

/* Restore callee-saved cache registers in the reverse of the push order. */
static bool emit_pop_saved_hregs(rv64_jit_writer_t *w)
{
    return
        /* add rsp, 8 */
        emit_u8(w, 0x48) && emit_u8(w, 0x83) &&
        emit_u8(w, 0xc4) && emit_u8(w, 0x08) &&
        emit_u8(w, 0x41) && emit_u8(w, 0x5f) &&
        emit_u8(w, 0x41) && emit_u8(w, 0x5e) &&
        emit_u8(w, 0x41) && emit_u8(w, 0x5d) &&
        emit_u8(w, 0x41) && emit_u8(w, 0x5c) &&
        emit_u8(w, 0x5d) &&
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

/* Emit `mov rax, [rax]`, loading one live 64-bit runtime guard value. */
static bool emit_mov_rax_m64_rax(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x8b) && emit_u8(w, 0x00);
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

/* Emit `add rdi, rax`, converting a PMEM offset while RCX keeps store data. */
static bool emit_add_rdi_rax(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x01) && emit_u8(w, 0xc7);
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

/* Compare a byte field in the RDX-pointed direct-link block with an immediate. */
static bool emit_cmp_rdxb_field_imm8(rv64_jit_writer_t *w, uint32_t offset,
                                     uint8_t value)
{
    Assert(offset <= INT8_MAX, "jit: RV64 block byte field offset is too large");
    return emit_u8(w, 0x80) && emit_u8(w, 0x7a) &&
           emit_u8(w, (uint8_t)offset) && emit_u8(w, value);
}

/* Compare a dword field in the RDX-pointed direct-link block with an immediate. */
static bool emit_cmp_rdxd_field_imm32(rv64_jit_writer_t *w, uint32_t offset,
                                      uint32_t value)
{
    Assert(offset <= INT8_MAX, "jit: RV64 block dword field offset is too large");
    return emit_u8(w, 0x81) && emit_u8(w, 0x7a) &&
           emit_u8(w, (uint8_t)offset) && emit_u32(w, value);
}

/* Compare a qword field in the RDX-pointed direct-link block with RAX. */
static bool emit_cmp_rdxq_field_rax(rv64_jit_writer_t *w, uint32_t offset)
{
    Assert(offset <= INT8_MAX, "jit: RV64 block qword field offset is too large");
    return emit_u8(w, 0x48) && emit_u8(w, 0x39) &&
           emit_u8(w, 0x42) && emit_u8(w, (uint8_t)offset);
}

/* Compare a qword field in the RDX-pointed direct-link block with a sign imm8. */
static bool emit_cmp_rdxq_field_imm8(rv64_jit_writer_t *w, uint32_t offset,
                                     uint8_t value)
{
    Assert(offset <= INT8_MAX, "jit: RV64 block qword field offset is too large");
    return emit_u8(w, 0x48) && emit_u8(w, 0x83) &&
           emit_u8(w, 0x7a) && emit_u8(w, (uint8_t)offset) && emit_u8(w, value);
}

/* Load a qword field from the RDX-pointed direct-link block into RAX. */
static bool emit_mov_rax_rdxq_field(rv64_jit_writer_t *w, uint32_t offset)
{
    Assert(offset <= INT8_MAX, "jit: RV64 block qword field offset is too large");
    return emit_u8(w, 0x48) && emit_u8(w, 0x8b) &&
           emit_u8(w, 0x42) && emit_u8(w, (uint8_t)offset);
}

/* Add a dword field from the RDX-pointed direct-link block into ECX. */
static bool emit_add_ecx_rdxd_field(rv64_jit_writer_t *w, uint32_t offset)
{
    Assert(offset <= INT8_MAX, "jit: RV64 block dword field offset is too large");
    return emit_u8(w, 0x03) && emit_u8(w, 0x4a) && emit_u8(w, (uint8_t)offset);
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

/* Emit `hreg op imm32` using x86 group-1 immediate ALU operations. */
static bool emit_hreg_imm32_alu64(rv64_jit_writer_t *w, rv64_jit_hreg_t hreg,
                                  uint8_t subop, int32_t imm)
{
    const uint8_t dst = jit_hreg_x86_reg(hreg);

    return emit_rex64(w, subop, dst) &&
           emit_u8(w, 0x81) &&
           emit_u8(w, jit_modrm(3, subop, dst)) &&
           emit_u32(w, (uint32_t)imm);
}

/* Emit `dst op src` directly between two cached 64-bit host registers. */
static bool emit_hreg_hreg_alu64(rv64_jit_writer_t *w, rv64_jit_hreg_t dst,
                                 rv64_jit_hreg_t src, uint8_t opcode)
{
    const uint8_t dst_reg = jit_hreg_x86_reg(dst);
    const uint8_t src_reg = jit_hreg_x86_reg(src);

    return emit_rex64(w, src_reg, dst_reg) &&
           emit_u8(w, opcode) &&
           emit_u8(w, jit_modrm(3, src_reg, dst_reg));
}

/* Emit `dst32 op src32` directly between cached host registers. */
static bool emit_hreg_hreg_alu32(rv64_jit_writer_t *w, rv64_jit_hreg_t dst,
                                 rv64_jit_hreg_t src, uint8_t opcode)
{
    const uint8_t dst_reg = jit_hreg_x86_reg(dst);
    const uint8_t src_reg = jit_hreg_x86_reg(src);

    return emit_rex32_if_needed(w, src_reg, dst_reg) &&
           emit_u8(w, opcode) &&
           emit_u8(w, jit_modrm(3, src_reg, dst_reg));
}

/* Emit `imul dst32, src32`, keeping the low 32-bit product in dst32. */
static bool emit_hreg_hreg_imul32(rv64_jit_writer_t *w, rv64_jit_hreg_t dst,
                                  rv64_jit_hreg_t src)
{
    const uint8_t dst_reg = jit_hreg_x86_reg(dst);
    const uint8_t src_reg = jit_hreg_x86_reg(src);

    return emit_rex32_if_needed(w, dst_reg, src_reg) &&
           emit_u8(w, 0x0f) && emit_u8(w, 0xaf) &&
           emit_u8(w, jit_modrm(3, dst_reg, src_reg));
}

/* Sign-extend one cached 32-bit W-form result back to RV64 XLEN. */
static bool emit_hreg_sext32(rv64_jit_writer_t *w, rv64_jit_hreg_t hreg)
{
    const uint8_t reg = jit_hreg_x86_reg(hreg);

    return emit_rex64(w, reg, reg) &&
           emit_u8(w, 0x63) &&
           emit_u8(w, jit_modrm(3, reg, reg));
}

/* Emit one immediate shift directly into a cached 64-bit host register. */
static bool emit_shift_hreg_imm(rv64_jit_writer_t *w, rv64_jit_hreg_t hreg,
                                uint8_t subop, uint8_t shamt)
{
    const uint8_t dst = jit_hreg_x86_reg(hreg);

    return emit_rex64(w, subop, dst) &&
           emit_u8(w, 0xc1) &&
           emit_u8(w, jit_modrm(3, subop, dst)) &&
           emit_u8(w, shamt);
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

/*
 * Guest-register cache.
 *
 * The register cache is a compile-time description of which guest GPR is
 * currently held in each callee-saved host register.  `valid` means the slot is
 * assigned to a guest register, `loaded` means native code has materialised its
 * current value, and `dirty` means `CPU_state.gpr[]` is stale until a flush.
 * The monotonically increasing age gives a simple spill choice when all slots
 * are occupied.  Guest x0 is special: reads materialise zero and writes are
 * discarded, so it never needs a dirty slot.
 *
 * Emitters snapshot this metadata before instructions that may fail emission.
 * If a later byte write would exceed the arena or an unsupported sub-case is
 * found, the snapshot is restored so the next fallback path still sees the
 * register state that matches the bytes already emitted.
 */
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

/* Mark a cache slot as the freshly written value of its assigned guest register. */
static void jit_reg_mark_written(rv64_jit_reg_cache_t *regs,
                                 rv64_jit_reg_slot_t *slot)
{
    slot->loaded = true;
    slot->dirty = true;
    slot->age = regs->next_age++;
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

/* Emit `jmp rax`, used by direct links to enter another block body. */
static bool emit_jmp_rax(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0xff) && emit_u8(w, 0xe0);
}

#if RV64_JIT_STATS
/* Emit a native-side increment for one 64-bit counter. */
static bool emit_inc_u64_counter(rv64_jit_writer_t *w, uint64_t *counter)
{
    /*
     * `48 ff 00` is `inc qword ptr [rax]`.  The helper deliberately clobbers
     * RAX; callers place it after address proof and before instructions that
     * overwrite RAX or no longer need it.
     */
    return emit_movabs_rax(w, (uint64_t)(uintptr_t)counter) &&
           emit_u8(w, 0x48) && emit_u8(w, 0xff) && emit_u8(w, 0x00);
}
#endif

/* Emit an optional native-side increment for one 64-bit JIT stat counter. */
static bool emit_inc_jit_stat_counter(rv64_jit_writer_t *w, uint64_t *counter)
{
#if RV64_JIT_STATS
    return emit_inc_u64_counter(w, counter);
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

/*
 * Block exits and direct links.
 *
 * Every exit has the same architectural contract: all completed guest
 * instructions have committed, no later instruction has partially committed,
 * dirty guest registers are visible in `CPU_state`, `cpu.pc` names the next
 * instruction for C or the interpreter, and the return count includes any
 * chained loop laps.
 *
 * A direct link may jump to another native block only after checking the cache
 * slot still has the expected PC, `satp`, ifetch privilege, data privilege tag
 * when relevant, body entry pointer and ifetch generation.  Any failed guard
 * returns to the dispatcher; the dispatcher then performs the slower byte and
 * page-table revalidation before running or recompiling the target.
 */
/* Flush cached registers and side-exit so the interpreter executes this PC. */
static bool emit_interpreter_side_exit(rv64_jit_writer_t *w,
                                       rv64_jit_reg_cache_t *regs, vaddr_t pc,
                                       uint32_t completed_count,
                                       bool loop_count_needed,
                                       rv64_jit_side_exit_reason_t reason)
{
    return jit_reg_emit_flush_all_dirty(w, regs) &&
           emit_store_pc_imm(w, pc) &&
           emit_inc_jit_stat_counter(w, &jit_stats.side_exit_by_reason[reason]) &&
           (loop_count_needed ? emit_return_loop_count(w, completed_count)
                              : emit_return_count(w, completed_count));
}

/* Add one conditional jump to the shared direct-link miss path. */
static bool emit_direct_link_miss_jcc(rv64_jit_writer_t *w, uint8_t jcc_opcode,
                                      uint8_t **miss_disps, uint32_t *miss_count)
{
    Assert(*miss_count < RV64_JIT_DIRECT_LINK_MISS_PATCHES,
           "jit: RV64 direct-link miss patch list overflow");
    return emit_jcc_rel32_placeholder(w, jcc_opcode, &miss_disps[(*miss_count)++]);
}

/* Emit the conservative block exit used when cross-block direct links are off. */
static bool emit_plain_block_exit(rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs,
                                  vaddr_t target_pc, uint32_t completed_count)
{
    return jit_reg_emit_flush_all_dirty(w, regs) &&
           emit_store_pc_imm(w, target_pc) &&
           emit_return_loop_count(w, completed_count);
}

/* Emit a guarded call to a known-next-PC native block, otherwise return to C. */
static bool emit_direct_link_exit(rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs,
                                  vaddr_t target_pc, uint32_t completed_count,
                                  bool source_uses_data_state,
                                  uint64_t *extra_taken_counter)
{
    const word_t satp = cpu.csr.satp;
    const uint32_t ifetch_state = jit_ifetch_state();
    rv64_jit_block_t *target =
        jit_cache_slot_context(target_pc, satp, ifetch_state);
    uint8_t *miss_disps[RV64_JIT_DIRECT_LINK_MISS_PATCHES];
    uint32_t miss_count = 0;

    const uint32_t valid_off = (uint32_t)offsetof(rv64_jit_block_t, valid);
    const uint32_t translated_off = (uint32_t)offsetof(rv64_jit_block_t, translated);
    const uint32_t uses_data_state_off =
        (uint32_t)offsetof(rv64_jit_block_t, uses_data_state);
    const uint32_t pc_off = (uint32_t)offsetof(rv64_jit_block_t, pc);
    const uint32_t satp_off = (uint32_t)offsetof(rv64_jit_block_t, satp);
    const uint32_t ifetch_state_off =
        (uint32_t)offsetof(rv64_jit_block_t, ifetch_state);
    const uint32_t data_state_off =
        (uint32_t)offsetof(rv64_jit_block_t, data_state);
    const uint32_t ifetch_generation_off =
        (uint32_t)offsetof(rv64_jit_block_t, ifetch_generation);
    const uint32_t insn_count_off =
        (uint32_t)offsetof(rv64_jit_block_t, insn_count);
    const uint32_t body_entry_off =
        (uint32_t)offsetof(rv64_jit_block_t, body_entry);
    const uint32_t data_state = jit_data_tlb_state(MEM_TYPE_READ);
    uint8_t *data_state_ok_disp = NULL;
    uint8_t *ifetch_generation_ok_disp = NULL;

    /*
     * The source block itself has already been matched by the C dispatcher.
     * Direct links duplicate only the cheap part of that validation.  If a
     * translated target may need source-page revalidation, the generation guard
     * misses back to C so jit_block_matches() owns the full page walk.
     */
    if (!jit_reg_emit_flush_all_dirty(w, regs) ||
        !emit_movabs_rdx(w, (uint64_t)(uintptr_t)target) ||
        !emit_cmp_rdxb_field_imm8(w, valid_off, 1) ||
        !emit_direct_link_miss_jcc(w, 0x85, miss_disps, &miss_count) ||
        !emit_movabs_rax(w, target_pc) ||
        !emit_cmp_rdxq_field_rax(w, pc_off) ||
        !emit_direct_link_miss_jcc(w, 0x85, miss_disps, &miss_count) ||
        !emit_movabs_rax(w, satp) ||
        !emit_cmp_rdxq_field_rax(w, satp_off) ||
        !emit_direct_link_miss_jcc(w, 0x85, miss_disps, &miss_count) ||
        !emit_cmp_rdxd_field_imm32(w, ifetch_state_off, ifetch_state) ||
        !emit_direct_link_miss_jcc(w, 0x85, miss_disps, &miss_count) ||
        !emit_cmp_rdxb_field_imm8(w, uses_data_state_off, 0))
    {
        return false;
    }

    if (source_uses_data_state)
    {
        if (!emit_jcc_rel32_placeholder(w, 0x84, &data_state_ok_disp) ||
            !emit_cmp_rdxd_field_imm32(w, data_state_off, data_state) ||
            !emit_direct_link_miss_jcc(w, 0x85, miss_disps, &miss_count))
        {
            return false;
        }
        patch_rel32(data_state_ok_disp, w->cur);
    }
    else if (!emit_direct_link_miss_jcc(w, 0x85, miss_disps, &miss_count))
    {
        return false;
    }

    if (!emit_cmp_rdxb_field_imm8(w, translated_off, 0) ||
        !emit_jcc_rel32_placeholder(w, 0x84, &ifetch_generation_ok_disp) ||
        !emit_movabs_rax(w, (uint64_t)(uintptr_t)&jit_ifetch_generation) ||
        !emit_mov_rax_m64_rax(w) ||
        !emit_cmp_rdxq_field_rax(w, ifetch_generation_off) ||
        !emit_direct_link_miss_jcc(w, 0x85, miss_disps, &miss_count))
    {
        return false;
    }
    patch_rel32(ifetch_generation_ok_disp, w->cur);

    if (!emit_cmp_rdxq_field_imm8(w, body_entry_off, 0) ||
        !emit_direct_link_miss_jcc(w, 0x84, miss_disps, &miss_count) ||
        !emit_movabs_rdx(w, (uint64_t)(uintptr_t)&jit_loop_extra) ||
        !emit_mov_eax_m32_rdx(w) ||
        !emit_add_eax_imm32(w, completed_count) ||
        !emit_mov_ecx_eax(w) ||
        !emit_movabs_rdx(w, (uint64_t)(uintptr_t)target) ||
        !emit_add_ecx_rdxd_field(w, insn_count_off) ||
        !emit_movabs_rdx(w, (uint64_t)(uintptr_t)&jit_entry_budget) ||
        !emit_cmp_ecx_m32_rdx(w) ||
        !emit_direct_link_miss_jcc(w, 0x87, miss_disps, &miss_count) ||
        !emit_movabs_rdx(w, (uint64_t)(uintptr_t)&jit_loop_extra) ||
        !emit_mov_m32_rdx_eax(w))
    {
        return false;
    }

#if RV64_JIT_STATS
    uint8_t *guarded_taken_disp = NULL;
    uint8_t *guarded_done_disp = NULL;

    if (!emit_movabs_rdx(w, (uint64_t)(uintptr_t)target) ||
        !emit_cmp_rdxb_field_imm8(w, translated_off, 0) ||
        !emit_jcc_rel32_placeholder(w, 0x85, &guarded_taken_disp) ||
        !emit_cmp_rdxb_field_imm8(w, uses_data_state_off, 0) ||
        !emit_jcc_rel32_placeholder(w, 0x84, &guarded_done_disp))
    {
        return false;
    }
    patch_rel32(guarded_taken_disp, w->cur);
    if (!emit_inc_jit_stat_counter(w, &jit_stats.direct_guarded_link_taken_count))
    {
        return false;
    }
    patch_rel32(guarded_done_disp, w->cur);
#endif

    if (!emit_inc_jit_stat_counter(w, &jit_stats.direct_link_taken_count) ||
        !(extra_taken_counter == NULL ||
          emit_inc_jit_stat_counter(w, extra_taken_counter)) ||
        !emit_movabs_rdx(w, (uint64_t)(uintptr_t)target) ||
        !emit_mov_rax_rdxq_field(w, body_entry_off) ||
        !emit_jmp_rax(w))
    {
        return false;
    }

    for (uint32_t i = 0; i < miss_count; i++)
    {
        patch_rel32(miss_disps[i], w->cur);
    }

    return emit_store_pc_imm(w, target_pc) &&
           emit_inc_jit_stat_counter(w, &jit_stats.direct_link_miss_count) &&
           emit_return_loop_count(w, completed_count);
}

/*
 * Inline memory emitters.
 *
 * Direct PMEM memory code is emitted only after earlier guards have proved
 * alignment, ordinary RAM range and no source-code write hazard.  The paged
 * variant adds an inline DTLB-hit proof: matching `satp`, VPN, permission state
 * and access rights, plus a same-page range check.  The slow edge from any
 * failed guard goes to the existing helper path, so page faults, MMIO, fresh
 * walks, cross-page accesses and invalidation side effects remain centralised in
 * the C implementation.
 */
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
                                        uint8_t **data_page_table_disp,
                                        uint8_t **ifetch_page_table_disp)
{
    /*
     * RDX is the PMEM byte offset.  A non-zero page-table refcount means a
     * direct write could stale a data-TLB entry or an instruction-fetch mapping,
     * so the helper must perform the store and run the invalidation hook.
     */
    return emit_mov_r8d_edx(w) &&
           emit_shr_r8d_imm(w, PAGE_SHIFT) &&
           emit_movabs_rax(w, (uint64_t)(uintptr_t)jit_data_tlb_pt_page_refs) &&
           emit_cmp_ref_word_zero_rax_r8(w) &&
           emit_jcc_rel32_placeholder(w, 0x85, data_page_table_disp) &&
           emit_movabs_rax(w, (uint64_t)(uintptr_t)jit_ifetch_pt_page_refs) &&
           emit_cmp_ref_word_zero_rax_r8(w) &&
           emit_jcc_rel32_placeholder(w, 0x85, ifetch_page_table_disp);
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
        if (!emit_interpreter_side_exit(w, &side_exit_regs, pc, completed_count,
                                        loop_count_needed,
                                        RV64_JIT_SIDE_EXIT_LOAD_GUARD))
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
    uint8_t *helper_done_disp = NULL;
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

    patch_rel32(range_slow_disp, w->cur);

    /*
     * An aligned out-of-PMEM bare-mode load may be MMIO.  Call the architectural
     * helper and continue so device callbacks still run in order without forcing
     * every polling loop back through the interpreter.
     */
    if (!jit_reg_emit_flush_all_dirty(w, regs) ||
        !emit_mov_rdi_rax(w) ||
        !emit_store_pc_imm(w, pc) ||
        !emit_call_abs(w, helper) ||
        !emit_load_cpu_base(w) ||
        !emit_movabs_r10(w, (uint64_t)(uintptr_t)guest_to_host(CONFIG_MBASE)) ||
        !jit_reg_write_rax(w, regs, rd) ||
        !emit_jmp_rel32_placeholder(w, &helper_done_disp))
    {
        return false;
    }

    if (align_slow_disp != NULL)
    {
        patch_rel32(align_slow_disp, w->cur);
        if (!emit_interpreter_side_exit(w, &side_exit_regs, pc, completed_count,
                                        loop_count_needed,
                                        RV64_JIT_SIDE_EXIT_LOAD_GUARD))
        {
            return false;
        }
    }

    patch_rel32(done_disp, w->cur);
    patch_rel32(helper_done_disp, w->cur);
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
    uint8_t *data_page_table_disp = NULL;
    uint8_t *ifetch_page_table_disp = NULL;
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
        !emit_store_page_table_guard(w, &data_page_table_disp,
                                     &ifetch_page_table_disp) ||
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
    patch_rel32(data_page_table_disp, slow_path);
    patch_rel32(ifetch_page_table_disp, slow_path);

    if (!jit_reg_emit_flush_all_dirty(w, regs) ||
        !emit_mov_rdx_rcx(w) ||
        !emit_mov_esi_imm32(w, len) ||
        !emit_store_pc_imm(w, pc) ||
        !emit_call_abs(w, (uintptr_t)jit_store_vaddr) ||
        !emit_load_cpu_base(w) ||
        !emit_movabs_r10(w, (uint64_t)(uintptr_t)guest_to_host(CONFIG_MBASE)) ||
        !emit_store_pc_imm(w, next_pc) ||
        !emit_inc_jit_stat_counter(w,
                                   &jit_stats.side_exit_by_reason[RV64_JIT_SIDE_EXIT_PAGED_STORE_HELPER]) ||
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
        if (!emit_interpreter_side_exit(w, &side_exit_regs, pc, completed_count,
                                        loop_count_needed,
                                        RV64_JIT_SIDE_EXIT_STORE_GUARD))
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

/* Emit one guarded bare-mode RV64 store that normally commits inline. */
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
    uint8_t *cross_chunk_disp = NULL;
    uint8_t *source_chunk_disp = NULL;
    uint8_t *data_page_table_disp = NULL;
    uint8_t *ifetch_page_table_disp = NULL;
    uint8_t *exit_disp = NULL;
    uint8_t *direct_done_disp = NULL;
    uint8_t *continue_disp = NULL;
    rv64_jit_reg_cache_t side_exit_regs;

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
     * Ordinary PMEM stores can write directly and keep executing.  Stores that
     * cross a source-tracking chunk, overlap compiled source, or touch cached
     * page-table bytes use the helper so exact invalidation happens after the
     * write and before any later translated fetch.
     */
    if (!jit_reg_read_rcx(w, regs, rs2) ||
        !emit_store_source_chunk_guard(w, len, &cross_chunk_disp,
                                       &source_chunk_disp) ||
        !emit_store_page_table_guard(w, &data_page_table_disp,
                                     &ifetch_page_table_disp) ||
        !emit_direct_pmem_store_from_rcx(w, len) ||
        !emit_jmp_rel32_placeholder(w, &direct_done_disp))
    {
        return false;
    }

    const uint8_t *helper_path = w->cur;
    patch_rel32(cross_chunk_disp, helper_path);
    patch_rel32(source_chunk_disp, helper_path);
    patch_rel32(data_page_table_disp, helper_path);
    patch_rel32(ifetch_page_table_disp, helper_path);

    if (!jit_reg_emit_flush_all_dirty(w, regs) ||
        !emit_mov_rdi_rdx(w) ||
        !emit_movabs_rax(w, (uint64_t)CONFIG_MBASE) ||
        !emit_add_rdi_rax(w) ||
        !emit_mov_rdx_rcx(w) ||
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
        !emit_inc_jit_stat_counter(w,
                                   &jit_stats.side_exit_by_reason[RV64_JIT_SIDE_EXIT_STORE_SOURCE]) ||
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

    if (!emit_interpreter_side_exit(w, &side_exit_regs, pc, completed_count,
                                    loop_count_needed,
                                    RV64_JIT_SIDE_EXIT_STORE_GUARD))
    {
        return false;
    }

    patch_rel32(direct_done_disp, w->cur);
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

/* Emit a one-source integer op directly in the destination cache slot. */
static bool emit_op_imm_hreg(rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs,
                             uint32_t rd, uint32_t rs1, uint8_t subop,
                             int32_t imm)
{
    if (rd == 0)
    {
        return true;
    }

    if (rs1 == 0)
    {
        switch (subop)
        {
        case 0x0: /* ADD: 0 + imm */
        case 0x1: /* OR: 0 | imm */
        case 0x6: /* XOR: 0 ^ imm */
            return jit_reg_write_imm(w, regs, rd, (uint64_t)(int64_t)imm);
        case 0x4: /* AND: 0 & imm */
            return jit_reg_write_imm(w, regs, rd, 0);
        default:
            return false;
        }
    }

    rv64_jit_reg_slot_t *src = jit_reg_loaded_slot(w, regs, rs1);
    if (src == NULL)
    {
        return false;
    }

    rv64_jit_reg_slot_t *dst = jit_reg_alloc(w, regs, rd);
    if (dst == NULL ||
        (dst != src && !emit_mov_hreg_hreg(w, dst->hreg, src->hreg)) ||
        !emit_hreg_imm32_alu64(w, dst->hreg, subop, imm))
    {
        return false;
    }

    jit_reg_mark_written(regs, dst);
    return true;
}

/* Emit a shift-immediate op directly in the destination cache slot. */
static bool emit_shift_imm_hreg(rv64_jit_writer_t *w,
                                rv64_jit_reg_cache_t *regs,
                                uint32_t rd, uint32_t rs1,
                                uint8_t subop, uint8_t shamt)
{
    if (rd == 0)
    {
        return true;
    }

    if (rs1 == 0)
    {
        return jit_reg_write_imm(w, regs, rd, 0);
    }

    rv64_jit_reg_slot_t *src = jit_reg_loaded_slot(w, regs, rs1);
    if (src == NULL)
    {
        return false;
    }

    rv64_jit_reg_slot_t *dst = jit_reg_alloc(w, regs, rd);
    if (dst == NULL ||
        (dst != src && !emit_mov_hreg_hreg(w, dst->hreg, src->hreg)) ||
        !emit_shift_hreg_imm(w, dst->hreg, subop, shamt))
    {
        return false;
    }

    jit_reg_mark_written(regs, dst);
    return true;
}

/* Emit a two-source integer ALU op directly between cached host registers. */
static bool emit_op_hreg(rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs,
                         uint32_t rd, uint32_t rs1, uint32_t rs2,
                         uint8_t opcode, bool commutative)
{
    if (rd == 0)
    {
        return true;
    }

    rv64_jit_reg_slot_t *src1 = jit_reg_loaded_slot(w, regs, rs1);
    rv64_jit_reg_slot_t *src2 = jit_reg_loaded_slot(w, regs, rs2);

    if (src1 == NULL || src2 == NULL)
    {
        return false;
    }

    rv64_jit_reg_slot_t *dst = NULL;
    rv64_jit_reg_slot_t *rhs = NULL;

    if (rd == rs1)
    {
        dst = src1;
        rhs = src2;
    }
    else if (commutative && rd == rs2)
    {
        dst = src2;
        rhs = src1;
    }
    else
    {
        dst = jit_reg_alloc(w, regs, rd);
        if (dst == NULL || !emit_mov_hreg_hreg(w, dst->hreg, src1->hreg))
        {
            return false;
        }
        rhs = src2;
    }

    if (!emit_hreg_hreg_alu64(w, dst->hreg, rhs->hreg, opcode))
    {
        return false;
    }

    jit_reg_mark_written(regs, dst);
    return true;
}

/* Emit one commutative RV64 W-form op directly in cached host registers. */
static bool emit_op32_hreg_commutative(rv64_jit_writer_t *w,
                                       rv64_jit_reg_cache_t *regs,
                                       uint32_t rd, uint32_t rs1,
                                       uint32_t rs2, uint8_t opcode,
                                       bool multiply)
{
    if (rd == 0)
    {
        return true;
    }

    if (rs1 == 0 || rs2 == 0)
    {
        return false;
    }

    rv64_jit_reg_slot_t *src1 = jit_reg_loaded_slot(w, regs, rs1);
    rv64_jit_reg_slot_t *src2 = jit_reg_loaded_slot(w, regs, rs2);

    if (src1 == NULL || src2 == NULL)
    {
        return false;
    }

    rv64_jit_reg_slot_t *dst = NULL;
    rv64_jit_reg_slot_t *rhs = NULL;

    if (rd == rs1)
    {
        dst = src1;
        rhs = src2;
    }
    else if (rd == rs2)
    {
        dst = src2;
        rhs = src1;
    }
    else
    {
        dst = jit_reg_alloc(w, regs, rd);
        if (dst == NULL)
        {
            return false;
        }

        if (dst == src1)
        {
            rhs = src2;
        }
        else if (dst == src2)
        {
            rhs = src1;
        }
        else
        {
            if (!emit_mov_hreg_hreg(w, dst->hreg, src1->hreg))
            {
                return false;
            }
            rhs = src2;
        }
    }

    if (!((multiply ? emit_hreg_hreg_imul32(w, dst->hreg, rhs->hreg)
                    : emit_hreg_hreg_alu32(w, dst->hreg, rhs->hreg, opcode)) &&
          emit_hreg_sext32(w, dst->hreg)))
    {
        return false;
    }

    jit_reg_mark_written(regs, dst);
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

    switch (funct3)
    {
    case 0x0: /* ADDI */
        return emit_op_imm_hreg(w, regs, rd, rs1, 0x0, imm);
    case 0x2: /* SLTI, signed compare; SETL is opcode 0x9c. */
        if (!jit_reg_read_rax(w, regs, rs1))
        {
            return false;
        }
        return emit_cmp_rax_imm32(w, imm) && emit_setcc_rax(w, 0x9c) && jit_reg_write_rax(w, regs, rd);
    case 0x3: /* SLTIU, unsigned compare; SETB is opcode 0x92. */
        if (!jit_reg_read_rax(w, regs, rs1))
        {
            return false;
        }
        return emit_cmp_rax_imm32(w, imm) && emit_setcc_rax(w, 0x92) && jit_reg_write_rax(w, regs, rd);
    case 0x4: /* XORI */
        return emit_op_imm_hreg(w, regs, rd, rs1, 0x6, imm);
    case 0x6: /* ORI */
        return emit_op_imm_hreg(w, regs, rd, rs1, 0x1, imm);
    case 0x7: /* ANDI */
        return emit_op_imm_hreg(w, regs, rd, rs1, 0x4, imm);
    case 0x1: /* SLLI; funct6 must be 000000 for RV64 base shifts. */
        if (bits(instr, 31, 26) != 0x00)
        {
            return false;
        }
        return emit_shift_imm_hreg(w, regs, rd, rs1, 0x4,
                                   (uint8_t)bits(instr, 25, 20));
    case 0x5: /* SRLI/SRAI; funct6 selects logical versus arithmetic right shift. */
        if (bits(instr, 31, 26) == 0x00)
        {
            return emit_shift_imm_hreg(w, regs, rd, rs1, 0x5,
                                       (uint8_t)bits(instr, 25, 20));
        }

        if (bits(instr, 31, 26) == 0x10)
        {
            return emit_shift_imm_hreg(w, regs, rd, rs1, 0x7,
                                       (uint8_t)bits(instr, 25, 20));
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

    switch (key)
    {
    case 0x000: /* ADD */
        return emit_op_hreg(w, regs, rd, rs1, rs2, 0x01, true);
    case 0x100: /* SUB */
        if (rd != rs2)
        {
            return emit_op_hreg(w, regs, rd, rs1, rs2, 0x29, false);
        }
        break;
    case 0x004: /* XOR */
        return emit_op_hreg(w, regs, rd, rs1, rs2, 0x31, true);
    case 0x006: /* OR */
        return emit_op_hreg(w, regs, rd, rs1, rs2, 0x09, true);
    case 0x007: /* AND */
        return emit_op_hreg(w, regs, rd, rs1, rs2, 0x21, true);
    default:
        break;
    }

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

    switch (key)
    {
    case 0x000: /* ADDW */
        if (rs1 != 0 && rs2 != 0)
        {
            return emit_op32_hreg_commutative(w, regs, rd, rs1, rs2, 0x01, false);
        }
        break;
    case 0x008: /* MULW */
        if (rs1 != 0 && rs2 != 0)
        {
            JIT_STAT_INC(native_m_ops);
            return emit_op32_hreg_commutative(w, regs, rd, rs1, rs2, 0x00, true);
        }
        break;
    default:
        break;
    }

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

/*
 * Branch chaining.
 *
 * Simple counted loops are allowed to stay inside one native function when the
 * taken target is the current block head and every instruction in the body can
 * be re-entered safely.  The generated backedge accumulates retired
 * instructions in `jit_loop_extra` and checks one more full lap against the
 * cpu_exec() budget before jumping.  If the next lap would exceed the budget,
 * the block returns to C with `cpu.pc` at the branch target, preserving bounded
 * device polling and interrupt checks.
 */
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
           emit_inc_jit_stat_counter(w,
                                     &jit_stats.side_exit_by_reason[RV64_JIT_SIDE_EXIT_CHAINED_OVER_BUDGET]) &&
           emit_mov_eax_ecx(w) &&
           emit_return_eax(w);
}

/* Emit one conditional branch with a taken side exit and fall-through fast path. */
static bool emit_branch(rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs,
                        uint32_t instr, vaddr_t pc,
                        vaddr_t block_start_pc, const uint8_t *block_start_native,
                        bool chain_safe, bool *branch_chained,
                        uint32_t exit_count, bool source_uses_data_state)
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
    else if (!jit_direct_link_enabled())
    {
        if (!emit_plain_block_exit(w, regs, target, exit_count))
        {
            return false;
        }
    }
    else if (!emit_direct_link_exit(w, regs, target, exit_count,
                                    source_uses_data_state,
                                    &jit_stats.direct_branch_link_taken_count))
    {
        return false;
    }

    patch_rel32(fallthrough_disp, w->cur);
    return true;
}

/* Emit JAL or JALR, both of which end the current native block. */
static bool emit_jump_instr(rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs,
                            uint32_t instr, vaddr_t pc,
                            uint32_t completed_count, bool loop_count_needed,
                            bool source_uses_data_state)
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
               (jit_direct_link_enabled()
                    ? emit_direct_link_exit(w, regs, target, completed_count + 1u,
                                            source_uses_data_state, NULL)
                    : emit_plain_block_exit(w, regs, target, completed_count + 1u));
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
    if (!emit_interpreter_side_exit(w, &side_exit_regs, pc, completed_count,
                                    loop_count_needed,
                                    RV64_JIT_SIDE_EXIT_JALR_MISALIGNED))
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

/*
 * Block validation and cache matching.
 *
 * Cache lookup is deliberately only a hint.  A slot match must re-check the
 * guest PC, `satp`, ifetch privilege, translation generation, source byte
 * segments and page-table dependencies before native code runs.  This is the
 * main safety net for self-modifying code, disk/DMA writes into PMEM and guest
 * page-table edits.  Unsupported cached slots are kept as negative entries so
 * the dispatcher does not repeatedly compile the same unsupported instruction.
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
static bool jit_translate_ifetch_collect(vaddr_t pc, paddr_t *paddr,
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
        if (!jit_data_sv39_canonical(pc) ||
            jit_data_cross_page(pc, RV64_INSN_SIZE))
        {
            return false;
        }

        const word_t vpn[3] = {
            ((word_t)pc >> 12) & 0x1ffu,
            ((word_t)pc >> 21) & 0x1ffu,
            ((word_t)pc >> 30) & 0x1ffu,
        };
        paddr_t pt_base =
            (paddr_t)((cpu.csr.satp & RV64_JIT_SATP_PPN_MASK) << PAGE_SHIFT);

        for (int level = 2; level >= 0; --level)
        {
            const paddr_t pte_addr =
                pt_base + (paddr_t)(vpn[level] * sizeof(uint64_t));

            if (!jit_data_pmem_range(pte_addr, sizeof(uint64_t)) ||
                (refs != NULL &&
                 !jit_ifetch_ref_builder_append(refs, pt_base)))
            {
                return false;
            }

            const word_t pte = (word_t)paddr_read(pte_addr, 8);

            if (!jit_data_pte_valid(pte))
            {
                return false;
            }

            const word_t ppn = jit_data_pte_ppn(pte);

            if (jit_data_pte_leaf(pte))
            {
                if (!jit_data_superpage_aligned(ppn, level) ||
                    !jit_ifetch_leaf_allows(pte))
                {
                    return false;
                }

                *paddr = jit_data_leaf_page_base(ppn, vpn, level) |
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
static bool jit_translate_ifetch_ex(vaddr_t pc, paddr_t *paddr, bool *translated)
{
    return jit_translate_ifetch_collect(pc, paddr, translated, NULL);
}

/* Check whether a cache slot still describes the current PC and source bytes. */
static bool jit_block_matches(rv64_jit_block_t *block, vaddr_t pc)
{
    if (!block->valid ||
        block->pc != pc ||
        block->satp != cpu.csr.satp ||
        block->ifetch_state != jit_ifetch_state())
    {
        return false;
    }

    if (block->uses_data_state &&
        block->data_state != jit_data_tlb_state(MEM_TYPE_READ))
    {
        return false;
    }

    if (block->translated)
    {
        if (block->ifetch_generation == jit_ifetch_generation)
        {
            JIT_STAT_INC(ifetch_generation_fast_hits);
            return true;
        }

        JIT_STAT_INC(ifetch_generation_revalidations);

        /*
         * Writes and SFENCE.VMA conservatively bump the global ifetch
         * generation.  Only after such a bump do we re-translate the virtual
         * pages touched by this block and refresh its generation if the
         * physical source bytes are still identical.
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
                !jit_translate_ifetch_collect(check_pc, &now, &translated, &refs) ||
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
        block->ifetch_generation = jit_ifetch_generation;
    }

    return true;
}

/* Publish a negative cache entry for a currently unsupported instruction. */
static void jit_mark_unsupported(vaddr_t pc, paddr_t paddr, bool translated)
{
    JIT_STAT_INC(blocks_unsupported);

    rv64_jit_ifetch_ref_builder_t refs = {0};

    if (translated)
    {
        paddr_t checked_paddr = 0;
        bool checked_translated = false;

        if (!jit_translate_ifetch_collect(pc, &checked_paddr,
                                          &checked_translated, &refs) ||
            !checked_translated ||
            checked_paddr != paddr)
        {
            return;
        }
    }

    rv64_jit_block_t *block = jit_cache_slot(pc);
    jit_block_discard(block);
    *block = (rv64_jit_block_t){
        .valid = true,
        .translated = translated,
        .uses_data_state = false,
        .pc = pc,
        .satp = cpu.csr.satp,
        .ifetch_state = jit_ifetch_state(),
        .data_state = jit_data_tlb_state(MEM_TYPE_READ),
        .ifetch_generation = jit_ifetch_generation,
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
    jit_ifetch_refs_ref(block);
    jit_source_chunks_ref(block);
    jit_source_reverse_map_add(block);
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
                                             bool first_translated)
{
    vaddr_t cur_pc = pc;
    uint32_t count = 0;

    while (count < max_insns && count < RV64_JIT_TRACE_MAX_INSNS)
    {
        paddr_t cur_paddr = 0;
        bool cur_translated = false;

        if (!jit_translate_ifetch_ex(cur_pc, &cur_paddr, &cur_translated) ||
            !in_pmem(cur_paddr) ||
            cur_translated != first_translated)
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
        count++;
    }

    return false;
}

/*
 * Compile one native region starting at the current guest PC.
 *
 * The compile pipeline is intentionally linear:
 *   1. Allocate aligned arena space and translate the first fetch.
 *   2. Emit the function prologue and initialise the register cache.
 *   3. Walk guest instructions until budget, trace limit, unsupported opcode,
 *      source-boundary change or terminating control flow.
 *   4. For each instruction, record physical source bytes and ifetch page-table
 *      refs before emitting bytes that can observe that instruction.
 *   5. Emit either a normal block exit, a guarded direct link, a side exit or a
 *      chained-loop backedge.
 *   6. Publish the block metadata only after code emission, source copying and
 *      reverse invalidation links are all complete.
 */
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

    const bool chain_safe_start =
        jit_block_has_chainable_backedge(pc, max_insns, first_translated);
    const bool loop_count_needed = true;
    const uint8_t *block_start_native = w.cur;
    vaddr_t cur_pc = pc;
    uint32_t count = 0;
    rv64_jit_source_builder_t source = {0};
    rv64_jit_ifetch_ref_builder_t ifetch_refs = {0};
    bool chain_safe = chain_safe_start;
    bool uses_data_state = false;
    rv64_jit_block_end_reason_t block_end_reason = RV64_JIT_BLOCK_END_BUDGET;

    while (count < max_insns && count < RV64_JIT_TRACE_MAX_INSNS)
    {
        /*
         * Re-translate every guest instruction, even inside one block. This keeps
         * the block metadata honest across page boundaries and avoids assuming that
         * adjacent virtual PCs are adjacent physical bytes.
         */
        paddr_t cur_paddr = 0;
        bool cur_translated = false;
        rv64_jit_ifetch_ref_builder_t ifetch_refs_start = ifetch_refs;

        if (!jit_translate_ifetch_collect(cur_pc, &cur_paddr, &cur_translated,
                                          &ifetch_refs) ||
            !in_pmem(cur_paddr) ||
            cur_translated != first_translated)
        {
            ifetch_refs = ifetch_refs_start;
            block_end_reason = RV64_JIT_BLOCK_END_SOURCE_BOUNDARY;
            break;
        }

        const uint32_t instr = (uint32_t)vaddr_ifetch(cur_pc, RV64_INSN_SIZE);
        const uint32_t opcode = instr & RV64_OPCODE_MASK;
        uint8_t *instr_start = w.cur;
        rv64_jit_reg_cache_t regs_start = regs;
        rv64_jit_source_builder_t source_start = source;
        bool end_block = false;

        if (!jit_source_builder_append(&source, cur_paddr, RV64_INSN_SIZE))
        {
            ifetch_refs = ifetch_refs_start;
            block_end_reason = RV64_JIT_BLOCK_END_SOURCE_BOUNDARY;
            break;
        }

        if (opcode == RV64_OPCODE_JAL ||
            opcode == RV64_OPCODE_JALR)
        {
            if (!emit_jump_instr(&w, &regs, instr, cur_pc, count,
                                 loop_count_needed, uses_data_state))
            {
                w.cur = instr_start;
                jit_reg_cache_restore(&regs, &regs_start);
                source = source_start;
                ifetch_refs = ifetch_refs_start;
                jit_stat_unsupported_opcode(instr);
                block_end_reason = RV64_JIT_BLOCK_END_UNSUPPORTED_AFTER_PREFIX;
                break;
            }
            block_end_reason = RV64_JIT_BLOCK_END_JUMP;
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
                source = source_start;
                ifetch_refs = ifetch_refs_start;
                jit_stat_unsupported_opcode(instr);
                block_end_reason = RV64_JIT_BLOCK_END_UNSUPPORTED_AFTER_PREFIX;
                break;
            }
            if ((cpu.csr.satp >> RV64_JIT_SATP_MODE_SHIFT) != 0)
            {
                uses_data_state = true;
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
                source = source_start;
                ifetch_refs = ifetch_refs_start;
                jit_stat_unsupported_opcode(instr);
                block_end_reason = RV64_JIT_BLOCK_END_UNSUPPORTED_AFTER_PREFIX;
                break;
            }
            if ((cpu.csr.satp >> RV64_JIT_SATP_MODE_SHIFT) != 0)
            {
                uses_data_state = true;
            }
        }
        else if (opcode == RV64_OPCODE_BRANCH)
        {
            bool branch_chained = false;

            if (!emit_branch(&w, &regs, instr, cur_pc, pc, block_start_native,
                             chain_safe, &branch_chained, count + 1u,
                             uses_data_state))
            {
                w.cur = instr_start;
                jit_reg_cache_restore(&regs, &regs_start);
                source = source_start;
                ifetch_refs = ifetch_refs_start;
                jit_stat_unsupported_opcode(instr);
                block_end_reason = RV64_JIT_BLOCK_END_UNSUPPORTED_AFTER_PREFIX;
                break;
            }

            if (branch_chained)
            {
                block_end_reason = RV64_JIT_BLOCK_END_CHAINED_LOOP;
                end_block = true;
            }
        }
        else if (!emit_instr(&w, &regs, instr, cur_pc, count + 1u))
        {
            w.cur = instr_start;
            jit_reg_cache_restore(&regs, &regs_start);
            source = source_start;
            ifetch_refs = ifetch_refs_start;
            jit_stat_unsupported_opcode(instr);
            block_end_reason = RV64_JIT_BLOCK_END_UNSUPPORTED_AFTER_PREFIX;
            break;
        }

        cur_pc += RV64_INSN_SIZE;
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

    if (!(jit_direct_link_enabled()
              ? emit_direct_link_exit(&w, &regs, cur_pc, count, uses_data_state, NULL)
              : emit_plain_block_exit(&w, &regs, cur_pc, count)))
    {
        return NULL;
    }

    __builtin___clear_cache((char *)w.start, (char *)w.cur);

    rv64_jit_block_t *block = jit_cache_slot(pc);
    jit_block_discard(block);
    *block = (rv64_jit_block_t){
        .valid = true,
        .translated = first_translated,
        .uses_data_state = uses_data_state,
        .pc = pc,
        .satp = cpu.csr.satp,
        .ifetch_state = jit_ifetch_state(),
        .data_state = jit_data_tlb_state(MEM_TYPE_READ),
        .ifetch_generation = jit_ifetch_generation,
        .paddr_start = first_paddr,
        .source_len = source.source_len,
        .source_segment_count = source.segment_count,
        .ifetch_pt_page_count = first_translated ? ifetch_refs.count : 0,
        .insn_count = count,
        .entry = (rv64_jit_entry_t)w.start,
        .body_entry = (rv64_jit_entry_t)block_start_native,
    };
    memcpy(block->source_segments, source.segments, sizeof(source.segments));
    memcpy(block->ifetch_pt_pages, ifetch_refs.pages,
           block->ifetch_pt_page_count * sizeof(block->ifetch_pt_pages[0]));
    jit_ifetch_refs_ref(block);
    jit_source_chunks_ref(block);
    jit_source_reverse_map_add(block);

    jit_code_used = (size_t)(w.cur - jit_code);
    JIT_STAT_INC(blocks_compiled);
    jit_stat_block_end(block_end_reason);
    if (first_translated)
    {
        JIT_STAT_INC(translated_blocks);
        if (((pc ^ (cur_pc - RV64_INSN_SIZE)) & ~(vaddr_t)PAGE_MASK) != 0)
        {
            JIT_STAT_INC(translated_cross_page_blocks);
        }
    }
    if (source.segment_count > 1u)
    {
        JIT_STAT_INC(segmented_source_blocks);
    }
    if (count > RV64_JIT_BLOCK_MAX_INSNS)
    {
        JIT_STAT_INC(trace_blocks);
        JIT_STAT_ADD(trace_insns, count);
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
    jit_ifetch_generation_bump();
}

/* Invalidate native blocks whose physical source bytes overlap a PMEM write. */
void isa_jit_invalidate_paddr(paddr_t addr, int len)
{
    JIT_STAT_INC(invalidation_requests);

    if (len <= 0 || jit_code == NULL)
    {
        return;
    }

    /*
     * This conservative generation bump covers page-table remaps for translated
     * instruction fetches.  Exact source-byte invalidation below still discards
     * native blocks whose physical code bytes changed.
     */
    jit_ifetch_generation_bump();

    if (jit_write_may_touch_data_tlb_page_table(addr, len))
    {
        JIT_STAT_INC(data_tlb_page_table_flushes);
        jit_data_tlb_flush();
    }

    if (!jit_write_may_touch_source_chunk(addr, len))
    {
        return;
    }

    size_t first = 0;
    size_t last = 0;

    if (jit_source_chunk_range(addr, (uint32_t)len, &first, &last))
    {
        JIT_STAT_INC(source_reverse_invalidations);

        for (size_t chunk = first; chunk <= last; chunk++)
        {
            uint32_t node = jit_source_chunk_heads[chunk];

            while (node != RV64_JIT_SOURCE_LINK_NULL)
            {
                const uint32_t next = jit_source_links[node].next;
                rv64_jit_block_t *block = &jit_cache[jit_source_links[node].block_index];

                if (block->valid &&
                    jit_block_source_overlaps(block, addr, len))
                {
                    jit_block_discard(block);
                    JIT_STAT_INC(invalidated_blocks);
                }

                node = next;
            }
        }

        return;
    }

    JIT_STAT_INC(source_full_invalidation_scans);
    for (size_t i = 0; i < RV64_JIT_CACHE_SIZE; i++)
    {
        rv64_jit_block_t *block = &jit_cache[i];

        if (block->valid &&
            jit_block_source_overlaps(block, addr, len))
        {
            jit_block_discard(block);
            JIT_STAT_INC(invalidated_blocks);
        }
    }
}

/*
 * Execute cached or newly compiled native RV64 blocks within the given budgets.
 *
 * This is the only entry point used by the generic CPU loop.  It first clamps
 * work to both the remaining instruction budget and the device-polling budget.
 * Each iteration then tries a direct cache hit, recompiles on a miss, or stops
 * cleanly on an unsupported negative entry.  A native function returning zero is
 * treated as a side exit that made no forward progress, so the interpreter can
 * execute the current instruction and report the precise trap or helper effect.
 *
 * The tiny loop ABI uses `jit_entry_budget` and `jit_loop_extra` so generated
 * chained loops can stay native while still returning exact retired counts.
 */
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

        if (block_budget > RV64_JIT_TRACE_MAX_INSNS)
        {
            block_budget = RV64_JIT_TRACE_MAX_INSNS;
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

static const char *const jit_block_end_reason_names[RV64_JIT_BLOCK_END_COUNT] = {
    "budget",
    "jump",
    "chained-loop",
    "source-boundary",
    "unsupported-after-prefix",
};

static const char *const jit_side_exit_reason_names[RV64_JIT_SIDE_EXIT_COUNT] = {
    "load-guard",
    "store-guard",
    "store-source",
    "paged-store-helper",
    "branch-taken",
    "chained-over-budget",
    "jalr-misaligned",
};
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
    uint64_t unsupported_opcode_total = 0;
    uint64_t unsupported_opcode_distinct = 0;

    for (uint32_t opcode = 0; opcode <= RV64_OPCODE_MASK; opcode++)
    {
        const uint64_t count = jit_stats.unsupported_by_opcode[opcode];
        unsupported_opcode_total += count;
        unsupported_opcode_distinct += count != 0 ? 1u : 0u;
    }

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
    Log("jit: translated cross-page blocks = %" PRIu64,
        jit_stats.translated_cross_page_blocks);
    Log("jit: segmented source blocks = %" PRIu64,
        jit_stats.segmented_source_blocks);
    Log("jit: trace blocks = %" PRIu64
        ", trace instructions = %" PRIu64,
        jit_stats.trace_blocks,
        jit_stats.trace_insns);
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
    Log("jit: helper loads = %" PRIu64
        ", helper stores = %" PRIu64,
        jit_stats.helper_load_count,
        jit_stats.helper_store_count);
    Log("jit: direct links taken = %" PRIu64
        ", misses = %" PRIu64,
        jit_stats.direct_link_taken_count,
        jit_stats.direct_link_miss_count);
    Log("jit: direct branch links taken = %" PRIu64,
        jit_stats.direct_branch_link_taken_count);
    Log("jit: direct guarded links taken = %" PRIu64,
        jit_stats.direct_guarded_link_taken_count);
    Log("jit: ifetch generation fast hits = %" PRIu64
        ", revalidations = %" PRIu64
        ", bumps = %" PRIu64,
        jit_stats.ifetch_generation_fast_hits,
        jit_stats.ifetch_generation_revalidations,
        jit_stats.ifetch_generation_bumps);
    Log("jit: source reverse invalidations = %" PRIu64
        ", full scans = %" PRIu64,
        jit_stats.source_reverse_invalidations,
        jit_stats.source_full_invalidation_scans);
    Log("jit: unsupported opcodes total = %" PRIu64
        ", distinct = %" PRIu64,
        unsupported_opcode_total,
        unsupported_opcode_distinct);
    for (uint32_t opcode = 0; opcode <= RV64_OPCODE_MASK; opcode++)
    {
        if (jit_stats.unsupported_by_opcode[opcode] != 0)
        {
            Log("jit: unsupported opcode 0x%02x = %" PRIu64,
                opcode,
                jit_stats.unsupported_by_opcode[opcode]);
        }
    }
    for (uint32_t reason = 0; reason < RV64_JIT_BLOCK_END_COUNT; reason++)
    {
        Log("jit: block end %s = %" PRIu64,
            jit_block_end_reason_names[reason],
            jit_stats.block_end_by_reason[reason]);
    }
    for (uint32_t reason = 0; reason < RV64_JIT_SIDE_EXIT_COUNT; reason++)
    {
        Log("jit: side exit %s = %" PRIu64,
            jit_side_exit_reason_names[reason],
            jit_stats.side_exit_by_reason[reason]);
    }
    Log("jit: zero side exits = %" PRIu64,
        jit_stats.zero_side_exits);
#else
    if (jit_stats_enabled)
    {
        Log("jit: stats requested, but this binary was built without RV64_JIT_STATS=1");
    }
#endif
}
#endif

#ifndef CONFIG_RV64
#include <isa-jit.h>
#include <isa.h>
#include <cpu/difftest.h>
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
 * scalar memory accesses and JALR, either emit guarded native side exits or fall
 * back to the strict fast/interpreter path when the native emitter cannot prove
 * the same trap ordering.
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

#define RV32_JIT_BLOCK_MAX_INSNS 64u
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
 * Keep enough spare arena space for one worst-case block before compiling.  A
 * 64-instruction block can contain guarded memory operations with cold helper
 * exits, so the old 4 KiB margin was too tight near the end of the arena.
 */
#define RV32_JIT_BLOCK_CODE_HEADROOM (32u * 1024u)
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
#if RV32_JIT_ENABLED
/* Sticky flag set when executable memory allocation fails. */
static bool jit_disabled = false;
#endif
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
/* Public guard used by fast PMEM stores before calling invalidation hooks. */
bool isa_jit_invalidation_active = false;

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
 * This deliberately keeps a small translation cache local to the JIT helper
 * path so generated code does not need to inline page walks.  A
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
#if RV32_JIT_ENABLED
    const char *value = getenv(name);
    return value != NULL && value[0] != '\0' &&
           !(value[0] == '0' && value[1] == '\0');
#else
    (void)name;
    return false;
#endif
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
 * Deliver a strict RISC-V trap from generated code.
 *
 * The native block calls this only after it has flushed earlier dirty guest
 * registers and before it performs the trapping instruction's destination
 * register write or memory write.  That preserves the same ordering as the
 * interpreter helpers: previous instructions are visible, the faulting memory
 * operation has no side effect, and mepc/mtval identify the instruction and
 * effective address that caused the trap.
 */
static void jit_raise_trap_tval(uint32_t cause, vaddr_t pc, vaddr_t tval)
{
    cpu.pc = isa_raise_intr_tval((word_t)cause, pc, (word_t)tval);
    difftest_skip_ref();
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
    if (jit_disabled)
    {
        return false;
    }

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
    isa_jit_invalidation_active = true;
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

/* Store EAX into `cpu.pc`, used after generated code computes a jump target. */
static bool emit_store_pc_eax(rv32_jit_writer_t *w)
{
    const uint32_t off = (uint32_t)offsetof(CPU_state, pc);

    /* mov dword ptr [r11 + pc_off], eax */
    return emit_u8(w, 0x41) && emit_u8(w, 0x89) && emit_u8(w, 0x83) && emit_u32(w, off);
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

/* Test selected low address bits without modifying EAX. */
static bool emit_test_eax_imm(rv32_jit_writer_t *w, uint32_t value)
{
    /* test eax, imm32 */
    return emit_u8(w, 0xa9) && emit_u32(w, value);
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

/* Load an immediate into EDI, the first System V integer argument register. */
static bool emit_mov_edi_imm(rv32_jit_writer_t *w, uint32_t value)
{
    return emit_u8(w, 0xbf) && emit_u32(w, value);
}

/* Load an immediate into ESI, the second System V integer argument register. */
static bool emit_mov_esi_imm(rv32_jit_writer_t *w, uint32_t value)
{
    return emit_u8(w, 0xbe) && emit_u32(w, value);
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
 * Emit a native side exit for a strict trap whose mtval is already in EAX.
 * Earlier dirty guest-register cache slots are flushed before the helper call,
 * but the trapping instruction's own destination write is emitted only on the
 * normal path after its guard has succeeded.
 */
static bool emit_trap_side_exit_from_eax(rv32_jit_writer_t *w,
                                         rv32_jit_reg_cache_t *regs,
                                         uint32_t cause, vaddr_t cur_pc,
                                         uint32_t exit_count,
                                         bool loop_count_needed)
{
    return jit_reg_emit_flush_all_dirty(w, regs) &&
           emit_mov_edx_eax(w) &&
           emit_mov_edi_imm(w, cause) &&
           emit_mov_esi_imm(w, cur_pc) &&
           emit_call_abs(w, (uintptr_t)jit_raise_trap_tval) &&
           (loop_count_needed
                ? emit_epilogue_return_loop_count(w, exit_count)
                : emit_epilogue_return_count(w, exit_count));
}

/*
 * Emit the strict alignment check shared by native JIT loads and stores.
 *
 * EAX contains the guest effective address.  Byte accesses are always naturally
 * aligned, so they need no guard.  Halfword and word accesses branch over an
 * out-of-line trap side exit when the low address bits are zero.  The side exit
 * flushes guest-register cache state produced by earlier instructions in this
 * block, passes the original effective address as mtval, and returns to
 * cpu_exec() after reporting that the trapping instruction retired.
 */
static bool emit_memory_alignment_guard(rv32_jit_writer_t *w,
                                        rv32_jit_reg_cache_t *regs,
                                        uint32_t len, uint32_t cause,
                                        vaddr_t cur_pc, uint32_t exit_count,
                                        bool loop_count_needed)
{
    if (len <= 1)
    {
        return true;
    }

    uint8_t *aligned_disp = NULL;

    if (!emit_test_eax_imm(w, len - 1u) ||
        !emit_jcc_rel32_placeholder(w, 0x84, &aligned_disp) ||
        !emit_trap_side_exit_from_eax(w, regs, cause, cur_pc, exit_count,
                                      loop_count_needed))
    {
        return false;
    }

    patch_rel32(aligned_disp, w->cur);
    return true;
}

/*
 * Translate one RV32 load instruction.
 *
 * The fast path performs direct PMEM loads inside the native block when Bare
 * mode or a simple Sv32 JIT TLB hit proves the final physical range.  The slow
 * path flushes dirty registers, sets cpu.pc to the load instruction, calls the
 * typed load helper, and reloads base registers that helper calls may clobber.
 */
static bool emit_load_instr(rv32_jit_writer_t *w, rv32_jit_reg_cache_t *regs,
                            uint32_t instr, vaddr_t cur_pc,
                            uint32_t exit_count, bool loop_count_needed)
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
            !emit_memory_alignment_guard(w, regs, len,
                                         RISCV32_CAUSE_LOAD_ADDR_MISALIGNED,
                                         cur_pc, exit_count,
                                         loop_count_needed) ||
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
        !emit_memory_alignment_guard(w, regs, len,
                                     RISCV32_CAUSE_LOAD_ADDR_MISALIGNED,
                                     cur_pc, exit_count,
                                     loop_count_needed) ||
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
static bool emit_store_instr(rv32_jit_writer_t *w, rv32_jit_reg_cache_t *regs,
                             uint32_t instr, vaddr_t cur_pc,
                             vaddr_t next_pc, uint32_t exit_count,
                             bool loop_count_needed)
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
            !emit_memory_alignment_guard(w, regs, len,
                                         RISCV32_CAUSE_STORE_ADDR_MISALIGNED,
                                         cur_pc, exit_count,
                                         loop_count_needed) ||
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
            !(loop_count_needed
                  ? emit_epilogue_return_loop_count(w, exit_count)
                  : emit_epilogue_return_count(w, exit_count)))
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
        !emit_memory_alignment_guard(w, regs, len,
                                     RISCV32_CAUSE_STORE_ADDR_MISALIGNED,
                                     cur_pc, exit_count,
                                     loop_count_needed) ||
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
        !(loop_count_needed
              ? emit_epilogue_return_loop_count(w, exit_count)
              : emit_epilogue_return_count(w, exit_count)))
    {
        return false;
    }

    patch_rel32(done_disp, w->cur);
    return true;
}

/* Dispatch a decoded LOAD or STORE opcode to its specialised emitter. */
static bool emit_load_store_instr(rv32_jit_writer_t *w,
                                  rv32_jit_reg_cache_t *regs, uint32_t instr, vaddr_t cur_pc,
                                  uint32_t exit_count,
                                  bool loop_count_needed)
{
    const uint32_t opcode = instr & 0x7fu;

    if (opcode == 0x03)
    {
        return emit_load_instr(w, regs, instr, cur_pc, exit_count,
                               loop_count_needed);
    }

    if (opcode == 0x23)
    {
        return emit_store_instr(w, regs, instr, cur_pc, cur_pc + 4u,
                                exit_count, loop_count_needed);
    }

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
    case 0x03: /* LOAD */
    case 0x23: /* STORE */
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
                                    rv32_jit_reg_cache_t *regs, uint32_t instr,
                                    vaddr_t pc, uint32_t exit_count)
{
    const uint32_t opcode = instr & 0x7fu;
    const uint32_t rd = bits(instr, 11, 7);
    const uint32_t funct3 = bits(instr, 14, 12);
    const uint32_t rs1 = bits(instr, 19, 15);

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
         * JALR computes and aligns the target before writing the link register.
         * The misaligned-target side exit must therefore run before rd is
         * changed, otherwise rd == rs1 cases would observe a link write that
         * should not happen for the trapping instruction.
         */
        uint8_t *aligned_disp = NULL;

        if (!jit_reg_read_eax(w, regs, rs1) ||
            !emit_add_eax_imm(w, (uint32_t)imm_i(instr)) ||
            !emit_and_eax_imm(w, 0xfffffffeu) ||
            !emit_test_eax_imm(w, 0x3u) ||
            !emit_jcc_rel32_placeholder(w, 0x84, &aligned_disp) ||
            !emit_trap_side_exit_from_eax(w, regs,
                                          RISCV32_CAUSE_INST_ADDR_MISALIGNED,
                                          pc, exit_count, false))
        {
            return false;
        }

        patch_rel32(aligned_disp, w->cur);

        return emit_store_pc_eax(w) &&
               emit_mov_eax_imm(w, pc + 4u) &&
               jit_reg_write_eax(w, regs, rd) &&
               jit_reg_emit_flush_all_dirty(w, regs);
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

/* Public hook: discard only the RV32 JIT's private Sv32 data translations. */
void isa_jit_flush_data_tlb(void)
{
    jit_tlb_flush();
}

/* Public hook: react to PMEM writes that can stale native code or JIT translations. */
void isa_jit_invalidate_paddr(paddr_t addr, int len)
{
    JIT_STAT_INC(invalidation_requests);

    /*
     * Physical writes are the common point shared by interpreter stores, JIT
     * helper stores, interpreter stores, and devices.  Two independent JIT caches
     * can become stale here:
     *
     *   1. native blocks translated from overwritten instruction bytes;
     *   2. local Sv32 translations whose root or level-0 PTE page was modified.
     *
     * The source-code check below uses the half-open interval [addr, addr + len).
     */

    if (len <= 0 || jit_code == NULL)
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
     * the same virtual PCs point at different physical source bytes. Re-translate
     * every virtual page touched by this block before trusting cached native code.
     * Most blocks fit in one page, while hot loops near a page boundary still keep
     * their native block if the recorded physical source bytes are unchanged.
     */

    if ((cpu.csr.satp & 0x80000000u) != 0)
    {
        uint32_t offset = 0;

        while (offset < block->source_len)
        {
            const vaddr_t check_pc = pc + (vaddr_t)offset;
            paddr_t now = 0;

            /*
             * A translation failure is treated as a cache miss. That keeps the JIT out
             * of cases where the normal interpreter path needs to raise or report the
             * underlying memory problem.
             */
            if (!jit_translate_ifetch(check_pc, &now) ||
                now != block->paddr_start + (paddr_t)offset)
            {
                return false;
            }

            const uint32_t page_left =
                PAGE_SIZE - (uint32_t)(check_pc & PAGE_MASK);
            const uint32_t remaining = block->source_len - offset;
            offset += page_left < remaining ? page_left : remaining;
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

    if (jit_code_used + RV32_JIT_BLOCK_CODE_HEADROOM > RV32_JIT_CODE_SIZE)
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
            if (!emit_control_flow_instr(&w, &regs, instr, cur_pc, count + 1u))
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

            if (!emit_load_store_instr(&w, &regs, instr, cur_pc, count + 1u,
                                       loop_count_needed))
            {
                /*
                 * Emitters may fail after writing a prefix of an x86 instruction. Roll
                 * back to the last complete native instruction before falling back.
                 */
                w.cur = instr_start;
                jit_reg_cache_restore(&regs, &regs_start);
                break;
            }
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
#endif
