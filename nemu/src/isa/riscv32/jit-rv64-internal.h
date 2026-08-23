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

/*
 * Retain the compact RV64 JIT vocabulary used throughout the emitter, while
 * deriving architectural values from the ISA-wide definitions.  This keeps
 * the JIT and interpreter decoders in agreement when the shared definitions
 * are audited or extended.
 */
#define RV64_INSN_SIZE RISCV_BASE_INSN_BYTES
#define RV64_OPCODE_MASK RISCV_OPCODE_MASK
/*
 * This NEMU RV64 configuration does not implement the compressed C extension,
 * so the guest architecture has IALIGN=32.  JAL, JALR, and a taken conditional
 * branch therefore require the target address bits selected by this mask to be
 * zero.
 */
#define RV64_IALIGN_MASK RISCV_IALIGN_32_MASK

/* RISC-V opcodes used by this first native subset. */
#define RV64_OPCODE_LOAD RISCV_OPCODE_LOAD
#define RV64_OPCODE_OP_IMM RISCV_OPCODE_OP_IMM
#define RV64_OPCODE_AUIPC RISCV_OPCODE_AUIPC
#define RV64_OPCODE_OP_IMM_32 RISCV_OPCODE_OP_IMM_32
#define RV64_OPCODE_STORE RISCV_OPCODE_STORE
#define RV64_OPCODE_OP RISCV_OPCODE_OP
#define RV64_OPCODE_LUI RISCV_OPCODE_LUI
#define RV64_OPCODE_OP_32 RISCV_OPCODE_OP_32
#define RV64_OPCODE_BRANCH RISCV_OPCODE_BRANCH
#define RV64_OPCODE_JALR RISCV_OPCODE_JALR
#define RV64_OPCODE_JAL RISCV_OPCODE_JAL

/* Floating-point major opcodes lowered through the shared SoftFloat helper. */
#define RV64_FP_OPCODE_LOAD RISCV_FP_OPCODE_LOAD
#define RV64_FP_OPCODE_STORE RISCV_FP_OPCODE_STORE
#define RV64_FP_OPCODE_FMADD RISCV_FP_OPCODE_FMADD
#define RV64_FP_OPCODE_FMSUB RISCV_FP_OPCODE_FMSUB
#define RV64_FP_OPCODE_FNMSUB RISCV_FP_OPCODE_FNMSUB
#define RV64_FP_OPCODE_FNMADD RISCV_FP_OPCODE_FNMADD
#define RV64_FP_OPCODE_OP RISCV_FP_OPCODE_OP

/* Architectural register and JALR values used by generic emitter logic. */
#define RV64_GPR_ZERO RISCV_GPR_ZERO
#define RV64_GPR_LINK RISCV_GPR_LINK
#define RV64_FUNCT3_JALR RISCV_JALR_FUNCT3
#define RV64_JALR_TARGET_LSB_MASK RISCV_JALR_TARGET_LSB_MASK

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
/* Shared shifts keep the C lookup and emitted runtime return lookup identical. */
#define RV64_JIT_CACHE_PC_SHIFT 2u
#define RV64_JIT_CACHE_SATP_MIX_SHIFT 12u
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
#define RV64_JIT_SV39_LEVELS RISCV64_SV39_LEVELS
#define RV64_JIT_BLOCK_MAX_IFETCH_PT_PAGES \
    (RV64_JIT_BLOCK_MAX_SOURCE_SEGMENTS * RV64_JIT_SV39_LEVELS)
#define RV64_JIT_BLOCK_MAX_SOURCE_CHUNKS \
    (((RV64_JIT_TRACE_MAX_INSNS * RV64_INSN_SIZE) + \
      RV64_JIT_SOURCE_CHUNK_SIZE - 1u) / \
         RV64_JIT_SOURCE_CHUNK_SIZE + \
     RV64_JIT_BLOCK_MAX_SOURCE_SEGMENTS)
#define RV64_JIT_SOURCE_LINK_NULL 0u
#define RV64_JIT_SOURCE_LINK_COUNT \
    ((size_t)RV64_JIT_CACHE_SIZE * \
         (size_t)RV64_JIT_BLOCK_MAX_SOURCE_CHUNKS + \
     1u)
/*
 * One direct link can emit eight miss branches: valid, PC, satp, instruction
 * privilege, optional data privilege, translation generation, body entry, and
 * execution-budget guards.  The inline DTLB proof has six: valid, satp, VPN,
 * privilege state, access permission, and same-page byte range.  Keeping these
 * independent maxima prevents a new guard in one path from silently borrowing
 * spare capacity that happened to belong to the other.
 */
#define RV64_JIT_DIRECT_LINK_GUARD_COUNT 8u
#define RV64_JIT_DIRECT_LINK_MAX_MISS_PATCHES \
    RV64_JIT_DIRECT_LINK_GUARD_COUNT
#define RV64_JIT_BLOCK_MAX_LINKS (RV64_JIT_TRACE_MAX_INSNS + 1u)
#define RV64_JIT_DTLB_MAX_SLOW_PATCHES 6u
/*
 * A non-linking compiler jump table needs more retained targets than the
 * mutable two-way call/return PIC below. Sixteen entries hold BF's measured
 * eight-target working set while keeping each source-owned sidecar to four
 * cache lines. The raw index consumes aligned instruction-address bits; every
 * entry remains a hint because the live authoritative slot and its exact
 * publication generation are validated before use.
 */
#define RV64_JIT_INDIRECT_JUMP_CACHE_ENTRIES 16u
#define RV64_JIT_INDIRECT_JUMP_CACHE_INDEX_MASK \
    (RV64_JIT_INDIRECT_JUMP_CACHE_ENTRIES - 1u)
#define RV64_JIT_INDIRECT_JUMP_CACHE_ENTRY_SHIFT 4u
#define RV64_JIT_INDIRECT_JUMP_CACHE_BYTE_MASK \
    (RV64_JIT_INDIRECT_JUMP_CACHE_INDEX_MASK << RV64_JIT_CACHE_PC_SHIFT)
#define RV64_JIT_INDIRECT_JUMP_CACHE_OFFSET_SHIFT \
    (RV64_JIT_INDIRECT_JUMP_CACHE_ENTRY_SHIFT - RV64_JIT_CACHE_PC_SHIFT)
#define RV64_JIT_INDIRECT_JUMP_CACHE_MAX_FIXUPS 1u
#define RV64_JIT_INDIRECT_JUMP_CACHE_LINE_SIZE 64u
#define RV64_JIT_INDIRECT_JUMP_CACHE_MAX_ALLOCATION \
    ((RV64_JIT_INDIRECT_JUMP_CACHE_LINE_SIZE - 1u) + \
     sizeof(rv64_jit_indirect_jump_cache_t))
/*
 * A JALR terminates its native block, so one block can own at most one dynamic
 * target site. Two ways retain both targets of a common alternating return or
 * function-pointer call without turning a megamorphic site into an unbounded
 * code or metadata allocation.
 */
#define RV64_JIT_INDIRECT_PIC_WAYS 2u
/*
 * The selector route encoding, primary/secondary hit statistics, and the
 * cache-line-sized hot header all deliberately describe exactly two ways.
 * Changing the constant therefore requires redesigning those three consumers,
 * rather than merely enlarging their arrays.
 */
_Static_assert(
    RV64_JIT_INDIRECT_PIC_WAYS == 2u,
    "RV64 JIT indirect PIC route ABI requires exactly two ways");
#define RV64_JIT_INDIRECT_PIC_MAX_FIXUPS 2u
#define RV64_JIT_INDIRECT_PIC_LINE_SIZE 64u
/*
 * Preserve patching through occasional phase changes, but stop rewriting a
 * source that repeatedly exceeds its two-way working set.  Eight occupied-way
 * replacements bound the cold-path cost while leaving stable mono- and
 * bimorphic sites permanently on their shortest native route.
 */
#define RV64_JIT_INDIRECT_PIC_PATCH_REPLACEMENT_LIMIT 8u
#define RV64_JIT_INDIRECT_PIC_MAX_ALLOCATION \
    ((RV64_JIT_INDIRECT_PIC_LINE_SIZE - 1u) + \
     sizeof(rv64_jit_indirect_pic_t))
/*
 * Each direct-MMIO candidate adds a range guard and a full native load arm to
 * every eligible Bare load site.  Bound that expansion; later maps simply keep
 * using the correct helper path.
 */
#define RV64_JIT_DIRECT_MMIO_MAX_MAPS 4u
/*
 * Direct-write guards are emitted at each eligible Bare store site. Keep the
 * expansion bounded independently from direct-readable maps; excess regions
 * retain the correct helper path.
 */
#define RV64_JIT_DIRECT_MMIO_MAX_REGIONS 4u
/*
 * A compiled block may cache four exact direct-MMIO routes in one trailing
 * cache line.  Limiting the sidecar keeps allocation and fixup metadata
 * bounded; later eligible sites retain the complete uncached classifier.
 */
#define RV64_JIT_MMIO_ROUTE_MAX_SITES 4u
#define RV64_JIT_MMIO_ROUTE_LINE_SIZE 64u
#define RV64_JIT_MMIO_ROUTE_NO_SITE UINT8_MAX
#define RV64_JIT_DIRECT_MMIO_MAX_CLASSIFIERS                           \
    ((RV64_JIT_DIRECT_MMIO_MAX_MAPS >                                  \
      RV64_JIT_DIRECT_MMIO_MAX_REGIONS)                                \
         ? RV64_JIT_DIRECT_MMIO_MAX_MAPS                               \
         : RV64_JIT_DIRECT_MMIO_MAX_REGIONS)
#define RV64_JIT_MMIO_ROUTE_MAX_FIXUPS                                  \
    (RV64_JIT_MMIO_ROUTE_MAX_SITES *                                    \
     (2u + 3u * RV64_JIT_DIRECT_MMIO_MAX_CLASSIFIERS))
#define RV64_JIT_MMIO_ROUTE_MAX_ALLOCATION                              \
    ((RV64_JIT_MMIO_ROUTE_LINE_SIZE - 1u) +                             \
     RV64_JIT_MMIO_ROUTE_LINE_SIZE)
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

/*
 * Keep the established JIT names for readable translation code, but source all
 * architectural paging values from the common Sv39 definitions.
 */
#define RV64_JIT_SATP_MODE_SHIFT RISCV64_SATP_MODE_SHIFT
#define RV64_JIT_SATP_MODE_BARE RISCV_SATP_MODE_BARE
#define RV64_JIT_SATP_MODE_SV39 RISCV64_SATP_MODE_SV39
#define RV64_JIT_SATP_PPN_MASK RISCV64_SATP_PPN_MASK
#define RV64_JIT_SV39_VPN_BITS RISCV64_SV39_VPN_BITS
#define RV64_JIT_SV39_VPN_MASK RISCV64_SV39_VPN_MASK
#define RV64_JIT_SV39_VPN_SHIFT(level) \
    (PAGE_SHIFT + (level) * RV64_JIT_SV39_VPN_BITS)
#define RV64_JIT_SV39_CANONICAL_SIGN_BIT \
    RISCV64_SV39_CANONICAL_SIGN_BIT
#define RV64_JIT_SV39_CANONICAL_HIGH_SHIFT \
    RISCV64_SV39_CANONICAL_HIGH_SHIFT
#define RV64_JIT_SV39_CANONICAL_HIGH_BITS \
    RISCV64_SV39_CANONICAL_HIGH_BITS
#define RV64_JIT_SV39_LEVEL1_LOW_PPN_MASK \
    RISCV64_SV39_LEVEL1_LOW_PPN_MASK
#define RV64_JIT_SV39_LEVEL2_LOW_PPN_MASK \
    RISCV64_SV39_LEVEL2_LOW_PPN_MASK
#define RV64_JIT_PTE_SIZE RISCV64_SV39_PTE_BYTES
#define RV64_JIT_PTE_V RISCV_PTE_V
#define RV64_JIT_PTE_R RISCV_PTE_R
#define RV64_JIT_PTE_W RISCV_PTE_W
#define RV64_JIT_PTE_X RISCV_PTE_X
#define RV64_JIT_PTE_U RISCV_PTE_U
#define RV64_JIT_PTE_A RISCV_PTE_A
#define RV64_JIT_PTE_D RISCV_PTE_D
#define RV64_JIT_PTE_RWX RISCV_PTE_RWX
#define RV64_JIT_PTE_NON_LEAF_RESERVED RISCV_PTE_NON_LEAF_RESERVED
#define RV64_JIT_PTE_PPN_SHIFT RISCV_PTE_PPN_SHIFT
#define RV64_JIT_PTE_PPN_MASK RISCV64_PTE_PPN_MASK
/*
 * The RV64 JIT has no Svnapot, Svpbmt, or Svrsw60t59b support.  The high Sv39
 * PTE region therefore contains both unsupported extension fields and reserved
 * bits, all of which must fault rather than produce a cached translation.  Add
 * the relevant state to the JIT guards before enabling any of those extensions
 * in NEMU's architectural walker.
 */
#define RV64_JIT_PTE_UNSUPPORTED_HIGH_MASK RISCV64_PTE_UNSUPPORTED_HIGH_MASK
#define RV64_JIT_MSTATUS_MPRV RISCV_MSTATUS_MPRV
#define RV64_JIT_MSTATUS_SUM RISCV_MSTATUS_SUM
#define RV64_JIT_MSTATUS_MXR RISCV_MSTATUS_MXR
#define RV64_JIT_MSTATUS_MPP_SHIFT RISCV_MSTATUS_MPP_SHIFT
#define RV64_JIT_MSTATUS_MPP_MASK RISCV_MSTATUS_MPP_MASK
#define RV64_JIT_DATA_TLB_READ 0x1u
#define RV64_JIT_DATA_TLB_WRITE 0x2u
#define RV64_JIT_DATA_TLB_ENTRY_SHIFT 6u
#define RV64_JIT_DATA_TLB_VPN_MIX_SHIFT 9u
#define RV64_JIT_DATA_TLB_SATP_MIX_SHIFT 12u
#define RV64_JIT_DATA_TLB_STATE_PRIV_MASK 0x3u
#define RV64_JIT_DATA_TLB_STATE_SUM (1u << 2)
#define RV64_JIT_DATA_TLB_STATE_MXR (1u << 3)

/*
 * The completed-store helpers return this one-bit ABI in EAX.  Zero asks
 * generated code to leave the current block because the store reached a CPU
 * boundary or invalidated compiled source or translation state; one proves
 * that execution may continue at the next guest instruction.
 */
enum
{
    RV64_JIT_STORE_MUST_EXIT = 0,
    RV64_JIT_STORE_MAY_CONTINUE = 1,
};

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
 * Source segments map guest PCs to the physical bytes used at compilation and
 * link the block into the reverse invalidation map.  The JIT does not keep a
 * second byte-for-byte snapshot.  Its safety therefore relies on the global
 * NEMU rule that every CPU, helper, DMA, and device write to PMEM calls
 * isa_jit_invalidate_paddr() before stale native code can execute.  Page-table
 * references are stored as PMEM page numbers because a write to any byte in one
 * of those pages can change a translation without changing instruction bytes.
 */
typedef uint32_t (*rv64_jit_entry_t)(void);

typedef struct rv64_jit_block rv64_jit_block_t;
typedef struct rv64_jit_link rv64_jit_link_t;

typedef enum
{
    RV64_JIT_INDIRECT_PIC_RETURN,
    RV64_JIT_INDIRECT_PIC_JALR,
    RV64_JIT_INDIRECT_PIC_KIND_COUNT,
} rv64_jit_indirect_pic_kind_t;

/*
 * Store a stable authoritative slot rather than a native-code pointer. The
 * slot's full-width PC is the target tag, while `target_generation` certifies
 * the exact context-sensitive publication that passed the full lookup. Target
 * invalidation clears the live slot generation before its arena bytes can be
 * reused, so a stale source-side hint cannot authorise replacement code.
 */
typedef struct
{
    uint64_t target_generation;
    rv64_jit_block_t *target_slot;
} rv64_jit_indirect_jump_cache_entry_t;

typedef struct
{
    rv64_jit_indirect_jump_cache_entry_t
        entries[RV64_JIT_INDIRECT_JUMP_CACHE_ENTRIES];
} rv64_jit_indirect_jump_cache_t;

typedef struct
{
    uint8_t *address_immediates[
        RV64_JIT_INDIRECT_JUMP_CACHE_MAX_FIXUPS];
    uint8_t fixup_count;
    bool used;
} rv64_jit_indirect_jump_cache_builder_t;

_Static_assert(
    (RV64_JIT_INDIRECT_JUMP_CACHE_ENTRIES &
     (RV64_JIT_INDIRECT_JUMP_CACHE_ENTRIES - 1u)) == 0,
    "RV64 JIT indirect jump cache entry count must be a power of two");
_Static_assert(RV64_JIT_INDIRECT_JUMP_CACHE_ENTRY_SHIFT >=
                   RV64_JIT_CACHE_PC_SHIFT,
               "RV64 JIT indirect jump cache entries are too small");
_Static_assert(RV64_JIT_INDIRECT_JUMP_CACHE_BYTE_MASK <= INT8_MAX,
               "RV64 JIT indirect jump cache byte mask exceeds imm8");
_Static_assert(
    sizeof(rv64_jit_indirect_jump_cache_entry_t) ==
        (1u << RV64_JIT_INDIRECT_JUMP_CACHE_ENTRY_SHIFT),
    "RV64 JIT indirect jump cache entry shift drifted");
_Static_assert(sizeof(rv64_jit_indirect_jump_cache_t) == 256u,
               "RV64 JIT indirect jump cache sidecar size drifted");

/*
 * A known-target exit owns one mutable link record in its source block's arena
 * sidecar. Dynamic PIC records use the same incoming target-slot lists, but
 * additionally require the exact publication generation learned by the slow
 * path before their selector may reach a native chain entry.
 */
struct rv64_jit_link
{
    uint8_t *selector_disp;
    uint8_t *target_disp;
    const uint8_t *guarded_path;
    const uint8_t *patched_path;
    vaddr_t target_pc;
    word_t target_satp;
    uint64_t target_generation;
    uint32_t target_ifetch_state;
    uint32_t target_slot_index;
    rv64_jit_block_t *source;
    rv64_jit_link_t *slot_prev;
    rv64_jit_link_t *slot_next;
    uint8_t pic_kind;
    uint8_t pic_way;
    bool patch_eligible;
    bool patched;
    bool dynamic;
};

/*
 * Cache the stable address of the authoritative direct-map slot, never a raw
 * native-code address. `target_generation` identifies the exact publication
 * whose complete guards authorised this entry; invalidation or slot collision
 * clears/changes that value before another native body can be selected.
 */
typedef struct
{
    vaddr_t target_pc;
    uint64_t target_generation;
    rv64_jit_block_t *target_slot;
} rv64_jit_indirect_pic_entry_t;

typedef struct
{
    rv64_jit_indirect_pic_entry_t ways[RV64_JIT_INDIRECT_PIC_WAYS];
    uint8_t next_victim;
    uint8_t kind;
    uint8_t patch_replacement_count;
    uint8_t guarded_only;
    uint8_t reserved[12];
    /* Cold reverse-link ownership follows the one-cache-line probe header. */
    rv64_jit_link_t links[RV64_JIT_INDIRECT_PIC_WAYS];
} rv64_jit_indirect_pic_t;

typedef struct
{
    uint8_t *address_immediates[RV64_JIT_INDIRECT_PIC_MAX_FIXUPS];
    uint8_t *selector_disps[RV64_JIT_INDIRECT_PIC_WAYS];
    uint8_t *target_disps[RV64_JIT_INDIRECT_PIC_WAYS];
    const uint8_t *guarded_paths[RV64_JIT_INDIRECT_PIC_WAYS];
    const uint8_t *patched_paths[RV64_JIT_INDIRECT_PIC_WAYS];
    uint8_t fixup_count;
    uint8_t kind;
    bool used;
} rv64_jit_indirect_pic_builder_t;

_Static_assert(offsetof(rv64_jit_indirect_pic_t, links) ==
                   RV64_JIT_INDIRECT_PIC_LINE_SIZE,
               "RV64 JIT indirect PIC hot header must occupy one cache line");
_Static_assert(sizeof(rv64_jit_indirect_pic_t) == 256u,
               "RV64 JIT indirect PIC sidecar size drifted");

typedef struct
{
    uint8_t *selector_disp;
    uint8_t *target_disp;
    const uint8_t *guarded_path;
    const uint8_t *patched_path;
    vaddr_t target_pc;
    word_t target_satp;
    uint32_t target_ifetch_state;
    bool patch_eligible;
} rv64_jit_link_build_record_t;

typedef struct
{
    rv64_jit_link_build_record_t records[RV64_JIT_BLOCK_MAX_LINKS];
    uint32_t count;
} rv64_jit_link_builder_t;

typedef struct
{
    uint8_t *start;
    uint8_t *cur;
    uint8_t *end;
    rv64_jit_link_builder_t *links;
    rv64_jit_indirect_pic_builder_t *indirect_pic;
    rv64_jit_indirect_jump_cache_builder_t *indirect_jump_cache;
    /* Distinguish arena exhaustion from an unsupported guest encoding. */
    bool overflowed;
} rv64_jit_writer_t;

/*
 * One site cache stores routing metadata, never a device value. `host_ptr`
 * points at the exact byte selected by `guest_addr_tag`. Every emitted route is
 * initialised from a complete compile-time direct-MMIO match; the generated
 * full-width tag comparison remains authoritative if the runtime address
 * differs. Width, signedness, and direction remain properties of generated
 * code. The current single execution thread publishes host first and tag last;
 * concurrent vCPUs must instead use per-vCPU sidecars or synchronised versioned
 * publication.
 */
typedef struct
{
    uint64_t guest_addr_tag;
    uint64_t host_ptr;
} rv64_jit_mmio_route_t;

typedef enum
{
    RV64_JIT_MMIO_ROUTE_TAG,
    RV64_JIT_MMIO_ROUTE_HOST,
} rv64_jit_mmio_route_field_t;

typedef struct
{
    uint8_t *disp32;
    const uint8_t *next_ip;
    uint8_t site;
    uint8_t field;
} rv64_jit_mmio_route_fixup_t;

typedef struct
{
    uint8_t site_count;
    uint8_t fixup_count;
    rv64_jit_mmio_route_t initial_routes[RV64_JIT_MMIO_ROUTE_MAX_SITES];
    rv64_jit_mmio_route_fixup_t
        fixups[RV64_JIT_MMIO_ROUTE_MAX_FIXUPS];
} rv64_jit_mmio_route_builder_t;

_Static_assert(sizeof(rv64_jit_mmio_route_t) == 16u,
               "RV64 JIT MMIO route entries must be 16 bytes");
_Static_assert(RV64_JIT_MMIO_ROUTE_MAX_SITES *
                       sizeof(rv64_jit_mmio_route_t) ==
                   RV64_JIT_MMIO_ROUTE_LINE_SIZE,
               "RV64 JIT MMIO route entries must fill one cache line");

typedef enum
{
    RV64_JIT_HREG_RBX = 0,
    RV64_JIT_HREG_RBP,
    RV64_JIT_HREG_R12,
    RV64_JIT_HREG_R13,
    RV64_JIT_HREG_R14,
    RV64_JIT_HREG_R15,
    /* Ordinary blocks use only the six callee-saved slots above. */
    RV64_JIT_HREG_BASE_COUNT,
    /*
     * A proven helper-free self-loop may additionally dedicate caller-saved R8
     * to one loop-carried guest register. No C helper can observe this mapping.
     */
    RV64_JIT_HREG_R8 = RV64_JIT_HREG_BASE_COUNT,
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
    /* Active slots: six normally, seven only for a preloaded stable self-loop. */
    uint32_t slot_count;
    uint32_t next_age;
    /* Hints affect victim choice only; dirty values are always written back. */
    uint32_t current_use_mask;
    uint32_t live_after_mask;
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

_Static_assert(sizeof(rv64_jit_data_tlb_entry_t) ==
                   (1u << RV64_JIT_DATA_TLB_ENTRY_SHIFT),
               "RV64 JIT data-TLB entry size must match the emitted index shift");
_Static_assert(((CONFIG_MBASE | CONFIG_MSIZE) & PAGE_MASK) == 0,
               "RV64 JIT PMEM must be page aligned");
_Static_assert((uint64_t)CONFIG_MSIZE <= (uint64_t)UINT32_MAX + 1u,
               "RV64 JIT inline PMEM offsets must fit in 32 bits");
_Static_assert(RV64_JIT_PMEM_CHUNK_COUNT <= UINT32_MAX,
               "RV64 JIT source chunk indexes must fit in 32 bits");
_Static_assert(RV64_JIT_SOURCE_LINK_COUNT <= UINT32_MAX,
               "RV64 JIT source-link frontier must fit in 32 bits");
_Static_assert(RV64_JIT_DATA_TLB_SIZE *RV64_JIT_SV39_LEVELS < UINT16_MAX,
               "RV64 JIT data-TLB dependency refs must fit in 16 bits");

struct rv64_jit_block
{
    bool valid;
    bool translated;
    bool uses_data_state;
    vaddr_t pc;
    word_t satp;
    uint32_t ifetch_state;
    uint32_t data_state;
    uint64_t ifetch_generation;
    /* Non-zero identity of this exact native publication. */
    uint64_t generation;
    uint32_t source_len;
    uint32_t source_segment_count;
    rv64_jit_source_segment_t source_segments[RV64_JIT_BLOCK_MAX_SOURCE_SEGMENTS];
    uint32_t insn_count;
    rv64_jit_entry_t entry;
    rv64_jit_entry_t body_entry;
    rv64_jit_entry_t chain_entry;
    /* Physical source metadata is cold and follows all emitted hot fields. */
    paddr_t paddr_start;
    uint32_t ifetch_pt_page_count;
    paddr_t ifetch_pt_pages[RV64_JIT_BLOCK_MAX_IFETCH_PT_PAGES];
    rv64_jit_link_t *outgoing_links;
    uint32_t outgoing_link_count;
    rv64_jit_indirect_pic_t *indirect_pic;
};

typedef enum
{
    RV64_JIT_BLOCK_END_BUDGET,
    RV64_JIT_BLOCK_END_JUMP,
    RV64_JIT_BLOCK_END_CHAINED_LOOP,
    RV64_JIT_BLOCK_END_FP_MEMORY,
    RV64_JIT_BLOCK_END_SOURCE_BOUNDARY,
    RV64_JIT_BLOCK_END_UNSUPPORTED_AFTER_PREFIX,
    RV64_JIT_BLOCK_END_COUNT,
} rv64_jit_block_end_reason_t;

typedef enum
{
    RV64_JIT_SIDE_EXIT_LOAD_GUARD,
    RV64_JIT_SIDE_EXIT_STORE_GUARD,
    RV64_JIT_SIDE_EXIT_STORE_SOURCE,
    RV64_JIT_SIDE_EXIT_STORE_HELPER,
    RV64_JIT_SIDE_EXIT_PAGED_STORE_HELPER,
    RV64_JIT_SIDE_EXIT_BRANCH_TAKEN,
    RV64_JIT_SIDE_EXIT_CHAINED_OVER_BUDGET,
    RV64_JIT_SIDE_EXIT_JALR_MISALIGNED,
    RV64_JIT_SIDE_EXIT_FP_FS_OFF,
    RV64_JIT_SIDE_EXIT_COUNT,
} rv64_jit_side_exit_reason_t;

/*
 * These OP-FP instructions are exact bit operations: they neither consult a
 * rounding mode nor change fflags.  A single decoder is shared by the loop
 * pre-scan and real emitter so an encoding cannot be considered helper-free by
 * one stage and then take a helper in the other.
 */
typedef enum
{
    RV64_JIT_FP_EXACT_FMV_X_W,
    RV64_JIT_FP_EXACT_FMV_W_X,
    RV64_JIT_FP_EXACT_FMV_X_D,
    RV64_JIT_FP_EXACT_FMV_D_X,
    RV64_JIT_FP_EXACT_FSGNJ_S,
    RV64_JIT_FP_EXACT_FSGNJN_S,
    RV64_JIT_FP_EXACT_FSGNJX_S,
    RV64_JIT_FP_EXACT_FSGNJ_D,
    RV64_JIT_FP_EXACT_FSGNJN_D,
    RV64_JIT_FP_EXACT_FSGNJX_D,
    RV64_JIT_FP_EXACT_FCLASS_S,
    RV64_JIT_FP_EXACT_FCLASS_D,
    RV64_JIT_FP_EXACT_OP_COUNT,
    RV64_JIT_FP_EXACT_INVALID = RV64_JIT_FP_EXACT_OP_COUNT,
} rv64_jit_fp_exact_op_t;

/*
 * FP loads and stores use the same width field, but loads name an FPR
 * destination while stores name an FPR source. Keeping all four operations
 * distinct makes run-time statistics sensitive to missing boxing, writeback,
 * or store lowering.
 */
typedef enum
{
    RV64_JIT_FP_MEMORY_FLW,
    RV64_JIT_FP_MEMORY_FLD,
    RV64_JIT_FP_MEMORY_FSW,
    RV64_JIT_FP_MEMORY_FSD,
    RV64_JIT_FP_MEMORY_OP_COUNT,
    RV64_JIT_FP_MEMORY_INVALID = RV64_JIT_FP_MEMORY_OP_COUNT,
} rv64_jit_fp_memory_op_t;

/*
 * Keep the complete RV64M instruction set in architectural funct3 order where
 * possible. Run-time counters use these identities to prove that each native
 * lowering actually executed, rather than merely counting a compiled site.
 */
typedef enum
{
    RV64_JIT_M_OP_MUL,
    RV64_JIT_M_OP_MULH,
    RV64_JIT_M_OP_MULHSU,
    RV64_JIT_M_OP_MULHU,
    RV64_JIT_M_OP_DIV,
    RV64_JIT_M_OP_DIVU,
    RV64_JIT_M_OP_REM,
    RV64_JIT_M_OP_REMU,
    RV64_JIT_M_OP_MULW,
    RV64_JIT_M_OP_DIVW,
    RV64_JIT_M_OP_DIVUW,
    RV64_JIT_M_OP_REMW,
    RV64_JIT_M_OP_REMUW,
    RV64_JIT_M_OP_COUNT,
} rv64_jit_m_op_t;

/*
 * Native sites are counted while a block is still being emitted.  A failed
 * compilation must therefore roll every field below back together with the
 * writer, otherwise the report would describe bytes which were never
 * published.  Keeping the complete transactional schema in one type makes a
 * newly added emitted-site counter part of snapshot and restore by default.
 */
typedef struct
{
    uint64_t native_loads;
    uint64_t native_stores;
    uint64_t native_jumps;
    uint64_t native_m_ops;
    uint64_t native_fp_exact_sites;
    uint64_t native_fp_memory_sites;
    uint64_t indirect_pic_sites;
    uint64_t indirect_jump_cache_sites;
    uint64_t reg_cache_spills;
    uint64_t reg_cache_dead_victims;
    uint64_t reg_cache_live_lru_avoided;
    uint64_t native_store_continuations;
    uint64_t native_paged_loads;
    uint64_t native_paged_stores;
    uint64_t inline_paged_loads;
    uint64_t inline_paged_stores;
    uint64_t direct_mmio_load_sites;
    uint64_t direct_mmio_store_sites;
    uint64_t fp_helper_sites;
    uint64_t fp_helper_gpr_effect_sites;
    uint64_t fp_helper_gpr_mappings_preserved;
    uint64_t fp_helper_gpr_selective_invalidations;
    uint64_t fp_helper_gpr_input_flushes;
    uint64_t fp_helper_gpr_dirty_mappings_preserved;
    uint64_t fp_helper_gpr_trap_stores;
} rv64_jit_emitted_site_stats_t;

typedef struct
{
    /* Dispatcher and native-entry activity measured while the guest runs. */
    uint64_t exec_requests;
    uint64_t cache_hits;
    uint64_t cache_misses;
    uint64_t unsupported_hits;
    uint64_t blocks_executed;
    uint64_t executed_insns;
    uint64_t zero_side_exits;
    uint64_t cpu_boundary_breaks;

    /*
     * Compilation and negative-cache outcomes.  These count published native
     * regions, cached unsupported entries, and their guest source rather than
     * later executions of generated code.
     */
    uint64_t blocks_compiled;
    uint64_t blocks_unsupported;
    uint64_t compiled_insns;
    uint64_t translated_blocks;
    uint64_t translated_cross_page_blocks;
    uint64_t segmented_source_blocks;
    uint64_t trace_blocks;
    uint64_t trace_insns;
    uint64_t unsupported_by_opcode[RV64_OPCODE_MASK + 1u];
    uint64_t block_end_by_reason[RV64_JIT_BLOCK_END_COUNT];

    /*
     * One emitted site may execute many times, so these values are not
     * run-time instruction counts.  Compilation snapshots this member as one
     * transaction while a candidate native block is still speculative.
     */
    rv64_jit_emitted_site_stats_t emitted_sites;

    /* These counters are recorded only after a block has been published. */
    uint64_t stable_loop_blocks;
    uint64_t stable_loop_preloaded_regs;

    /* Run-time side exits, helper calls, and Sv39 data-TLB activity. */
    uint64_t side_exit_by_reason[RV64_JIT_SIDE_EXIT_COUNT];
    uint64_t data_tlb_hits;
    uint64_t data_tlb_misses;
    uint64_t data_tlb_fills;
    uint64_t data_tlb_flushes;
    uint64_t data_tlb_page_table_flushes;
    uint64_t data_tlb_direct_loads;
    uint64_t data_tlb_direct_stores;
    uint64_t inline_paged_load_hits;
    uint64_t inline_paged_store_hits;
    uint64_t inline_direct_mmio_load_hits;
    uint64_t inline_direct_mmio_store_hits;
    uint64_t direct_mmio_load_route_hits;
    uint64_t direct_mmio_load_route_misses;
    uint64_t direct_mmio_load_route_fills;
    uint64_t direct_mmio_store_route_hits;
    uint64_t direct_mmio_store_route_misses;
    uint64_t direct_mmio_store_route_fills;
    uint64_t helper_load_count;
    uint64_t helper_store_count;
    uint64_t paged_store_helper_continuations;
    uint64_t bare_mmio_load_calls;
    uint64_t bare_mmio_store_calls;
    uint64_t bare_mmio_store_continuations;
    uint64_t bare_mmio_store_boundary_exits;
    uint64_t bare_mmio_store_invalidation_exits;
    uint64_t fp_helper_calls;
    uint64_t fp_helper_continuations;
    uint64_t fp_helper_trap_exits;
    uint64_t fp_helper_memory_exits;
    uint64_t native_m_executions[RV64_JIT_M_OP_COUNT];
    uint64_t native_fp_exact_executions[RV64_JIT_FP_EXACT_OP_COUNT];
    uint64_t native_fp_memory_executions[RV64_JIT_FP_MEMORY_OP_COUNT];

    /* Direct-link outcomes are incremented by the running generated code. */
    uint64_t direct_link_taken_count;
    uint64_t direct_link_miss_count;
    uint64_t direct_branch_link_taken_count;
    uint64_t direct_guarded_link_taken_count;
    uint64_t patched_direct_link_taken_count;
    uint64_t direct_link_patch_resolutions;
    uint64_t direct_link_patch_unlinks;
    uint64_t direct_return_link_taken_count;
    uint64_t direct_return_link_miss_count;
    uint64_t direct_jalr_link_taken_count;
    uint64_t direct_jalr_link_miss_count;
    uint64_t indirect_jump_cache_hits;
    uint64_t indirect_jump_cache_misses;
    uint64_t indirect_jump_cache_fills;
    uint64_t indirect_jump_cache_replacements;
    uint64_t indirect_jump_cache_stale_rejections;
    uint64_t indirect_jump_cache_budget_rejections;
    uint64_t indirect_pic_hits[RV64_JIT_INDIRECT_PIC_KIND_COUNT];
    uint64_t indirect_pic_secondary_hits[RV64_JIT_INDIRECT_PIC_KIND_COUNT];
    uint64_t indirect_pic_misses[RV64_JIT_INDIRECT_PIC_KIND_COUNT];
    uint64_t indirect_pic_fills[RV64_JIT_INDIRECT_PIC_KIND_COUNT];
    uint64_t indirect_pic_replacements[RV64_JIT_INDIRECT_PIC_KIND_COUNT];
    uint64_t indirect_pic_stale_rejections[RV64_JIT_INDIRECT_PIC_KIND_COUNT];
    uint64_t indirect_pic_budget_rejections[RV64_JIT_INDIRECT_PIC_KIND_COUNT];
    uint64_t indirect_pic_patch_resolutions[RV64_JIT_INDIRECT_PIC_KIND_COUNT];
    uint64_t indirect_pic_patch_unlinks[RV64_JIT_INDIRECT_PIC_KIND_COUNT];
    uint64_t indirect_pic_source_detaches[RV64_JIT_INDIRECT_PIC_KIND_COUNT];
    uint64_t indirect_pic_target_detaches[RV64_JIT_INDIRECT_PIC_KIND_COUNT];
    uint64_t indirect_pic_patched_entries[RV64_JIT_INDIRECT_PIC_KIND_COUNT];
    uint64_t indirect_pic_patch_downgrades[RV64_JIT_INDIRECT_PIC_KIND_COUNT];

    /* Validation and invalidation maintenance performed while NEMU runs. */
    uint64_t ifetch_generation_fast_hits;
    uint64_t ifetch_generation_revalidations;
    uint64_t ifetch_generation_bumps;
    uint64_t ifetch_generation_avoided_bumps;
    uint64_t source_reverse_invalidations;
    uint64_t source_full_invalidation_scans;
    uint64_t source_link_sequential_allocations;
    uint64_t source_link_recycled_allocations;
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
    uint8_t *slow_disps[RV64_JIT_DTLB_MAX_SLOW_PATCHES];
    uint32_t count;
} rv64_jit_tlb_guard_patch_t;

/* Extract an inclusive bit range from a 32-bit RISC-V instruction. */
static inline uint32_t bits(uint32_t value, int hi, int lo)
{
    /*
     * All callers pass 0 <= lo <= hi < 32.  Forming the mask by shifting
     * UINT32_MAX also keeps a future full-width [31:0] request well-defined;
     * `(1u << 32) - 1` would not be valid C.
     */
    const unsigned width = (unsigned)(hi - lo + 1);
    const uint32_t mask = UINT32_MAX >> (32u - width);
    return (value >> lo) & mask;
}

/*
 * Common RISC-V instruction formats keep these register and function fields in
 * fixed positions.  Naming the extractions once avoids scattering numeric bit
 * tuples through the emitters.  Shift-immediate instructions use funct6/shamt6
 * at XLEN=64 and funct7/shamt5 for the RV64 word-operation forms.
 */
static inline uint32_t rv64_instr_rd(uint32_t instr)
{
    return bits(instr, 11, 7);
}

static inline uint32_t rv64_instr_funct3(uint32_t instr)
{
    return bits(instr, 14, 12);
}

static inline uint32_t rv64_instr_rs1(uint32_t instr)
{
    return bits(instr, 19, 15);
}

static inline uint32_t rv64_instr_rs2(uint32_t instr)
{
    return bits(instr, 24, 20);
}

static inline uint32_t rv64_instr_funct7(uint32_t instr)
{
    return bits(instr, 31, 25);
}

static inline uint32_t rv64_instr_funct6(uint32_t instr)
{
    return bits(instr, 31, 26);
}

static inline uint32_t rv64_instr_shamt6(uint32_t instr)
{
    return bits(instr, 25, 20);
}

static inline uint32_t rv64_instr_shamt5(uint32_t instr)
{
    return bits(instr, 24, 20);
}

#ifdef CONFIG_RISCV_FPU
/*
 * Decode only complete, architecturally valid encodings in the first native
 * exact-FP tier.  In particular, move/class instructions reserve rs2=0 and
 * sign injection defines only funct3 values 0-2.
 */
static inline rv64_jit_fp_exact_op_t
rv64_jit_decode_fp_exact(uint32_t instr)
{
    if ((instr & RV64_OPCODE_MASK) != RV64_FP_OPCODE_OP)
    {
        return RV64_JIT_FP_EXACT_INVALID;
    }

    const uint32_t funct7 = rv64_instr_funct7(instr);
    const uint32_t funct3 = rv64_instr_funct3(instr);
    const uint32_t rs2 = rv64_instr_rs2(instr);

    switch (funct7)
    {
    case 0x10u: /* FSGNJ.S, FSGNJN.S, FSGNJX.S. */
        switch (funct3)
        {
        case 0:
            return RV64_JIT_FP_EXACT_FSGNJ_S;
        case 1:
            return RV64_JIT_FP_EXACT_FSGNJN_S;
        case 2:
            return RV64_JIT_FP_EXACT_FSGNJX_S;
        default:
            return RV64_JIT_FP_EXACT_INVALID;
        }
    case 0x70u: /* FMV.X.W and FCLASS.S share funct7. */
        if (rs2 != 0)
        {
            return RV64_JIT_FP_EXACT_INVALID;
        }
        if (funct3 == 0)
        {
            return RV64_JIT_FP_EXACT_FMV_X_W;
        }
        return funct3 == 1
                   ? RV64_JIT_FP_EXACT_FCLASS_S
                   : RV64_JIT_FP_EXACT_INVALID;
    case 0x78u: /* FMV.W.X. */
        return rs2 == 0 && funct3 == 0
                   ? RV64_JIT_FP_EXACT_FMV_W_X
                   : RV64_JIT_FP_EXACT_INVALID;
#ifdef CONFIG_RISCV_D
    case 0x11u: /* FSGNJ.D, FSGNJN.D, FSGNJX.D. */
        switch (funct3)
        {
        case 0:
            return RV64_JIT_FP_EXACT_FSGNJ_D;
        case 1:
            return RV64_JIT_FP_EXACT_FSGNJN_D;
        case 2:
            return RV64_JIT_FP_EXACT_FSGNJX_D;
        default:
            return RV64_JIT_FP_EXACT_INVALID;
        }
    case 0x71u: /* FMV.X.D and FCLASS.D share funct7. */
        if (rs2 != 0)
        {
            return RV64_JIT_FP_EXACT_INVALID;
        }
        if (funct3 == 0)
        {
            return RV64_JIT_FP_EXACT_FMV_X_D;
        }
        return funct3 == 1
                   ? RV64_JIT_FP_EXACT_FCLASS_D
                   : RV64_JIT_FP_EXACT_INVALID;
    case 0x79u: /* FMV.D.X. */
        return rs2 == 0 && funct3 == 0
                   ? RV64_JIT_FP_EXACT_FMV_D_X
                   : RV64_JIT_FP_EXACT_INVALID;
#endif
    default:
        return RV64_JIT_FP_EXACT_INVALID;
    }
}

/* Decode only the F/D memory widths implemented by the scalar interpreter. */
static inline rv64_jit_fp_memory_op_t
rv64_jit_decode_fp_memory(uint32_t instr)
{
    const uint32_t opcode = instr & RV64_OPCODE_MASK;
    const uint32_t width = rv64_instr_funct3(instr);

    if (opcode == RV64_FP_OPCODE_LOAD)
    {
        if (width == RV64_FUNCT3_LW)
        {
            return RV64_JIT_FP_MEMORY_FLW;
        }
#ifdef CONFIG_RISCV_D
        if (width == RV64_FUNCT3_LD)
        {
            return RV64_JIT_FP_MEMORY_FLD;
        }
#endif
    }
    else if (opcode == RV64_FP_OPCODE_STORE)
    {
        if (width == RV64_FUNCT3_SW)
        {
            return RV64_JIT_FP_MEMORY_FSW;
        }
#ifdef CONFIG_RISCV_D
        if (width == RV64_FUNCT3_SD)
        {
            return RV64_JIT_FP_MEMORY_FSD;
        }
#endif
    }

    return RV64_JIT_FP_MEMORY_INVALID;
}
#endif

/* Sign-extend an instruction field whose sign bit is at width - 1. */
static inline int64_t sext(uint32_t value, unsigned width)
{
    /*
     * RV64 immediates in this file are at most 32 bits before extension.  The
     * xor-and-subtract form is independent of the implementation's signed
     * right-shift behaviour: values below the sign bit stay positive and values
     * at or above it subtract twice the sign-bit weight.
     */
    const uint32_t mask = UINT32_MAX >> (32u - width);
    const uint32_t sign = 1u << (width - 1u);
    return (int64_t)(((value & mask) ^ sign)) - (int64_t)sign;
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

#ifdef CONFIG_RISCV_FPU
/* Return byte offsets for raw FLEN=64 state reached through the fixed R11 base. */
static inline uint32_t jit_fpr_offset(uint32_t reg)
{
    return (uint32_t)(offsetof(CPU_state, fpr) + reg * sizeof(cpu.fpr[0]));
}

static inline uint32_t jit_mstatus_offset(void)
{
    return (uint32_t)offsetof(CPU_state, csr.mstatus);
}
#endif

/* Return the byte offset of the guest PC inside CPU_state. */
static inline uint32_t jit_pc_offset(void)
{
    return (uint32_t)offsetof(CPU_state, pc);
}

/*
 * Fold the fetch context into the low 32 bits used by the direct-mapped cache.
 * Only the low log2(cache-size) bits survive the final mask, so truncating this
 * context term before XORing the shifted PC is exactly equivalent to the
 * original full-width expression.
 */
static inline uint32_t rv64_jit_cache_context_mix(word_t satp,
                                                   uint32_t ifetch_state)
{
    return (uint32_t)(satp ^
                      (satp >> RV64_JIT_CACHE_SATP_MIX_SHIFT) ^
                      ifetch_state);
}

/* Return the existing cache index for one full guest fetch context. */
static inline uint32_t rv64_jit_cache_hash_context(vaddr_t pc, word_t satp,
                                                    uint32_t ifetch_state)
{
    return (uint32_t)(((pc >> RV64_JIT_CACHE_PC_SHIFT) ^
                       rv64_jit_cache_context_mix(satp, ifetch_state)) &
                      (RV64_JIT_CACHE_SIZE - 1u));
}

extern rv64_jit_block_t rv64_jit_cache[RV64_JIT_CACHE_SIZE];
extern rv64_jit_data_tlb_entry_t rv64_jit_data_tlb[RV64_JIT_DATA_TLB_SIZE];
extern uint16_t rv64_jit_data_tlb_pt_page_refs[RV64_JIT_PMEM_PAGE_COUNT];
extern uint32_t rv64_jit_ifetch_pt_page_refs[RV64_JIT_PMEM_PAGE_COUNT];
extern uint32_t rv64_jit_source_chunk_refs[RV64_JIT_PMEM_CHUNK_COUNT];
extern uint32_t rv64_jit_source_chunk_heads[RV64_JIT_PMEM_CHUNK_COUNT];
extern rv64_jit_source_link_t rv64_jit_source_links[RV64_JIT_SOURCE_LINK_COUNT];
extern uint32_t rv64_jit_source_link_free_head;
extern uint8_t *rv64_jit_code;
extern size_t rv64_jit_code_used;
extern rv64_jit_stats_t rv64_jit_stats;
extern uint64_t rv64_jit_ifetch_generation;
/*
 * Changes whenever native cache ownership is invalidated: either a physical
 * source write actually discards at least one entry or the complete arena is
 * reset. Callback-backed MMIO helpers snapshot this independently from the
 * translated-ifetch generation so DMA and broad flushes cannot be hidden by
 * the precise page-table generation policy.
 */
extern uint64_t rv64_jit_native_cache_epoch;
extern volatile uint32_t rv64_jit_entry_budget;
extern volatile uint32_t rv64_jit_loop_extra;
extern volatile bool rv64_jit_cpu_boundary_requested;

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
void rv64_jit_dump_stats_report(void);
bool rv64_jit_direct_link_enabled(void);
bool rv64_jit_return_link_enabled(void);
bool rv64_jit_fp_gpr_effects_enabled(void);
void rv64_jit_perf_map_init(bool requested);
void rv64_jit_perf_map_reset(void);
void rv64_jit_perf_map_publish(const rv64_jit_block_t *block,
                               const uint8_t *native_start,
                               size_t native_size);
void rv64_jit_ifetch_generation_bump(void);
void rv64_jit_data_tlb_flush(void);
bool rv64_jit_write_may_touch_data_tlb_page_table(paddr_t addr, int len);
bool rv64_jit_write_may_touch_ifetch_page_table(paddr_t addr, int len);
uint32_t rv64_jit_data_tlb_state(int type);
bool rv64_jit_data_tlb_probe_read(vaddr_t addr, uint32_t len);
bool rv64_jit_data_tlb_probe_write(vaddr_t addr, uint32_t len);
uint32_t rv64_jit_ifetch_state(void);
uint64_t rv64_jit_load_i8(vaddr_t addr);
uint64_t rv64_jit_load_i16(vaddr_t addr);
uint64_t rv64_jit_load_i32(vaddr_t addr);
uint64_t rv64_jit_load_u64(vaddr_t addr);
uint64_t rv64_jit_load_u8(vaddr_t addr);
uint64_t rv64_jit_load_u16(vaddr_t addr);
uint64_t rv64_jit_load_u32(vaddr_t addr);
uint64_t rv64_jit_load_bare_i8(paddr_t addr);
uint64_t rv64_jit_load_bare_i16(paddr_t addr);
uint64_t rv64_jit_load_bare_i32(paddr_t addr);
uint64_t rv64_jit_load_bare_u64(paddr_t addr);
uint64_t rv64_jit_load_bare_u8(paddr_t addr);
uint64_t rv64_jit_load_bare_u16(paddr_t addr);
uint64_t rv64_jit_load_bare_u32(paddr_t addr);
uint32_t rv64_jit_store_vaddr_continue(vaddr_t addr, uint32_t len,
                                       uint64_t data);
uint32_t rv64_jit_store_bare_continue(paddr_t addr, uint32_t len,
                                      uint64_t data);
uint32_t rv64_jit_store_pmem_continue(paddr_t addr, uint32_t len, uint64_t data);
size_t rv64_jit_align_up(size_t value, size_t align);
bool rv64_jit_source_chunk_range(paddr_t addr, uint32_t len,
                                 size_t *first, size_t *last);
bool rv64_jit_source_builder_append(rv64_jit_source_builder_t *source,
                                    paddr_t paddr, uint32_t len);
void rv64_jit_ifetch_refs_ref(const rv64_jit_block_t *block);
void rv64_jit_source_reverse_map_init(void);
void rv64_jit_source_reverse_map_reset(void);
void rv64_jit_source_reverse_map_add(rv64_jit_block_t *block);
void rv64_jit_source_chunks_ref(const rv64_jit_block_t *block);
bool rv64_jit_write_may_touch_source_chunk(paddr_t addr, int len);
void rv64_jit_block_discard(rv64_jit_block_t *block);
void rv64_jit_links_reset(void);
void rv64_jit_links_source_published(rv64_jit_block_t *block);
void rv64_jit_links_target_published(rv64_jit_block_t *block);
void rv64_jit_links_block_discard(rv64_jit_block_t *block);
uint64_t rv64_jit_allocate_block_generation(void);
rv64_jit_entry_t rv64_jit_indirect_pic_refill(
    rv64_jit_indirect_pic_t *pic, vaddr_t target_pc,
    rv64_jit_block_t *target_slot);
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
void rv64_jit_reg_cache_set_liveness(rv64_jit_reg_cache_t *regs,
                                     uint32_t current_use_mask,
                                     uint32_t live_after_mask);
bool rv64_jit_prepare_stable_loop_regs(rv64_jit_writer_t *w,
                                       rv64_jit_reg_cache_t *regs,
                                       uint32_t reg_mask,
                                       uint32_t loop_insn_count);
bool rv64_jit_emit_prologue(rv64_jit_writer_t *w);
bool rv64_jit_finalise_indirect_pic(
    rv64_jit_writer_t *w, rv64_jit_indirect_pic_builder_t *builder,
    rv64_jit_indirect_pic_t **pic);
bool rv64_jit_finalise_indirect_jump_cache(
    rv64_jit_writer_t *w,
    rv64_jit_indirect_jump_cache_builder_t *builder);
bool rv64_jit_emit_chain_entry(rv64_jit_writer_t *w, vaddr_t pc,
                               uint32_t insn_count,
                               const uint8_t *body_entry,
                               const uint8_t **chain_entry);
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
                              bool source_uses_data_state);
bool rv64_jit_emit_load_instr(rv64_jit_writer_t *w,
                              rv64_jit_reg_cache_t *regs,
                              uint32_t instr, vaddr_t pc,
                              uint32_t completed_count,
                              rv64_jit_mmio_route_builder_t *mmio_routes);
bool rv64_jit_emit_store_instr(rv64_jit_writer_t *w,
                               rv64_jit_reg_cache_t *regs,
                               uint32_t instr, vaddr_t pc,
                               vaddr_t next_pc,
                               uint32_t completed_count,
                               rv64_jit_mmio_route_builder_t *mmio_routes);
bool rv64_jit_finalise_mmio_routes(
    rv64_jit_writer_t *w, rv64_jit_mmio_route_builder_t *mmio_routes);
#ifdef CONFIG_RISCV_FPU
bool rv64_jit_emit_fp_instr(rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs, uint32_t instr, vaddr_t pc, uint32_t completed_count,
                            bool *ends_block);
#endif
bool rv64_jit_emit_branch(rv64_jit_writer_t *w,
                          rv64_jit_reg_cache_t *regs,
                          uint32_t instr, vaddr_t pc,
                          vaddr_t block_start_pc,
                          const uint8_t *native_body_entry,
                          bool can_chain_self_backedge,
                          bool stable_self_backedge,
                          bool *emitted_native_backedge,
                          uint32_t retired_including_current,
                          bool current_block_uses_data_translation_state);
bool rv64_jit_emit_instr(rv64_jit_writer_t *w,
                         rv64_jit_reg_cache_t *regs,
                         uint32_t instr, vaddr_t pc,
                         uint32_t exit_count);
rv64_jit_block_t *rv64_jit_compile_block(vaddr_t pc, uint32_t max_insns);

#endif /* CONFIG_RV64 */

#endif /* __RISCV64_JIT_INTERNAL_H__ */
