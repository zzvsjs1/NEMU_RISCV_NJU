#ifndef __RISCV64_JIT_INTERNAL_H__
#define __RISCV64_JIT_INTERNAL_H__

#include <generated/autoconf.h>

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
#include <string.h>

/* Private declarations shared by the normal RV64 JIT compilation units. */
#if defined(__x86_64__) && defined(CONFIG_RV64_JIT) && \
    defined(CONFIG_TARGET_NATIVE_ELF) && !defined(CONFIG_TRACE) && \
    !defined(CONFIG_DIFFTEST) && !defined(CONFIG_WATCHPOINT) && \
    !defined(CONFIG_MTRACE) && !defined(CONFIG_FTRACE)
#define RV64_JIT_ENABLED 1
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

/* RISC-V funct3 values used by load, store, branch and integer emitters. */
#define RV64_FUNCT3_LB 0x0u
#define RV64_FUNCT3_LH 0x1u
#define RV64_FUNCT3_LW 0x2u
#define RV64_FUNCT3_LD 0x3u
#define RV64_FUNCT3_LBU 0x4u
#define RV64_FUNCT3_LHU 0x5u
#define RV64_FUNCT3_LWU 0x6u
#define RV64_FUNCT3_SB 0x0u
#define RV64_FUNCT3_SH 0x1u
#define RV64_FUNCT3_SW 0x2u
#define RV64_FUNCT3_SD 0x3u
#define RV64_FUNCT3_BEQ 0x0u
#define RV64_FUNCT3_BNE 0x1u
#define RV64_FUNCT3_BLT 0x4u
#define RV64_FUNCT3_BGE 0x5u
#define RV64_FUNCT3_BLTU 0x6u
#define RV64_FUNCT3_BGEU 0x7u
#define RV64_FUNCT3_ADD_SUB 0x0u
#define RV64_FUNCT3_SLL 0x1u
#define RV64_FUNCT3_SLT 0x2u
#define RV64_FUNCT3_SLTU 0x3u
#define RV64_FUNCT3_XOR 0x4u
#define RV64_FUNCT3_SRL_SRA 0x5u
#define RV64_FUNCT3_OR 0x6u
#define RV64_FUNCT3_AND 0x7u

/* RISC-V funct6/funct7 values used to distinguish shift and OP variants. */
#define RV64_FUNCT6_SHIFT_LOGICAL 0x00u
#define RV64_FUNCT6_SHIFT_ARITH 0x10u
#define RV64_FUNCT7_BASE 0x00u
#define RV64_FUNCT7_MULDIV 0x01u
#define RV64_FUNCT7_SUB_SRA 0x20u
#define RV64_OP_KEY(funct7, funct3) (((funct7) << 3) | (funct3))

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
 * A trace-length region covers RV64_JIT_TRACE_MAX_INSNS fixed-width
 * instructions.  Keep the formula explicit so the source metadata grows with
 * the trace limit instead of relying on an old basic-block constant.
 */
#define RV64_JIT_BLOCK_MAX_SOURCE_SEGMENTS \
    (((RV64_JIT_TRACE_MAX_INSNS * RV64_INSN_SIZE) + PAGE_SIZE - 1u) / \
         PAGE_SIZE + \
     1u)
#define RV64_JIT_SV39_LEVELS 3u
#define RV64_JIT_BLOCK_MAX_IFETCH_PT_PAGES \
    (RV64_JIT_BLOCK_MAX_SOURCE_SEGMENTS * RV64_JIT_SV39_LEVELS)
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
#define RV64_JIT_SV39_VPN_BITS 9u
#define RV64_JIT_SV39_VPN_MASK \
    ((word_t)((1u << RV64_JIT_SV39_VPN_BITS) - 1u))
#define RV64_JIT_SV39_VPN_SHIFT(level) \
    (PAGE_SHIFT + (level) * RV64_JIT_SV39_VPN_BITS)
#define RV64_JIT_SV39_CANONICAL_SIGN_BIT 38u
#define RV64_JIT_SV39_CANONICAL_HIGH_SHIFT 39u
#define RV64_JIT_SV39_CANONICAL_HIGH_BITS 25u
#define RV64_JIT_SV39_LEVEL1_LOW_PPN_MASK 0x1ffu
#define RV64_JIT_SV39_LEVEL2_LOW_PPN_MASK 0x3ffffu
#define RV64_JIT_PTE_SIZE (sizeof(uint64_t))
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
#define RV64_JIT_DATA_TLB_ENTRY_SHIFT 6u
#define RV64_JIT_DATA_TLB_VPN_MIX_SHIFT 9u
#define RV64_JIT_DATA_TLB_SATP_MIX_SHIFT 12u
#define RV64_JIT_DATA_TLB_STATE_PRIV_MASK 0x3u
#define RV64_JIT_DATA_TLB_STATE_SUM (1u << 2)
#define RV64_JIT_DATA_TLB_STATE_MXR (1u << 3)

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
    uint64_t pt_pages[RV64_JIT_SV39_LEVELS];
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
    uint8_t *slow_disps[RV64_JIT_DIRECT_LINK_MISS_PATCHES];
    uint32_t count;
} rv64_jit_tlb_guard_patch_t;


/* Extract an inclusive bit range from a 32-bit RISC-V instruction. */
static inline uint32_t bits(uint32_t value, int hi, int lo)
{
    /*
     * All current callers pass 0 <= lo <= hi < 32. The `(1u << width) - 1`
     * mask is therefore well-defined and keeps only the requested field.
     */
    return (value >> lo) & ((1u << (hi - lo + 1)) - 1u);
}

/* Sign-extend an instruction field whose sign bit is at width - 1. */
static inline int64_t sext(uint32_t value, unsigned width)
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
static inline int64_t imm_i(uint32_t instr)
{
    return sext(bits(instr, 31, 20), 12);
}

/* Decode the S-format store immediate as a signed XLEN value. */
static inline int64_t imm_s(uint32_t instr)
{
    const uint32_t imm = bits(instr, 11, 7) | (bits(instr, 31, 25) << 5);
    return sext(imm, 12);
}

/* Decode the B-format immediate, including the implicit low zero bit. */
static inline int64_t imm_b(uint32_t instr)
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
static inline int64_t imm_u_sext(uint32_t instr)
{
    /* `0xfffff000` keeps imm[31:12], the upper 20-bit U-type payload. */
    return (int64_t)(int32_t)(instr & 0xfffff000u);
}

/* Decode the J-format immediate, including the implicit low zero bit. */
static inline int64_t imm_j(uint32_t instr)
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
static inline uint32_t jit_gpr_offset(uint32_t reg)
{
    return (uint32_t)(offsetof(CPU_state, gpr) + reg * sizeof(cpu.gpr[0]));
}

/* Return the byte offset of the guest PC inside CPU_state. */
static inline uint32_t jit_pc_offset(void)
{
    return (uint32_t)offsetof(CPU_state, pc);
}


extern rv64_jit_block_t rv64_jit_cache[RV64_JIT_CACHE_SIZE];
extern rv64_jit_data_tlb_entry_t rv64_jit_data_tlb[RV64_JIT_DATA_TLB_SIZE];
extern uint16_t rv64_jit_data_tlb_pt_page_refs[RV64_JIT_PMEM_PAGE_COUNT];
extern uint16_t rv64_jit_ifetch_pt_page_refs[RV64_JIT_PMEM_PAGE_COUNT];
extern uint16_t rv64_jit_source_chunk_refs[RV64_JIT_PMEM_CHUNK_COUNT];
extern uint32_t rv64_jit_source_chunk_heads[RV64_JIT_PMEM_CHUNK_COUNT];
extern rv64_jit_source_link_t rv64_jit_source_links[RV64_JIT_SOURCE_LINK_COUNT];
extern uint32_t rv64_jit_source_link_free_head;
extern uint8_t *rv64_jit_code;
extern size_t rv64_jit_code_used;
extern rv64_jit_stats_t rv64_jit_stats;
extern uint64_t rv64_jit_ifetch_generation;
extern volatile uint32_t rv64_jit_entry_budget;
extern volatile uint32_t rv64_jit_loop_extra;

#if RV64_JIT_STATS
#define JIT_STAT_INC(field) \
    do \
    { \
        rv64_jit_stats.field++; \
    } while (0)
#define JIT_STAT_ADD(field, value) \
    do \
    { \
        rv64_jit_stats.field += (value); \
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


void rv64_jit_stat_unsupported_opcode(uint32_t instr);
void rv64_jit_stat_block_end(rv64_jit_block_end_reason_t reason);
bool rv64_jit_direct_link_enabled(void);
void rv64_jit_ifetch_generation_bump(void);
void rv64_jit_data_tlb_flush(void);
bool rv64_jit_write_may_touch_data_tlb_page_table(paddr_t addr, int len);
bool rv64_jit_write_may_touch_ifetch_page_table(paddr_t addr, int len);
uint32_t rv64_jit_data_tlb_state(int type);
uint32_t rv64_jit_ifetch_state(void);
uint64_t rv64_jit_load_i8(vaddr_t addr);
uint64_t rv64_jit_load_i16(vaddr_t addr);
uint64_t rv64_jit_load_i32(vaddr_t addr);
uint64_t rv64_jit_load_u64(vaddr_t addr);
uint64_t rv64_jit_load_u8(vaddr_t addr);
uint64_t rv64_jit_load_u16(vaddr_t addr);
uint64_t rv64_jit_load_u32(vaddr_t addr);
void rv64_jit_store_vaddr(vaddr_t addr, uint32_t len, uint64_t data);
uint32_t rv64_jit_store_pmem_continue(paddr_t addr, uint32_t len, uint64_t data);
uint64_t rv64_jit_m_result(uint64_t lhs, uint64_t rhs, uint32_t instr);
size_t rv64_jit_align_up(size_t value, size_t align);
bool rv64_jit_source_chunk_range(paddr_t addr, uint32_t len,
                                 size_t *first, size_t *last);
bool rv64_jit_source_builder_append(rv64_jit_source_builder_t *source,
                                    paddr_t paddr, uint32_t len);
void rv64_jit_ifetch_refs_ref(const rv64_jit_block_t *block);
void rv64_jit_source_reverse_map_reset(void);
void rv64_jit_source_reverse_map_add(rv64_jit_block_t *block);
void rv64_jit_source_chunks_ref(const rv64_jit_block_t *block);
bool rv64_jit_write_may_touch_source_chunk(paddr_t addr, int len);
void rv64_jit_block_discard(rv64_jit_block_t *block);
bool rv64_jit_block_source_overlaps(const rv64_jit_block_t *block,
                                    paddr_t addr, int len);
rv64_jit_block_t *rv64_jit_cache_slot_context(vaddr_t pc, word_t satp,
                                              uint32_t ifetch_state);
rv64_jit_block_t *rv64_jit_cache_slot(vaddr_t pc);
bool rv64_jit_code_init(void);
void rv64_jit_arena_reset(void);
bool rv64_jit_block_matches(rv64_jit_block_t *block, vaddr_t pc);
void rv64_jit_mark_unsupported(vaddr_t pc, paddr_t paddr, bool translated);
bool rv64_jit_translate_ifetch_collect(vaddr_t pc, paddr_t *paddr,
                                       bool *translated,
                                       rv64_jit_ifetch_ref_builder_t *refs);
bool rv64_jit_translate_ifetch_ex(vaddr_t pc, paddr_t *paddr,
                                  bool *translated);
void rv64_jit_reg_cache_init(rv64_jit_reg_cache_t *regs);
void rv64_jit_reg_cache_restore(rv64_jit_reg_cache_t *regs,
                                const rv64_jit_reg_cache_t *snapshot);
bool rv64_jit_emit_prologue(rv64_jit_writer_t *w);
bool rv64_jit_emit_direct_link_exit(rv64_jit_writer_t *w,
                                    rv64_jit_reg_cache_t *regs,
                                    vaddr_t target_pc,
                                    uint32_t completed_count,
                                    bool source_uses_data_state,
                                    uint64_t *extra_taken_counter);
bool rv64_jit_emit_plain_block_exit(rv64_jit_writer_t *w,
                                    rv64_jit_reg_cache_t *regs,
                                    vaddr_t target_pc,
                                    uint32_t completed_count);
bool rv64_jit_emit_jump_instr(rv64_jit_writer_t *w,
                              rv64_jit_reg_cache_t *regs,
                              uint32_t instr, vaddr_t pc,
                              uint32_t completed_count,
                              bool loop_count_needed,
                              bool source_uses_data_state);
bool rv64_jit_emit_load_instr(rv64_jit_writer_t *w,
                              rv64_jit_reg_cache_t *regs,
                              uint32_t instr, vaddr_t pc,
                              uint32_t completed_count,
                              bool loop_count_needed);
bool rv64_jit_emit_store_instr(rv64_jit_writer_t *w,
                               rv64_jit_reg_cache_t *regs,
                               uint32_t instr, vaddr_t pc,
                               vaddr_t next_pc,
                               uint32_t completed_count,
                               bool loop_count_needed);
bool rv64_jit_emit_branch(rv64_jit_writer_t *w,
                          rv64_jit_reg_cache_t *regs,
                          uint32_t instr, vaddr_t pc,
                          vaddr_t block_start_pc,
                          const uint8_t *block_start_native,
                          bool chain_safe,
                          bool *branch_chained,
                          uint32_t exit_count,
                          bool source_uses_data_state);
bool rv64_jit_emit_instr(rv64_jit_writer_t *w,
                         rv64_jit_reg_cache_t *regs,
                         uint32_t instr, vaddr_t pc,
                         uint32_t exit_count);
rv64_jit_block_t *rv64_jit_compile_block(vaddr_t pc, uint32_t max_insns);

#endif /* CONFIG_RV64 */

#endif /* __RISCV64_JIT_INTERNAL_H__ */
