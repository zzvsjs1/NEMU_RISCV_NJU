#include <generated/autoconf.h>

#include "local-include/reg.h"
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

/*
 * First x86 JIT milestone.
 *
 * The interpreter in inst.c remains the architectural reference.  This file
 * started by translating a tiny fault-free IA-32 subset into x86-64 code:
 *
 *   - MOV r32, imm32
 *   - MOV r32, r32
 *   - LEA r32, m
 *   - NOP
 *
 * The second milestone adds predecoded helper-backed instructions for the
 * common IA-32 integer core used by AM CPU tests: stack traffic, 32-bit
 * memory/register ALU, EFLAGS, calls, returns, and short/direct branches.  The
 * generated block still returns to the interpreter before any unsupported
 * instruction commits, so wider coverage does not weaken the fallback
 * boundary.
 *
 * Paged Nanos-lite execution is supported only for flat segment bases/limits.
 * Instruction fetch and cache validation translate virtual PCs through the x86
 * MMU and key blocks by CR3.  Ordinary paged MOV loads/stores can now use a
 * small guarded DTLB: misses still go through the architectural MMU to preserve
 * faults and A/D-bit updates, while hits let generated code touch PMEM directly
 * when page-table and self-modifying-code guards prove that continuing is safe.
 */

#if defined(__x86_64__) && defined(CONFIG_X86_JIT) && \
    defined(CONFIG_TARGET_NATIVE_ELF) && !defined(CONFIG_TRACE) && \
    !defined(CONFIG_DIFFTEST) && !defined(CONFIG_WATCHPOINT) && \
    !defined(CONFIG_MTRACE) && !defined(CONFIG_FTRACE)
#define X86_JIT_ENABLED 1
#include <sys/mman.h>
#include <unistd.h>
#else
#define X86_JIT_ENABLED 0
#endif

#ifdef CONFIG_X86_JIT_STATS
#define X86_JIT_STATS 1
#else
#define X86_JIT_STATS 0
#endif

/*
 * IA-32 paging and flag constants used by both decode-time checks and emitted
 * native guards.  The bit positions follow Intel's CR0/CR4/PDE/PTE/EFLAGS
 * layout: CR0.WP is bit 16, CR0.PG is bit 31, CR4.PSE is bit 4, CR4.PAE is
 * bit 5, PTE.P is bit 0, and PDE.PS is bit 7.  Page-directory/table addresses
 * occupy bits 31..12,
 * so the low 12 bits are masked away before comparing CR3 keys or page bases.
 */
#define X86_CR0_WP 0x00010000u
#define X86_CR0_PG 0x80000000u
#define X86_CR4_PSE 0x00000010u
#define X86_CR4_PAE 0x00000020u
#define X86_PTE_P 0x001u
#define X86_PTE_PS 0x080u
#define X86_PTE_ADDR_MASK ((paddr_t)0xfffff000u)
#define X86_DWORD_LIMIT 0xffffffffu
#define X86_BYTE_MASK 0xffu
#define X86_WORD_MASK 0xffffu
/*
 * Intel defines EFLAGS bit 1 as reserved and always read as one.  Keep it set
 * after every helper-side flag write so PUSHF/POPF-visible state matches IA-32.
 */
#define X86_EFLAGS_FIXED_ONE (1u << 1)
/*
 * AF is carry/borrow from bit 3 to bit 4.  The usual x86 formula
 * (lhs ^ rhs ^ result) therefore only needs bit 4, i.e. mask 0x10.
 * PF is even parity of the low byte; fold that byte to four bits and use the
 * 16-bit lookup table rather than counting bits on every helper ALU operation.
 */
#define X86_AUX_CARRY_BIT 0x10u
#define X86_PARITY_FOLD_NIBBLE_MASK 0xfu
#define X86_PARITY_FOLD_TABLE 0x6996u

/* IA-32 arithmetic status flags used by ALU, Jcc, SETcc, and flag capture. */
#define X86_FLAG_CF (1u << 0)
#define X86_FLAG_PF (1u << 2)
#define X86_FLAG_AF (1u << 4)
#define X86_FLAG_ZF (1u << 6)
#define X86_FLAG_SF (1u << 7)
#define X86_FLAG_OF (1u << 11)
/* Full status-flag set written by ADD/SUB-family helpers. */
#define X86_EFLAGS_STATUS_MASK \
  (X86_FLAG_CF | X86_FLAG_PF | X86_FLAG_AF | \
      X86_FLAG_ZF | X86_FLAG_SF | X86_FLAG_OF)
/* Logical instructions copy PF/ZF/SF from the host flags and clear CF/OF/AF. */
#define X86_EFLAGS_LOGIC_COPY_MASK (X86_FLAG_PF | X86_FLAG_ZF | X86_FLAG_SF)
/* INC/DEC update all arithmetic status flags except CF, which is preserved. */
#define X86_EFLAGS_INCDEC_COPY_MASK \
  (X86_FLAG_PF | X86_FLAG_AF | X86_FLAG_ZF | X86_FLAG_SF | X86_FLAG_OF)

/*
 * Guest operand widths are stored as byte counts.  IA-32 defaults to 32-bit
 * operands here; opcode 66H switches many decodes to WORD, while byte opcodes
 * force BYTE.
 */
#define X86_WIDTH_BYTE 1u
#define X86_WIDTH_WORD 2u
#define X86_WIDTH_DWORD 4u
/* IA-32 shift/rotate counts use only CL[4:0] for 8/16/32-bit operands. */
#define X86_SHIFT_COUNT_MASK 0x1fu
#define X86_BITS_PER_BYTE 8u
#define X86_WORD_BITS 16u
#define X86_DWORD_BITS 32u
/* 2^32, used when reconstructing signed EDX:EAX dividends for IDIV. */
#define X86_DWORD_BASE 0x100000000ll

/*
 * JIT sizing knobs.  Cache, L0, hot, RET, and DTLB sizes are powers of two
 * because the lookup helpers mask hashes instead of using division.  Trace and
 * source limits bound compile time, native code size, and invalidation scans.
 */
#define X86_JIT_BLOCK_MAX_INSNS 64u
#define X86_JIT_BATCH_MAX_INSNS 65536u
#define X86_JIT_CACHE_SETS 4096u
#define X86_JIT_CACHE_WAYS 4u
#define X86_JIT_CACHE_SIZE (X86_JIT_CACHE_SETS * X86_JIT_CACHE_WAYS)
#define X86_JIT_L0_SIZE 4096u
#define X86_JIT_HOT_TABLE_SIZE 4096u
#define X86_JIT_RET_CACHE_SIZE 4096u
#define X86_JIT_RET_CACHE_MASK (X86_JIT_RET_CACHE_SIZE - 1u)
#define X86_JIT_DEFAULT_BLOCK_LIMIT 32u
#define X86_JIT_TRACE_HOT_THRESHOLD 1024u
#define X86_JIT_TRACE_MAX_BLOCKS 12u
#define X86_JIT_TRACE_MAX_INSNS 192u
#define X86_JIT_TRACE_MIN_INSNS 2u
#define X86_JIT_CODE_SIZE (16u * 1024u * 1024u)
#define X86_JIT_BLOCK_CODE_HEADROOM 16384u
#define X86_JIT_CODE_ALIGN 16u
#define X86_JIT_MAX_SOURCE_BYTES 1024u
#define X86_JIT_SOURCE_PAGE_SHIFT 12u
#define X86_JIT_SOURCE_PAGE_SIZE (1u << X86_JIT_SOURCE_PAGE_SHIFT)
#define X86_JIT_SOURCE_PAGE_COUNT \
  (((size_t)CONFIG_MSIZE + X86_JIT_SOURCE_PAGE_SIZE - 1u) / \
      X86_JIT_SOURCE_PAGE_SIZE)
#define X86_JIT_SOURCE_PAGE_BLOCK_LIMIT 16u
#define X86_JIT_BLOCK_SOURCE_PAGE_LIMIT 4u
#define X86_JIT_BLOCK_SOURCE_RANGE_LIMIT 4u
#define X86_JIT_TRACE_SOURCE_SPAN_LIMIT 12u
#define X86_JIT_EXIT_EDGE_LIMIT 12u
#define X86_JIT_DTLB_SIZE 256u
/* Per-entry access bits for the private data TLB; writes imply read access. */
#define X86_JIT_DTLB_READ 0x1u
#define X86_JIT_DTLB_WRITE 0x2u

typedef uint32_t (*x86_jit_entry_t)(uint32_t remaining_budget);

typedef struct {
  uint8_t *start;
  uint8_t *cur;
  uint8_t *end;
} x86_jit_writer_t;

typedef struct {
  vaddr_t pc;
  vaddr_t cur;
} x86_jit_reader_t;

typedef struct {
  int base_reg;
  int index_reg;
  uint8_t scale;
  uint32_t disp;
} x86_jit_ea_t;

/*
 * Lazy flags describe how much of cpu.eflags is already materialised.  Native
 * ALU emission can leave host flags live and delay copying them into cpu.eflags
 * until a later guest instruction actually reads them or a helper call needs
 * architectural state.
 */
typedef enum {
  X86_LAZY_FLAGS_NONE,
  X86_LAZY_FLAGS_MATERIALISED,
  X86_LAZY_FLAGS_HOST_VALID,
  X86_LAZY_FLAGS_ADD,
  X86_LAZY_FLAGS_SUB,
  X86_LAZY_FLAGS_LOGIC,
  X86_LAZY_FLAGS_INCDEC,
  X86_LAZY_FLAGS_IMUL_CF_OF,
} x86_lazy_flags_kind_t;

typedef struct {
  x86_lazy_flags_kind_t kind;
  uint32_t copy_mask;
  uint32_t clear_mask;
} x86_lazy_flags_t;

/*
 * Per-block emission state.  The small register cache maps guest IA-32 GPRs to
 * selected host registers, while the boolean capability bits record which base
 * pointers and runtime facilities are live in the generated ABI at this point.
 */
typedef struct {
  bool valid;
  int guest_to_host[8];
  int host_to_guest[16];
  bool guest_dirty[8];
  bool guest_loaded[8];
  x86_lazy_flags_t flags;
  bool uses_loop_accounting;
  bool uses_global_loop_accounting;
  bool may_call_helper;
  bool may_touch_pmem;
  bool may_write_guest_code;
  bool has_cpu_base;
  bool has_pmem_base;
  bool has_source_bitmap_base;
  bool trace_mode;
} x86_jit_emit_ctx_t;

/*
 * Stack-window analysis lets generated code keep PUSH/POP/CALL/RET inside a
 * guarded PMEM window.  Store bounds are tracked separately so self-modifying
 * stack writes can still fall back before touching translated source pages.
 */
typedef struct {
  bool valid;
  bool has_store;
  bool has_load;
  bool paged;
  int32_t min_offset;
  int32_t max_offset;
  int32_t store_min_offset;
  int32_t store_max_offset;
} x86_jit_stack_window_t;

/* Decoded IR opcodes: native forms emit host code, HELPERS call jit_helper_exec. */
typedef enum {
  X86_JIT_OP_NOP,
  X86_JIT_OP_MOV_IMM_REG,
  X86_JIT_OP_MOV_REG_REG,
  X86_JIT_OP_LEA,
  X86_JIT_OP_ALU_REG_REG,
  X86_JIT_OP_ALU_IMM_REG,
  X86_JIT_OP_TEST_REG_REG,
  X86_JIT_OP_TEST_EAX_IMM,
  X86_JIT_OP_DOUBLE_SHIFT_REG_IMM,
  X86_JIT_OP_CDQ,
  X86_JIT_OP_JMP_REL,
  X86_JIT_OP_JCC_REL,
  X86_JIT_OP_HELPER,
} x86_jit_op_t;

/*
 * Helper kinds name interpreter-equivalent slow paths.  The decode stores one
 * of these when an instruction is supported semantically but is not worth, or
 * not safe, to inline directly in the current generated block.
 */
typedef enum {
  X86_JIT_HELPER_NONE,
  X86_JIT_HELPER_MOV_RM_REG,
  X86_JIT_HELPER_MOV_REG_RM,
  X86_JIT_HELPER_MOV_IMM_RM,
  X86_JIT_HELPER_MOV_EAX_MOFFS,
  X86_JIT_HELPER_MOV_MOFFS_EAX,
  X86_JIT_HELPER_ALU_RM_REG,
  X86_JIT_HELPER_ALU_REG_RM,
  X86_JIT_HELPER_ALU_IMM_RM,
  X86_JIT_HELPER_ALU_EAX_IMM,
  X86_JIT_HELPER_TEST_RM_REG,
  X86_JIT_HELPER_TEST_EAX_IMM,
  X86_JIT_HELPER_PUSH_REG,
  X86_JIT_HELPER_PUSH_IMM,
  X86_JIT_HELPER_PUSH_RM,
  X86_JIT_HELPER_POP_REG,
  X86_JIT_HELPER_POP_RM,
  X86_JIT_HELPER_CALL_REL,
  X86_JIT_HELPER_CALL_RM,
  X86_JIT_HELPER_RET,
  X86_JIT_HELPER_LEAVE,
  X86_JIT_HELPER_JMP_REL,
  X86_JIT_HELPER_JMP_RM,
  X86_JIT_HELPER_JCC_REL,
  X86_JIT_HELPER_INCDEC_REG,
  X86_JIT_HELPER_INCDEC_RM,
  X86_JIT_HELPER_NOT_RM,
  X86_JIT_HELPER_NEG_RM,
  X86_JIT_HELPER_TEST_IMM_RM,
  X86_JIT_HELPER_MUL_RM,
  X86_JIT_HELPER_IMUL_ACC_RM,
  X86_JIT_HELPER_DIV_RM,
  X86_JIT_HELPER_IDIV_RM,
  X86_JIT_HELPER_SETCC_RM8,
  X86_JIT_HELPER_MOVZX_REG_RM8,
  X86_JIT_HELPER_MOVZX_REG_RM16,
  X86_JIT_HELPER_MOVSX_REG_RM8,
  X86_JIT_HELPER_MOVSX_REG_RM16,
  X86_JIT_HELPER_SHIFT_RM,
  X86_JIT_HELPER_IMUL_REG_RM,
  X86_JIT_HELPER_COUNT,
} x86_jit_helper_t;

/*
 * Predecoded IA-32 instruction.  `width` is a byte count, `alu_op` uses the
 * Intel Group-1 /reg encoding order, and `cc` is the low condition-code nibble
 * used by Jcc/SETcc opcodes.
 */
typedef struct {
  x86_jit_op_t op;
  x86_jit_helper_t helper;
  vaddr_t pc;
  vaddr_t next_pc;
  bool rm_is_reg;
  bool ends_block;
  bool count_from_cl;
  uint8_t dst;
  uint8_t src;
  uint8_t rm_reg;
  uint8_t width;
  uint8_t alu_op;
  uint8_t cc;
  uint16_t ordinal;
  uint32_t imm;
  int32_t rel;
  x86_jit_ea_t ea;
} x86_jit_insn_t;

/* Physical source ranges are used to invalidate native blocks after PMEM writes. */
typedef struct {
  paddr_t start;
  paddr_t end;
} x86_jit_source_range_t;

/* Trace source bytes can come from disjoint guest PCs, so store pc/offset spans. */
typedef struct {
  vaddr_t pc;
  uint16_t offset;
  uint16_t len;
} x86_jit_source_span_t;

/*
 * Page-mode compiled code is identified by more than CR3.  The NEMU x86 MMU
 * implemented here is 32-bit non-PAE paging, so the relevant dynamic inputs are
 * CR3's page-directory base, CR0.PG/CR0.WP, CR4.PSE, CPL, and the conservative
 * page-table/source generation maintained by the JIT invalidation hooks.  PAE,
 * NX, and IA-32e are not modelled in this NEMU path, so native paged fast paths
 * reject PAE instead of adding unsupported architectural bits to this key.
 */
typedef struct {
  uint32_t cr3_key;
  uint32_t state;
  uint32_t paging_generation;
} x86_jit_translation_key_t;

/* Block exit categories used for direct chaining and statistics. */
typedef enum {
  X86_JIT_EXIT_FALLTHROUGH,
  X86_JIT_EXIT_TAKEN,
  X86_JIT_EXIT_JMP,
  X86_JIT_EXIT_CALL,
  X86_JIT_EXIT_RET,
} x86_jit_exit_kind_t;

/* Reasons a chainable exit currently uses its slow stub instead of a target. */
typedef enum {
  X86_JIT_CHAIN_SLOW_UNLINKED,
  X86_JIT_CHAIN_SLOW_COLD_TRACE,
  X86_JIT_CHAIN_SLOW_SIDE_BRANCH,
  X86_JIT_CHAIN_SLOW_HELPER,
  X86_JIT_CHAIN_SLOW_UNACCEPTED_SUCCESSOR,
  X86_JIT_CHAIN_SLOW_BLOCK_NOT_CHAINABLE,
} x86_jit_chain_slow_reason_t;

/* Chainability of each possible successor of a decoded block. */
typedef struct {
  bool fallthrough;
  bool taken;
  bool target;
} x86_jit_edge_chainability_t;

/*
 * Patchable exit edge.  `patch_site` is the rel32 jump emitted in the source
 * block.  It initially targets `slow_exit`, and later may target a hit counter
 * stub or the successor block's chain entry.  `target_generation` prevents
 * stale patches after cache invalidation.
 */
typedef struct {
  bool valid;
  vaddr_t target_pc;
  uint8_t *patch_site;
  uint8_t *slow_exit;
  uint8_t *hit_stub;
  uint8_t *hit_patch_site;
  uint32_t target_generation;
  uint8_t kind;
  uint8_t slow_reason;
} x86_jit_exit_edge_t;

/* Hot block metadata kept in the fast cache line.  Large source bytes live cold. */
typedef struct {
  bool valid;
  bool unsupported;
  bool paging;
  vaddr_t pc;
  uint32_t cr3_key;
  x86_jit_translation_key_t translation_key;
  uint32_t source_len;
  uint8_t unsupported_opcode2;
  x86_jit_entry_t c_entry;
  uint8_t *chain_entry;
  uint8_t *chain_guard_count_imm;
  uint32_t generation;
  bool accepts_chain;
  bool is_trace;
  uint32_t guest_insns;
  uint32_t cache_age;
  bool uses_loop_accounting;
  bool uses_global_loop_accounting;
  uint8_t exit_count;
  x86_jit_exit_edge_t exits[X86_JIT_EXIT_EDGE_LIMIT];
  uint32_t cold_index;
} x86_jit_block_t;

/*
 * Cold block payload: original source bytes, reverse-invalidation indexes, and
 * predecoded trace instructions.  Keeping this separate makes the hot lookup
 * table smaller and improves cache behaviour for common cache hits.
 */
typedef struct {
  uint8_t source[X86_JIT_MAX_SOURCE_BYTES];
  uint16_t source_page_count;
  bool source_page_overflow;
  size_t source_pages[X86_JIT_BLOCK_SOURCE_PAGE_LIMIT];
  uint16_t source_range_count;
  bool source_range_overflow;
  x86_jit_source_range_t source_ranges[X86_JIT_BLOCK_SOURCE_RANGE_LIMIT];
  uint16_t source_span_count;
  x86_jit_source_span_t source_spans[X86_JIT_TRACE_SOURCE_SPAN_LIMIT];
  x86_jit_insn_t insns[X86_JIT_TRACE_MAX_INSNS];
} x86_jit_block_cold_t;

/* Reverse map from a PMEM source page to blocks that may contain its bytes. */
typedef struct {
  uint32_t block_indices[X86_JIT_SOURCE_PAGE_BLOCK_LIMIT];
  uint16_t count;
  bool overflow;
} x86_jit_source_page_blocks_t;

/*
 * Private data TLB entry.  Generated code reads these fields by fixed offsets,
 * so the static asserts below deliberately fail the build if padding changes.
 */
typedef struct {
  bool valid;
  uint8_t access;
  uint32_t cr3_key;
  uint32_t state;
  uint32_t vpn;
  paddr_t pg_paddr;
} x86_jit_dtlb_entry_t;

_Static_assert(offsetof(x86_jit_dtlb_entry_t, valid) == 0,
    "x86 JIT DTLB valid offset changed");
_Static_assert(offsetof(x86_jit_dtlb_entry_t, access) == 1,
    "x86 JIT DTLB access offset changed");
_Static_assert(offsetof(x86_jit_dtlb_entry_t, cr3_key) == 4,
    "x86 JIT DTLB cr3_key offset changed");
_Static_assert(offsetof(x86_jit_dtlb_entry_t, state) == 8,
    "x86 JIT DTLB state offset changed");
_Static_assert(offsetof(x86_jit_dtlb_entry_t, vpn) == 12,
    "x86 JIT DTLB vpn offset changed");
_Static_assert(offsetof(x86_jit_dtlb_entry_t, pg_paddr) == 16,
    "x86 JIT DTLB pg_paddr offset changed");

typedef struct {
  bool valid;
  vaddr_t pc;
  uint32_t cr3_key;
  x86_jit_translation_key_t translation_key;
  uint32_t hot_index;
  uint32_t generation;
} x86_jit_l0_entry_t;

/* Per-PC heat counters and optional trace pointer for trace selection. */
typedef struct {
  bool valid;
  vaddr_t pc;
  uint32_t cr3_key;
  x86_jit_translation_key_t translation_key;
  uint32_t exec_count;
  uint32_t taken_count;
  uint32_t fallthrough_count;
  bool trace_compiled;
  bool trace_failed;
  uint32_t trace_index;
  uint32_t trace_generation;
} x86_jit_hot_info_t;

/*
 * Direct RET target cache entry.  The explicit padding keeps each entry 16
 * bytes, which makes indexed native loads simple and stable.
 */
typedef struct {
  x86_jit_translation_key_t translation_key;
  uint32_t block_generation;
} x86_jit_ret_cache_meta_t;

typedef struct {
  vaddr_t target_pc;
  uint32_t pad;
  uint8_t *chain_entry;
} x86_jit_ret_cache_entry_t;

_Static_assert(sizeof(x86_jit_ret_cache_entry_t) == 16,
    "x86 JIT RET cache entry size changed");
_Static_assert(sizeof(x86_jit_ret_cache_meta_t) == 16,
    "x86 JIT RET cache metadata size changed");
_Static_assert(offsetof(x86_jit_ret_cache_entry_t, target_pc) == 0,
    "x86 JIT RET cache target offset changed");
_Static_assert(offsetof(x86_jit_ret_cache_entry_t, chain_entry) == 8,
    "x86 JIT RET cache chain-entry offset changed");

typedef struct {
  uint8_t *budget_exit_disp;
  uint8_t *target_miss_disp;
  uint8_t *key_cr3_miss_disp;
  uint8_t *key_state_miss_disp;
  uint8_t *key_generation_miss_disp;
  uint8_t *generation_slot_null_disp;
  uint8_t *block_generation_miss_disp;
  uint8_t *entry_null_disp;
} x86_jit_indirect_cache_patches_t;

typedef struct {
  uint64_t exec_requests;
  uint64_t cache_hits;
  uint64_t cache_misses;
  uint64_t blocks_compiled;
  uint64_t paged_blocks_compiled;
  uint64_t blocks_unsupported;
  uint64_t compiled_insns;
  uint64_t blocks_executed;
  uint64_t executed_insns;
  uint64_t unsupported_hits;
  uint64_t native_alu_ops;
  uint64_t native_alu_jcc_fusions;
  uint64_t native_alu_jcc_resident_loops;
  uint64_t native_incdec_ops;
  uint64_t native_incdec_jcc_backedges;
  uint64_t native_incdec_resident_loops;
  uint64_t native_branch_ops;
  uint64_t native_pmem_loads;
  uint64_t native_pmem_stores;
  uint64_t dtlb_read_hits;
  uint64_t dtlb_write_hits;
  uint64_t dtlb_fills;
  uint64_t dtlb_fallbacks;
  uint64_t dtlb_flushes;
  uint64_t native_mul_ops;
  uint64_t native_imul_ops;
  uint64_t native_div_ops;
  uint64_t native_shift_ops;
  uint64_t native_not_ops;
  uint64_t native_movzx_ops;
  uint64_t native_movsx_ops;
  uint64_t helper_calls;
  uint64_t helper_incdec_calls;
  uint64_t helper_incdec_reg_calls;
  uint64_t helper_incdec_rm_calls;
  uint64_t flag_materialisations;
  uint64_t guest_gpr_loads_emitted;
  uint64_t guest_gpr_stores_emitted;
  uint64_t native_pmem_guards_emitted;
  uint64_t direct_chain_patches;
  uint64_t paged_chain_patches;
  uint64_t batch_entries;
  uint64_t traces_compiled;
  uint64_t paged_traces_compiled;
  uint64_t trace_compile_failures;
  uint64_t trace_hits;
  uint64_t paged_trace_hits;
  uint64_t side_exits;
  uint64_t paged_chain_hits;
  uint64_t paged_ret_cache_hits;
  uint64_t paged_ret_cache_misses;
  uint64_t paged_dtlb_read_hits;
  uint64_t paged_dtlb_write_hits;
  uint64_t paged_dtlb_fallbacks;
  uint64_t smc_invalidation_exits;
  uint64_t blocks_chainable;
  uint64_t blocks_not_chainable;
  uint64_t blocks_not_chainable_jmp;
  uint64_t blocks_not_chainable_jcc_one_side;
  uint64_t blocks_not_chainable_jcc_both_sides;
  uint64_t blocks_not_chainable_ret;
  uint64_t blocks_not_chainable_call_rm;
  uint64_t blocks_not_chainable_unsupported_successor;
  uint64_t traces_using_regcache;
  uint64_t traces_not_using_regcache;
  uint64_t helper_by_kind[X86_JIT_HELPER_COUNT];
  uint64_t helper_shift_rm_reg;
  uint64_t helper_shift_rm_mem;
  uint64_t helper_shift_rm_cl;
  uint64_t helper_shift_rm_imm;
  uint64_t helper_shift_rm_width[5];
  uint64_t helper_shift_rm_op[8];
  uint64_t invalidation_requests;
  uint64_t invalidation_page_skips;
  uint64_t precise_invalidation_scans;
  uint64_t invalidated_blocks;
  uint64_t page_table_write_invalidations;
  uint64_t source_alias_invalidations;
  uint64_t cr3_or_paging_key_mismatches;
  uint64_t cross_page_fallbacks;
  uint64_t mmio_fallbacks;
  uint64_t stack_fast_fallbacks;
  uint64_t paged_large_page_fallbacks;
  uint64_t unsupported_paging_mode_fallbacks;
  uint64_t paged_source_validation_failures;
  uint64_t arena_resets;
  uint64_t unsupported_by_opcode[256];
  uint64_t unsupported_hits_by_opcode[256];
  uint64_t unsupported_0f_by_opcode[256];
  uint64_t unsupported_0f_hits_by_opcode[256];
} x86_jit_stats_t;

/* Intel Group-1 ALU /reg field order used by opcodes 80/81/83 and ModR/M.reg. */
enum {
  X86_ALU_ADD,
  X86_ALU_OR,
  X86_ALU_ADC,
  X86_ALU_SBB,
  X86_ALU_AND,
  X86_ALU_SUB,
  X86_ALU_XOR,
  X86_ALU_CMP,
};

/*
 * Intel condition-code low nibble.  Short Jcc is 70H|cc, near Jcc is 0F 80H|cc,
 * and SETcc is 0F 90H|cc.
 */
enum {
  X86_CC_O  = 0x0,  /* OF == 1. */
  X86_CC_NO = 0x1,  /* OF == 0. */
  X86_CC_B  = 0x2,  /* CF == 1; aliases C/NAE, unsigned below. */
  X86_CC_AE = 0x3,  /* CF == 0; aliases NB/NC, unsigned above-or-equal. */
  X86_CC_Z  = 0x4,  /* ZF == 1; alias E. */
  X86_CC_NZ = 0x5,  /* ZF == 0; alias NE. */
  X86_CC_BE = 0x6,  /* CF == 1 || ZF == 1; alias NA. */
  X86_CC_A  = 0x7,  /* CF == 0 && ZF == 0; alias NBE. */
  X86_CC_S  = 0x8,  /* SF == 1. */
  X86_CC_NS = 0x9,  /* SF == 0. */
  X86_CC_P  = 0xa,  /* PF == 1; alias PE. */
  X86_CC_NP = 0xb,  /* PF == 0; alias PO. */
  X86_CC_L  = 0xc,  /* SF != OF; signed less-than. */
  X86_CC_GE = 0xd,  /* SF == OF; signed greater-or-equal. */
  X86_CC_LE = 0xe,  /* ZF == 1 || SF != OF; signed less-or-equal. */
  X86_CC_G  = 0xf,  /* ZF == 0 && SF == OF; signed greater-than. */
};

bool isa_jit_invalidation_active = false;

#if X86_JIT_ENABLED

static x86_jit_block_t jit_cache[X86_JIT_CACHE_SIZE];
static uint8_t *jit_code = NULL;
static size_t jit_code_used = 0;
static bool jit_runtime_options_init = false;
static bool jit_env_force_disable = false;
static bool jit_env_enable = false;
static bool jit_stats_enabled = false;
static bool jit_helpers_enabled = false;
static bool jit_verify_source_enabled = false;
static bool jit_4way_cache_enabled = true;
static bool jit_paged_fastpath_enabled = true;
static bool jit_l0_cache_enabled = true;
static bool jit_regcache_enabled = false;
static bool jit_lazy_flags_enabled = true;
static bool jit_hot_cold_cache_enabled = true;
static bool jit_flat_source_enabled = true;
static bool jit_chain_enabled = true;
static bool jit_trace_enabled = false;
static bool jit_batch_trampoline_enabled = false;
static bool jit_stack_fast_enabled = false;
static bool jit_paged_trace_enabled = true;
static bool jit_paged_chain_enabled = true;
static bool jit_paged_retcache_enabled = false;
static bool jit_paged_batch_enabled = false;
static bool jit_paged_regcache_enabled = false;
static bool jit_paged_stack_fast_enabled = false;
static bool jit_paged_aggressive_enabled = false;
static bool jit_fast_chain_enabled = true;
static bool jit_edge_pc_store_enabled = true;
static bool jit_chain_abort_check_enabled = true;
static bool jit_trace_regcache_enabled = true;
static bool jit_trace_sibling_enabled = true;
static bool jit_trace_loopback_enabled = true;
static bool jit_regcache_wide_enabled = true;
static bool jit_native_idiv_enabled = false;
static bool jit_native_high_byte_test_enabled = false;
static uint32_t jit_runtime_block_limit = X86_JIT_DEFAULT_BLOCK_LIMIT;
static uint32_t jit_trace_hot_threshold = X86_JIT_TRACE_HOT_THRESHOLD;
static x86_jit_block_cold_t jit_cache_cold[X86_JIT_CACHE_SIZE];
static x86_jit_l0_entry_t jit_l0_cache[X86_JIT_L0_SIZE];
static x86_jit_hot_info_t jit_hot_info[X86_JIT_HOT_TABLE_SIZE];
static x86_jit_ret_cache_entry_t jit_ret_cache[X86_JIT_RET_CACHE_SIZE];
static x86_jit_ret_cache_meta_t jit_ret_cache_meta[X86_JIT_RET_CACHE_SIZE];
static const uint32_t *jit_ret_cache_generation_slot[X86_JIT_RET_CACHE_SIZE];
static volatile uint32_t jit_entry_budget = 0;
static volatile uint32_t jit_loop_extra = 0;
static volatile uint32_t jit_chain_abort = 0;
static volatile uint32_t jit_fault_guest_count = 0;
static volatile uint64_t jit_direct_chain_hits_runtime = 0;
static volatile uint64_t jit_trace_hits_runtime = 0;
static volatile uint64_t jit_side_exits_runtime = 0;
static volatile uint64_t jit_chain_exit_budget_runtime = 0;
static volatile uint64_t jit_chain_exit_abort_runtime = 0;
static volatile uint64_t jit_chain_exit_unlinked_runtime = 0;
static volatile uint64_t jit_chain_exit_cold_trace_runtime = 0;
static volatile uint64_t jit_chain_exit_side_branch_runtime = 0;
static volatile uint64_t jit_chain_exit_helper_runtime = 0;
static volatile uint64_t jit_chain_exit_unaccepted_successor_runtime = 0;
static volatile uint64_t jit_chain_exit_block_not_chainable_runtime = 0;
static volatile uint64_t jit_trace_side_exit_taken_runtime = 0;
static volatile uint64_t jit_trace_side_exit_fallthrough_runtime = 0;
static volatile uint64_t jit_trace_loopback_runtime = 0;
static volatile uint64_t jit_smc_invalidation_exits_runtime = 0;
static volatile uint64_t jit_mov_rm_reg_slow_exits_runtime = 0;
static volatile uint64_t jit_ret_cache_hits_runtime = 0;
static volatile uint64_t jit_ret_cache_misses_runtime = 0;
static volatile uint64_t jit_sibling_trace_hits_runtime = 0;
static volatile uint32_t jit_dtlb_scratch = 0;
static volatile uint32_t jit_dtlb_value_scratch = 0;
static bool jit_source_page_has_code[X86_JIT_SOURCE_PAGE_COUNT];
static x86_jit_source_page_blocks_t
    jit_source_page_blocks[X86_JIT_SOURCE_PAGE_COUNT];
static bool jit_page_table_page_has_mapping[X86_JIT_SOURCE_PAGE_COUNT];
static x86_jit_dtlb_entry_t jit_dtlb[X86_JIT_DTLB_SIZE];
static uint32_t jit_cache_age_clock = 1;
static uint32_t jit_cache_generation = 1;
static uint32_t jit_paging_generation = 1;
static x86_jit_stats_t jit_stats;

#if X86_JIT_STATS
static const char *const jit_helper_names[X86_JIT_HELPER_COUNT] = {
  [X86_JIT_HELPER_NONE] = "none",
  [X86_JIT_HELPER_MOV_RM_REG] = "mov-rm-reg",
  [X86_JIT_HELPER_MOV_REG_RM] = "mov-reg-rm",
  [X86_JIT_HELPER_MOV_IMM_RM] = "mov-imm-rm",
  [X86_JIT_HELPER_MOV_EAX_MOFFS] = "mov-eax-moffs",
  [X86_JIT_HELPER_MOV_MOFFS_EAX] = "mov-moffs-eax",
  [X86_JIT_HELPER_ALU_RM_REG] = "alu-rm-reg",
  [X86_JIT_HELPER_ALU_REG_RM] = "alu-reg-rm",
  [X86_JIT_HELPER_ALU_IMM_RM] = "alu-imm-rm",
  [X86_JIT_HELPER_ALU_EAX_IMM] = "alu-eax-imm",
  [X86_JIT_HELPER_TEST_RM_REG] = "test-rm-reg",
  [X86_JIT_HELPER_TEST_EAX_IMM] = "test-eax-imm",
  [X86_JIT_HELPER_PUSH_REG] = "push-reg",
  [X86_JIT_HELPER_PUSH_IMM] = "push-imm",
  [X86_JIT_HELPER_PUSH_RM] = "push-rm",
  [X86_JIT_HELPER_POP_REG] = "pop-reg",
  [X86_JIT_HELPER_POP_RM] = "pop-rm",
  [X86_JIT_HELPER_CALL_REL] = "call-rel",
  [X86_JIT_HELPER_CALL_RM] = "call-rm",
  [X86_JIT_HELPER_RET] = "ret",
  [X86_JIT_HELPER_LEAVE] = "leave",
  [X86_JIT_HELPER_JMP_REL] = "jmp-rel",
  [X86_JIT_HELPER_JMP_RM] = "jmp-rm",
  [X86_JIT_HELPER_JCC_REL] = "jcc-rel",
  [X86_JIT_HELPER_INCDEC_REG] = "incdec-reg",
  [X86_JIT_HELPER_INCDEC_RM] = "incdec-rm",
  [X86_JIT_HELPER_NOT_RM] = "not-rm",
  [X86_JIT_HELPER_NEG_RM] = "neg-rm",
  [X86_JIT_HELPER_TEST_IMM_RM] = "test-imm-rm",
  [X86_JIT_HELPER_MUL_RM] = "mul-rm",
  [X86_JIT_HELPER_IMUL_ACC_RM] = "imul-acc-rm",
  [X86_JIT_HELPER_DIV_RM] = "div-rm",
  [X86_JIT_HELPER_IDIV_RM] = "idiv-rm",
  [X86_JIT_HELPER_SETCC_RM8] = "setcc-rm8",
  [X86_JIT_HELPER_MOVZX_REG_RM8] = "movzx-reg-rm8",
  [X86_JIT_HELPER_MOVZX_REG_RM16] = "movzx-reg-rm16",
  [X86_JIT_HELPER_MOVSX_REG_RM8] = "movsx-reg-rm8",
  [X86_JIT_HELPER_MOVSX_REG_RM16] = "movsx-reg-rm16",
  [X86_JIT_HELPER_SHIFT_RM] = "shift-rm",
  [X86_JIT_HELPER_IMUL_REG_RM] = "imul-reg-rm",
};
#endif

#define JIT_STAT_INC(field) \
  do { \
    if (__builtin_expect(jit_stats_enabled, false)) jit_stats.field++; \
  } while (0)

#define JIT_STAT_ADD(field, value) \
  do { \
    if (__builtin_expect(jit_stats_enabled, false)) jit_stats.field += (value); \
  } while (0)

static bool patch_rel32(uint8_t *disp, const uint8_t *target);
static void jit_unpatch_incoming_edges(const x86_jit_block_t *target);
static void jit_link_edges_to_target(x86_jit_block_t *target);
static void jit_link_block_exits(x86_jit_block_t *block);
static void jit_reset_arena(void);
static void jit_ret_cache_clear(void);
static void jit_dtlb_flush(void);
static void jit_cache_bump_generation(void);
static uint32_t jit_dtlb_state(void);
static bool jit_dtlb_mark_page_tables(vaddr_t addr);
static bool jit_batch_cpu_base_available(void);
static bool jit_target_probe_accepts_chain(vaddr_t pc);
static bool jit_range_may_touch_source_pages(paddr_t addr, int len);
static bool jit_decode_insn(x86_jit_reader_t *r, x86_jit_insn_t *out);
static bool emit_insn(x86_jit_writer_t *w, const x86_jit_insn_t *insn);
static bool emit_paged_dtlb_mov_reg_rm_load(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn);
static bool emit_paged_dtlb_mov_rm_reg_store(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn);
static bool emit_shift_eax_imm(x86_jit_writer_t *w, uint8_t shift_op,
    uint8_t count);
static bool jit_native_alu_writes_result(uint8_t alu_op);
static bool jit_helper_may_touch_guest_memory(const x86_jit_insn_t *insn);
static uint32_t jit_native_alu_flag_copy_mask(uint8_t alu_op);
static bool emit_capture_status_flags(x86_jit_writer_t *w,
    uint32_t copy_mask);
static bool emit_capture_status_flags_custom(x86_jit_writer_t *w,
    uint32_t copy_mask, uint32_t clear_mask);
static bool emit_runtime_counter_inc(x86_jit_writer_t *w,
    volatile uint64_t *counter);
static bool emit_load_chain_abort_ecx(x86_jit_writer_t *w);
static bool emit_load_loop_extra_eax(x86_jit_writer_t *w);
static bool emit_return_completed(x86_jit_writer_t *w, vaddr_t pc,
    uint32_t count);
static bool emit_condition_bool_eax(x86_jit_writer_t *w, uint8_t cc);
static bool emit_store_edx_eax_pair(x86_jit_writer_t *w);
static bool emit_indirect_target_cache_jump(x86_jit_writer_t *w,
    uint32_t count, x86_jit_indirect_cache_patches_t *patches);
static bool emit_indirect_target_cache_slow_exits(x86_jit_writer_t *w,
    x86_jit_indirect_cache_patches_t *patches);
static bool emit_paged_dtlb_translate_addr_eax(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn, uint8_t width, bool is_write,
    uint8_t **slow_disp);
static bool emit_store_dtlb_scratch_eax(x86_jit_writer_t *w);
static bool emit_load_dtlb_scratch_eax(x86_jit_writer_t *w);
static bool emit_alu_rm32_r32(x86_jit_writer_t *w, uint8_t alu_op,
    uint8_t rm, uint8_t reg);
static bool emit_alu_reg_imm32(x86_jit_writer_t *w, uint8_t alu_op,
    uint8_t reg, uint32_t imm);
static bool emit_alu_eax_imm32(x86_jit_writer_t *w, uint8_t alu_op,
    uint32_t imm);
static bool emit_mov_eax_ecx(x86_jit_writer_t *w);
static bool emit_alu_eax_ecx_width(x86_jit_writer_t *w, uint8_t alu_op,
    uint8_t width);
static x86_jit_block_t *jit_compile_trace(vaddr_t pc, uint32_t max_insns,
    x86_jit_hot_info_t *hot);
static void jit_emit_ctx_init(x86_jit_emit_ctx_t *ctx);
static bool jit_decode_block(vaddr_t pc, uint32_t max_insns,
    x86_jit_insn_t *insns, uint32_t *count_out, vaddr_t *end_pc_out);
static void jit_analyse_block(const x86_jit_insn_t *insns, uint32_t count,
    x86_jit_emit_ctx_t *ctx);

/* Return true when an environment flag is set to a non-empty, non-"0" value. */
static bool jit_env_flag_enabled(const char *name) {
  const char *value = getenv(name);
  return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
}

/* Return true only for the explicit "0" spelling used to disable defaults. */
static bool jit_env_flag_disabled(const char *name) {
  const char *value = getenv(name);
  return value != NULL && value[0] != '\0' && strcmp(value, "0") == 0;
}

/* Feature switches default to enabled unless the matching environment flag is 0. */
static bool jit_env_flag_default_enabled(const char *name) {
  return !jit_env_flag_disabled(name);
}

/* Parse an unsigned environment override and clamp it to a safe configured range. */
static uint32_t jit_env_u32(const char *name, uint32_t fallback,
    uint32_t min_value, uint32_t max_value) {
  const char *value = getenv(name);
  if (value == NULL || value[0] == '\0') return fallback;

  char *end = NULL;
  unsigned long parsed = strtoul(value, &end, 0);
  if (end == value || *end != '\0') return fallback;
  if (parsed < min_value) return min_value;
  if (parsed > max_value) return max_value;
  return (uint32_t)parsed;
}

/*
 * Cache all runtime switches once.  These flags intentionally do not change
 * during a NEMU run, because generated code can bake several of them into its
 * ABI and guard layout.
 */
static void jit_init_runtime_options(void) {
  if (!jit_runtime_options_init) {
    jit_env_force_disable = jit_env_flag_enabled("NEMU_DISABLE_JIT");
    jit_stats_enabled = jit_env_flag_enabled("NEMU_JIT_STATS");
    jit_helpers_enabled = !jit_env_flag_disabled("NEMU_X86_JIT_HELPERS");
    jit_verify_source_enabled =
        jit_env_flag_enabled("NEMU_X86_JIT_VERIFY_SOURCE");
    jit_env_enable = !jit_env_flag_disabled("NEMU_X86_JIT");
    jit_4way_cache_enabled =
        jit_env_flag_default_enabled("NEMU_X86_JIT_4WAY_CACHE");
    jit_paged_fastpath_enabled =
        jit_env_flag_default_enabled("NEMU_X86_JIT_PAGED_FASTPATH");
    jit_l0_cache_enabled =
        jit_env_flag_default_enabled("NEMU_X86_JIT_L0_CACHE");
    jit_regcache_enabled =
        jit_env_flag_enabled("NEMU_X86_JIT_REGCACHE");
    jit_lazy_flags_enabled =
        jit_env_flag_default_enabled("NEMU_X86_JIT_LAZY_FLAGS");
    jit_hot_cold_cache_enabled =
        jit_env_flag_default_enabled("NEMU_X86_JIT_HOT_COLD_CACHE");
    jit_flat_source_enabled =
        jit_env_flag_default_enabled("NEMU_X86_JIT_FLAT_SOURCE");
    jit_chain_enabled =
        jit_env_flag_default_enabled("NEMU_X86_JIT_CHAIN");
    jit_trace_enabled =
        jit_env_flag_enabled("NEMU_X86_JIT_TRACE");
    jit_batch_trampoline_enabled =
        jit_env_flag_default_enabled("NEMU_X86_JIT_BATCH");
    jit_stack_fast_enabled =
        jit_env_flag_enabled("NEMU_X86_JIT_STACK_FAST");
    jit_paged_aggressive_enabled =
        jit_env_flag_enabled("NEMU_X86_JIT_PAGED_AGGRESSIVE");
    jit_paged_trace_enabled =
        jit_env_flag_default_enabled("NEMU_X86_JIT_PAGED_TRACE") ||
        jit_paged_aggressive_enabled;
    jit_paged_chain_enabled =
        jit_env_flag_default_enabled("NEMU_X86_JIT_PAGED_CHAIN") ||
        jit_paged_aggressive_enabled;
    jit_paged_retcache_enabled =
        jit_env_flag_enabled("NEMU_X86_JIT_PAGED_RETCACHE") ||
        jit_paged_aggressive_enabled;
    jit_paged_batch_enabled =
        jit_env_flag_default_enabled("NEMU_X86_JIT_PAGED_BATCH") ||
        jit_paged_aggressive_enabled;
    jit_paged_regcache_enabled =
        jit_env_flag_enabled("NEMU_X86_JIT_PAGED_REGCACHE") ||
        jit_paged_aggressive_enabled;
    jit_paged_stack_fast_enabled =
        jit_env_flag_enabled("NEMU_X86_JIT_PAGED_STACK_FAST") ||
        jit_paged_aggressive_enabled;
    jit_fast_chain_enabled =
        jit_env_flag_default_enabled("NEMU_X86_JIT_FAST_CHAIN");
    jit_edge_pc_store_enabled =
        jit_env_flag_default_enabled("NEMU_X86_JIT_EDGE_PC_STORE");
    jit_chain_abort_check_enabled =
        jit_env_flag_default_enabled("NEMU_X86_JIT_CHAIN_ABORT_CHECK");
    jit_trace_regcache_enabled =
        jit_env_flag_default_enabled("NEMU_X86_JIT_TRACE_REGCACHE");
    jit_trace_sibling_enabled =
        jit_env_flag_default_enabled("NEMU_X86_JIT_TRACE_SIBLING");
    jit_trace_loopback_enabled =
        jit_env_flag_default_enabled("NEMU_X86_JIT_TRACE_LOOPBACK");
    jit_regcache_wide_enabled =
        jit_env_flag_default_enabled("NEMU_X86_JIT_REGCACHE_WIDE");
    jit_native_idiv_enabled =
        jit_env_flag_enabled("NEMU_X86_JIT_NATIVE_IDIV");
    jit_native_high_byte_test_enabled =
        jit_env_flag_enabled("NEMU_X86_JIT_HIGH_BYTE_TEST");
    jit_runtime_block_limit = jit_env_u32("NEMU_X86_JIT_BLOCK_LIMIT",
        X86_JIT_DEFAULT_BLOCK_LIMIT, 1u, X86_JIT_BLOCK_MAX_INSNS);
    jit_trace_hot_threshold = jit_env_u32("NEMU_X86_JIT_TRACE_THRESHOLD",
        X86_JIT_TRACE_HOT_THRESHOLD, 1u, UINT32_MAX);
    jit_runtime_options_init = true;
  }
}

/* Public availability checks use this gate before entering or compiling JIT code. */
static bool jit_runtime_disabled(void) {
  jit_init_runtime_options();
  return jit_env_force_disable || !jit_env_enable;
}

/* Helpers can be disabled separately while keeping the tiny native-only subset. */
static bool jit_helper_translation_enabled(void) {
  jit_init_runtime_options();
  return jit_helpers_enabled;
}

#if X86_JIT_STATS
/* Compute a rounded fixed-point ratio with two decimal places. */
static uint64_t jit_ratio_x100(uint64_t numerator, uint64_t denominator) {
  if (denominator == 0) return 0;
  return (numerator * 100u + denominator / 2u) / denominator;
}

/* Compute a rounded percentage scaled by 100, for log output such as 12.34%. */
static uint64_t jit_percent_x100(uint64_t numerator, uint64_t denominator) {
  if (denominator == 0) return 0;
  return (numerator * 10000u + denominator / 2u) / denominator;
}
#endif

/* IA-32 paging is active when CR0.PG is set. */
static bool jit_paging_enabled(void) {
  return (cpu.cr0 & X86_CR0_PG) != 0;
}

/* Key translated blocks by CR3 page-directory base while paging is active. */
static uint32_t jit_cr3_key(void) {
  return jit_paging_enabled() ? (uint32_t)(cpu.cr3 & ~(uint32_t)PAGE_MASK) : 0u;
}

/*
 * Native paged paths follow the implemented NEMU x86 MMU model: 32-bit
 * non-PAE paging.  CR4.PSE is not rejected globally because ordinary 4 KiB
 * PDEs remain legal when PSE is enabled; each PDE.PS leaf is rejected at the
 * page-walk dependency step so the helper/MMU preserves 4 MiB and reserved-bit
 * behaviour.  NEMU has no EFER/NX/IA-32e state in this path, so CR4.PAE is the
 * unsupported mode bit that must stay on the interpreter/helper path.
 */
static bool jit_paging_mode_supported(void) {
  return !jit_paging_enabled() || (cpu.cr4 & X86_CR4_PAE) == 0;
}

static bool jit_paged_fastpath_mode_ready(void) {
  if (jit_paging_mode_supported()) return true;
  JIT_STAT_INC(unsupported_paging_mode_fallbacks);
  return false;
}

static uint32_t jit_paging_state_key(void) {
  if (!jit_paging_enabled()) return 0u;

  uint32_t state = jit_dtlb_state();
  state |= 1u << 4; /* CR0.PG is set. */
  return state;
}

static x86_jit_translation_key_t jit_current_translation_key(void) {
  if (!jit_paging_enabled()) {
    return (x86_jit_translation_key_t){0};
  }

  return (x86_jit_translation_key_t){
    .cr3_key = jit_cr3_key(),
    .state = jit_paging_state_key(),
    .paging_generation = jit_paging_generation,
  };
}

static bool jit_translation_key_equal(x86_jit_translation_key_t a,
    x86_jit_translation_key_t b) {
  return a.cr3_key == b.cr3_key &&
         a.state == b.state &&
         a.paging_generation == b.paging_generation;
}

static bool jit_translation_context_equal(x86_jit_translation_key_t a,
    x86_jit_translation_key_t b) {
  return a.cr3_key == b.cr3_key && a.state == b.state;
}

static bool jit_block_translation_key_matches(const x86_jit_block_t *block) {
  if (block == NULL || !block->valid ||
      block->paging != jit_paging_enabled()) {
    return false;
  }
  if (!block->paging) return block->cr3_key == 0;
  if (!jit_paged_fastpath_mode_ready()) return false;

  const bool matches = jit_translation_key_equal(block->translation_key,
      jit_current_translation_key());
  if (!matches) JIT_STAT_INC(cr3_or_paging_key_mismatches);
  return matches;
}

static bool jit_trace_translation_key_matches(const x86_jit_hot_info_t *hot,
    const x86_jit_block_t *trace) {
  return hot != NULL && trace != NULL &&
         jit_translation_key_equal(hot->translation_key,
             trace->translation_key) &&
         jit_block_translation_key_matches(trace);
}

/* Fast chaining requires the trampoline ABI and is paged only under its guard. */
static bool jit_fast_chain_runtime_enabled(void) {
  jit_init_runtime_options();
  return jit_fast_chain_enabled && jit_batch_trampoline_enabled &&
         (!jit_paging_enabled() ||
          (jit_paged_chain_enabled && jit_paged_batch_enabled));
}

static bool jit_indirect_target_cache_runtime_enabled(void) {
  return !jit_paging_enabled() || jit_paged_retcache_enabled;
}

/* The direct-fetch fast path is safe only when the four data/code segments are flat. */
static bool jit_flat_segments(void) {
  for (uint32_t i = 0; i < 4; i++) {
    if (cpu.seg_cache[i].base != 0 || cpu.seg_cache[i].limit != X86_DWORD_LIMIT) {
      return false;
    }
  }

  return true;
}

/* Fetch instruction bytes directly from PMEM for flat, non-paged execution. */
static bool jit_flat_direct_fetch(vaddr_t pc, uint32_t len,
    const uint8_t **host) {
  if (!jit_flat_source_enabled || host == NULL || len == 0) return false;
  if (jit_paging_enabled() || !jit_flat_segments()) return false;
  if (!in_pmem_range((paddr_t)pc, (int)len)) return false;

  *host = guest_to_host((paddr_t)pc);
  return true;
}

/* Decode the MMU helper's low PAGE_MASK bits into a MEM_RET_* status. */
static int jit_translate_status(paddr_t ret) {
  return (int)(ret & (paddr_t)PAGE_MASK);
}

/* Strip the MMU helper's status bits to recover the translated page base. */
static paddr_t jit_translate_page(paddr_t ret) {
  return ret & ~(paddr_t)PAGE_MASK;
}

/* Translate one guest virtual range to PMEM, preserving the MMU's fault checks. */
static bool jit_vaddr_to_paddr(vaddr_t addr, uint32_t len, int type,
    paddr_t *pa) {
  if (len == 0) return false;

  const int mmu = isa_mmu_check(addr, (int)len, type);
  if (mmu == MMU_DIRECT) {
    *pa = (paddr_t)addr;
    return in_pmem_range(*pa, (int)len);
  }

  if (mmu != MMU_TRANSLATE) return false;
  if (!jit_paged_fastpath_mode_ready()) return false;

  const paddr_t ret = isa_mmu_translate(addr, (int)len, type);
  if (jit_translate_status(ret) != MEM_RET_OK) return false;

  *pa = jit_translate_page(ret) | (paddr_t)(addr & PAGE_MASK);
  return in_pmem_range(*pa, (int)len);
}

static bool jit_translate_ifetch_byte(vaddr_t addr, paddr_t *pa) {
  if (!jit_vaddr_to_paddr(addr, 1u, MEM_TYPE_IFETCH, pa)) return false;
  if (jit_paging_enabled() && !jit_dtlb_mark_page_tables(addr)) {
    JIT_STAT_INC(paged_source_validation_failures);
    return false;
  }
  return true;
}

/* Read one instruction byte through the same path that block validation uses. */
static bool jit_vaddr_read_u8(vaddr_t addr, uint8_t *value) {
  const uint8_t *host = NULL;
  if (jit_flat_direct_fetch(addr, 1u, &host)) {
    *value = *host;
    return true;
  }

  paddr_t pa = 0;
  if (!jit_translate_ifetch_byte(addr, &pa)) return false;
  *value = host_read(guest_to_host(pa), 1);
  return true;
}

/* Advance the decoder by one byte, returning false if instruction fetch faults. */
static bool jit_read_u8(x86_jit_reader_t *r, uint8_t *value) {
  if (!jit_vaddr_read_u8(r->cur, value)) return false;
  r->cur++;
  return true;
}

/* Read a little-endian 32-bit immediate/displacement from the guest stream. */
static bool jit_read_u32(x86_jit_reader_t *r, uint32_t *value) {
  const uint8_t *host = NULL;
  if (jit_flat_direct_fetch(r->cur, 4u, &host)) {
    memcpy(value, host, sizeof(uint32_t));
    r->cur += 4u;
    return true;
  }

  uint32_t data = 0;
  for (uint32_t i = 0; i < 4u; i++) {
    uint8_t byte = 0;
    if (!jit_read_u8(r, &byte)) return false;
    data |= (uint32_t)byte << (i * 8u);
  }
  *value = data;
  return true;
}

/* Read a little-endian 16-bit immediate/displacement from the guest stream. */
static bool jit_read_u16(x86_jit_reader_t *r, uint32_t *value) {
  const uint8_t *host = NULL;
  if (jit_flat_direct_fetch(r->cur, 2u, &host)) {
    uint16_t data = 0;
    memcpy(&data, host, sizeof(data));
    *value = data;
    r->cur += 2u;
    return true;
  }

  uint32_t data = 0;
  for (uint32_t i = 0; i < 2u; i++) {
    uint8_t byte = 0;
    if (!jit_read_u8(r, &byte)) return false;
    data |= (uint32_t)byte << (i * 8u);
  }
  *value = data;
  return true;
}

/* Read an 8-bit signed relative displacement and sign-extend it to int32_t. */
static bool jit_read_i8(x86_jit_reader_t *r, int32_t *value) {
  uint8_t raw = 0;
  if (!jit_read_u8(r, &raw)) return false;
  *value = (int8_t)raw;
  return true;
}

/* Snapshot guest source bytes so later cache hits can detect modified code. */
static bool jit_copy_source(vaddr_t pc, uint32_t len, uint8_t *dst) {
  if (len > X86_JIT_MAX_SOURCE_BYTES) return false;
  if (len == 0) return true;

  const uint8_t *host = NULL;
  if (jit_flat_direct_fetch(pc, len, &host)) {
    memcpy(dst, host, len);
    return true;
  }

  for (uint32_t i = 0; i < len; i++) {
    if (!jit_vaddr_read_u8(pc + i, &dst[i])) return false;
  }
  return true;
}

/* Convert a PMEM address into the source-page bitmap index used for invalidation. */
static bool jit_paddr_source_page(paddr_t addr, size_t *page) {
  if (!in_pmem(addr)) return false;

  const paddr_t offset = addr - (paddr_t)CONFIG_MBASE;
  const size_t idx = (size_t)(offset >> X86_JIT_SOURCE_PAGE_SHIFT);
  if (idx >= X86_JIT_SOURCE_PAGE_COUNT) return false;

  *page = idx;
  return true;
}

/* Check that a physical byte range is non-empty, non-wrapping, and inside PMEM. */
static bool jit_pmem_range(paddr_t addr, uint32_t len) {
  if (len == 0) return false;

  const paddr_t end = addr + (paddr_t)len - 1u;
  return end >= addr && likely(in_pmem(addr) && in_pmem(end));
}

/* Return true when an access reaches past the end of its current 4 KiB page. */
static bool jit_cross_page(vaddr_t addr, uint32_t len) {
  return len == 0 || (uint32_t)(addr & PAGE_MASK) + len > PAGE_SIZE;
}

/*
 * Fold paging permission mode into a small DTLB state key: bits 0..1 are CPL,
 * bit 2 tracks CR0.WP, and bit 3 tracks CR4.PSE.  A change in any of these can
 * change page-permission or large-page behaviour, so cached translations must
 * no longer match.  CR4.PAE is deliberately absent: it is unsupported by this
 * native page walker and guarded as a fallback-only mode before DTLB lookup.
 */
static uint32_t jit_dtlb_state(void) {
  uint32_t state = cpu.cs & 0x3u;

  if ((cpu.cr0 & X86_CR0_WP) != 0) state |= 1u << 2;
  if ((cpu.cr4 & X86_CR4_PSE) != 0) state |= 1u << 3;
  return state;
}

/* Hash virtual page, CR3 key, and permission state into the power-of-two DTLB. */
static uint32_t jit_dtlb_index(uint32_t vpn, uint32_t cr3_key,
    uint32_t state) {
  return (vpn ^ (vpn >> 10) ^ (cr3_key >> PAGE_SHIFT) ^ state) &
      (X86_JIT_DTLB_SIZE - 1u);
}

/* Drop private data translations while preserving page-table dependency marks. */
static void jit_dtlb_clear_entries(void) {
  memset(jit_dtlb, 0, sizeof(jit_dtlb));
  JIT_STAT_INC(dtlb_flushes);
}

/* Drop all private data translations and page-table reverse markers. */
static void jit_dtlb_flush(void) {
  jit_dtlb_clear_entries();
  memset(jit_page_table_page_has_mapping, 0,
      sizeof(jit_page_table_page_has_mapping));
}

/* Remember that a PMEM page was used as a page directory/table page. */
static void jit_mark_page_table_paddr(paddr_t addr) {
  size_t page = 0;
  if (jit_paddr_source_page(addr, &page)) {
    jit_page_table_page_has_mapping[page] = true;
  }
}

/* Quickly reject PMEM writes that cannot touch any page table used by DTLB entries. */
static bool jit_range_may_touch_page_table_pages(paddr_t addr, int len) {
  if (len <= 0) return false;

  const paddr_t end = addr + (paddr_t)len - 1u;
  if (end < addr) return true;

  const paddr_t pmem_start = (paddr_t)CONFIG_MBASE;
  const paddr_t pmem_end = pmem_start + (paddr_t)CONFIG_MSIZE - 1u;
  if (end < pmem_start || addr > pmem_end) return false;

  const paddr_t first_addr = addr < pmem_start ? pmem_start : addr;
  const paddr_t last_addr = end > pmem_end ? pmem_end : end;
  size_t first = 0, last = 0;
  if (!jit_paddr_source_page(first_addr, &first) ||
      !jit_paddr_source_page(last_addr, &last)) {
    return true;
  }

  for (size_t page = first; page <= last; page++) {
    if (jit_page_table_page_has_mapping[page]) return true;
  }

  return false;
}

/*
 * Record page-directory and page-table pages walked for `addr`.  The inline DTLB
 * handles only 4 KiB leaves; 4 MiB PDE.PS translations fall back to the MMU so
 * reserved-bit and large-page semantics stay exact.
 */
static bool jit_dtlb_mark_page_tables(vaddr_t addr) {
  if (!jit_paging_enabled()) return true;

  const paddr_t pd_base = (paddr_t)(cpu.cr3 & X86_PTE_ADDR_MASK);
  const paddr_t pde_addr =
      pd_base + (paddr_t)(((addr >> 22) & 0x3ffu) * sizeof(uint32_t));
  if (!in_pmem_range(pde_addr, 4)) return false;

  jit_mark_page_table_paddr(pde_addr);
  const uint32_t pde = (uint32_t)paddr_read(pde_addr, 4);
  if ((pde & X86_PTE_P) == 0) return false;

  /*
   * Keep the first x86 DTLB step to normal 4 KiB leaves.  The slow helper keeps
   * 4 MiB PDE.PS pages exact, including CR4.PSE reserved-bit fault behaviour.
   */
  if ((pde & X86_PTE_PS) != 0) {
    JIT_STAT_INC(paged_large_page_fallbacks);
    return false;
  }

  const paddr_t pt_base = (paddr_t)(pde & X86_PTE_ADDR_MASK);
  const paddr_t pte_addr =
      pt_base + (paddr_t)(((addr >> 12) & 0x3ffu) * sizeof(uint32_t));
  if (!in_pmem_range(pte_addr, 4)) return false;

  jit_mark_page_table_paddr(pte_addr);
  const uint32_t pte = (uint32_t)paddr_read(pte_addr, 4);
  return (pte & X86_PTE_P) != 0;
}

/* Count a DTLB miss/fallback and signal generated code to use the slow path. */
static void *jit_dtlb_fallback(void) {
  JIT_STAT_INC(dtlb_fallbacks);
  JIT_STAT_INC(paged_dtlb_fallbacks);
  return NULL;
}

static void *jit_dtlb_translate(const x86_jit_insn_t *insn, uint32_t addr,
    uint32_t len, uint32_t is_write) {
  const uint32_t access = is_write ? X86_JIT_DTLB_WRITE : X86_JIT_DTLB_READ;
  const int type = is_write ? MEM_TYPE_WRITE : MEM_TYPE_READ;

  cpu.pc = insn->pc;
  jit_fault_guest_count = jit_loop_extra + insn->ordinal;

  if ((len != X86_WIDTH_BYTE && len != X86_WIDTH_WORD &&
          len != X86_WIDTH_DWORD) ||
      jit_cross_page(addr, len)) {
    if (jit_cross_page(addr, len)) JIT_STAT_INC(cross_page_fallbacks);
    return jit_dtlb_fallback();
  }

  const uint32_t vpn = addr >> PAGE_SHIFT;
  const uint32_t cr3_key = jit_cr3_key();
  const uint32_t state = jit_dtlb_state();
  x86_jit_dtlb_entry_t *entry =
      &jit_dtlb[jit_dtlb_index(vpn, cr3_key, state)];

  /*
   * A DTLB hit still checks PMEM bounds and self-modifying-code/page-table
   * hazards before returning a host pointer.  Writes that may stale code or
   * page walks must use the slow path so the normal invalidation hooks run.
   */
  if (likely(entry->valid &&
             entry->cr3_key == cr3_key &&
             entry->state == state &&
             entry->vpn == vpn &&
             (entry->access & access) != 0)) {
    const paddr_t pa = entry->pg_paddr | (paddr_t)(addr & PAGE_MASK);

    if (!jit_pmem_range(pa, len)) {
      JIT_STAT_INC(mmio_fallbacks);
      return jit_dtlb_fallback();
    }
    if (is_write &&
        (jit_range_may_touch_source_pages(pa, (int)len) ||
            jit_range_may_touch_page_table_pages(pa, (int)len))) {
      return jit_dtlb_fallback();
    }

    if (is_write) {
      JIT_STAT_INC(dtlb_write_hits);
      JIT_STAT_INC(paged_dtlb_write_hits);
    }
    else {
      JIT_STAT_INC(dtlb_read_hits);
      JIT_STAT_INC(paged_dtlb_read_hits);
    }
    return guest_to_host(pa);
  }

  paddr_t pa = 0;
  if (!jit_vaddr_to_paddr(addr, len, type, &pa) ||
      !jit_dtlb_mark_page_tables(addr)) {
    return jit_dtlb_fallback();
  }
  if (!jit_pmem_range(pa, len)) {
    JIT_STAT_INC(mmio_fallbacks);
    return jit_dtlb_fallback();
  }

  if (is_write &&
      (jit_range_may_touch_source_pages(pa, (int)len) ||
          jit_range_may_touch_page_table_pages(pa, (int)len))) {
    return jit_dtlb_fallback();
  }

  *entry = (x86_jit_dtlb_entry_t){
      .valid = true,
      .access = is_write ? (X86_JIT_DTLB_READ | X86_JIT_DTLB_WRITE) : access,
      .cr3_key = cr3_key,
      .state = state,
      .vpn = vpn,
      .pg_paddr = pa & ~(paddr_t)PAGE_MASK,
  };
  JIT_STAT_INC(dtlb_fills);
  return guest_to_host(pa);
}

/* Convert a hot-cache pointer to its index for cold-side arrays and reverse maps. */
static uint32_t jit_block_index(const x86_jit_block_t *block) {
  return (uint32_t)(block - jit_cache);
}

/* Return the cold payload paired with a hot block entry. */
static x86_jit_block_cold_t *jit_block_cold(x86_jit_block_t *block) {
  return &jit_cache_cold[jit_block_index(block)];
}

/* Const version of jit_block_cold() for lookup/validation paths. */
static const x86_jit_block_cold_t *jit_block_cold_const(
    const x86_jit_block_t *block) {
  return &jit_cache_cold[jit_block_index(block)];
}

/* Advance the global cache generation, keeping zero as the invalid sentinel. */
static void jit_cache_bump_generation(void) {
  jit_cache_generation++;
  if (jit_cache_generation == 0) jit_cache_generation = 1;
}

/* Conservative page-table/source generation used by paged block keys. */
static void jit_paging_bump_generation(void) {
  jit_paging_generation++;
  if (jit_paging_generation == 0) jit_paging_generation = 1;
  jit_ret_cache_clear();
}

/* Add one block to a source-page reverse map, or mark overflow for scan fallback. */
static void jit_source_page_add_block(size_t page, uint32_t block_index) {
  if (page >= X86_JIT_SOURCE_PAGE_COUNT) return;

  x86_jit_source_page_blocks_t *blocks = &jit_source_page_blocks[page];
  for (uint16_t i = 0; i < blocks->count; i++) {
    if (blocks->block_indices[i] == block_index) {
      jit_source_page_has_code[page] = true;
      return;
    }
  }

  if (blocks->count < X86_JIT_SOURCE_PAGE_BLOCK_LIMIT) {
    blocks->block_indices[blocks->count++] = block_index;
  }
  else {
    blocks->overflow = true;
  }
  jit_source_page_has_code[page] = true;
}

/* Remove one block from a source-page reverse map after discard/invalidation. */
static void jit_source_page_remove_block(size_t page, uint32_t block_index) {
  if (page >= X86_JIT_SOURCE_PAGE_COUNT) return;

  x86_jit_source_page_blocks_t *blocks = &jit_source_page_blocks[page];
  for (uint16_t i = 0; i < blocks->count; i++) {
    if (blocks->block_indices[i] == block_index) {
      blocks->block_indices[i] = blocks->block_indices[blocks->count - 1u];
      blocks->count--;
      break;
    }
  }

  if (blocks->count == 0 && !blocks->overflow) {
    jit_source_page_has_code[page] = false;
  }
}

/* Detach a block from every PMEM source-page reverse map it registered. */
static void jit_block_unregister_source_pages(x86_jit_block_t *block) {
  x86_jit_block_cold_t *cold = jit_block_cold(block);
  if (cold->source_page_count == 0) return;

  const uint32_t block_index = jit_block_index(block);
  for (uint16_t i = 0; i < cold->source_page_count; i++) {
    jit_source_page_remove_block(cold->source_pages[i], block_index);
  }
  cold->source_page_count = 0;
}

/* Register one PMEM source page for precise invalidation of this block. */
static void jit_block_register_source_page(x86_jit_block_t *block,
    size_t page) {
  x86_jit_block_cold_t *cold = jit_block_cold(block);
  for (uint16_t i = 0; i < cold->source_page_count; i++) {
    if (cold->source_pages[i] == page) return;
  }

  if (cold->source_page_count >= X86_JIT_BLOCK_SOURCE_PAGE_LIMIT) {
    cold->source_page_overflow = true;
    jit_source_page_blocks[page].overflow = true;
    jit_source_page_has_code[page] = true;
    return;
  }

  cold->source_pages[cold->source_page_count++] = page;
  jit_source_page_add_block(page, jit_block_index(block));
}

/* Coalesce adjacent PMEM source bytes into a small set of precise ranges. */
static void jit_block_register_source_range(x86_jit_block_t *block,
    paddr_t pa) {
  x86_jit_block_cold_t *cold = jit_block_cold(block);
  if (cold->source_range_count != 0) {
    x86_jit_source_range_t *last =
        &cold->source_ranges[cold->source_range_count - 1u];
    if (last->end != (paddr_t)-1 && pa == last->end + 1u) {
      last->end = pa;
      return;
    }
  }

  if (cold->source_range_count >= X86_JIT_BLOCK_SOURCE_RANGE_LIMIT) {
    cold->source_range_overflow = true;
    return;
  }

  cold->source_ranges[cold->source_range_count++] =
      (x86_jit_source_range_t){ .start = pa, .end = pa };
}

/* Register the physical bytes reached by a guest virtual instruction range. */
static bool jit_register_source_vaddr_range(x86_jit_block_t *block,
    vaddr_t pc, uint32_t len) {
  if (len == 0) return true;
  x86_jit_block_cold_t *cold = jit_block_cold(block);
  const uint8_t *host = NULL;
  if (jit_flat_direct_fetch(pc, len, &host)) {
    (void)host;
    const paddr_t start = (paddr_t)pc;
    const paddr_t end = start + (paddr_t)len - 1u;
    size_t first = 0;
    size_t last = 0;
    if (end < start || !jit_paddr_source_page(start, &first) ||
        !jit_paddr_source_page(end, &last)) {
      cold->source_page_overflow = true;
      cold->source_range_overflow = true;
      return false;
    }

    for (size_t page = first; page <= last; page++) {
      jit_block_register_source_page(block, page);
    }

    for (paddr_t pa = start; pa <= end; pa++) {
      jit_block_register_source_range(block, pa);
      if (pa == (paddr_t)-1) break;
    }
    return true;
  }

  for (uint32_t i = 0; i < len; i++) {
    paddr_t pa = 0;
    size_t page = 0;
    if (!jit_translate_ifetch_byte(pc + i, &pa) ||
        !jit_paddr_source_page(pa, &page)) {
      cold->source_page_overflow = true;
      cold->source_range_overflow = true;
      JIT_STAT_INC(paged_source_validation_failures);
      return false;
    }
    jit_block_register_source_page(block, page);
    jit_block_register_source_range(block, pa);
  }
  return true;
}

/* Reset and register source-byte reverse maps for one linear block. */
static bool jit_mark_source_pages(x86_jit_block_t *block, vaddr_t pc,
    uint32_t len) {
  x86_jit_block_cold_t *cold = jit_block_cold(block);
  cold->source_page_count = 0;
  cold->source_page_overflow = false;
  cold->source_range_count = 0;
  cold->source_range_overflow = false;
  return jit_register_source_vaddr_range(block, pc, len);
}

/* Reset and register source-byte reverse maps for all spans in a trace block. */
static bool jit_mark_trace_source_pages(x86_jit_block_t *block) {
  x86_jit_block_cold_t *cold = jit_block_cold(block);
  cold->source_page_count = 0;
  cold->source_page_overflow = false;
  cold->source_range_count = 0;
  cold->source_range_overflow = false;

  for (uint16_t i = 0; i < cold->source_span_count; i++) {
    const x86_jit_source_span_t *span = &cold->source_spans[i];
    if (!jit_register_source_vaddr_range(block, span->pc, span->len)) {
      return false;
    }
  }
  return true;
}

/* Mark one block invalid and unpatch all direct edges that target it. */
static void jit_block_invalidate(x86_jit_block_t *block) {
  if (block->valid) {
    jit_ret_cache_clear();
    jit_unpatch_incoming_edges(block);
  }
  if (block->valid) jit_block_unregister_source_pages(block);
  block->valid = false;
  jit_cache_bump_generation();
}

/* Fully remove one block from lookup and source-page reverse indexes. */
static void jit_block_discard(x86_jit_block_t *block) {
  jit_block_invalidate(block);
  memset(block, 0, sizeof(*block));
}

/* Return true when a PMEM write range may overlap any translated source page. */
static bool jit_range_may_touch_source_pages(paddr_t addr, int len) {
  if (len <= 0) return false;

  const paddr_t end = addr + (paddr_t)len - 1u;
  if (end < addr) return true;

  const paddr_t pmem_start = (paddr_t)CONFIG_MBASE;
  const paddr_t pmem_end = pmem_start + (paddr_t)CONFIG_MSIZE - 1u;
  if (end < pmem_start || addr > pmem_end) return false;

  const paddr_t first_addr = addr < pmem_start ? pmem_start : addr;
  const paddr_t last_addr = end > pmem_end ? pmem_end : end;
  size_t first = 0, last = 0;
  if (!jit_paddr_source_page(first_addr, &first) ||
      !jit_paddr_source_page(last_addr, &last)) {
    return true;
  }

  for (size_t page = first; page <= last; page++) {
    if (jit_source_page_has_code[page]) return true;
  }

  return false;
}

static bool jit_source_page_range(paddr_t addr, int len, size_t *first,
    size_t *last) {
  if (len <= 0) return false;

  const paddr_t end = addr + (paddr_t)len - 1u;
  if (end < addr) {
    *first = 0;
    *last = X86_JIT_SOURCE_PAGE_COUNT - 1u;
    return true;
  }

  const paddr_t pmem_start = (paddr_t)CONFIG_MBASE;
  const paddr_t pmem_end = pmem_start + (paddr_t)CONFIG_MSIZE - 1u;
  if (end < pmem_start || addr > pmem_end) return false;

  const paddr_t first_addr = addr < pmem_start ? pmem_start : addr;
  const paddr_t last_addr = end > pmem_end ? pmem_end : end;
  if (!jit_paddr_source_page(first_addr, first) ||
      !jit_paddr_source_page(last_addr, last)) {
    *first = 0;
    *last = X86_JIT_SOURCE_PAGE_COUNT - 1u;
  }
  return true;
}

static bool jit_block_touches_source_page_range(const x86_jit_block_t *block,
    size_t first, size_t last) {
  const x86_jit_block_cold_t *cold = jit_block_cold_const(block);
  if (cold->source_page_overflow) return true;
  for (uint16_t i = 0; i < cold->source_page_count; i++) {
    const size_t page = cold->source_pages[i];
    if (page >= first && page <= last) return true;
  }
  return false;
}

static bool jit_block_source_overlaps_paddr_range(
    const x86_jit_block_t *block, paddr_t addr, paddr_t end) {
  const x86_jit_block_cold_t *cold = jit_block_cold_const(block);
  if (end < addr || cold->source_range_overflow) return true;

  for (uint16_t i = 0; i < cold->source_range_count; i++) {
    const x86_jit_source_range_t *range = &cold->source_ranges[i];
    if (addr <= range->end && end >= range->start) return true;
  }
  return false;
}

/* Build an all-ones mask for an 8-, 16-, or 32-bit guest operand. */
static uint32_t jit_width_mask(uint8_t width) {
  return width == X86_WIDTH_DWORD ? X86_DWORD_LIMIT :
      ((1u << (width * 8u)) - 1u);
}

/* Return the top bit of the selected guest operand width. */
static uint32_t jit_sign_bit(uint8_t width) {
  return 1u << (width * 8u - 1u);
}

/* Truncate a value exactly as an IA-32 destination write of this width would. */
static uint32_t jit_mask_width(uint32_t val, uint8_t width) {
  return val & jit_width_mask(width);
}

/* Sign-extend an 8-, 16-, or 32-bit guest value into the uint32_t working type. */
static uint32_t jit_sign_extend(uint32_t val, uint8_t width) {
  switch (width) {
    case X86_WIDTH_BYTE: return (uint32_t)(int32_t)(int8_t)val;
    case X86_WIDTH_WORD: return (uint32_t)(int32_t)(int16_t)val;
    case X86_WIDTH_DWORD: return val;
    default: panic("x86 JIT helper bad sign-extension width %u", width);
  }
}

/* View the low guest-width bits as a signed value for overflow/divide checks. */
static int64_t jit_signed_width(uint32_t val, uint8_t width) {
  switch (width) {
    case X86_WIDTH_BYTE: return (int8_t)val;
    case X86_WIDTH_WORD: return (int16_t)val;
    case X86_WIDTH_DWORD: return (int32_t)val;
    default: panic("x86 JIT helper bad signed width %u", width);
  }
}

/* Test one architectural EFLAGS bit. */
static bool jit_flag_get(uint32_t flag) {
  return (cpu.eflags & flag) != 0;
}

/* Set or clear one architectural EFLAGS bit while preserving Intel's fixed bit 1. */
static void jit_flag_set(uint32_t flag, bool val) {
  if (val) cpu.eflags |= flag;
  else cpu.eflags &= ~flag;
  cpu.eflags |= X86_EFLAGS_FIXED_ONE;
}

/* Return true when the low byte contains an even number of one bits. */
static bool jit_parity_even(uint8_t val) {
  val ^= val >> 4;
  val &= X86_PARITY_FOLD_NIBBLE_MASK;
  return ((X86_PARITY_FOLD_TABLE >> val) & 1u) == 0;
}

/* Set ZF/SF/PF from a result; these are common to arithmetic and logical ops. */
static void jit_set_zsp_flags(uint32_t result, uint8_t width) {
  result = jit_mask_width(result, width);
  jit_flag_set(X86_FLAG_ZF, result == 0);
  jit_flag_set(X86_FLAG_SF, (result & jit_sign_bit(width)) != 0);
  jit_flag_set(X86_FLAG_PF, jit_parity_even(result & X86_BYTE_MASK));
}

/* Set flags for ADD-like operations: CF is unsigned carry, OF is signed overflow. */
static void jit_set_add_flags(uint32_t lhs, uint32_t rhs, uint32_t result,
    uint8_t width) {
  const uint32_t mask = jit_width_mask(width);
  const uint32_t sign = jit_sign_bit(width);
  const uint32_t l = lhs & mask;
  const uint32_t r = rhs & mask;
  const uint32_t res = result & mask;
  const uint64_t raw = (uint64_t)l + (uint64_t)r;

  jit_set_zsp_flags(res, width);
  jit_flag_set(X86_FLAG_CF, raw > mask);
  jit_flag_set(X86_FLAG_AF, ((l ^ r ^ res) & X86_AUX_CARRY_BIT) != 0);
  jit_flag_set(X86_FLAG_OF, ((~(l ^ r) & (l ^ res) & sign) != 0));
}

/* Set flags for SUB/CMP-like operations; CF represents an unsigned borrow. */
static void jit_set_sub_flags(uint32_t lhs, uint32_t rhs, uint32_t result,
    uint8_t width) {
  const uint32_t mask = jit_width_mask(width);
  const uint32_t sign = jit_sign_bit(width);
  const uint32_t l = lhs & mask;
  const uint32_t r = rhs & mask;
  const uint32_t res = result & mask;

  jit_set_zsp_flags(res, width);
  jit_flag_set(X86_FLAG_CF, l < r);
  jit_flag_set(X86_FLAG_AF, ((l ^ r ^ res) & X86_AUX_CARRY_BIT) != 0);
  jit_flag_set(X86_FLAG_OF, (((l ^ r) & (l ^ res) & sign) != 0));
}

/* Set flags for ADC, including the incoming carry in both unsigned and signed tests. */
static void jit_set_adc_flags(uint32_t lhs, uint32_t rhs, uint32_t result,
    uint8_t width, uint32_t carry) {
  const uint32_t mask = jit_width_mask(width);
  const uint32_t l = lhs & mask;
  const uint32_t r = rhs & mask;
  const uint32_t res = result & mask;
  const uint64_t raw = (uint64_t)l + (uint64_t)r + (uint64_t)carry;
  const int64_t signed_raw = jit_signed_width(lhs, width) +
      jit_signed_width(rhs, width) + (int64_t)carry;
  const int64_t min = -(1ll << (width * X86_BITS_PER_BYTE - 1u));
  const int64_t max = (1ll << (width * X86_BITS_PER_BYTE - 1u)) - 1ll;

  jit_set_zsp_flags(res, width);
  jit_flag_set(X86_FLAG_CF, raw > mask);
  jit_flag_set(X86_FLAG_AF, ((l ^ r ^ res) & X86_AUX_CARRY_BIT) != 0);
  jit_flag_set(X86_FLAG_OF, signed_raw < min || signed_raw > max);
}

/* Set flags for SBB, where CF contributes one extra unit to the subtrahend. */
static void jit_set_sbb_flags(uint32_t lhs, uint32_t rhs, uint32_t result,
    uint8_t width, uint32_t carry) {
  const uint32_t mask = jit_width_mask(width);
  const uint32_t l = lhs & mask;
  const uint32_t r = rhs & mask;
  const uint32_t res = result & mask;
  const uint64_t subtrahend = (uint64_t)r + (uint64_t)carry;
  const int64_t signed_raw = jit_signed_width(lhs, width) -
      jit_signed_width(rhs, width) - (int64_t)carry;
  const int64_t min = -(1ll << (width * X86_BITS_PER_BYTE - 1u));
  const int64_t max = (1ll << (width * X86_BITS_PER_BYTE - 1u)) - 1ll;

  jit_set_zsp_flags(res, width);
  jit_flag_set(X86_FLAG_CF, (uint64_t)l < subtrahend);
  jit_flag_set(X86_FLAG_AF, ((l ^ r ^ res) & X86_AUX_CARRY_BIT) != 0);
  jit_flag_set(X86_FLAG_OF, signed_raw < min || signed_raw > max);
}

/* Logical operations define ZF/SF/PF and clear CF/OF/AF in this model. */
static void jit_set_logic_flags(uint32_t result, uint8_t width) {
  jit_set_zsp_flags(result, width);
  jit_flag_set(X86_FLAG_CF, false);
  jit_flag_set(X86_FLAG_OF, false);
  jit_flag_set(X86_FLAG_AF, false);
}

static uint32_t jit_reg_read(uint8_t reg, uint8_t width);
static void jit_reg_write(uint8_t reg, uint8_t width, uint32_t data);
static uint32_t jit_rm_read(const x86_jit_insn_t *insn, uint8_t width);
static void jit_rm_write_defer_flags(const x86_jit_insn_t *insn, uint8_t width,
    uint32_t data, uint32_t old_eflags, uint32_t new_eflags);

/* Execute one IA-32 Group-1 ALU operation in the helper path. */
static uint32_t jit_alu_exec(uint8_t op, uint32_t lhs, uint32_t rhs,
    uint8_t width) {
  uint32_t result = 0;

  switch (op) {
    case X86_ALU_ADD:
      result = lhs + rhs;
      jit_set_add_flags(lhs, rhs, result, width);
      break;
    case X86_ALU_OR:
      result = lhs | rhs;
      jit_set_logic_flags(result, width);
      break;
    case X86_ALU_ADC: {
      const uint32_t carry = jit_flag_get(X86_FLAG_CF) ? 1u : 0u;
      result = lhs + rhs + carry;
      jit_set_adc_flags(lhs, rhs, result, width, carry);
      break;
    }
    case X86_ALU_SBB: {
      const uint32_t carry = jit_flag_get(X86_FLAG_CF) ? 1u : 0u;
      result = lhs - rhs - carry;
      jit_set_sbb_flags(lhs, rhs, result, width, carry);
      break;
    }
    case X86_ALU_AND:
      result = lhs & rhs;
      jit_set_logic_flags(result, width);
      break;
    case X86_ALU_SUB:
    case X86_ALU_CMP:
      result = lhs - rhs;
      jit_set_sub_flags(lhs, rhs, result, width);
      break;
    case X86_ALU_XOR:
      result = lhs ^ rhs;
      jit_set_logic_flags(result, width);
      break;
    default:
      panic("x86 JIT helper bad ALU op %u", op);
  }

  return jit_mask_width(result, width);
}

/*
 * Helper implementation for Group-2 shifts and rotates.  The /reg field uses
 * Intel order: 0 ROL, 1 ROR, 2 RCL, 3 RCR, 4 SHL/SAL, 5 SHR, 6 SHL/SAL alias,
 * 7 SAR.
 */
static void jit_shift_rm(const x86_jit_insn_t *insn) {
  uint32_t count = insn->count_from_cl ? reg_b(R_ECX) : insn->imm;
  count &= X86_SHIFT_COUNT_MASK;
  if (count == 0) return;

  const uint32_t bits = insn->width * X86_BITS_PER_BYTE;
  uint32_t lhs = jit_rm_read(insn, insn->width);
  uint32_t result = lhs;
  bool cf = false;
  bool of = false;
  const uint32_t old_eflags = cpu.eflags;

  if (insn->alu_op == 2 || insn->alu_op == 3) {
    uint32_t rotate_count = bits == X86_DWORD_BITS ?
        count : count % (bits + 1u);
    if (rotate_count == 0) return;

    const uint64_t operand_mask = jit_width_mask(insn->width);
    const uint64_t ring_mask = (1ull << (bits + 1u)) - 1ull;
    uint64_t ring = ((uint64_t)(jit_flag_get(X86_FLAG_CF) ? 1u : 0u) << bits) |
        (jit_mask_width(lhs, insn->width) & operand_mask);

    if (insn->alu_op == 2) {
      ring = ((ring << rotate_count) |
          (ring >> ((bits + 1u) - rotate_count))) & ring_mask;
    }
    else {
      ring = ((ring >> rotate_count) |
          (ring << ((bits + 1u) - rotate_count))) & ring_mask;
    }

    result = (uint32_t)(ring & operand_mask);
    cf = ((ring >> bits) & 1u) != 0;
    jit_flag_set(X86_FLAG_CF, cf);
    if (rotate_count == 1) {
      if (insn->alu_op == 2) {
        jit_flag_set(X86_FLAG_OF,
            (((result & jit_sign_bit(insn->width)) != 0) != cf));
      }
      else {
        jit_flag_set(X86_FLAG_OF,
            ((result ^ (result << 1)) & jit_sign_bit(insn->width)) != 0);
      }
    }

    const uint32_t new_eflags = cpu.eflags;
    jit_rm_write_defer_flags(insn, insn->width, result,
        old_eflags, new_eflags);
    return;
  }

  if (insn->alu_op == 0 || insn->alu_op == 1) {
    uint32_t rotate_count = count % bits;
    if (rotate_count == 0) return;

    lhs = jit_mask_width(lhs, insn->width);
    if (insn->alu_op == 0) {
      result = jit_mask_width((lhs << rotate_count) |
          (lhs >> (bits - rotate_count)), insn->width);
      cf = (result & 1u) != 0;
      if (rotate_count == 1) {
        of = (((result & jit_sign_bit(insn->width)) != 0) != cf);
      }
    }
    else {
      result = jit_mask_width((lhs >> rotate_count) |
          (lhs << (bits - rotate_count)), insn->width);
      cf = (result & jit_sign_bit(insn->width)) != 0;
      if (rotate_count == 1) {
        of = ((result ^ (result << 1)) & jit_sign_bit(insn->width)) != 0;
      }
    }

    jit_flag_set(X86_FLAG_CF, cf);
    if (rotate_count == 1) jit_flag_set(X86_FLAG_OF, of);
    const uint32_t new_eflags = cpu.eflags;
    jit_rm_write_defer_flags(insn, insn->width, result,
        old_eflags, new_eflags);
    return;
  }

  switch (insn->alu_op) {
    case 4:
    case 6:
      result = jit_mask_width(lhs << count, insn->width);
      if (count <= bits) cf = ((lhs >> (bits - count)) & 1u) != 0;
      if (count == 1) {
        of = (((result ^ lhs) & jit_sign_bit(insn->width)) != 0);
      }
      break;
    case 5:
      result = jit_mask_width(lhs, insn->width) >> count;
      cf = ((lhs >> (count - 1u)) & 1u) != 0;
      if (count == 1) of = (lhs & jit_sign_bit(insn->width)) != 0;
      break;
    case 7:
      result = jit_mask_width(
          (uint32_t)((int32_t)jit_sign_extend(lhs, insn->width) >> count),
          insn->width);
      cf = ((lhs >> (count - 1u)) & 1u) != 0;
      of = false;
      break;
    default:
      panic("x86 JIT helper bad shift op %u", insn->alu_op);
  }

  jit_set_zsp_flags(result, insn->width);
  jit_flag_set(X86_FLAG_CF, cf);
  if (count == 1) jit_flag_set(X86_FLAG_OF, of);
  const uint32_t new_eflags = cpu.eflags;
  jit_rm_write_defer_flags(insn, insn->width, result,
      old_eflags, new_eflags);
}

/* Signed two-operand IMUL: write the truncated low half and flag truncation. */
static void jit_imul_reg_rm(const x86_jit_insn_t *insn) {
  const int64_t lhs = jit_signed_width(jit_reg_read(insn->dst, insn->width),
      insn->width);
  const int64_t rhs = jit_signed_width(jit_rm_read(insn, insn->width),
      insn->width);
  const int64_t product = lhs * rhs;
  const uint32_t low = jit_mask_width((uint32_t)product, insn->width);
  const bool truncated = product != jit_signed_width(low, insn->width);

  jit_reg_write(insn->dst, insn->width, low);
  jit_flag_set(X86_FLAG_CF, truncated);
  jit_flag_set(X86_FLAG_OF, truncated);
}

/* Unsigned one-operand MUL: implicit AL/AX/EAX times r/m, high half sets CF/OF. */
static void jit_mul_rm(const x86_jit_insn_t *insn) {
  const uint32_t lhs = jit_rm_read(insn, insn->width);
  bool high_nonzero = false;

  if (insn->width == X86_WIDTH_BYTE) {
    const uint16_t product = (uint8_t)reg_b(R_AL) * (uint8_t)lhs;
    reg_w(R_AX) = product;
    high_nonzero = (product >> X86_BITS_PER_BYTE) != 0;
  }
  else if (insn->width == X86_WIDTH_WORD) {
    const uint32_t product = (uint16_t)reg_w(R_AX) * (uint16_t)lhs;
    reg_w(R_AX) = product;
    reg_w(R_DX) = product >> X86_WORD_BITS;
    high_nonzero = (product >> X86_WORD_BITS) != 0;
  }
  else {
    const uint64_t product = (uint64_t)cpu.eax *
        (uint64_t)jit_mask_width(lhs, X86_WIDTH_DWORD);
    cpu.eax = product;
    cpu.edx = product >> X86_DWORD_BITS;
    high_nonzero = cpu.edx != 0;
  }

  jit_flag_set(X86_FLAG_CF, high_nonzero);
  jit_flag_set(X86_FLAG_OF, high_nonzero);
}

/* Signed one-operand IMUL with implicit accumulator and high-half truncation flags. */
static void jit_imul_acc_rm(const x86_jit_insn_t *insn) {
  const uint32_t lhs = jit_rm_read(insn, insn->width);
  bool truncated = false;

  if (insn->width == X86_WIDTH_BYTE) {
    const int16_t product = (int16_t)((int8_t)reg_b(R_AL) * (int8_t)lhs);
    reg_w(R_AX) = (uint16_t)product;
    truncated = product != (int16_t)(int8_t)product;
  }
  else if (insn->width == X86_WIDTH_WORD) {
    const int32_t product = (int32_t)((int16_t)reg_w(R_AX) * (int16_t)lhs);
    reg_w(R_AX) = (uint16_t)product;
    reg_w(R_DX) = (uint32_t)product >> X86_WORD_BITS;
    truncated = product != (int32_t)(int16_t)product;
  }
  else {
    const int64_t product = (int64_t)(int32_t)cpu.eax *
        (int64_t)(int32_t)lhs;
    cpu.eax = product;
    cpu.edx = (uint64_t)product >> X86_DWORD_BITS;
    truncated = product != (int64_t)(int32_t)cpu.eax;
  }

  jit_flag_set(X86_FLAG_CF, truncated);
  jit_flag_set(X86_FLAG_OF, truncated);
}

/* Unsigned DIV helper for the implicit AX, DX:AX, or EDX:EAX dividend forms. */
static void jit_div_rm(const x86_jit_insn_t *insn) {
  const uint32_t lhs = jit_rm_read(insn, insn->width);

  if (insn->width == X86_WIDTH_BYTE) {
    const uint16_t dividend = reg_w(R_AX);
    const uint8_t divisor = lhs;
    Assert(divisor != 0, "x86 JIT div by zero at pc = " FMT_WORD, cpu.pc);
    const uint16_t quotient = dividend / divisor;
    const uint8_t remainder = dividend % divisor;
    Assert(quotient <= X86_BYTE_MASK,
        "x86 JIT div quotient overflow at pc = " FMT_WORD, cpu.pc);
    reg_b(R_AL) = quotient;
    reg_b(R_AH) = remainder;
  }
  else if (insn->width == X86_WIDTH_WORD) {
    const uint32_t dividend = ((uint32_t)reg_w(R_DX) << X86_WORD_BITS) |
        reg_w(R_AX);
    const uint16_t divisor = lhs;
    Assert(divisor != 0, "x86 JIT div by zero at pc = " FMT_WORD, cpu.pc);
    const uint32_t quotient = dividend / divisor;
    const uint16_t remainder = dividend % divisor;
    Assert(quotient <= X86_WORD_MASK,
        "x86 JIT div quotient overflow at pc = " FMT_WORD, cpu.pc);
    reg_w(R_AX) = quotient;
    reg_w(R_DX) = remainder;
  }
  else {
    const uint64_t dividend = ((uint64_t)cpu.edx << X86_DWORD_BITS) | cpu.eax;
    const uint32_t divisor = lhs;
    Assert(divisor != 0, "x86 JIT div by zero at pc = " FMT_WORD, cpu.pc);
    const uint64_t quotient = dividend / divisor;
    Assert(quotient <= X86_DWORD_LIMIT,
        "x86 JIT div quotient overflow at pc = " FMT_WORD, cpu.pc);
    cpu.eax = quotient;
    cpu.edx = dividend % divisor;
  }
}

/* Signed IDIV helper for the implicit AX, DX:AX, or EDX:EAX dividend forms. */
static void jit_idiv_rm(const x86_jit_insn_t *insn) {
  const uint32_t lhs = jit_rm_read(insn, insn->width);

  if (insn->width == X86_WIDTH_BYTE) {
    const int16_t dividend = (int16_t)reg_w(R_AX);
    const int8_t divisor = lhs;
    Assert(divisor != 0, "x86 JIT idiv by zero at pc = " FMT_WORD, cpu.pc);
    Assert(!(dividend == INT16_MIN && divisor == -1),
        "x86 JIT idiv quotient overflow at pc = " FMT_WORD, cpu.pc);
    const int16_t quotient = dividend / divisor;
    const int8_t remainder = dividend % divisor;
    Assert(quotient >= INT8_MIN && quotient <= INT8_MAX,
        "x86 JIT idiv quotient overflow at pc = " FMT_WORD, cpu.pc);
    reg_b(R_AL) = quotient;
    reg_b(R_AH) = remainder;
  }
  else if (insn->width == X86_WIDTH_WORD) {
    const int32_t dividend = (int32_t)(((uint32_t)reg_w(R_DX) << X86_WORD_BITS) |
        reg_w(R_AX));
    const int16_t divisor = lhs;
    Assert(divisor != 0, "x86 JIT idiv by zero at pc = " FMT_WORD, cpu.pc);
    Assert(!(dividend == INT32_MIN && divisor == -1),
        "x86 JIT idiv quotient overflow at pc = " FMT_WORD, cpu.pc);
    const int32_t quotient = dividend / divisor;
    const int16_t remainder = dividend % divisor;
    Assert(quotient >= INT16_MIN && quotient <= INT16_MAX,
        "x86 JIT idiv quotient overflow at pc = " FMT_WORD, cpu.pc);
    reg_w(R_AX) = quotient;
    reg_w(R_DX) = remainder;
  }
  else {
    const int64_t dividend =
        (int64_t)(int32_t)cpu.edx * X86_DWORD_BASE + cpu.eax;
    const int32_t divisor = lhs;
    Assert(divisor != 0, "x86 JIT idiv by zero at pc = " FMT_WORD, cpu.pc);
    Assert(!(dividend == INT64_MIN && divisor == -1),
        "x86 JIT idiv quotient overflow at pc = " FMT_WORD, cpu.pc);
    const int64_t quotient = dividend / divisor;
    Assert(quotient >= INT32_MIN && quotient <= INT32_MAX,
        "x86 JIT idiv quotient overflow at pc = " FMT_WORD, cpu.pc);
    cpu.eax = quotient;
    cpu.edx = dividend % divisor;
  }
}

/* Evaluate Intel's condition-code formulas against materialised guest EFLAGS. */
static bool jit_cc_eval(uint8_t cc) {
  const bool cf = jit_flag_get(X86_FLAG_CF);
  const bool zf = jit_flag_get(X86_FLAG_ZF);
  const bool sf = jit_flag_get(X86_FLAG_SF);
  const bool of = jit_flag_get(X86_FLAG_OF);
  const bool pf = jit_flag_get(X86_FLAG_PF);

  switch (cc & 0xfu) {
    case X86_CC_O:  return of;
    case X86_CC_NO: return !of;
    case X86_CC_B:  return cf;
    case X86_CC_AE: return !cf;
    case X86_CC_Z:  return zf;
    case X86_CC_NZ: return !zf;
    case X86_CC_BE: return cf || zf;
    case X86_CC_A:  return !cf && !zf;
    case X86_CC_S:  return sf;
    case X86_CC_NS: return !sf;
    case X86_CC_P:  return pf;
    case X86_CC_NP: return !pf;
    case X86_CC_L:  return sf != of;
    case X86_CC_GE: return sf == of;
    case X86_CC_LE: return zf || (sf != of);
    case X86_CC_G:  return !zf && (sf == of);
    default: panic("x86 JIT helper bad condition code %u", cc);
  }
}

/* Read a guest GPR at the selected byte/word/dword width. */
static uint32_t jit_reg_read(uint8_t reg, uint8_t width) {
  switch (width) {
    case X86_WIDTH_BYTE: return reg_b(reg);
    case X86_WIDTH_WORD: return reg_w(reg);
    case X86_WIDTH_DWORD: return reg_l(reg);
    default: panic("x86 JIT helper bad register width %u", width);
  }
}

/* Write a guest GPR at the selected byte/word/dword width. */
static void jit_reg_write(uint8_t reg, uint8_t width, uint32_t data) {
  switch (width) {
    case X86_WIDTH_BYTE: reg_b(reg) = data; return;
    case X86_WIDTH_WORD: reg_w(reg) = data; return;
    case X86_WIDTH_DWORD: reg_l(reg) = data; return;
    default: panic("x86 JIT helper bad register width %u", width);
  }
}

/* Read guest memory through the architectural virtual-memory path. */
static uint32_t jit_mem_read(vaddr_t addr, uint8_t width) {
  return vaddr_read(addr, width);
}

/* Write guest memory through the architectural virtual-memory path. */
static void jit_mem_write(vaddr_t addr, uint8_t width, uint32_t data) {
  vaddr_write(addr, width, data);
}

/* Compute a 32-bit IA-32 effective address: base + index * scale + displacement. */
static uint32_t jit_ea_addr(const x86_jit_insn_t *insn) {
  uint32_t addr = insn->ea.disp;

  if (insn->ea.base_reg >= 0) {
    addr += reg_l(insn->ea.base_reg);
  }
  if (insn->ea.index_reg >= 0) {
    addr += reg_l(insn->ea.index_reg) << insn->ea.scale;
  }

  return addr;
}

/* Read a decoded ModR/M r/m operand from either a register or memory. */
static uint32_t jit_rm_read(const x86_jit_insn_t *insn, uint8_t width) {
  if (insn->rm_is_reg) {
    return jit_reg_read(insn->rm_reg, width);
  }

  return jit_mem_read(jit_ea_addr(insn), width);
}

/* Write a decoded ModR/M r/m operand to either a register or memory. */
static void jit_rm_write(const x86_jit_insn_t *insn, uint8_t width,
    uint32_t data) {
  if (insn->rm_is_reg) {
    jit_reg_write(insn->rm_reg, width, data);
  }
  else {
    jit_mem_write(jit_ea_addr(insn), width, data);
  }
}

/*
 * For memory destinations, write the data before committing new EFLAGS.  If the
 * memory write faults, the guest observes the old flags, matching interpreter
 * ordering and architectural exception behaviour.
 */
static void jit_rm_write_defer_flags(const x86_jit_insn_t *insn, uint8_t width,
    uint32_t data, uint32_t old_eflags, uint32_t new_eflags) {
  if (insn->rm_is_reg) {
    jit_rm_write(insn, width, data);
    cpu.eflags = new_eflags | X86_EFLAGS_FIXED_ONE;
  }
  else {
    cpu.eflags = old_eflags | X86_EFLAGS_FIXED_ONE;
    jit_rm_write(insn, width, data);
    cpu.eflags = new_eflags | X86_EFLAGS_FIXED_ONE;
  }
}

/* Push a 32-bit value on the IA-32 stack. */
static void jit_push32(uint32_t data) {
  cpu.esp -= X86_WIDTH_DWORD;
  jit_mem_write(cpu.esp, X86_WIDTH_DWORD, data);
}

/* Pop a 32-bit value from the IA-32 stack. */
static uint32_t jit_pop32(void) {
  uint32_t data = jit_mem_read(cpu.esp, X86_WIDTH_DWORD);
  cpu.esp += X86_WIDTH_DWORD;
  return data;
}

/* Resolve a decoded relative branch target from next_pc plus signed displacement. */
static uint32_t jit_branch_target(const x86_jit_insn_t *insn) {
  return (uint32_t)(insn->next_pc + insn->rel);
}

/* Slow semantic executor for decoded helper-backed instructions. */
static void jit_helper_exec(const x86_jit_insn_t *insn) {
  uint32_t lhs = 0, rhs = 0, result = 0;
  uint32_t old_eflags = 0, new_eflags = 0;

  cpu.pc = insn->pc;
  jit_fault_guest_count = jit_loop_extra + insn->ordinal;
  JIT_STAT_INC(helper_calls);
  if (__builtin_expect(jit_stats_enabled, false) &&
      insn->helper < X86_JIT_HELPER_COUNT) {
    jit_stats.helper_by_kind[insn->helper]++;
    if (insn->helper == X86_JIT_HELPER_SHIFT_RM) {
      if (insn->rm_is_reg) {
        jit_stats.helper_shift_rm_reg++;
      }
      else {
        jit_stats.helper_shift_rm_mem++;
      }
      if (insn->count_from_cl) {
        jit_stats.helper_shift_rm_cl++;
      }
      else {
        jit_stats.helper_shift_rm_imm++;
      }
      if (insn->width < 4) jit_stats.helper_shift_rm_width[insn->width]++;
      jit_stats.helper_shift_rm_op[insn->alu_op & 0x7u]++;
    }
  }

  switch (insn->helper) {
    case X86_JIT_HELPER_MOV_RM_REG:
      jit_rm_write(insn, insn->width, jit_reg_read(insn->src, insn->width));
      return;
    case X86_JIT_HELPER_MOV_REG_RM:
      jit_reg_write(insn->dst, insn->width, jit_rm_read(insn, insn->width));
      return;
    case X86_JIT_HELPER_MOV_IMM_RM:
      jit_rm_write(insn, insn->width, insn->imm);
      return;
    case X86_JIT_HELPER_MOV_EAX_MOFFS:
      jit_reg_write(R_EAX, insn->width, jit_mem_read(insn->imm, insn->width));
      return;
    case X86_JIT_HELPER_MOV_MOFFS_EAX:
      jit_mem_write(insn->imm, insn->width, jit_reg_read(R_EAX, insn->width));
      return;
    case X86_JIT_HELPER_ALU_RM_REG:
      lhs = jit_rm_read(insn, insn->width);
      rhs = jit_reg_read(insn->src, insn->width);
      old_eflags = cpu.eflags;
      result = jit_alu_exec(insn->alu_op, lhs, rhs, insn->width);
      if (insn->alu_op != X86_ALU_CMP) {
        new_eflags = cpu.eflags;
        jit_rm_write_defer_flags(insn, insn->width, result,
            old_eflags, new_eflags);
      }
      return;
    case X86_JIT_HELPER_ALU_REG_RM:
      lhs = jit_reg_read(insn->dst, insn->width);
      rhs = jit_rm_read(insn, insn->width);
      result = jit_alu_exec(insn->alu_op, lhs, rhs, insn->width);
      if (insn->alu_op != X86_ALU_CMP) {
        jit_reg_write(insn->dst, insn->width, result);
      }
      return;
    case X86_JIT_HELPER_ALU_IMM_RM:
      lhs = jit_rm_read(insn, insn->width);
      old_eflags = cpu.eflags;
      result = jit_alu_exec(insn->alu_op, lhs, insn->imm, insn->width);
      if (insn->alu_op != X86_ALU_CMP) {
        new_eflags = cpu.eflags;
        jit_rm_write_defer_flags(insn, insn->width, result,
            old_eflags, new_eflags);
      }
      return;
    case X86_JIT_HELPER_ALU_EAX_IMM:
      lhs = jit_reg_read(R_EAX, insn->width);
      result = jit_alu_exec(insn->alu_op, lhs, insn->imm, insn->width);
      if (insn->alu_op != X86_ALU_CMP) {
        jit_reg_write(R_EAX, insn->width, result);
      }
      return;
    case X86_JIT_HELPER_TEST_RM_REG:
      lhs = jit_rm_read(insn, insn->width);
      rhs = jit_reg_read(insn->src, insn->width);
      jit_set_logic_flags(lhs & rhs, insn->width);
      return;
    case X86_JIT_HELPER_TEST_EAX_IMM:
      lhs = jit_reg_read(R_EAX, insn->width);
      jit_set_logic_flags(lhs & insn->imm, insn->width);
      return;
    case X86_JIT_HELPER_PUSH_REG:
      jit_push32(jit_reg_read(insn->src, X86_WIDTH_DWORD));
      return;
    case X86_JIT_HELPER_PUSH_IMM:
      jit_push32(insn->imm);
      return;
    case X86_JIT_HELPER_PUSH_RM:
      jit_push32(jit_rm_read(insn, X86_WIDTH_DWORD));
      return;
    case X86_JIT_HELPER_POP_REG:
      jit_reg_write(insn->dst, X86_WIDTH_DWORD, jit_pop32());
      return;
    case X86_JIT_HELPER_POP_RM:
      jit_rm_write(insn, X86_WIDTH_DWORD, jit_pop32());
      return;
    case X86_JIT_HELPER_CALL_REL:
      jit_push32(insn->next_pc);
      cpu.pc = jit_branch_target(insn);
      return;
    case X86_JIT_HELPER_CALL_RM:
      result = jit_rm_read(insn, X86_WIDTH_DWORD);
      jit_push32(insn->next_pc);
      cpu.pc = result;
      return;
    case X86_JIT_HELPER_RET:
      cpu.pc = jit_pop32();
      return;
    case X86_JIT_HELPER_LEAVE:
      cpu.esp = cpu.ebp;
      cpu.ebp = jit_pop32();
      return;
    case X86_JIT_HELPER_JMP_REL:
      cpu.pc = jit_branch_target(insn);
      return;
    case X86_JIT_HELPER_JMP_RM:
      cpu.pc = jit_rm_read(insn, X86_WIDTH_DWORD);
      return;
    case X86_JIT_HELPER_JCC_REL:
      cpu.pc = jit_cc_eval(insn->cc) ? jit_branch_target(insn) : insn->next_pc;
      return;
    case X86_JIT_HELPER_INCDEC_REG:
      JIT_STAT_INC(helper_incdec_calls);
      JIT_STAT_INC(helper_incdec_reg_calls);
      old_eflags = cpu.eflags;
      lhs = jit_reg_read(insn->dst, insn->width);
      result = jit_alu_exec(insn->alu_op, lhs, 1u, insn->width);
      jit_flag_set(X86_FLAG_CF, (old_eflags & X86_FLAG_CF) != 0);
      jit_reg_write(insn->dst, insn->width, result);
      return;
    case X86_JIT_HELPER_INCDEC_RM:
      JIT_STAT_INC(helper_incdec_calls);
      JIT_STAT_INC(helper_incdec_rm_calls);
      old_eflags = cpu.eflags;
      lhs = jit_rm_read(insn, insn->width);
      result = jit_alu_exec(insn->alu_op, lhs, 1u, insn->width);
      jit_flag_set(X86_FLAG_CF, (old_eflags & X86_FLAG_CF) != 0);
      new_eflags = cpu.eflags;
      jit_rm_write_defer_flags(insn, insn->width, result,
          old_eflags, new_eflags);
      return;
    case X86_JIT_HELPER_NOT_RM:
      jit_rm_write(insn, insn->width, ~jit_rm_read(insn, insn->width));
      return;
    case X86_JIT_HELPER_NEG_RM:
      old_eflags = cpu.eflags;
      lhs = jit_rm_read(insn, insn->width);
      result = jit_alu_exec(X86_ALU_SUB, 0, lhs, insn->width);
      jit_flag_set(X86_FLAG_CF, jit_mask_width(lhs, insn->width) != 0);
      new_eflags = cpu.eflags;
      jit_rm_write_defer_flags(insn, insn->width, result,
          old_eflags, new_eflags);
      return;
    case X86_JIT_HELPER_TEST_IMM_RM:
      jit_set_logic_flags(jit_rm_read(insn, insn->width) & insn->imm,
          insn->width);
      return;
    case X86_JIT_HELPER_MUL_RM:
      jit_mul_rm(insn);
      return;
    case X86_JIT_HELPER_IMUL_ACC_RM:
      jit_imul_acc_rm(insn);
      return;
    case X86_JIT_HELPER_DIV_RM:
      jit_div_rm(insn);
      return;
    case X86_JIT_HELPER_IDIV_RM:
      jit_idiv_rm(insn);
      return;
    case X86_JIT_HELPER_SETCC_RM8:
      jit_rm_write(insn, X86_WIDTH_BYTE, jit_cc_eval(insn->cc) ? 1u : 0u);
      return;
    case X86_JIT_HELPER_MOVZX_REG_RM8:
      jit_reg_write(insn->dst, X86_WIDTH_DWORD,
          jit_rm_read(insn, X86_WIDTH_BYTE));
      return;
    case X86_JIT_HELPER_MOVZX_REG_RM16:
      jit_reg_write(insn->dst, X86_WIDTH_DWORD,
          jit_rm_read(insn, X86_WIDTH_WORD));
      return;
    case X86_JIT_HELPER_MOVSX_REG_RM8:
      jit_reg_write(insn->dst, insn->width,
          jit_sign_extend(jit_rm_read(insn, X86_WIDTH_BYTE), X86_WIDTH_BYTE));
      return;
    case X86_JIT_HELPER_MOVSX_REG_RM16:
      jit_reg_write(insn->dst, insn->width,
          jit_sign_extend(jit_rm_read(insn, X86_WIDTH_WORD), X86_WIDTH_WORD));
      return;
    case X86_JIT_HELPER_SHIFT_RM:
      jit_shift_rm(insn);
      return;
    case X86_JIT_HELPER_IMUL_REG_RM:
      jit_imul_reg_rm(insn);
      return;
    default:
      panic("x86 JIT helper bad helper op %u", insn->helper);
  }
}

/*
 * Raw x86-64 emitter layer.  The byte constants in these tiny functions are
 * host-machine opcodes, ModR/M bytes, SIB bytes, prefixes, or immediates.  The
 * function name is the assembly contract: for example emit_movabs_rdx() emits
 * `REX.W + BA rd io` for `mov rdx, imm64`, and emit_jcc_rel32_placeholder()
 * emits `0F 80+cc rel32` with the displacement patched later.
 */
/* Append one byte to the native code buffer. */
static bool emit_u8(x86_jit_writer_t *w, uint8_t value) {
  if (w->cur >= w->end) return false;
  *w->cur++ = value;
  return true;
}

/* Append a little-endian 32-bit immediate/displacement. */
static bool emit_u32(x86_jit_writer_t *w, uint32_t value) {
  for (uint32_t i = 0; i < 4; i++) {
    if (!emit_u8(w, (uint8_t)(value >> (i * 8)))) return false;
  }
  return true;
}

/* Append a little-endian 64-bit immediate/displacement. */
static bool emit_u64(x86_jit_writer_t *w, uint64_t value) {
  for (uint32_t i = 0; i < 8; i++) {
    if (!emit_u8(w, (uint8_t)(value >> (i * 8)))) return false;
  }
  return true;
}

/* Emit host code for movabs rdx; bytes below are x86-64 encodings. */
static bool emit_movabs_rdx(x86_jit_writer_t *w, uint64_t value) {
  return emit_u8(w, 0x48) && emit_u8(w, 0xba) && emit_u64(w, value);
}

/* Emit host code for movabs rax; bytes below are x86-64 encodings. */
static bool emit_movabs_rax(x86_jit_writer_t *w, uint64_t value) {
  return emit_u8(w, 0x48) && emit_u8(w, 0xb8) && emit_u64(w, value);
}

/* Emit host code for mov eax moffs64; bytes below are x86-64 encodings. */
static bool emit_mov_eax_moffs64(x86_jit_writer_t *w, uint64_t addr) {
  return emit_u8(w, 0xa1) && emit_u64(w, addr);
}

/* Emit host code for mov moffs64 eax; bytes below are x86-64 encodings. */
static bool emit_mov_moffs64_eax(x86_jit_writer_t *w, uint64_t addr) {
  return emit_u8(w, 0xa3) && emit_u64(w, addr);
}

/* Emit host code for movabs rdi; bytes below are x86-64 encodings. */
static bool emit_movabs_rdi(x86_jit_writer_t *w, uint64_t value) {
  return emit_u8(w, 0x48) && emit_u8(w, 0xbf) && emit_u64(w, value);
}

/* Emit host code for mov r10 r13; bytes below are x86-64 encodings. */
static bool emit_mov_r10_r13(x86_jit_writer_t *w) {
  return emit_u8(w, 0x4d) && emit_u8(w, 0x89) && emit_u8(w, 0xea);
}

/* Emit host code for mov r10 r15; bytes below are x86-64 encodings. */
static bool emit_mov_r10_r15(x86_jit_writer_t *w) {
  return emit_u8(w, 0x4d) && emit_u8(w, 0x89) && emit_u8(w, 0xfa);
}

/* Emit host code for movabs r10; bytes below are x86-64 encodings. */
static bool emit_movabs_r10(x86_jit_writer_t *w, uint64_t value) {
  /*
   * The batch trampoline pins PMEM and source-bitmap bases in callee-saved
   * registers.  Reusing r13/r15 avoids repeated 64-bit immediates in hot code.
   */
  if (jit_batch_cpu_base_available()) {
    if (value == (uint64_t)(uintptr_t)guest_to_host(CONFIG_MBASE)) {
      return emit_mov_r10_r13(w);
    }
    if (value == (uint64_t)(uintptr_t)jit_source_page_has_code) {
      return emit_mov_r10_r15(w);
    }
  }

  return emit_u8(w, 0x49) && emit_u8(w, 0xba) && emit_u64(w, value);
}

/* Emit host code for mov r10 ret cache base; bytes below are x86-64 encodings. */
static bool emit_mov_r10_ret_cache_base(x86_jit_writer_t *w) {
  return emit_u8(w, 0x49) && emit_u8(w, 0xba) &&
         emit_u64(w, (uint64_t)(uintptr_t)jit_ret_cache);
}

/* Emit host code for mov r10 ret-cache metadata base. */
static bool emit_mov_r10_ret_cache_meta_base(x86_jit_writer_t *w) {
  return emit_u8(w, 0x49) && emit_u8(w, 0xba) &&
         emit_u64(w, (uint64_t)(uintptr_t)jit_ret_cache_meta);
}

/* Emit host code for mov r10 ret-cache generation-slot base. */
static bool emit_mov_r10_ret_cache_generation_slot_base(x86_jit_writer_t *w) {
  return emit_u8(w, 0x49) && emit_u8(w, 0xba) &&
         emit_u64(w, (uint64_t)(uintptr_t)jit_ret_cache_generation_slot);
}

/* Emit host code for movabs r11; bytes below are x86-64 encodings. */
static bool emit_movabs_r11(x86_jit_writer_t *w, uint64_t value) {
  return emit_u8(w, 0x49) && emit_u8(w, 0xbb) && emit_u64(w, value);
}

/* Emit host code for call rax; bytes below are x86-64 encodings. */
static bool emit_call_rax(x86_jit_writer_t *w) {
  return emit_u8(w, 0xff) && emit_u8(w, 0xd0);
}

/* Emit host code for push rdi; bytes below are x86-64 encodings. */
static bool emit_push_rdi(x86_jit_writer_t *w) {
  return emit_u8(w, 0x57);
}

/* Emit host code for pop rdi; bytes below are x86-64 encodings. */
static bool emit_pop_rdi(x86_jit_writer_t *w) {
  return emit_u8(w, 0x5f);
}

/* Emit host code for push rsi; bytes below are x86-64 encodings. */
static bool emit_push_rsi(x86_jit_writer_t *w) {
  return emit_u8(w, 0x56);
}

/* Emit host code for pop rsi; bytes below are x86-64 encodings. */
static bool emit_pop_rsi(x86_jit_writer_t *w) {
  return emit_u8(w, 0x5e);
}

/* Emit host code for push rdx; bytes below are x86-64 encodings. */
static bool emit_push_rdx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x52);
}

/* Emit host code for pop rdx; bytes below are x86-64 encodings. */
static bool emit_pop_rdx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x5a);
}

/* Emit host code for sub rsp imm8; bytes below are x86-64 encodings. */
static bool emit_sub_rsp_imm8(x86_jit_writer_t *w, uint8_t value) {
  return emit_u8(w, 0x48) && emit_u8(w, 0x83) &&
         emit_u8(w, 0xec) && emit_u8(w, value);
}

/* Emit host code for add rsp imm8; bytes below are x86-64 encodings. */
static bool emit_add_rsp_imm8(x86_jit_writer_t *w, uint8_t value) {
  return emit_u8(w, 0x48) && emit_u8(w, 0x83) &&
         emit_u8(w, 0xc4) && emit_u8(w, value);
}

/* Emit host code for mov eax imm32; bytes below are x86-64 encodings. */
static bool emit_mov_eax_imm32(x86_jit_writer_t *w, uint32_t value) {
  return emit_u8(w, 0xb8) && emit_u32(w, value);
}

/* Emit host code for mov ecx imm32; bytes below are x86-64 encodings. */
static bool emit_mov_ecx_imm32(x86_jit_writer_t *w, uint32_t value) {
  return emit_u8(w, 0xb9) && emit_u32(w, value);
}

/* Emit host code for mov edx imm32; bytes below are x86-64 encodings. */
static bool emit_mov_edx_imm32(x86_jit_writer_t *w, uint32_t value) {
  return emit_u8(w, 0xba) && emit_u32(w, value);
}

/* Emit host code for mov r11d imm32; bytes below are x86-64 encodings. */
static bool emit_mov_r11d_imm32(x86_jit_writer_t *w, uint32_t value) {
  return emit_u8(w, 0x41) && emit_u8(w, 0xbb) && emit_u32(w, value);
}

/* Emit host code for xor edx edx; bytes below are x86-64 encodings. */
static bool emit_xor_edx_edx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x31) && emit_u8(w, 0xd2);
}

/* Emit host code for mov m32 rdx imm32; bytes below are x86-64 encodings. */
static bool emit_mov_m32_rdx_imm32(x86_jit_writer_t *w, uint32_t value) {
  return emit_u8(w, 0xc7) && emit_u8(w, 0x02) && emit_u32(w, value);
}

/* Emit host code for mov byte ptr [rdx], al; it leaves host flags unchanged. */
static bool emit_mov_m8_rdx_al(x86_jit_writer_t *w) {
  return emit_u8(w, 0x88) && emit_u8(w, 0x02);
}

/* Emit host code for mov m16 r11 ax; bytes below are x86-64 encodings. */
static bool emit_mov_m16_r11_ax(x86_jit_writer_t *w) {
  return emit_u8(w, 0x66) && emit_u8(w, 0x41) &&
         emit_u8(w, 0x89) && emit_u8(w, 0x03);
}

/* Emit host code for mov m16 r11 dx; bytes below are x86-64 encodings. */
static bool emit_mov_m16_r11_dx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x66) && emit_u8(w, 0x41) &&
         emit_u8(w, 0x89) && emit_u8(w, 0x13);
}

static bool emit_mov_m32_r12_disp32_imm32(x86_jit_writer_t *w,
    uint32_t disp, uint32_t value) {
  return emit_u8(w, 0x41) && emit_u8(w, 0xc7) &&
         emit_u8(w, 0x84) && emit_u8(w, 0x24) &&
         emit_u32(w, disp) && emit_u32(w, value);
}

/* Emit host code for mov ecx m32 rdx; bytes below are x86-64 encodings. */
static bool emit_mov_ecx_m32_rdx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x8b) && emit_u8(w, 0x0a);
}

/* Emit host code for mov ecx m32 r10; bytes below are x86-64 encodings. */
static bool emit_mov_ecx_m32_r10(x86_jit_writer_t *w) {
  return emit_u8(w, 0x41) && emit_u8(w, 0x8b) && emit_u8(w, 0x0a);
}

/* Emit host code for mov ecx m32 r11; bytes below are x86-64 encodings. */
static bool emit_mov_ecx_m32_r11(x86_jit_writer_t *w) {
  return emit_u8(w, 0x41) && emit_u8(w, 0x8b) && emit_u8(w, 0x0b);
}

/* Emit host code for mov r10d m32 r10; bytes below are x86-64 encodings. */
static bool emit_mov_r10d_m32_r10(x86_jit_writer_t *w) {
  return emit_u8(w, 0x45) && emit_u8(w, 0x8b) && emit_u8(w, 0x12);
}

/* Emit host code for mov eax m32 r12 disp32; bytes below are x86-64 encodings. */
static bool emit_mov_eax_m32_r12_disp32(x86_jit_writer_t *w, uint32_t disp) {
  return emit_u8(w, 0x41) && emit_u8(w, 0x8b) &&
         emit_u8(w, 0x84) && emit_u8(w, 0x24) && emit_u32(w, disp);
}

/* Emit host code for mov ecx m32 r12 disp32; bytes below are x86-64 encodings. */
static bool emit_mov_ecx_m32_r12_disp32(x86_jit_writer_t *w, uint32_t disp) {
  return emit_u8(w, 0x41) && emit_u8(w, 0x8b) &&
         emit_u8(w, 0x8c) && emit_u8(w, 0x24) && emit_u32(w, disp);
}

/* Emit host code for mov r11d m32 r12 disp32; bytes below are x86-64 encodings. */
static bool emit_mov_r11d_m32_r12_disp32(x86_jit_writer_t *w, uint32_t disp) {
  return emit_u8(w, 0x45) && emit_u8(w, 0x8b) &&
         emit_u8(w, 0x9c) && emit_u8(w, 0x24) && emit_u32(w, disp);
}

/* Emit host code for mov m32 r12 disp32 eax; bytes below are x86-64 encodings. */
static bool emit_mov_m32_r12_disp32_eax(x86_jit_writer_t *w, uint32_t disp) {
  return emit_u8(w, 0x41) && emit_u8(w, 0x89) &&
         emit_u8(w, 0x84) && emit_u8(w, 0x24) && emit_u32(w, disp);
}

/* Emit host code for mov eax m32 r10 rdx; bytes below are x86-64 encodings. */
static bool emit_mov_eax_m32_r10_rdx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x41) && emit_u8(w, 0x8b) &&
         emit_u8(w, 0x04) && emit_u8(w, 0x12);
}

/* Emit host code for mov eax m32 r13 rdx; bytes below are x86-64 encodings. */
static bool emit_mov_eax_m32_r13_rdx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x41) && emit_u8(w, 0x8b) &&
         emit_u8(w, 0x44) && emit_u8(w, 0x15) && emit_u8(w, 0x00);
}

/* Emit host code for mov ecx m32 r10 rdx; bytes below are x86-64 encodings. */
static bool emit_mov_ecx_m32_r10_rdx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x41) && emit_u8(w, 0x8b) &&
         emit_u8(w, 0x0c) && emit_u8(w, 0x12);
}

/* Emit host code for movzx eax m8 r10 rdx; bytes below are x86-64 encodings. */
static bool emit_movzx_eax_m8_r10_rdx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x41) && emit_u8(w, 0x0f) &&
         emit_u8(w, 0xb6) && emit_u8(w, 0x04) && emit_u8(w, 0x12);
}

/* Emit host code for movzx eax m16 r10 rdx; bytes below are x86-64 encodings. */
static bool emit_movzx_eax_m16_r10_rdx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x41) && emit_u8(w, 0x0f) &&
         emit_u8(w, 0xb7) && emit_u8(w, 0x04) && emit_u8(w, 0x12);
}

/* Emit host code for movsx eax m8 r10 rdx; bytes below are x86-64 encodings. */
static bool emit_movsx_eax_m8_r10_rdx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x41) && emit_u8(w, 0x0f) &&
         emit_u8(w, 0xbe) && emit_u8(w, 0x04) && emit_u8(w, 0x12);
}

/* Emit host code for movsx eax m16 r10 rdx; bytes below are x86-64 encodings. */
static bool emit_movsx_eax_m16_r10_rdx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x41) && emit_u8(w, 0x0f) &&
         emit_u8(w, 0xbf) && emit_u8(w, 0x04) && emit_u8(w, 0x12);
}

/* Emit host code for movsx eax al; bytes below are x86-64 encodings. */
static bool emit_movsx_eax_al(x86_jit_writer_t *w) {
  return emit_u8(w, 0x0f) && emit_u8(w, 0xbe) && emit_u8(w, 0xc0);
}

/* Emit host code for movsx eax ax; bytes below are x86-64 encodings. */
static bool emit_movsx_eax_ax(x86_jit_writer_t *w) {
  return emit_u8(w, 0x0f) && emit_u8(w, 0xbf) && emit_u8(w, 0xc0);
}

/* Emit host code for movzx ecx m8 r10 rdx; bytes below are x86-64 encodings. */
static bool emit_movzx_ecx_m8_r10_rdx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x41) && emit_u8(w, 0x0f) &&
         emit_u8(w, 0xb6) && emit_u8(w, 0x0c) && emit_u8(w, 0x12);
}

/* Emit host code for movzx ecx m16 r10 rdx; bytes below are x86-64 encodings. */
static bool emit_movzx_ecx_m16_r10_rdx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x41) && emit_u8(w, 0x0f) &&
         emit_u8(w, 0xb7) && emit_u8(w, 0x0c) && emit_u8(w, 0x12);
}

/* Emit host code for mov m8 r10 rdx al; bytes below are x86-64 encodings. */
static bool emit_mov_m8_r10_rdx_al(x86_jit_writer_t *w) {
  return emit_u8(w, 0x41) && emit_u8(w, 0x88) &&
         emit_u8(w, 0x04) && emit_u8(w, 0x12);
}

/* Emit host code for mov m16 r10 rdx ax; bytes below are x86-64 encodings. */
static bool emit_mov_m16_r10_rdx_ax(x86_jit_writer_t *w) {
  return emit_u8(w, 0x66) && emit_u8(w, 0x41) && emit_u8(w, 0x89) &&
         emit_u8(w, 0x04) && emit_u8(w, 0x12);
}

/* Emit host code for mov m32 r10 rdx r11d; bytes below are x86-64 encodings. */
static bool emit_mov_m32_r10_rdx_r11d(x86_jit_writer_t *w) {
  return emit_u8(w, 0x45) && emit_u8(w, 0x89) &&
         emit_u8(w, 0x1c) && emit_u8(w, 0x12);
}

/* Emit host code for mov m32 r13 rdx r11d; bytes below are x86-64 encodings. */
static bool emit_mov_m32_r13_rdx_r11d(x86_jit_writer_t *w) {
  return emit_u8(w, 0x45) && emit_u8(w, 0x89) &&
         emit_u8(w, 0x5c) && emit_u8(w, 0x15) && emit_u8(w, 0x00);
}

/* Emit host code for mov r11d m32 r10 rdx; bytes below are x86-64 encodings. */
static bool emit_mov_r11d_m32_r10_rdx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x45) && emit_u8(w, 0x8b) &&
         emit_u8(w, 0x1c) && emit_u8(w, 0x12);
}

static bool emit_mov_r11d_m32_r10_rcx_disp8(x86_jit_writer_t *w,
    uint8_t disp) {
  return emit_u8(w, 0x45) && emit_u8(w, 0x8b) &&
         emit_u8(w, 0x5c) && emit_u8(w, 0x0a) && emit_u8(w, disp);
}

static bool emit_mov_r11_m64_r10_rcx_disp8(x86_jit_writer_t *w,
    uint8_t disp) {
  return emit_u8(w, 0x4d) && emit_u8(w, 0x8b) &&
         emit_u8(w, 0x5c) && emit_u8(w, 0x0a) && emit_u8(w, disp);
}

/* Emit host code for mov r11 m64 r10 rdx; bytes below are x86-64 encodings. */
static bool emit_mov_r11_m64_r10_rdx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x4d) && emit_u8(w, 0x8b) &&
         emit_u8(w, 0x1c) && emit_u8(w, 0x12);
}

/* Emit host code for mov eax m32 r10 disp8; bytes below are x86-64 encodings. */
static bool emit_mov_eax_m32_r10_disp8(x86_jit_writer_t *w, uint8_t disp) {
  return emit_u8(w, 0x41) && emit_u8(w, 0x8b) &&
         emit_u8(w, 0x42) && emit_u8(w, disp);
}

/* Emit host code for mov m32 r10 rdx eax; bytes below are x86-64 encodings. */
static bool emit_mov_m32_r10_rdx_eax(x86_jit_writer_t *w) {
  return emit_u8(w, 0x41) && emit_u8(w, 0x89) &&
         emit_u8(w, 0x04) && emit_u8(w, 0x12);
}

/* Emit host code for not m32 r10 rdx; bytes below are x86-64 encodings. */
static bool emit_not_m32_r10_rdx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x41) && emit_u8(w, 0xf7) &&
         emit_u8(w, 0x14) && emit_u8(w, 0x12);
}

/* Emit host code for mov r11d m32 r11; bytes below are x86-64 encodings. */
static bool emit_mov_r11d_m32_r11(x86_jit_writer_t *w) {
  return emit_u8(w, 0x45) && emit_u8(w, 0x8b) && emit_u8(w, 0x1b);
}

/* Emit host code for add eax m32 rdx; bytes below are x86-64 encodings. */
static bool emit_add_eax_m32_rdx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x03) && emit_u8(w, 0x02);
}

/* Emit host code for shl ecx imm; bytes below are x86-64 encodings. */
static bool emit_shl_ecx_imm(x86_jit_writer_t *w, uint8_t value) {
  return value == 0 || (emit_u8(w, 0xc1) && emit_u8(w, 0xe1) && emit_u8(w, value));
}

/* Emit host code for shl edx imm; bytes below are x86-64 encodings. */
static bool emit_shl_edx_imm(x86_jit_writer_t *w, uint8_t value) {
  return value == 0 ||
         (emit_u8(w, 0xc1) && emit_u8(w, 0xe2) && emit_u8(w, value));
}

/* Emit host code for add eax ecx; bytes below are x86-64 encodings. */
static bool emit_add_eax_ecx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x01) && emit_u8(w, 0xc8);
}

/* Emit host code for sub eax esi; bytes below are x86-64 encodings. */
static bool emit_sub_eax_esi(x86_jit_writer_t *w) {
  return emit_u8(w, 0x29) && emit_u8(w, 0xf0);
}

/* Emit host code for sub ecx esi; bytes below are x86-64 encodings. */
static bool emit_sub_ecx_esi(x86_jit_writer_t *w) {
  return emit_u8(w, 0x29) && emit_u8(w, 0xf1);
}

/* Emit host code for xor ecx eax; bytes below are x86-64 encodings. */
static bool emit_xor_ecx_eax(x86_jit_writer_t *w) {
  return emit_u8(w, 0x31) && emit_u8(w, 0xc1);
}

/* Emit host code for xor ecx r11d; bytes below are x86-64 encodings. */
static bool emit_xor_ecx_r11d(x86_jit_writer_t *w) {
  return emit_u8(w, 0x44) && emit_u8(w, 0x31) && emit_u8(w, 0xd9);
}

/* Emit host code for add eax imm32; bytes below are x86-64 encodings. */
static bool emit_add_eax_imm32(x86_jit_writer_t *w, uint32_t value) {
  return emit_u8(w, 0x05) && emit_u32(w, value);
}

/* Emit host code for add esi imm32; bytes below are x86-64 encodings. */
static bool emit_add_esi_imm32(x86_jit_writer_t *w, uint32_t value) {
  return emit_u8(w, 0x81) && emit_u8(w, 0xc6) && emit_u32(w, value);
}

static bool emit_add_eax_imm32_placeholder(x86_jit_writer_t *w,
    uint8_t **imm) {
  if (!emit_u8(w, 0x05)) return false;
  *imm = w->cur;
  return emit_u32(w, 0);
}

/* Emit host code for add edx imm32; bytes below are x86-64 encodings. */
static bool emit_add_edx_imm32(x86_jit_writer_t *w, uint32_t value) {
  return emit_u8(w, 0x81) && emit_u8(w, 0xc2) && emit_u32(w, value);
}

/* Emit host code for cmp edx imm32; bytes below are x86-64 encodings. */
static bool emit_cmp_edx_imm32(x86_jit_writer_t *w, uint32_t value) {
  return emit_u8(w, 0x81) && emit_u8(w, 0xfa) && emit_u32(w, value);
}

/* Emit host code for cmp eax edi; bytes below are x86-64 encodings. */
static bool emit_cmp_eax_edi(x86_jit_writer_t *w) {
  return emit_u8(w, 0x39) && emit_u8(w, 0xf8);
}

/* Emit host code for cmp esi edi; bytes below are x86-64 encodings. */
static bool emit_cmp_esi_edi(x86_jit_writer_t *w) {
  return emit_u8(w, 0x39) && emit_u8(w, 0xfe);
}

/* Emit host code for add m64 rdx imm8; bytes below are x86-64 encodings. */
static bool emit_add_m64_rdx_imm8(x86_jit_writer_t *w, uint8_t value) {
  return emit_u8(w, 0x48) && emit_u8(w, 0x83) &&
         emit_u8(w, 0x02) && emit_u8(w, value);
}

/* Emit host code for mov edx eax; bytes below are x86-64 encodings. */
static bool emit_mov_edx_eax(x86_jit_writer_t *w) {
  return emit_u8(w, 0x89) && emit_u8(w, 0xc2);
}

/* Emit host code for mov edx ecx; bytes below are x86-64 encodings. */
static bool emit_mov_edx_ecx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x89) && emit_u8(w, 0xca);
}

/* Emit host code for mov eax edx; bytes below are x86-64 encodings. */
static bool emit_mov_eax_edx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x89) && emit_u8(w, 0xd0);
}

/* Emit host code for mov eax esi; bytes below are x86-64 encodings. */
static bool emit_mov_eax_esi(x86_jit_writer_t *w) {
  return emit_u8(w, 0x89) && emit_u8(w, 0xf0);
}

/* Emit host code for mov esi eax; bytes below are x86-64 encodings. */
static bool emit_mov_esi_eax(x86_jit_writer_t *w) {
  return emit_u8(w, 0x89) && emit_u8(w, 0xc6);
}

/* Emit host code for mov r10 rax; bytes below are x86-64 encodings. */
static bool emit_mov_r10_rax(x86_jit_writer_t *w) {
  return emit_u8(w, 0x49) && emit_u8(w, 0x89) && emit_u8(w, 0xc2);
}

/* Emit host code for mov r14 rax; bytes below are x86-64 encodings. */
static bool emit_mov_r14_rax(x86_jit_writer_t *w) {
  return emit_u8(w, 0x49) && emit_u8(w, 0x89) && emit_u8(w, 0xc6);
}

/* Emit host code for mov eax r11d; bytes below are x86-64 encodings. */
static bool emit_mov_eax_r11d(x86_jit_writer_t *w) {
  return emit_u8(w, 0x44) && emit_u8(w, 0x89) && emit_u8(w, 0xd8);
}

/* Emit host code for mov r10d eax; bytes below are x86-64 encodings. */
static bool emit_mov_r10d_eax(x86_jit_writer_t *w) {
  return emit_u8(w, 0x41) && emit_u8(w, 0x89) && emit_u8(w, 0xc2);
}

/* Emit host code for mov r10d ecx; bytes below are x86-64 encodings. */
static bool emit_mov_r10d_ecx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x41) && emit_u8(w, 0x89) && emit_u8(w, 0xca);
}

/* Emit host code for mov ecx r10d; bytes below are x86-64 encodings. */
static bool emit_mov_ecx_r10d(x86_jit_writer_t *w) {
  return emit_u8(w, 0x44) && emit_u8(w, 0x89) && emit_u8(w, 0xd1);
}

/* Emit host code for sub ecx eax; bytes below are x86-64 encodings. */
static bool emit_sub_ecx_eax(x86_jit_writer_t *w) {
  return emit_u8(w, 0x29) && emit_u8(w, 0xc1);
}

/* Emit host code for sub eax ecx; bytes below are x86-64 encodings. */
static bool emit_sub_eax_ecx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x29) && emit_u8(w, 0xc8);
}

/* Emit host code for sub r14 rax; bytes below are x86-64 encodings. */
static bool emit_sub_r14_rax(x86_jit_writer_t *w) {
  return emit_u8(w, 0x49) && emit_u8(w, 0x29) && emit_u8(w, 0xc6);
}

/* Emit host code for mov r11d ecx; bytes below are x86-64 encodings. */
static bool emit_mov_r11d_ecx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x41) && emit_u8(w, 0x89) && emit_u8(w, 0xcb);
}

/* Emit host code for mov r11d eax; bytes below are x86-64 encodings. */
static bool emit_mov_r11d_eax(x86_jit_writer_t *w) {
  return emit_u8(w, 0x41) && emit_u8(w, 0x89) && emit_u8(w, 0xc3);
}

/* Emit host code for mov r11d esi; bytes below are x86-64 encodings. */
static bool emit_mov_r11d_esi(x86_jit_writer_t *w) {
  return emit_u8(w, 0x41) && emit_u8(w, 0x89) && emit_u8(w, 0xf3);
}

/* Emit host code for mov r11d edx; bytes below are x86-64 encodings. */
static bool emit_mov_r11d_edx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x41) && emit_u8(w, 0x89) && emit_u8(w, 0xd3);
}

/* Emit host code for mov edx r11d; bytes below are x86-64 encodings. */
static bool emit_mov_edx_r11d(x86_jit_writer_t *w) {
  return emit_u8(w, 0x44) && emit_u8(w, 0x89) && emit_u8(w, 0xda);
}

/* Emit host code for mov ecx r11d; bytes below are x86-64 encodings. */
static bool emit_mov_ecx_r11d(x86_jit_writer_t *w) {
  return emit_u8(w, 0x44) && emit_u8(w, 0x89) && emit_u8(w, 0xd9);
}

/* Emit host code for mov ecx edx; bytes below are x86-64 encodings. */
static bool emit_mov_ecx_edx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x89) && emit_u8(w, 0xd1);
}

/* Emit host code for mov ecx eax; bytes below are x86-64 encodings. */
static bool emit_mov_ecx_eax(x86_jit_writer_t *w) {
  return emit_u8(w, 0x89) && emit_u8(w, 0xc1);
}

/* Emit host code for mov eax ecx; bytes below are x86-64 encodings. */
static bool emit_mov_eax_ecx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x89) && emit_u8(w, 0xc8);
}

/* Emit host code for mov eax edi; bytes below are x86-64 encodings. */
static bool emit_mov_eax_edi(x86_jit_writer_t *w) {
  return emit_u8(w, 0x89) && emit_u8(w, 0xf8);
}

/* Emit host code for mov ecx edi; bytes below are x86-64 encodings. */
static bool emit_mov_ecx_edi(x86_jit_writer_t *w) {
  return emit_u8(w, 0x89) && emit_u8(w, 0xf9);
}

/* Emit host code for cmp edx ecx; bytes below are x86-64 encodings. */
static bool emit_cmp_edx_ecx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x39) && emit_u8(w, 0xca);
}

/* Emit host code for cmp edx eax; bytes below are x86-64 encodings. */
static bool emit_cmp_edx_eax(x86_jit_writer_t *w) {
  return emit_u8(w, 0x39) && emit_u8(w, 0xc2);
}

/* Emit host code for cmp r11d eax; bytes below are x86-64 encodings. */
static bool emit_cmp_r11d_eax(x86_jit_writer_t *w) {
  return emit_u8(w, 0x41) && emit_u8(w, 0x39) && emit_u8(w, 0xc3);
}

/* Emit host code for cmp r11d imm32; bytes below are x86-64 encodings. */
static bool emit_cmp_r11d_imm32(x86_jit_writer_t *w, uint32_t value) {
  return emit_u8(w, 0x41) && emit_u8(w, 0x81) &&
         emit_u8(w, 0xfb) && emit_u32(w, value);
}

/* Emit host code for test r11 r11; bytes below are x86-64 encodings. */
static bool emit_test_r11_r11(x86_jit_writer_t *w) {
  return emit_u8(w, 0x4d) && emit_u8(w, 0x85) && emit_u8(w, 0xdb);
}

/* Emit host code for jmp r11; bytes below are x86-64 encodings. */
static bool emit_jmp_r11(x86_jit_writer_t *w) {
  return emit_u8(w, 0x41) && emit_u8(w, 0xff) && emit_u8(w, 0xe3);
}

/* Emit host code for cmp ecx imm32; bytes below are x86-64 encodings. */
static bool emit_cmp_ecx_imm32(x86_jit_writer_t *w, uint32_t value) {
  return emit_u8(w, 0x81) && emit_u8(w, 0xf9) && emit_u32(w, value);
}

static bool emit_cmp_m8_r10_disp8_imm8(x86_jit_writer_t *w, uint8_t disp,
    uint8_t value) {
  return emit_u8(w, 0x41) && emit_u8(w, 0x80) &&
         emit_u8(w, 0x7a) && emit_u8(w, disp) && emit_u8(w, value);
}

static bool emit_test_m8_r10_disp8_imm8(x86_jit_writer_t *w, uint8_t disp,
    uint8_t value) {
  return emit_u8(w, 0x41) && emit_u8(w, 0xf6) &&
         emit_u8(w, 0x42) && emit_u8(w, disp) && emit_u8(w, value);
}

/* Emit host code for cmp m32 r10 disp8 ecx; bytes below are x86-64 encodings. */
static bool emit_cmp_m32_r10_disp8_ecx(x86_jit_writer_t *w, uint8_t disp) {
  return emit_u8(w, 0x41) && emit_u8(w, 0x39) &&
         emit_u8(w, 0x4a) && emit_u8(w, disp);
}

/* Emit host code for cmp m32 r10 disp8 r11d; bytes below are x86-64 encodings. */
static bool emit_cmp_m32_r10_disp8_r11d(x86_jit_writer_t *w, uint8_t disp) {
  return emit_u8(w, 0x45) && emit_u8(w, 0x39) &&
         emit_u8(w, 0x5a) && emit_u8(w, disp);
}

/* Emit host code for cmp m32 r10 rcx disp8 r11d; bytes below are x86-64 encodings. */
static bool emit_cmp_m32_r10_rcx_disp8_r11d(x86_jit_writer_t *w,
    uint8_t disp) {
  return emit_u8(w, 0x45) && emit_u8(w, 0x39) &&
         emit_u8(w, 0x5c) && emit_u8(w, 0x0a) && emit_u8(w, disp);
}

/* Emit host code for test ecx ecx; bytes below are x86-64 encodings. */
static bool emit_test_ecx_ecx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x85) && emit_u8(w, 0xc9);
}

/* Emit host code for and ecx imm32; bytes below are x86-64 encodings. */
static bool emit_and_ecx_imm32(x86_jit_writer_t *w, uint32_t value) {
  return emit_u8(w, 0x81) && emit_u8(w, 0xe1) && emit_u32(w, value);
}

/* Emit host code for and edx imm32; bytes below are x86-64 encodings. */
static bool emit_and_edx_imm32(x86_jit_writer_t *w, uint32_t value) {
  return emit_u8(w, 0x81) && emit_u8(w, 0xe2) && emit_u32(w, value);
}

/* Emit host code for shr ecx imm; bytes below are x86-64 encodings. */
static bool emit_shr_ecx_imm(x86_jit_writer_t *w, uint8_t value) {
  return value == 0 || (emit_u8(w, 0xc1) && emit_u8(w, 0xe9) && emit_u8(w, value));
}

/* Emit host code for shr r11d imm; bytes below are x86-64 encodings. */
static bool emit_shr_r11d_imm(x86_jit_writer_t *w, uint8_t value) {
  return value == 0 || (emit_u8(w, 0x41) && emit_u8(w, 0xc1) &&
      emit_u8(w, 0xeb) && emit_u8(w, value));
}

/* Emit host code for test r10d imm32; bytes below are x86-64 encodings. */
static bool emit_test_r10d_imm32(x86_jit_writer_t *w, uint32_t value) {
  return emit_u8(w, 0x41) && emit_u8(w, 0xf7) &&
         emit_u8(w, 0xc2) && emit_u32(w, value);
}

/* Emit host code for imul eax eax imm32; bytes below are x86-64 encodings. */
static bool emit_imul_eax_eax_imm32(x86_jit_writer_t *w, uint32_t value) {
  return emit_u8(w, 0x69) && emit_u8(w, 0xc0) && emit_u32(w, value);
}

/* Emit host code for add r10 rax; bytes below are x86-64 encodings. */
static bool emit_add_r10_rax(x86_jit_writer_t *w) {
  return emit_u8(w, 0x49) && emit_u8(w, 0x01) && emit_u8(w, 0xc2);
}

/* Emit host code for add rax r10; bytes below are x86-64 encodings. */
static bool emit_add_rax_r10(x86_jit_writer_t *w) {
  return emit_u8(w, 0x4c) && emit_u8(w, 0x01) && emit_u8(w, 0xd0);
}

/* Emit host code for or eax edx; bytes below are x86-64 encodings. */
static bool emit_or_eax_edx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x09) && emit_u8(w, 0xd0);
}

/* Emit host code for lea r11d r11d disp8; bytes below are x86-64 encodings. */
static bool emit_lea_r11d_r11d_disp8(x86_jit_writer_t *w, int8_t disp) {
  return emit_u8(w, 0x45) && emit_u8(w, 0x8d) &&
         emit_u8(w, 0x5b) && emit_u8(w, (uint8_t)disp);
}

/* Emit host code for lea r10d r10d disp8; bytes below are x86-64 encodings. */
static bool emit_lea_r10d_r10d_disp8(x86_jit_writer_t *w, int8_t disp) {
  return emit_u8(w, 0x45) && emit_u8(w, 0x8d) &&
         emit_u8(w, 0x52) && emit_u8(w, (uint8_t)disp);
}

/* Emit host code for movzx ecx m8 r10 rcx; bytes below are x86-64 encodings. */
static bool emit_movzx_ecx_m8_r10_rcx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x41) && emit_u8(w, 0x0f) &&
         emit_u8(w, 0xb6) && emit_u8(w, 0x0c) && emit_u8(w, 0x0a);
}

/* Emit host code for movzx ecx m8 r15 rcx; bytes below are x86-64 encodings. */
static bool emit_movzx_ecx_m8_r15_rcx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x41) && emit_u8(w, 0x0f) &&
         emit_u8(w, 0xb6) && emit_u8(w, 0x4c) &&
         emit_u8(w, 0x0f) && emit_u8(w, 0x00);
}

/* Emit host code for pushfq; bytes below are x86-64 encodings. */
static bool emit_pushfq(x86_jit_writer_t *w) {
  return emit_u8(w, 0x9c);
}

/* Emit host code for pop rax; bytes below are x86-64 encodings. */
static bool emit_pop_rax(x86_jit_writer_t *w) {
  return emit_u8(w, 0x58);
}

/* Emit host code for test eax imm32; bytes below are x86-64 encodings. */
static bool emit_test_eax_imm32(x86_jit_writer_t *w, uint32_t value) {
  return emit_u8(w, 0xa9) && emit_u32(w, value);
}

static bool emit_test_eax_imm_width(x86_jit_writer_t *w, uint8_t width,
    uint32_t value) {
  if (width == X86_WIDTH_DWORD) {
    return emit_test_eax_imm32(w, value);
  }
  if (width == X86_WIDTH_WORD) {
    return emit_u8(w, 0x66) && emit_u8(w, 0xa9) &&
           emit_u8(w, (uint8_t)value) && emit_u8(w, (uint8_t)(value >> 8));
  }
  if (width == X86_WIDTH_BYTE) {
    return emit_u8(w, 0xa8) && emit_u8(w, (uint8_t)value);
  }
  return false;
}

/* Emit host code for test eax ecx; bytes below are x86-64 encodings. */
static bool emit_test_eax_ecx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x85) && emit_u8(w, 0xc8);
}

/* Emit host code for test rax rax; bytes below are x86-64 encodings. */
static bool emit_test_rax_rax(x86_jit_writer_t *w) {
  return emit_u8(w, 0x48) && emit_u8(w, 0x85) && emit_u8(w, 0xc0);
}

/* Emit host code for test ax cx; bytes below are x86-64 encodings. */
static bool emit_test_ax_cx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x66) && emit_u8(w, 0x85) && emit_u8(w, 0xc8);
}

/* Emit host code for test al cl; bytes below are x86-64 encodings. */
static bool emit_test_al_cl(x86_jit_writer_t *w) {
  return emit_u8(w, 0x84) && emit_u8(w, 0xc8);
}

/* Emit host code for or eax ecx; bytes below are x86-64 encodings. */
static bool emit_or_eax_ecx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x09) && emit_u8(w, 0xc8);
}

/* Emit host code for imul eax ecx; bytes below are x86-64 encodings. */
static bool emit_imul_eax_ecx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x0f) && emit_u8(w, 0xaf) && emit_u8(w, 0xc1);
}

/* Emit host code for imul ax cx; bytes below are x86-64 encodings. */
static bool emit_imul_ax_cx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x66) && emit_u8(w, 0x0f) &&
         emit_u8(w, 0xaf) && emit_u8(w, 0xc1);
}

/* Emit host code for mul ecx; bytes below are x86-64 encodings. */
static bool emit_mul_ecx(x86_jit_writer_t *w) {
  return emit_u8(w, 0xf7) && emit_u8(w, 0xe1);
}

/* Emit host code for mul cl; bytes below are x86-64 encodings. */
static bool emit_mul_cl(x86_jit_writer_t *w) {
  return emit_u8(w, 0xf6) && emit_u8(w, 0xe1);
}

/* Emit host code for mul cx; bytes below are x86-64 encodings. */
static bool emit_mul_cx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x66) && emit_u8(w, 0xf7) && emit_u8(w, 0xe1);
}

/* Emit host code for imul acc ecx; bytes below are x86-64 encodings. */
static bool emit_imul_acc_ecx(x86_jit_writer_t *w) {
  return emit_u8(w, 0xf7) && emit_u8(w, 0xe9);
}

/* Emit host code for imul acc cl; bytes below are x86-64 encodings. */
static bool emit_imul_acc_cl(x86_jit_writer_t *w) {
  return emit_u8(w, 0xf6) && emit_u8(w, 0xe9);
}

/* Emit host code for imul acc cx; bytes below are x86-64 encodings. */
static bool emit_imul_acc_cx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x66) && emit_u8(w, 0xf7) && emit_u8(w, 0xe9);
}

static bool emit_shift_eax_imm(x86_jit_writer_t *w, uint8_t shift_op,
    uint8_t count) {
  return emit_u8(w, 0xc1) &&
         emit_u8(w, (uint8_t)(0xc0u | ((shift_op & 0x7u) << 3))) &&
         emit_u8(w, count);
}

static bool emit_shift_eax_imm_width(x86_jit_writer_t *w, uint8_t shift_op,
    uint8_t width, uint8_t count) {
  const uint8_t modrm = (uint8_t)(0xc0u | ((shift_op & 0x7u) << 3));

  if (width == X86_WIDTH_BYTE) {
    return emit_u8(w, 0xc0) && emit_u8(w, modrm) && emit_u8(w, count);
  }
  if (width == X86_WIDTH_WORD) {
    return emit_u8(w, 0x66) && emit_u8(w, 0xc1) &&
           emit_u8(w, modrm) && emit_u8(w, count);
  }
  if (width == X86_WIDTH_DWORD) return emit_shift_eax_imm(w, shift_op, count);
  return false;
}

/* Emit host code for shift eax cl; bytes below are x86-64 encodings. */
static bool emit_shift_eax_cl(x86_jit_writer_t *w, uint8_t shift_op) {
  return emit_u8(w, 0xd3) &&
         emit_u8(w, (uint8_t)(0xc0u | ((shift_op & 0x7u) << 3)));
}

static bool emit_shift_eax_cl_width(x86_jit_writer_t *w, uint8_t shift_op,
    uint8_t width) {
  const uint8_t modrm = (uint8_t)(0xc0u | ((shift_op & 0x7u) << 3));

  if (width == X86_WIDTH_BYTE) {
    return emit_u8(w, 0xd2) && emit_u8(w, modrm);
  }
  if (width == X86_WIDTH_WORD) {
    return emit_u8(w, 0x66) && emit_u8(w, 0xd3) && emit_u8(w, modrm);
  }
  if (width == X86_WIDTH_DWORD) return emit_shift_eax_cl(w, shift_op);
  return false;
}

static bool emit_double_shift_eax_ecx_imm(x86_jit_writer_t *w,
    bool is_right, uint8_t count) {
  return emit_u8(w, 0x0f) &&
         emit_u8(w, is_right ? 0xac : 0xa4) &&
         emit_u8(w, 0xc8) &&
         emit_u8(w, count);
}

/* Emit host code for not eax; bytes below are x86-64 encodings. */
static bool emit_not_eax(x86_jit_writer_t *w) {
  return emit_u8(w, 0xf7) && emit_u8(w, 0xd0);
}

/* Emit host code for neg eax width; bytes below are x86-64 encodings. */
static bool emit_neg_eax_width(x86_jit_writer_t *w, uint8_t width) {
  if (width == X86_WIDTH_DWORD) {
    return emit_u8(w, 0xf7) && emit_u8(w, 0xd8);
  }
  if (width == X86_WIDTH_WORD) {
    return emit_u8(w, 0x66) && emit_u8(w, 0xf7) && emit_u8(w, 0xd8);
  }
  if (width == X86_WIDTH_BYTE) {
    return emit_u8(w, 0xf6) && emit_u8(w, 0xd8);
  }
  return false;
}

/* Emit host code for div ecx; bytes below are x86-64 encodings. */
static bool emit_div_ecx(x86_jit_writer_t *w) {
  return emit_u8(w, 0xf7) && emit_u8(w, 0xf1);
}

/* Emit host code for idiv ecx; bytes below are x86-64 encodings. */
static bool emit_idiv_ecx(x86_jit_writer_t *w) {
  return emit_u8(w, 0xf7) && emit_u8(w, 0xf9);
}

/* Emit host code for cdq host; bytes below are x86-64 encodings. */
static bool emit_cdq_host(x86_jit_writer_t *w) {
  return emit_u8(w, 0x99);
}

/* Emit host code for ret; bytes below are x86-64 encodings. */
static bool emit_ret(x86_jit_writer_t *w) {
  return emit_u8(w, 0xc3);
}

static bool emit_jcc_rel32_placeholder(x86_jit_writer_t *w, uint8_t cc,
    uint8_t **disp) {
  if (!emit_u8(w, 0x0f) || !emit_u8(w, 0x80u | (cc & 0xfu))) return false;
  *disp = w->cur;
  return emit_u32(w, 0);
}

/* Emit host code for jmp rel32 placeholder; bytes below are x86-64 encodings. */
static bool emit_jmp_rel32_placeholder(x86_jit_writer_t *w, uint8_t **disp) {
  if (!emit_u8(w, 0xe9)) return false;
  *disp = w->cur;
  return emit_u32(w, 0);
}

/* Emit host code for jrcxz rel8 placeholder; bytes below are x86-64 encodings. */
static bool emit_jrcxz_rel8_placeholder(x86_jit_writer_t *w, uint8_t **disp) {
  if (!emit_u8(w, 0xe3)) return false;
  *disp = w->cur;
  return emit_u8(w, 0);
}

/* Patch a reserved relative branch displacement to reach target. */
static bool patch_rel8(uint8_t *disp, const uint8_t *target) {
  const int64_t rel = (int64_t)(target - (disp + 1));
  if (rel < INT8_MIN || rel > INT8_MAX) return false;

  *disp = (uint8_t)(int8_t)rel;
  return true;
}

/* Patch a reserved relative branch displacement to reach target. */
static bool patch_rel32(uint8_t *disp, const uint8_t *target) {
  const int64_t rel = (int64_t)(target - (disp + 4));
  if (rel < INT32_MIN || rel > INT32_MAX) return false;

  const uint32_t encoded = (uint32_t)(int32_t)rel;
  for (uint32_t i = 0; i < 4; i++) {
    disp[i] = (uint8_t)(encoded >> (i * 8));
  }
  return true;
}

/* Patch a reserved little-endian 32-bit immediate field. */
static void patch_u32(uint8_t *dst, uint32_t value) {
  for (uint32_t i = 0; i < 4; i++) {
    dst[i] = (uint8_t)(value >> (i * 8));
  }
}

/* Return true when generated code can address CPU_state through r12. */
static bool jit_batch_cpu_base_available(void) {
  return jit_batch_trampoline_enabled &&
         (!jit_paging_enabled() || jit_paged_batch_enabled);
}

/* Convert an absolute CPU_state member address to a signed disp32 offset. */
static bool jit_cpu_disp32(uintptr_t addr, uint32_t *disp) {
  const intptr_t delta = (intptr_t)(addr - (uintptr_t)&cpu);
  if (delta < INT32_MIN || delta > INT32_MAX) return false;
  *disp = (uint32_t)(int32_t)delta;
  return true;
}

/* Emit host code for ret count; bytes below are x86-64 encodings. */
static bool emit_ret_count(x86_jit_writer_t *w, uint32_t count) {
  if (jit_fast_chain_runtime_enabled()) {
    return emit_mov_eax_esi(w) &&
           emit_add_eax_imm32(w, count) &&
           emit_u8(w, 0xc3);
  }

  return emit_mov_eax_imm32(w, count) && emit_u8(w, 0xc3);
}

/* Return the CPU_state address of one 32-bit guest GPR. */
static uintptr_t jit_gpr_addr(uint8_t reg) {
  Assert(reg < 8, "bad x86 JIT register %u", reg);
  return (uintptr_t)&cpu.gpr[reg]._32;
}

/* Return the CPU_state address of one IA-32 byte register. */
static uintptr_t jit_gpr_byte_addr(uint8_t reg) {
  Assert(reg < 8, "bad x86 JIT byte register %u", reg);
  return (uintptr_t)&cpu.gpr[reg & 0x3u]._8[reg >> 2];
}

/* Emit host code for store reg imm; bytes below are x86-64 encodings. */
static bool emit_store_reg_imm(x86_jit_writer_t *w, uint8_t reg, uint32_t value) {
  JIT_STAT_INC(guest_gpr_stores_emitted);
  uint32_t disp = 0;
  if (jit_batch_cpu_base_available() && jit_cpu_disp32(jit_gpr_addr(reg), &disp)) {
    return emit_mov_m32_r12_disp32_imm32(w, disp, value);
  }
  return emit_movabs_rdx(w, jit_gpr_addr(reg)) &&
         emit_mov_m32_rdx_imm32(w, value);
}

/* Emit host code for load reg eax; bytes below are x86-64 encodings. */
static bool emit_load_reg_eax(x86_jit_writer_t *w, uint8_t reg) {
  JIT_STAT_INC(guest_gpr_loads_emitted);
  uint32_t disp = 0;
  if (jit_batch_cpu_base_available() && jit_cpu_disp32(jit_gpr_addr(reg), &disp)) {
    return emit_mov_eax_m32_r12_disp32(w, disp);
  }
  return emit_mov_eax_moffs64(w, jit_gpr_addr(reg));
}

/* Emit host code for load reg ecx; bytes below are x86-64 encodings. */
static bool emit_load_reg_ecx(x86_jit_writer_t *w, uint8_t reg) {
  JIT_STAT_INC(guest_gpr_loads_emitted);
  uint32_t disp = 0;
  if (jit_batch_cpu_base_available() && jit_cpu_disp32(jit_gpr_addr(reg), &disp)) {
    return emit_mov_ecx_m32_r12_disp32(w, disp);
  }
  return emit_movabs_rdx(w, jit_gpr_addr(reg)) &&
         emit_mov_ecx_m32_rdx(w);
}

/* Emit host code for load reg edx; bytes below are x86-64 encodings. */
static bool emit_load_reg_edx(x86_jit_writer_t *w, uint8_t reg) {
  return emit_load_reg_eax(w, reg) && emit_mov_edx_eax(w);
}

/* Emit host code for load reg r11d; bytes below are x86-64 encodings. */
static bool emit_load_reg_r11d(x86_jit_writer_t *w, uint8_t reg) {
  JIT_STAT_INC(guest_gpr_loads_emitted);
  uint32_t disp = 0;
  if (jit_batch_cpu_base_available() && jit_cpu_disp32(jit_gpr_addr(reg), &disp)) {
    return emit_mov_r11d_m32_r12_disp32(w, disp);
  }
  return emit_movabs_r11(w, jit_gpr_addr(reg)) &&
         emit_mov_r11d_m32_r11(w);
}

/* Emit host code for store reg eax; bytes below are x86-64 encodings. */
static bool emit_store_reg_eax(x86_jit_writer_t *w, uint8_t reg) {
  JIT_STAT_INC(guest_gpr_stores_emitted);
  uint32_t disp = 0;
  if (jit_batch_cpu_base_available() && jit_cpu_disp32(jit_gpr_addr(reg), &disp)) {
    return emit_mov_m32_r12_disp32_eax(w, disp);
  }
  return emit_mov_moffs64_eax(w, jit_gpr_addr(reg));
}

static bool emit_store_reg_ax_no_flags(x86_jit_writer_t *w, uint8_t reg) {
  JIT_STAT_INC(guest_gpr_stores_emitted);
  Assert(reg < 8, "bad x86 JIT register %u", reg);
  return emit_movabs_r11(w, (uintptr_t)&cpu.gpr[reg]._16) &&
         emit_mov_m16_r11_ax(w);
}

static bool emit_store_reg_dx_no_flags(x86_jit_writer_t *w, uint8_t reg) {
  JIT_STAT_INC(guest_gpr_stores_emitted);
  Assert(reg < 8, "bad x86 JIT register %u", reg);
  return emit_movabs_r11(w, (uintptr_t)&cpu.gpr[reg]._16) &&
         emit_mov_m16_r11_dx(w);
}

/* Emit a 32-bit REX prefix when either selected host register is r8..r15. */
static bool emit_rex32_reg_rm(x86_jit_writer_t *w, uint8_t reg,
    uint8_t rm) {
  uint8_t rex = 0x40u;
  if (reg >= 8u) rex |= 0x04u;
  if (rm >= 8u) rex |= 0x01u;
  return rex == 0x40u || emit_u8(w, rex);
}

/* Emit a register-direct ModR/M byte: mod=3, reg field, and r/m field. */
static bool emit_modrm_reg_reg(x86_jit_writer_t *w, uint8_t reg,
    uint8_t rm) {
  return emit_u8(w, (uint8_t)(0xc0u | ((reg & 0x7u) << 3) |
      (rm & 0x7u)));
}

/* Emit `mov r32, imm32` for any host register encoded by the low three bits. */
static bool emit_mov_host_imm32(x86_jit_writer_t *w, uint8_t host,
    uint32_t value) {
  if (host >= 8u && !emit_u8(w, 0x41)) return false;
  return emit_u8(w, (uint8_t)(0xb8u + (host & 0x7u))) &&
         emit_u32(w, value);
}

/* Emit a 32-bit host-register copy. */
static bool emit_mov_host_host(x86_jit_writer_t *w, uint8_t dst,
    uint8_t src) {
  return emit_rex32_reg_rm(w, src, dst) &&
         emit_u8(w, 0x89) &&
         emit_modrm_reg_reg(w, src, dst);
}

/* Emit a 32-bit host-register ALU operation using the Intel Group-1 order. */
static bool emit_alu_host_host(x86_jit_writer_t *w, uint8_t alu_op,
    uint8_t dst, uint8_t src) {
  uint8_t opcode = 0;
  switch (alu_op) {
    case X86_ALU_ADD: opcode = 0x01; break;
    case X86_ALU_OR:  opcode = 0x09; break;
    case X86_ALU_ADC: opcode = 0x11; break;
    case X86_ALU_SBB: opcode = 0x19; break;
    case X86_ALU_AND: opcode = 0x21; break;
    case X86_ALU_SUB: opcode = 0x29; break;
    case X86_ALU_XOR: opcode = 0x31; break;
    case X86_ALU_CMP: opcode = 0x39; break;
    default: return false;
  }

  return emit_rex32_reg_rm(w, src, dst) &&
         emit_u8(w, opcode) &&
         emit_modrm_reg_reg(w, src, dst);
}

/* Emit a 32-bit host-register ALU immediate operation using opcode 81 /digit. */
static bool emit_alu_host_imm32(x86_jit_writer_t *w, uint8_t alu_op,
    uint8_t host, uint32_t imm) {
  return emit_rex32_reg_rm(w, 0, host) &&
         emit_u8(w, 0x81) &&
         emit_modrm_reg_reg(w, alu_op & 0x7u, host) &&
         emit_u32(w, imm);
}

/* Emit `test r32, r32` between two host registers. */
static bool emit_test_host_host(x86_jit_writer_t *w, uint8_t left,
    uint8_t right) {
  return emit_rex32_reg_rm(w, right, left) &&
         emit_u8(w, 0x85) &&
         emit_modrm_reg_reg(w, right, left);
}

/* Load one guest GPR from CPU_state into a cached host register. */
static bool emit_load_guest_to_host(x86_jit_writer_t *w, uint8_t host,
    uint8_t guest) {
  uint32_t disp = 0;
  if (!jit_cpu_disp32(jit_gpr_addr(guest), &disp)) return false;

  JIT_STAT_INC(guest_gpr_loads_emitted);
  return emit_rex32_reg_rm(w, host, 12u) &&
         emit_u8(w, 0x8b) &&
         emit_u8(w, (uint8_t)(0x84u | ((host & 0x7u) << 3))) &&
         emit_u8(w, 0x24) &&
         emit_u32(w, disp);
}

/* Store one cached host register back to a guest GPR in CPU_state. */
static bool emit_store_host_to_guest(x86_jit_writer_t *w, uint8_t guest,
    uint8_t host) {
  uint32_t disp = 0;
  if (!jit_cpu_disp32(jit_gpr_addr(guest), &disp)) return false;

  JIT_STAT_INC(guest_gpr_stores_emitted);
  return emit_rex32_reg_rm(w, host, 12u) &&
         emit_u8(w, 0x89) &&
         emit_u8(w, (uint8_t)(0x84u | ((host & 0x7u) << 3))) &&
         emit_u8(w, 0x24) &&
         emit_u32(w, disp);
}

/*
 * Register-cache host sets.  rbx/rbp/r14 are callee-saved and compact for
 * normal blocks; traces may also use r8..r11 because the trampoline owns the
 * surrounding call frame.
 */
static const uint8_t jit_regcache_hosts[] = { 3u, 5u, 14u };
static const uint8_t jit_regcache_trace_wide_hosts[] =
    { 8u, 9u, 10u, 11u, 3u, 5u, 14u };

/* Select the narrow or wide host-register set for this emission context. */
static const uint8_t *jit_regcache_host_set(const x86_jit_emit_ctx_t *ctx,
    uint32_t *count) {
  if (ctx != NULL && ctx->trace_mode && jit_regcache_wide_enabled) {
    *count = (uint32_t)(sizeof(jit_regcache_trace_wide_hosts) /
        sizeof(jit_regcache_trace_wide_hosts[0]));
    return jit_regcache_trace_wide_hosts;
  }

  *count = (uint32_t)(sizeof(jit_regcache_hosts) /
      sizeof(jit_regcache_hosts[0]));
  return jit_regcache_hosts;
}

/* The register cache is only valid when the generated ABI has a live CPU base. */
static bool jit_regcache_active(const x86_jit_emit_ctx_t *ctx) {
  return ctx != NULL && ctx->valid && jit_regcache_enabled &&
         (!jit_paging_enabled() || jit_paged_regcache_enabled) &&
         ctx->has_cpu_base && jit_batch_cpu_base_available();
}

/* Write back and free one guest GPR from the register cache. */
static bool jit_regcache_flush_guest(x86_jit_writer_t *w,
    x86_jit_emit_ctx_t *ctx, uint8_t guest) {
  if (!jit_regcache_active(ctx) || guest >= 8u ||
      ctx->guest_to_host[guest] < 0) {
    return true;
  }

  const uint8_t host = (uint8_t)ctx->guest_to_host[guest];
  if (ctx->guest_dirty[guest] &&
      !emit_store_host_to_guest(w, guest, host)) {
    return false;
  }

  ctx->guest_dirty[guest] = false;
  ctx->guest_loaded[guest] = false;
  ctx->guest_to_host[guest] = -1;
  ctx->host_to_guest[host] = -1;
  return true;
}

/* Write back and free all cached guest GPRs before helpers or exits. */
static bool jit_regcache_flush_all(x86_jit_writer_t *w,
    x86_jit_emit_ctx_t *ctx) {
  if (!jit_regcache_active(ctx)) return true;
  for (uint8_t guest = 0; guest < 8u; guest++) {
    if (!jit_regcache_flush_guest(w, ctx, guest)) return false;
  }
  return true;
}

/* Allocate a host register for one guest GPR, evicting a non-avoided host if needed. */
static bool jit_regcache_alloc_host(x86_jit_writer_t *w,
    x86_jit_emit_ctx_t *ctx, uint8_t guest, uint8_t *host_out,
    uint16_t avoid_hosts) {
  if (!jit_regcache_active(ctx) || guest >= 8u) return false;

  if (ctx->guest_to_host[guest] >= 0) {
    *host_out = (uint8_t)ctx->guest_to_host[guest];
    return true;
  }

  uint32_t host_count = 0;
  const uint8_t *hosts = jit_regcache_host_set(ctx, &host_count);
  for (uint32_t i = 0; i < host_count; i++) {
    const uint8_t host = hosts[i];
    if ((avoid_hosts & (uint16_t)(1u << host)) == 0 &&
        ctx->host_to_guest[host] < 0) {
      ctx->host_to_guest[host] = guest;
      ctx->guest_to_host[guest] = host;
      *host_out = host;
      return true;
    }
  }

  uint8_t host = UINT8_MAX;
  for (uint32_t i = 0; i < host_count; i++) {
    const uint8_t candidate = hosts[i];
    if ((avoid_hosts & (uint16_t)(1u << candidate)) == 0) {
      host = candidate;
      break;
    }
  }
  if (host == UINT8_MAX) return false;

  const int old_guest = ctx->host_to_guest[host];
  if (old_guest >= 0 &&
      !jit_regcache_flush_guest(w, ctx, (uint8_t)old_guest)) {
    return false;
  }

  ctx->host_to_guest[host] = guest;
  ctx->guest_to_host[guest] = host;
  *host_out = host;
  return true;
}

/* Get a host register containing the current value of a guest GPR. */
static bool jit_regcache_get_read(x86_jit_writer_t *w,
    x86_jit_emit_ctx_t *ctx, uint8_t guest, uint8_t *host_out,
    uint16_t avoid_hosts) {
  if (!jit_regcache_alloc_host(w, ctx, guest, host_out, avoid_hosts)) {
    return false;
  }
  if (!ctx->guest_loaded[guest]) {
    if (!emit_load_guest_to_host(w, *host_out, guest)) return false;
    ctx->guest_loaded[guest] = true;
  }
  return true;
}

/* Get a host register for a guest GPR write, optionally loading the old value. */
static bool jit_regcache_get_write(x86_jit_writer_t *w,
    x86_jit_emit_ctx_t *ctx, uint8_t guest, bool read_old,
    uint8_t *host_out, uint16_t avoid_hosts) {
  if (read_old) {
    return jit_regcache_get_read(w, ctx, guest, host_out, avoid_hosts);
  }
  if (!jit_regcache_alloc_host(w, ctx, guest, host_out, avoid_hosts)) {
    return false;
  }
  ctx->guest_loaded[guest] = true;
  ctx->guest_dirty[guest] = true;
  return true;
}

/* Mark a cached guest GPR dirty after native code changes the host copy. */
static void jit_regcache_mark_dirty(x86_jit_emit_ctx_t *ctx, uint8_t guest) {
  if (ctx == NULL || guest >= 8u) return;
  ctx->guest_dirty[guest] = true;
  ctx->guest_loaded[guest] = true;
}

/* Emit a direct write to cpu.pc with an immediate guest PC. */
static bool emit_store_pc_imm(x86_jit_writer_t *w, vaddr_t pc) {
  return emit_movabs_rdx(w, (uintptr_t)&cpu.pc) &&
         emit_mov_m32_rdx_imm32(w, pc);
}

/* Emit a direct write to cpu.pc using EAX as the new guest PC. */
static bool emit_store_pc_eax(x86_jit_writer_t *w) {
  return emit_mov_moffs64_eax(w, (uintptr_t)&cpu.pc);
}

/* Add one guest GPR value to EAX while building a guest effective address. */
static bool emit_add_reg_to_eax(x86_jit_writer_t *w, uint8_t reg) {
  return emit_movabs_rdx(w, jit_gpr_addr(reg)) &&
         emit_add_eax_m32_rdx(w);
}

/* Emit guest 32-bit effective-address calculation into EAX. */
static bool emit_guest_ea_eax(x86_jit_writer_t *w, const x86_jit_ea_t *ea) {
  if (!emit_mov_eax_imm32(w, ea->disp)) return false;

  if (ea->base_reg >= 0 &&
      !emit_add_reg_to_eax(w, (uint8_t)ea->base_reg)) {
    return false;
  }

  if (ea->index_reg >= 0) {
    if (!emit_load_reg_ecx(w, (uint8_t)ea->index_reg) ||
        !emit_shl_ecx_imm(w, ea->scale) ||
        !emit_add_eax_ecx(w)) {
      return false;
    }
  }

  return true;
}

/* Emit LEA by calculating the decoded effective address and writing the destination. */
static bool emit_lea(x86_jit_writer_t *w, const x86_jit_insn_t *insn) {
  if (!emit_guest_ea_eax(w, &insn->ea)) return false;
  return emit_store_reg_eax(w, insn->dst);
}

/* Call the generic helper while preserving the generated ABI's argument registers. */
static bool emit_helper_call(x86_jit_writer_t *w, const x86_jit_insn_t *insn) {
  uint8_t *continue_disp = NULL;

  if (!emit_push_rdi(w) ||
      !emit_push_rsi(w) ||
      !emit_movabs_rdi(w, (uintptr_t)insn) ||
      !emit_movabs_rax(w, (uintptr_t)jit_helper_exec) ||
      !emit_sub_rsp_imm8(w, 8u) ||
      !emit_call_rax(w) ||
      !emit_add_rsp_imm8(w, 8u) ||
      !emit_pop_rsi(w) ||
      !emit_pop_rdi(w)) {
    return false;
  }

  if (!jit_paging_enabled() || !jit_helper_may_touch_guest_memory(insn)) {
    return true;
  }

  if (!emit_load_chain_abort_ecx(w) ||
      !emit_test_ecx_ecx(w) ||
      !emit_jcc_rel32_placeholder(w, X86_CC_Z, &continue_disp) ||
      (jit_stats_enabled &&
          !emit_runtime_counter_inc(w, &jit_smc_invalidation_exits_runtime))) {
    return false;
  }

  if (insn->ends_block) {
    if (!(jit_fast_chain_runtime_enabled() ?
            (emit_mov_eax_esi(w) &&
             emit_add_eax_imm32(w, insn->ordinal) &&
             emit_ret(w)) :
            (emit_load_loop_extra_eax(w) &&
             emit_add_eax_imm32(w, insn->ordinal) &&
             emit_ret(w)))) {
      return false;
    }
  }
  else if (!emit_return_completed(w, insn->next_pc, insn->ordinal)) {
    return false;
  }

  return patch_rel32(continue_disp, w->cur);
}

/* Call the DTLB/MMU slow translator with the address currently in EAX. */
static bool emit_dtlb_translate_call(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn, uint8_t width, bool is_write) {
  return emit_push_rdi(w) &&
         emit_push_rsi(w) &&
         emit_movabs_rdi(w, (uintptr_t)insn) &&
         emit_mov_esi_eax(w) &&
         emit_mov_edx_imm32(w, width) &&
         emit_mov_ecx_imm32(w, is_write ? 1u : 0u) &&
         emit_movabs_rax(w, (uintptr_t)jit_dtlb_translate) &&
         emit_sub_rsp_imm8(w, 8u) &&
         emit_call_rax(w) &&
         emit_add_rsp_imm8(w, 8u) &&
         emit_pop_rsi(w) &&
         emit_pop_rdi(w);
}

/* Capture the page-mode key that is already guarded before this block runs. */
static bool jit_paged_dtlb_context_key(uint32_t *cr3_key, uint32_t *state) {
  if (cr3_key == NULL || state == NULL ||
      !jit_paging_enabled() || !jit_paging_mode_supported()) {
    return false;
  }

  const x86_jit_translation_key_t key = jit_current_translation_key();
  /*
   * DTLB entries are indexed by jit_dtlb_state() bits only.  The block
   * translation key also carries a CR0.PG bit so non-paged and paged compiled
   * code cannot alias; do not feed that extra bit into the DTLB hash.
   */
  *cr3_key = key.cr3_key;
  *state = key.state & ~(1u << 4);
  return true;
}

/*
 * Inline read-DTLB fast path.  Input EAX is a guest virtual address; on hit, the
 * result is a host pointer in RAX.  All miss tests branch to `miss_native`,
 * where the normal helper translates and may return NULL for a slow block exit.
 */
static bool emit_paged_dtlb_read_hit_inline(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn, uint8_t width, uint8_t **slow_disp) {
  uint8_t *miss_disp[9];
  uint32_t miss_count = 0;
  uint32_t cr3_key = 0;
  uint32_t state = 0;

  if (width != X86_WIDTH_BYTE && width != X86_WIDTH_WORD &&
      width != X86_WIDTH_DWORD) {
    return false;
  }
  if (sizeof(paddr_t) != sizeof(uint32_t)) return false;
  if (!jit_paged_dtlb_context_key(&cr3_key, &state)) return false;

  /*
   * The block entry and direct-chain checks already prove the CR3 and paging
   * mode key.  Use those constants for the DTLB hash and entry comparisons so
   * hot memory accesses do not reload cpu.cr3/cr0/cr4/cs on every hit.
   */
  if (!emit_mov_edx_eax(w) ||
      !emit_mov_ecx_eax(w) ||
      !emit_and_ecx_imm32(w, PAGE_MASK) ||
      !emit_cmp_ecx_imm32(w, PAGE_SIZE - width) ||
      !emit_jcc_rel32_placeholder(w, X86_CC_A, &miss_disp[miss_count++]) ||
      !emit_mov_ecx_edx(w) ||
      !emit_shr_ecx_imm(w, PAGE_SHIFT) ||
      !emit_mov_r11d_ecx(w) ||
      !emit_mov_eax_ecx(w) ||
      !emit_shift_eax_imm(w, 5u, 10u) ||
      !emit_alu_rm32_r32(w, X86_ALU_XOR, R_EAX, R_ECX) ||
      !emit_alu_eax_imm32(w, X86_ALU_XOR, cr3_key >> PAGE_SHIFT) ||
      !emit_alu_eax_imm32(w, X86_ALU_XOR, state) ||
      !emit_alu_eax_imm32(w, X86_ALU_AND, X86_JIT_DTLB_SIZE - 1u) ||
      !emit_imul_eax_eax_imm32(w, sizeof(x86_jit_dtlb_entry_t)) ||
      !emit_movabs_r10(w, (uint64_t)(uintptr_t)jit_dtlb) ||
      !emit_add_r10_rax(w) ||
      !emit_cmp_m8_r10_disp8_imm8(w,
          (uint8_t)offsetof(x86_jit_dtlb_entry_t, valid), 0u) ||
      !emit_jcc_rel32_placeholder(w, X86_CC_Z, &miss_disp[miss_count++]) ||
      !emit_test_m8_r10_disp8_imm8(w,
          (uint8_t)offsetof(x86_jit_dtlb_entry_t, access),
          X86_JIT_DTLB_READ) ||
      !emit_jcc_rel32_placeholder(w, X86_CC_Z, &miss_disp[miss_count++]) ||
      !emit_mov_ecx_imm32(w, state) ||
      !emit_cmp_m32_r10_disp8_ecx(w,
          (uint8_t)offsetof(x86_jit_dtlb_entry_t, state)) ||
      !emit_jcc_rel32_placeholder(w, X86_CC_NZ, &miss_disp[miss_count++]) ||
      !emit_cmp_m32_r10_disp8_r11d(w,
          (uint8_t)offsetof(x86_jit_dtlb_entry_t, vpn)) ||
      !emit_jcc_rel32_placeholder(w, X86_CC_NZ, &miss_disp[miss_count++]) ||
      !emit_mov_ecx_imm32(w, cr3_key) ||
      !emit_cmp_m32_r10_disp8_ecx(w,
          (uint8_t)offsetof(x86_jit_dtlb_entry_t, cr3_key)) ||
      !emit_jcc_rel32_placeholder(w, X86_CC_NZ, &miss_disp[miss_count++])) {
    return false;
  }

  if (jit_stats_enabled) {
    if (!emit_mov_r11d_edx(w) ||
        !emit_runtime_counter_inc(w, &jit_stats.dtlb_read_hits) ||
        !emit_runtime_counter_inc(w, &jit_stats.paged_dtlb_read_hits) ||
        !emit_mov_edx_r11d(w)) {
      return false;
    }
  }

  if (!emit_mov_eax_m32_r10_disp8(w,
          (uint8_t)offsetof(x86_jit_dtlb_entry_t, pg_paddr)) ||
      !emit_and_edx_imm32(w, PAGE_MASK) ||
      !emit_or_eax_edx(w) ||
      !emit_add_eax_imm32(w, 0u - (uint32_t)CONFIG_MBASE) ||
      !emit_movabs_r10(w, (uint64_t)(uintptr_t)guest_to_host(CONFIG_MBASE)) ||
      !emit_add_rax_r10(w)) {
    return false;
  }

  uint8_t *done_disp = NULL;
  if (!emit_jmp_rel32_placeholder(w, &done_disp)) return false;

  uint8_t *miss_native = w->cur;
  for (uint32_t i = 0; i < miss_count; i++) {
    if (!patch_rel32(miss_disp[i], miss_native)) return false;
  }
  if (!emit_mov_eax_edx(w) ||
      !emit_dtlb_translate_call(w, insn, width, false) ||
      !emit_test_rax_rax(w) ||
      !emit_jcc_rel32_placeholder(w, X86_CC_Z, slow_disp)) {
    return false;
  }

  return patch_rel32(done_disp, w->cur);
}

/*
 * Enter generated code through the optional batch trampoline.  Register ABI:
 *   r12 = &cpu, r13 = guest_to_host(CONFIG_MBASE), r15 = source-page bitmap,
 *   edi = instruction budget, esi = retired instruction count.
 * The callee-saved registers keep these bases live across direct chains.
 */
static uint32_t jit_batch_enter(x86_jit_entry_t entry,
    uint32_t remaining_budget) {
  if (!jit_batch_trampoline_enabled) {
    return entry(remaining_budget);
  }

  JIT_STAT_INC(batch_entries);
  const uintptr_t cpu_base = (uintptr_t)&cpu;
  const uintptr_t pmem_base = (uintptr_t)guest_to_host(CONFIG_MBASE);
  const uintptr_t source_bitmap = (uintptr_t)jit_source_page_has_code;
  uint32_t ret = 0;

  /*
   * The current generated ABI still returns with a plain RET.  This wrapper
   * therefore calls, rather than jumps to, the first generated block.  Direct
   * chains stay inside this saved-register window until a generated exit RETs.
   */
  __asm__ volatile(
      "movq %[entry], %%r11\n\t"
      "movl %[budget], %%edi\n\t"
      "movq %[cpu_base], %%r8\n\t"
      "movq %[pmem_base], %%r9\n\t"
      "movq %[source_bitmap], %%r10\n\t"
      "xorl %%esi, %%esi\n\t"
      "pushq %%rbx\n\t"
      "pushq %%rbp\n\t"
      "pushq %%r12\n\t"
      "pushq %%r13\n\t"
      "pushq %%r14\n\t"
      "pushq %%r15\n\t"
      "movq %%r8, %%r12\n\t"
      "movq %%r9, %%r13\n\t"
      "movq %%r10, %%r15\n\t"
      "subq $8, %%rsp\n\t"
      "call *%%r11\n\t"
      "addq $8, %%rsp\n\t"
      "popq %%r15\n\t"
      "popq %%r14\n\t"
      "popq %%r13\n\t"
      "popq %%r12\n\t"
      "popq %%rbp\n\t"
      "popq %%rbx\n\t"
      : "=a"(ret)
      : [entry] "r"(entry),
        [budget] "rm"(remaining_budget),
        [cpu_base] "r"(cpu_base),
        [pmem_base] "r"(pmem_base),
        [source_bitmap] "r"(source_bitmap)
      : "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11",
        "memory", "cc");
  return ret;
}

/*
 * Guard a direct PMEM access.  The emitted code subtracts CONFIG_MBASE with
 * unsigned arithmetic, then checks the resulting offset is within CONFIG_MSIZE.
 */
static bool emit_direct_pmem_guard_edx(x86_jit_writer_t *w, uint32_t len,
    uint8_t **slow_disp) {
  if (len == 0 || len > CONFIG_MSIZE) return false;

  JIT_STAT_INC(native_pmem_guards_emitted);
  return emit_add_edx_imm32(w, 0u - (uint32_t)CONFIG_MBASE) &&
         emit_cmp_edx_imm32(w, (uint32_t)CONFIG_MSIZE - len) &&
         emit_jcc_rel32_placeholder(w, X86_CC_A, slow_disp);
}

/*
 * Guard direct stores against source-code invalidation.  A store crossing a
 * source-page boundary or touching a page marked as translated code falls back
 * so the central invalidation path can discard stale native blocks.
 */
static bool emit_direct_store_source_guard_edx(x86_jit_writer_t *w,
    uint32_t len, uint8_t **cross_page_slow_disp,
    uint8_t **source_page_slow_disp) {
  if (len == 0 || len > X86_JIT_SOURCE_PAGE_SIZE) return false;

  JIT_STAT_INC(native_pmem_guards_emitted);
  if (!emit_mov_ecx_edx(w) ||
      !emit_and_ecx_imm32(w, X86_JIT_SOURCE_PAGE_SIZE - 1u) ||
      !emit_cmp_ecx_imm32(w, X86_JIT_SOURCE_PAGE_SIZE - len) ||
      !emit_jcc_rel32_placeholder(w, X86_CC_A, cross_page_slow_disp) ||
      !emit_mov_ecx_edx(w) ||
      !emit_shr_ecx_imm(w, X86_JIT_SOURCE_PAGE_SHIFT)) {
    return false;
  }

  if (jit_batch_cpu_base_available()) {
    if (!emit_movzx_ecx_m8_r15_rcx(w)) return false;
  }
  else if (!emit_movabs_r10(w, (uint64_t)(uintptr_t)jit_source_page_has_code) ||
      !emit_movzx_ecx_m8_r10_rcx(w)) {
    return false;
  }

  return emit_test_ecx_ecx(w) &&
         emit_jcc_rel32_placeholder(w, X86_CC_NZ, source_page_slow_disp);
}

/* Guard direct stores against page-table writes that would stale the DTLB. */
static bool emit_direct_store_page_table_guard_edx(x86_jit_writer_t *w,
    uint32_t len, uint8_t **cross_page_slow_disp,
    uint8_t **page_table_slow_disp) {
  if (len == 0 || len > X86_JIT_SOURCE_PAGE_SIZE) return false;

  JIT_STAT_INC(native_pmem_guards_emitted);
  return emit_mov_ecx_edx(w) &&
         emit_and_ecx_imm32(w, X86_JIT_SOURCE_PAGE_SIZE - 1u) &&
         emit_cmp_ecx_imm32(w, X86_JIT_SOURCE_PAGE_SIZE - len) &&
         emit_jcc_rel32_placeholder(w, X86_CC_A, cross_page_slow_disp) &&
         emit_mov_ecx_edx(w) &&
         emit_shr_ecx_imm(w, X86_JIT_SOURCE_PAGE_SHIFT) &&
         emit_movabs_r10(w, (uint64_t)(uintptr_t)jit_page_table_page_has_mapping) &&
         emit_movzx_ecx_m8_r10_rcx(w) &&
         emit_test_ecx_ecx(w) &&
         emit_jcc_rel32_placeholder(w, X86_CC_NZ, page_table_slow_disp);
}

/*
 * Inline write-DTLB fast path.  It mirrors the read path, then rejects writes to
 * translated source pages or tracked page-table pages so the C helper can run
 * the normal invalidation hooks before any guest-visible store commits.
 */
static bool emit_paged_dtlb_write_hit_inline(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn, uint8_t width, uint8_t **slow_disp) {
  uint8_t *miss_disp[13];
  uint32_t miss_count = 0;
  uint32_t cr3_key = 0;
  uint32_t state = 0;

  if (width != X86_WIDTH_BYTE && width != X86_WIDTH_WORD &&
      width != X86_WIDTH_DWORD) {
    return false;
  }
  if (sizeof(paddr_t) != sizeof(uint32_t)) return false;
  if (!jit_paged_dtlb_context_key(&cr3_key, &state)) return false;

  if (!emit_mov_edx_eax(w) ||
      !emit_push_rdx(w) ||
      !emit_mov_ecx_eax(w) ||
      !emit_and_ecx_imm32(w, PAGE_MASK) ||
      !emit_cmp_ecx_imm32(w, PAGE_SIZE - width) ||
      !emit_jcc_rel32_placeholder(w, X86_CC_A, &miss_disp[miss_count++]) ||
      !emit_mov_ecx_edx(w) ||
      !emit_shr_ecx_imm(w, PAGE_SHIFT) ||
      !emit_mov_r11d_ecx(w) ||
      !emit_mov_eax_ecx(w) ||
      !emit_shift_eax_imm(w, 5u, 10u) ||
      !emit_alu_rm32_r32(w, X86_ALU_XOR, R_EAX, R_ECX) ||
      !emit_alu_eax_imm32(w, X86_ALU_XOR, cr3_key >> PAGE_SHIFT) ||
      !emit_alu_eax_imm32(w, X86_ALU_XOR, state) ||
      !emit_alu_eax_imm32(w, X86_ALU_AND, X86_JIT_DTLB_SIZE - 1u) ||
      !emit_imul_eax_eax_imm32(w, sizeof(x86_jit_dtlb_entry_t)) ||
      !emit_movabs_r10(w, (uint64_t)(uintptr_t)jit_dtlb) ||
      !emit_add_r10_rax(w) ||
      !emit_cmp_m8_r10_disp8_imm8(w,
          (uint8_t)offsetof(x86_jit_dtlb_entry_t, valid), 0u) ||
      !emit_jcc_rel32_placeholder(w, X86_CC_Z, &miss_disp[miss_count++]) ||
      !emit_test_m8_r10_disp8_imm8(w,
          (uint8_t)offsetof(x86_jit_dtlb_entry_t, access),
          X86_JIT_DTLB_WRITE) ||
      !emit_jcc_rel32_placeholder(w, X86_CC_Z, &miss_disp[miss_count++]) ||
      !emit_mov_ecx_imm32(w, state) ||
      !emit_cmp_m32_r10_disp8_ecx(w,
          (uint8_t)offsetof(x86_jit_dtlb_entry_t, state)) ||
      !emit_jcc_rel32_placeholder(w, X86_CC_NZ, &miss_disp[miss_count++]) ||
      !emit_cmp_m32_r10_disp8_r11d(w,
          (uint8_t)offsetof(x86_jit_dtlb_entry_t, vpn)) ||
      !emit_jcc_rel32_placeholder(w, X86_CC_NZ, &miss_disp[miss_count++]) ||
      !emit_mov_ecx_imm32(w, cr3_key) ||
      !emit_cmp_m32_r10_disp8_ecx(w,
          (uint8_t)offsetof(x86_jit_dtlb_entry_t, cr3_key)) ||
      !emit_jcc_rel32_placeholder(w, X86_CC_NZ, &miss_disp[miss_count++])) {
    return false;
  }

  if (jit_stats_enabled) {
    if (!emit_mov_r11d_edx(w) ||
        !emit_runtime_counter_inc(w, &jit_stats.dtlb_write_hits) ||
        !emit_runtime_counter_inc(w, &jit_stats.paged_dtlb_write_hits) ||
        !emit_mov_edx_r11d(w)) {
      return false;
    }
  }

  if (!emit_mov_eax_m32_r10_disp8(w,
          (uint8_t)offsetof(x86_jit_dtlb_entry_t, pg_paddr)) ||
      !emit_and_edx_imm32(w, PAGE_MASK) ||
      !emit_or_eax_edx(w) ||
      !emit_mov_edx_eax(w) ||
      !emit_add_edx_imm32(w, 0u - (uint32_t)CONFIG_MBASE) ||
      !emit_direct_store_source_guard_edx(w, width,
          &miss_disp[miss_count], &miss_disp[miss_count + 1]) ||
      !emit_direct_store_page_table_guard_edx(w, width,
          &miss_disp[miss_count + 2], &miss_disp[miss_count + 3])) {
    return false;
  }
  miss_count += 4;

  if (!emit_add_eax_imm32(w, 0u - (uint32_t)CONFIG_MBASE) ||
      !emit_movabs_r10(w, (uint64_t)(uintptr_t)guest_to_host(CONFIG_MBASE)) ||
      !emit_add_rax_r10(w)) {
    return false;
  }

  uint8_t *done_disp = NULL;
  if (!emit_pop_rdx(w) ||
      !emit_jmp_rel32_placeholder(w, &done_disp)) {
    return false;
  }

  uint8_t *miss_native = w->cur;
  for (uint32_t i = 0; i < miss_count; i++) {
    if (!patch_rel32(miss_disp[i], miss_native)) return false;
  }
  if (!emit_pop_rax(w) ||
      !emit_dtlb_translate_call(w, insn, width, true) ||
      !emit_test_rax_rax(w) ||
      !emit_jcc_rel32_placeholder(w, X86_CC_Z, slow_disp)) {
    return false;
  }

  return patch_rel32(done_disp, w->cur);
}

/* Load a 32-bit stack value from PMEM offset EDX into EAX. */
static bool emit_stack_load_dword_eax(x86_jit_writer_t *w) {
  if (jit_paging_enabled()) return false;
  if (jit_batch_cpu_base_available()) return emit_mov_eax_m32_r13_rdx(w);
  return emit_movabs_r10(w, (uint64_t)(uintptr_t)guest_to_host(CONFIG_MBASE)) &&
         emit_mov_eax_m32_r10_rdx(w);
}

/* Store r11d to a 32-bit stack slot at PMEM offset EDX. */
static bool emit_stack_store_dword_r11d(x86_jit_writer_t *w) {
  if (jit_paging_enabled()) return false;
  if (jit_batch_cpu_base_available()) return emit_mov_m32_r13_rdx_r11d(w);
  return emit_movabs_r10(w, (uint64_t)(uintptr_t)guest_to_host(CONFIG_MBASE)) &&
         emit_mov_m32_r10_rdx_r11d(w);
}

/* Return to the dispatcher before executing a stack fast path that failed guards. */
static bool emit_stack_window_slow_return(x86_jit_writer_t *w, vaddr_t pc) {
  return emit_store_pc_imm(w, pc) &&
         (jit_fast_chain_runtime_enabled() ? emit_mov_eax_esi(w) :
             emit_mov_eax_imm32(w, 0u)) &&
         emit_ret(w);
}

/* Guard one ESP-relative stack window against leaving PMEM. */
static bool emit_stack_window_pmem_guard(x86_jit_writer_t *w,
    int32_t min_offset, uint32_t len, uint8_t **slow_disp) {
  if (len == 0 || len > CONFIG_MSIZE) return false;
  return emit_load_reg_eax(w, R_ESP) &&
         emit_add_eax_imm32(w, (uint32_t)min_offset) &&
         emit_mov_edx_eax(w) &&
         emit_direct_pmem_guard_edx(w, len, slow_disp);
}

/*
 * Emit all preconditions for the stack fast path.  Loads and stores must remain
 * inside PMEM; stores also must not touch translated source pages.
 */
static bool emit_stack_window_guard(x86_jit_writer_t *w, vaddr_t pc,
    const x86_jit_stack_window_t *window,
    const x86_jit_insn_t *guard_insn) {
  uint8_t *access_pmem_slow_disp = NULL;
  uint8_t *store_pmem_slow_disp = NULL;
  uint8_t *store_cross_page_slow_disp = NULL;
  uint8_t *store_source_page_slow_disp = NULL;
  uint8_t *done_disp = NULL;

  if (window == NULL || !window->valid || !jit_fast_chain_runtime_enabled()) {
    return true;
  }

  const int64_t access_len64 =
      (int64_t)window->max_offset - (int64_t)window->min_offset +
      (int64_t)X86_WIDTH_DWORD;
  if (access_len64 <= 0 || access_len64 > CONFIG_MSIZE) return false;

  if (window->paged) {
    /*
     * Paged stack-window fast path is intentionally single-slot only.  The
     * DTLB guard below proves the current stack slot and leaves r14 holding
     * host_pointer - linear_address, so the existing ESP-relative emitters can
     * use r14 + runtime linear ESP.  Multi-slot windows stay on per-instruction
     * DTLB paths until their fault ordering has dedicated tests.
     */
    if (guard_insn == NULL || access_len64 != X86_WIDTH_DWORD ||
        !jit_batch_cpu_base_available()) {
      return false;
    }

    if (!emit_load_reg_eax(w, R_ESP) ||
        !emit_add_eax_imm32(w, (uint32_t)window->min_offset) ||
        !emit_store_dtlb_scratch_eax(w) ||
        !emit_paged_dtlb_translate_addr_eax(w, guard_insn, X86_WIDTH_DWORD,
            window->has_store, &access_pmem_slow_disp) ||
        !emit_mov_r14_rax(w) ||
        !emit_load_dtlb_scratch_eax(w) ||
        !emit_sub_r14_rax(w) ||
        !emit_jmp_rel32_placeholder(w, &done_disp)) {
      return false;
    }

    uint8_t *slow_native = w->cur;
    if (!patch_rel32(access_pmem_slow_disp, slow_native) ||
        !emit_stack_window_slow_return(w, pc)) {
      return false;
    }

    return patch_rel32(done_disp, w->cur);
  }

  if (!emit_stack_window_pmem_guard(w, window->min_offset,
      (uint32_t)access_len64, &access_pmem_slow_disp)) {
    return false;
  }

  if (window->has_store) {
    const int64_t store_len64 =
        (int64_t)window->store_max_offset -
        (int64_t)window->store_min_offset + (int64_t)X86_WIDTH_DWORD;
    if (store_len64 <= 0 || store_len64 > X86_JIT_SOURCE_PAGE_SIZE) {
      return false;
    }
    if (!emit_stack_window_pmem_guard(w, window->store_min_offset,
        (uint32_t)store_len64, &store_pmem_slow_disp) ||
        !emit_direct_store_source_guard_edx(w, (uint32_t)store_len64,
            &store_cross_page_slow_disp, &store_source_page_slow_disp)) {
      return false;
    }
  }

  if (!emit_jmp_rel32_placeholder(w, &done_disp)) return false;

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(access_pmem_slow_disp, slow_native) ||
      (store_pmem_slow_disp != NULL &&
          !patch_rel32(store_pmem_slow_disp, slow_native)) ||
      (store_cross_page_slow_disp != NULL &&
          !patch_rel32(store_cross_page_slow_disp, slow_native)) ||
      (store_source_page_slow_disp != NULL &&
          !patch_rel32(store_source_page_slow_disp, slow_native)) ||
      !emit_stack_window_slow_return(w, pc)) {
    return false;
  }

  return patch_rel32(done_disp, w->cur);
}

/* Load the current non-trampoline retired-count accumulator into EAX. */
static bool emit_load_loop_extra_eax(x86_jit_writer_t *w) {
  return emit_mov_eax_moffs64(w, (uintptr_t)&jit_loop_extra);
}

/* Load the current non-trampoline retired-count accumulator into ECX. */
static bool emit_load_loop_extra_ecx(x86_jit_writer_t *w) {
  return emit_movabs_rdx(w, (uintptr_t)&jit_loop_extra) &&
         emit_mov_ecx_m32_rdx(w);
}

/* Store EAX into the non-trampoline retired-count accumulator. */
static bool emit_store_loop_extra_eax(x86_jit_writer_t *w) {
  return emit_mov_moffs64_eax(w, (uintptr_t)&jit_loop_extra);
}

/* Load the current entry budget from memory into ECX. */
static bool emit_load_entry_budget_ecx(x86_jit_writer_t *w) {
  return emit_movabs_rdx(w, (uintptr_t)&jit_entry_budget) &&
         emit_mov_ecx_m32_rdx(w);
}

/* Copy the trampoline/function budget argument from EDI to ECX. */
static bool emit_load_entry_budget_arg_ecx(x86_jit_writer_t *w) {
  return emit_mov_ecx_edi(w);
}

/* Copy the trampoline/function budget argument from EDI to EAX. */
static bool emit_load_entry_budget_arg_eax(x86_jit_writer_t *w) {
  return emit_mov_eax_edi(w);
}

/* Load the async chain-abort flag, checked before long direct-chain runs. */
static bool emit_load_chain_abort_ecx(x86_jit_writer_t *w) {
  return emit_movabs_rdx(w, (uintptr_t)&jit_chain_abort) &&
         emit_mov_ecx_m32_rdx(w);
}

/* Load materialised guest EFLAGS into EAX. */
static bool emit_load_eflags_eax(x86_jit_writer_t *w) {
  return emit_mov_eax_moffs64(w, (uintptr_t)&cpu.eflags);
}

/* Load materialised guest EFLAGS into ECX. */
static bool emit_load_eflags_ecx(x86_jit_writer_t *w) {
  return emit_movabs_rdx(w, (uintptr_t)&cpu.eflags) &&
         emit_mov_ecx_m32_rdx(w);
}

/* Load materialised guest EFLAGS into r11d. */
static bool emit_load_eflags_r11d(x86_jit_writer_t *w) {
  return emit_movabs_r11(w, (uintptr_t)&cpu.eflags) &&
         emit_mov_r11d_m32_r11(w);
}

/* Store EAX as materialised guest EFLAGS. */
static bool emit_store_eflags_eax(x86_jit_writer_t *w) {
  return emit_mov_moffs64_eax(w, (uintptr_t)&cpu.eflags);
}

/* ADC/SBB consume guest CF, so native emission must seed host CF first. */
static bool jit_alu_reads_carry(uint8_t alu_op) {
  return alu_op == X86_ALU_ADC || alu_op == X86_ALU_SBB;
}

/* Emit BT ecx, imm8; the selected guest EFLAGS bit is copied into host CF. */
static bool emit_bt_ecx_imm8(x86_jit_writer_t *w, uint8_t bit) {
  return emit_u8(w, 0x0f) && emit_u8(w, 0xba) &&
         emit_u8(w, 0xe1) && emit_u8(w, bit);
}

/* Emit BT r11d, imm8; the selected guest EFLAGS bit is copied into host CF. */
static bool emit_bt_r11d_imm8(x86_jit_writer_t *w, uint8_t bit) {
  return emit_u8(w, 0x41) && emit_u8(w, 0x0f) && emit_u8(w, 0xba) &&
         emit_u8(w, 0xe3) && emit_u8(w, bit);
}

/* Seed host CF from guest EFLAGS.CF using ECX as the materialised copy. */
static bool emit_guest_cf_to_host_cf_ecx(x86_jit_writer_t *w) {
  return emit_load_eflags_ecx(w) && emit_bt_ecx_imm8(w, 0);
}

/* Seed host CF from guest EFLAGS.CF using r11d as the materialised copy. */
static bool emit_guest_cf_to_host_cf_r11(x86_jit_writer_t *w) {
  return emit_load_eflags_r11d(w) && emit_bt_r11d_imm8(w, 0);
}

/* Emit a register-direct 32-bit ALU operation; rm is the destination field. */
static bool emit_alu_rm32_r32(x86_jit_writer_t *w, uint8_t alu_op,
    uint8_t rm, uint8_t reg) {
  uint8_t opcode = 0;
  switch (alu_op) {
    case X86_ALU_ADD: opcode = 0x01; break;
    case X86_ALU_OR:  opcode = 0x09; break;
    case X86_ALU_ADC: opcode = 0x11; break;
    case X86_ALU_SBB: opcode = 0x19; break;
    case X86_ALU_AND: opcode = 0x21; break;
    case X86_ALU_SUB: opcode = 0x29; break;
    case X86_ALU_XOR: opcode = 0x31; break;
    case X86_ALU_CMP: opcode = 0x39; break;
    default: return false;
  }

  return emit_u8(w, opcode) &&
         emit_u8(w, (uint8_t)(0xc0u | ((reg & 0x7u) << 3) | (rm & 0x7u)));
}

static bool emit_alu_eax_r11_width(x86_jit_writer_t *w, uint8_t alu_op,
    uint8_t width) {
  uint8_t opcode = 0;

  switch (alu_op) {
    case X86_ALU_ADD: opcode = width == X86_WIDTH_BYTE ? 0x00 : 0x01; break;
    case X86_ALU_OR:  opcode = width == X86_WIDTH_BYTE ? 0x08 : 0x09; break;
    case X86_ALU_ADC: opcode = width == X86_WIDTH_BYTE ? 0x10 : 0x11; break;
    case X86_ALU_SBB: opcode = width == X86_WIDTH_BYTE ? 0x18 : 0x19; break;
    case X86_ALU_AND: opcode = width == X86_WIDTH_BYTE ? 0x20 : 0x21; break;
    case X86_ALU_SUB: opcode = width == X86_WIDTH_BYTE ? 0x28 : 0x29; break;
    case X86_ALU_XOR: opcode = width == X86_WIDTH_BYTE ? 0x30 : 0x31; break;
    case X86_ALU_CMP: opcode = width == X86_WIDTH_BYTE ? 0x38 : 0x39; break;
    default: return false;
  }

  if (width == X86_WIDTH_WORD && !emit_u8(w, 0x66)) return false;
  if (width != X86_WIDTH_BYTE && width != X86_WIDTH_WORD &&
      width != X86_WIDTH_DWORD) {
    return false;
  }
  return emit_u8(w, 0x44) && emit_u8(w, opcode) && emit_u8(w, 0xd8);
}

static bool emit_alu_reg_imm32(x86_jit_writer_t *w, uint8_t alu_op,
    uint8_t reg, uint32_t imm) {
  return emit_u8(w, 0x81) &&
         emit_u8(w, (uint8_t)(0xc0u | ((alu_op & 0x7u) << 3) |
             (reg & 0x7u))) &&
         emit_u32(w, imm);
}

static bool emit_alu_eax_imm32(x86_jit_writer_t *w, uint8_t alu_op,
    uint32_t imm) {
  return emit_alu_reg_imm32(w, alu_op, R_EAX, imm);
}

static bool emit_alu_eax_imm_width(x86_jit_writer_t *w, uint8_t alu_op,
    uint8_t width, uint32_t imm) {
  if (width == X86_WIDTH_DWORD) {
    return emit_alu_eax_imm32(w, alu_op, imm);
  }
  if (width == X86_WIDTH_WORD) {
    return emit_u8(w, 0x66) &&
           emit_u8(w, 0x81) &&
           emit_u8(w, (uint8_t)(0xc0u | ((alu_op & 0x7u) << 3))) &&
           emit_u8(w, (uint8_t)imm) &&
           emit_u8(w, (uint8_t)(imm >> 8));
  }
  if (width == X86_WIDTH_BYTE) {
    return emit_u8(w, 0x80) &&
           emit_u8(w, (uint8_t)(0xc0u | ((alu_op & 0x7u) << 3))) &&
           emit_u8(w, (uint8_t)imm);
  }

  return false;
}

/* Emit host code for load pmem eax width; bytes below are x86-64 encodings. */
static bool emit_load_pmem_eax_width(x86_jit_writer_t *w, uint8_t width) {
  switch (width) {
    case X86_WIDTH_BYTE:
      return emit_movzx_eax_m8_r10_rdx(w);
    case X86_WIDTH_WORD:
      return emit_movzx_eax_m16_r10_rdx(w);
    case X86_WIDTH_DWORD:
      return emit_mov_eax_m32_r10_rdx(w);
    default:
      return false;
  }
}

/* Emit host code for load pmem ecx width; bytes below are x86-64 encodings. */
static bool emit_load_pmem_ecx_width(x86_jit_writer_t *w, uint8_t width) {
  switch (width) {
    case X86_WIDTH_BYTE:
      return emit_movzx_ecx_m8_r10_rdx(w);
    case X86_WIDTH_WORD:
      return emit_movzx_ecx_m16_r10_rdx(w);
    case X86_WIDTH_DWORD:
      return emit_mov_ecx_m32_r10_rdx(w);
    default:
      return false;
  }
}

/* Emit host code for load host ptr rax width; bytes below are x86-64 encodings. */
static bool emit_load_host_ptr_rax_width(x86_jit_writer_t *w, uint8_t width) {
  switch (width) {
    case X86_WIDTH_BYTE:
      return emit_u8(w, 0x0f) && emit_u8(w, 0xb6) && emit_u8(w, 0x00);
    case X86_WIDTH_WORD:
      return emit_u8(w, 0x0f) && emit_u8(w, 0xb7) && emit_u8(w, 0x00);
    case X86_WIDTH_DWORD:
      return emit_u8(w, 0x8b) && emit_u8(w, 0x00);
    default:
      return false;
  }
}

/* Emit host code for store pmem eax width; bytes below are x86-64 encodings. */
static bool emit_store_pmem_eax_width(x86_jit_writer_t *w, uint8_t width) {
  switch (width) {
    case X86_WIDTH_BYTE:
      return emit_mov_m8_r10_rdx_al(w);
    case X86_WIDTH_WORD:
      return emit_mov_m16_r10_rdx_ax(w);
    case X86_WIDTH_DWORD:
      return emit_mov_m32_r10_rdx_eax(w);
    default:
      return false;
  }
}

static bool emit_store_host_ptr_r10_eax_width(x86_jit_writer_t *w,
    uint8_t width) {
  switch (width) {
    case X86_WIDTH_BYTE:
      return emit_u8(w, 0x41) && emit_u8(w, 0x88) && emit_u8(w, 0x02);
    case X86_WIDTH_WORD:
      return emit_u8(w, 0x66) && emit_u8(w, 0x41) &&
             emit_u8(w, 0x89) && emit_u8(w, 0x02);
    case X86_WIDTH_DWORD:
      return emit_u8(w, 0x41) && emit_u8(w, 0x89) && emit_u8(w, 0x02);
    default:
      return false;
  }
}

/* Emit host code for test eax ecx width; bytes below are x86-64 encodings. */
static bool emit_test_eax_ecx_width(x86_jit_writer_t *w, uint8_t width) {
  switch (width) {
    case X86_WIDTH_BYTE:
      return emit_test_al_cl(w);
    case X86_WIDTH_WORD:
      return emit_test_ax_cx(w);
    case X86_WIDTH_DWORD:
      return emit_test_eax_ecx(w);
    default:
      return false;
  }
}

static bool jit_native_low_byte_reg(uint8_t reg);

static bool emit_store_reg_eax_width(x86_jit_writer_t *w, uint8_t reg,
    uint8_t width) {
  uint32_t keep_mask = 0;
  uint32_t value_mask = 0;

  if (width == X86_WIDTH_DWORD) return emit_store_reg_eax(w, reg);
  if (width == X86_WIDTH_BYTE) {
    if (!jit_native_low_byte_reg(reg)) return false;
    keep_mask = 0xffffff00u;
    value_mask = X86_BYTE_MASK;
  }
  else if (width == X86_WIDTH_WORD) {
    keep_mask = 0xffff0000u;
    value_mask = X86_WORD_MASK;
  }
  else {
    return false;
  }

  return emit_alu_eax_imm32(w, X86_ALU_AND, value_mask) &&
         emit_load_reg_ecx(w, reg) &&
         emit_alu_reg_imm32(w, X86_ALU_AND, R_ECX, keep_mask) &&
         emit_or_eax_ecx(w) &&
         emit_store_reg_eax(w, reg);
}

/* Emit host code for load byte reg to eax; bytes below are x86-64 encodings. */
static bool emit_load_byte_reg_to_eax(x86_jit_writer_t *w, uint8_t reg) {
  if (!emit_load_reg_eax(w, reg & 0x3u)) return false;
  return reg < 4u || emit_shift_eax_imm(w, 5u, 8u);
}

/* Emit host code for store al to byte reg; bytes below are x86-64 encodings. */
static bool emit_store_al_to_byte_reg(x86_jit_writer_t *w, uint8_t reg) {
  JIT_STAT_INC(guest_gpr_stores_emitted);
  return emit_movabs_rdx(w, jit_gpr_byte_addr(reg)) &&
         emit_mov_m8_rdx_al(w);
}

static bool emit_store_loaded_rm_to_reg(x86_jit_writer_t *w, uint8_t reg,
    uint8_t width) {
  if (width == X86_WIDTH_BYTE) return emit_store_al_to_byte_reg(w, reg);
  return emit_store_reg_eax_width(w, reg, width);
}

static bool emit_load_reg_to_eax_width(x86_jit_writer_t *w, uint8_t reg,
    uint8_t width) {
  if (width == X86_WIDTH_BYTE) return emit_load_byte_reg_to_eax(w, reg);
  return emit_load_reg_r11d(w, reg) && emit_mov_eax_r11d(w);
}

static bool emit_paged_dtlb_translate_addr_eax(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn, uint8_t width, bool is_write,
    uint8_t **slow_disp) {
  if (!is_write &&
      emit_paged_dtlb_read_hit_inline(w, insn, width, slow_disp)) {
    return true;
  }
  if (is_write &&
      emit_paged_dtlb_write_hit_inline(w, insn, width, slow_disp)) {
    return true;
  }

  return emit_dtlb_translate_call(w, insn, width, is_write) &&
         emit_test_rax_rax(w) &&
         emit_jcc_rel32_placeholder(w, X86_CC_Z, slow_disp);
}

static bool emit_paged_dtlb_translate_ea(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn, uint8_t width, bool is_write,
    uint8_t **slow_disp) {
  return emit_guest_ea_eax(w, &insn->ea) &&
         emit_paged_dtlb_translate_addr_eax(w, insn, width, is_write,
             slow_disp);
}

static bool emit_paged_dtlb_load_ea_eax(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn, uint8_t width, bool is_write,
    uint8_t **slow_disp) {
  if (!emit_paged_dtlb_translate_ea(w, insn, width, is_write, slow_disp)) {
    return false;
  }
  if (is_write && !emit_mov_r10_rax(w)) return false;
  return emit_load_host_ptr_rax_width(w, width);
}

static bool emit_paged_dtlb_mov_reg_rm_load(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *slow_disp = NULL;
  uint8_t *done_disp = NULL;
  const uint8_t width = insn->width;

  if (insn->rm_is_reg) return false;
  if (width != X86_WIDTH_BYTE && width != X86_WIDTH_WORD &&
      width != X86_WIDTH_DWORD) {
    return false;
  }

  if (!emit_paged_dtlb_translate_ea(w, insn, width, false, &slow_disp) ||
      !emit_load_host_ptr_rax_width(w, width) ||
      !emit_store_loaded_rm_to_reg(w, insn->dst, width) ||
      !emit_jmp_rel32_placeholder(w, &done_disp)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(slow_disp, slow_native) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  JIT_STAT_INC(native_pmem_loads);
  return patch_rel32(done_disp, w->cur);
}

static bool emit_paged_dtlb_mov_rm_reg_store(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *slow_disp = NULL;
  uint8_t *done_disp = NULL;
  const uint8_t width = insn->width;

  if (insn->rm_is_reg) return false;
  if (width != X86_WIDTH_BYTE && width != X86_WIDTH_WORD &&
      width != X86_WIDTH_DWORD) {
    return false;
  }
  if (width == X86_WIDTH_BYTE && insn->src >= 8u) return false;

  if (!emit_paged_dtlb_translate_ea(w, insn, width, true, &slow_disp) ||
      !emit_mov_r10_rax(w) ||
      !emit_load_reg_to_eax_width(w, insn->src, width) ||
      !emit_store_host_ptr_r10_eax_width(w, width) ||
      !emit_jmp_rel32_placeholder(w, &done_disp)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(slow_disp, slow_native)) {
    return false;
  }
  if (!emit_helper_call(w, insn)) {
    return false;
  }

  JIT_STAT_INC(native_pmem_stores);
  return patch_rel32(done_disp, w->cur);
}

static bool emit_paged_dtlb_mov_imm_rm(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *slow_disp = NULL;
  uint8_t *done_disp = NULL;
  const uint8_t width = insn->width;

  if (insn->rm_is_reg) return false;
  if (width != X86_WIDTH_BYTE && width != X86_WIDTH_WORD &&
      width != X86_WIDTH_DWORD) {
    return false;
  }

  if (!emit_paged_dtlb_translate_ea(w, insn, width, true, &slow_disp) ||
      !emit_mov_r10_rax(w) ||
      !emit_mov_eax_imm32(w, insn->imm) ||
      !emit_store_host_ptr_r10_eax_width(w, width) ||
      !emit_jmp_rel32_placeholder(w, &done_disp)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(slow_disp, slow_native) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  JIT_STAT_INC(native_pmem_stores);
  return patch_rel32(done_disp, w->cur);
}

static bool emit_paged_dtlb_mov_eax_moffs(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *slow_disp = NULL;
  uint8_t *done_disp = NULL;
  const uint8_t width = insn->width;

  if (width != X86_WIDTH_BYTE && width != X86_WIDTH_WORD &&
      width != X86_WIDTH_DWORD) {
    return false;
  }

  if (!emit_mov_eax_imm32(w, insn->imm) ||
      !emit_paged_dtlb_translate_addr_eax(w, insn, width, false,
          &slow_disp) ||
      !emit_load_host_ptr_rax_width(w, width) ||
      !emit_store_loaded_rm_to_reg(w, R_EAX, width) ||
      !emit_jmp_rel32_placeholder(w, &done_disp)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(slow_disp, slow_native) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  JIT_STAT_INC(native_pmem_loads);
  return patch_rel32(done_disp, w->cur);
}

static bool emit_paged_dtlb_mov_moffs_eax(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *slow_disp = NULL;
  uint8_t *done_disp = NULL;
  const uint8_t width = insn->width;

  if (width != X86_WIDTH_BYTE && width != X86_WIDTH_WORD &&
      width != X86_WIDTH_DWORD) {
    return false;
  }

  if (!emit_mov_eax_imm32(w, insn->imm) ||
      !emit_paged_dtlb_translate_addr_eax(w, insn, width, true,
          &slow_disp) ||
      !emit_mov_r10_rax(w) ||
      !emit_load_reg_to_eax_width(w, R_EAX, width) ||
      !emit_store_host_ptr_r10_eax_width(w, width) ||
      !emit_jmp_rel32_placeholder(w, &done_disp)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(slow_disp, slow_native) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  JIT_STAT_INC(native_pmem_stores);
  return patch_rel32(done_disp, w->cur);
}

static bool emit_paged_dtlb_movzx_reg_rm(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *slow_disp = NULL;
  uint8_t *done_disp = NULL;
  uint8_t width = 0;

  if (insn->rm_is_reg) return false;
  if (insn->helper == X86_JIT_HELPER_MOVZX_REG_RM8) {
    width = X86_WIDTH_BYTE;
  }
  else if (insn->helper == X86_JIT_HELPER_MOVZX_REG_RM16) {
    width = X86_WIDTH_WORD;
  }
  else {
    return false;
  }

  if (!emit_paged_dtlb_load_ea_eax(w, insn, width, false, &slow_disp) ||
      !emit_store_reg_eax(w, insn->dst) ||
      !emit_jmp_rel32_placeholder(w, &done_disp)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(slow_disp, slow_native) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  JIT_STAT_INC(native_movzx_ops);
  JIT_STAT_INC(native_pmem_loads);
  return patch_rel32(done_disp, w->cur);
}

static bool emit_paged_dtlb_movsx_reg_rm(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *slow_disp = NULL;
  uint8_t *done_disp = NULL;
  uint8_t width = 0;

  if (insn->rm_is_reg) return false;
  if (insn->width != X86_WIDTH_WORD && insn->width != X86_WIDTH_DWORD) {
    return false;
  }
  if (insn->helper == X86_JIT_HELPER_MOVSX_REG_RM8) {
    width = X86_WIDTH_BYTE;
  }
  else if (insn->helper == X86_JIT_HELPER_MOVSX_REG_RM16) {
    width = X86_WIDTH_WORD;
  }
  else {
    return false;
  }

  if (!emit_paged_dtlb_load_ea_eax(w, insn, width, false, &slow_disp)) {
    return false;
  }
  if (width == X86_WIDTH_BYTE) {
    if (!emit_movsx_eax_al(w)) return false;
  }
  else if (!emit_movsx_eax_ax(w)) {
    return false;
  }
  if (!emit_store_reg_eax_width(w, insn->dst, insn->width) ||
      !emit_jmp_rel32_placeholder(w, &done_disp)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(slow_disp, slow_native) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  JIT_STAT_INC(native_movsx_ops);
  JIT_STAT_INC(native_pmem_loads);
  return patch_rel32(done_disp, w->cur);
}

static bool emit_paged_dtlb_alu_rm_reg(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *slow_disp = NULL;
  uint8_t *done_disp = NULL;
  const bool writes_result = jit_native_alu_writes_result(insn->alu_op);
  const uint8_t width = insn->width;

  if (insn->rm_is_reg) return false;
  if (width != X86_WIDTH_BYTE && width != X86_WIDTH_WORD &&
      width != X86_WIDTH_DWORD) {
    return false;
  }
  if (width == X86_WIDTH_BYTE && !jit_native_low_byte_reg(insn->src)) {
    return false;
  }

  if (!emit_paged_dtlb_load_ea_eax(w, insn, width, writes_result,
          &slow_disp) ||
      (jit_alu_reads_carry(insn->alu_op) &&
          !emit_guest_cf_to_host_cf_ecx(w)) ||
      !emit_load_reg_r11d(w, insn->src) ||
      !emit_alu_eax_r11_width(w, insn->alu_op, width)) {
    return false;
  }
  if (writes_result && !emit_store_host_ptr_r10_eax_width(w, width)) {
    return false;
  }
  if (!emit_capture_status_flags(w,
          jit_native_alu_flag_copy_mask(insn->alu_op)) ||
      !emit_jmp_rel32_placeholder(w, &done_disp)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(slow_disp, slow_native) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  JIT_STAT_INC(native_alu_ops);
  JIT_STAT_INC(native_pmem_loads);
  if (writes_result) JIT_STAT_INC(native_pmem_stores);
  return patch_rel32(done_disp, w->cur);
}

static bool emit_paged_dtlb_alu_imm_rm(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *slow_disp = NULL;
  uint8_t *done_disp = NULL;
  const bool writes_result = jit_native_alu_writes_result(insn->alu_op);
  const uint8_t width = insn->width;

  if (insn->rm_is_reg) return false;
  if (width != X86_WIDTH_BYTE && width != X86_WIDTH_WORD &&
      width != X86_WIDTH_DWORD) {
    return false;
  }

  if (!emit_paged_dtlb_load_ea_eax(w, insn, width, writes_result,
          &slow_disp) ||
      (jit_alu_reads_carry(insn->alu_op) &&
          !emit_guest_cf_to_host_cf_ecx(w)) ||
      !emit_alu_eax_imm_width(w, insn->alu_op, width, insn->imm)) {
    return false;
  }
  if (writes_result && !emit_store_host_ptr_r10_eax_width(w, width)) {
    return false;
  }
  if (!emit_capture_status_flags(w,
          jit_native_alu_flag_copy_mask(insn->alu_op)) ||
      !emit_jmp_rel32_placeholder(w, &done_disp)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(slow_disp, slow_native) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  JIT_STAT_INC(native_alu_ops);
  JIT_STAT_INC(native_pmem_loads);
  if (writes_result) JIT_STAT_INC(native_pmem_stores);
  return patch_rel32(done_disp, w->cur);
}

static bool emit_paged_dtlb_alu_reg_rm(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *slow_disp = NULL;
  uint8_t *done_disp = NULL;
  const bool writes_result = jit_native_alu_writes_result(insn->alu_op);
  const uint8_t width = insn->width;

  if (insn->rm_is_reg) return false;
  if (width != X86_WIDTH_BYTE && width != X86_WIDTH_WORD &&
      width != X86_WIDTH_DWORD) {
    return false;
  }
  if (width == X86_WIDTH_BYTE && !jit_native_low_byte_reg(insn->dst)) {
    return false;
  }

  if (!emit_paged_dtlb_load_ea_eax(w, insn, width, false, &slow_disp) ||
      !emit_mov_ecx_eax(w) ||
      !emit_load_reg_eax(w, insn->dst) ||
      (jit_alu_reads_carry(insn->alu_op) &&
          !emit_guest_cf_to_host_cf_r11(w)) ||
      !emit_alu_eax_ecx_width(w, insn->alu_op, width)) {
    return false;
  }
  if (writes_result && !emit_store_reg_eax(w, insn->dst)) return false;
  if (!emit_capture_status_flags(w,
          jit_native_alu_flag_copy_mask(insn->alu_op)) ||
      !emit_jmp_rel32_placeholder(w, &done_disp)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(slow_disp, slow_native) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  JIT_STAT_INC(native_alu_ops);
  JIT_STAT_INC(native_pmem_loads);
  return patch_rel32(done_disp, w->cur);
}

static bool emit_paged_dtlb_test_rm_reg(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *slow_disp = NULL;
  uint8_t *done_disp = NULL;
  const uint8_t width = insn->width;

  if (insn->rm_is_reg) return false;
  if (width != X86_WIDTH_BYTE && width != X86_WIDTH_WORD &&
      width != X86_WIDTH_DWORD) {
    return false;
  }
  if (width == X86_WIDTH_BYTE && !jit_native_low_byte_reg(insn->src)) {
    return false;
  }

  if (!emit_paged_dtlb_load_ea_eax(w, insn, width, false, &slow_disp) ||
      !emit_load_reg_ecx(w, insn->src) ||
      !emit_test_eax_ecx_width(w, width) ||
      !emit_capture_status_flags(w, X86_EFLAGS_LOGIC_COPY_MASK) ||
      !emit_jmp_rel32_placeholder(w, &done_disp)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(slow_disp, slow_native) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  JIT_STAT_INC(native_alu_ops);
  JIT_STAT_INC(native_pmem_loads);
  return patch_rel32(done_disp, w->cur);
}

static bool emit_paged_dtlb_test_imm_rm(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *slow_disp = NULL;
  uint8_t *done_disp = NULL;
  const uint8_t width = insn->width;

  if (insn->rm_is_reg) return false;
  if (width != X86_WIDTH_BYTE && width != X86_WIDTH_WORD &&
      width != X86_WIDTH_DWORD) {
    return false;
  }

  if (!emit_paged_dtlb_load_ea_eax(w, insn, width, false, &slow_disp) ||
      !emit_test_eax_imm_width(w, width, insn->imm) ||
      !emit_capture_status_flags(w, X86_EFLAGS_LOGIC_COPY_MASK) ||
      !emit_jmp_rel32_placeholder(w, &done_disp)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(slow_disp, slow_native) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  JIT_STAT_INC(native_alu_ops);
  JIT_STAT_INC(native_pmem_loads);
  return patch_rel32(done_disp, w->cur);
}

/* Emit host code for stack push addr eax; bytes below are x86-64 encodings. */
static bool emit_stack_push_addr_eax(x86_jit_writer_t *w) {
  return emit_load_reg_eax(w, R_ESP) &&
         emit_add_eax_imm32(w, 0u - X86_WIDTH_DWORD);
}

/* Emit host code for commit stack push esp; bytes below are x86-64 encodings. */
static bool emit_commit_stack_push_esp(x86_jit_writer_t *w) {
  return emit_stack_push_addr_eax(w) &&
         emit_store_reg_eax(w, R_ESP);
}

/* Emit host code for commit stack pop esp; bytes below are x86-64 encodings. */
static bool emit_commit_stack_pop_esp(x86_jit_writer_t *w) {
  return emit_load_reg_eax(w, R_ESP) &&
         emit_add_eax_imm32(w, X86_WIDTH_DWORD) &&
         emit_store_reg_eax(w, R_ESP);
}

/* Emit host code for store dtlb scratch eax; bytes below are x86-64 encodings. */
static bool emit_store_dtlb_scratch_eax(x86_jit_writer_t *w) {
  return emit_mov_moffs64_eax(w, (uintptr_t)&jit_dtlb_scratch);
}

/* Emit host code for load dtlb scratch eax; bytes below are x86-64 encodings. */
static bool emit_load_dtlb_scratch_eax(x86_jit_writer_t *w) {
  return emit_mov_eax_moffs64(w, (uintptr_t)&jit_dtlb_scratch);
}

/* Emit host code for store dtlb value scratch eax; bytes below are x86-64 encodings. */
static bool emit_store_dtlb_value_scratch_eax(x86_jit_writer_t *w) {
  return emit_mov_moffs64_eax(w, (uintptr_t)&jit_dtlb_value_scratch);
}

/* Emit host code for load dtlb value scratch eax; bytes below are x86-64 encodings. */
static bool emit_load_dtlb_value_scratch_eax(x86_jit_writer_t *w) {
  return emit_mov_eax_moffs64(w, (uintptr_t)&jit_dtlb_value_scratch);
}

static bool emit_paged_dtlb_push_reg(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *slow_disp = NULL;
  uint8_t *done_disp = NULL;

  if (insn->width != X86_WIDTH_DWORD) return false;

  if (!emit_stack_push_addr_eax(w) ||
      !emit_paged_dtlb_translate_addr_eax(w, insn, X86_WIDTH_DWORD, true,
          &slow_disp) ||
      !emit_mov_r10_rax(w) ||
      !emit_load_reg_to_eax_width(w, insn->src, X86_WIDTH_DWORD) ||
      !emit_store_host_ptr_r10_eax_width(w, X86_WIDTH_DWORD) ||
      !emit_commit_stack_push_esp(w) ||
      !emit_jmp_rel32_placeholder(w, &done_disp)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(slow_disp, slow_native) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  JIT_STAT_INC(native_pmem_stores);
  return patch_rel32(done_disp, w->cur);
}

static bool emit_paged_dtlb_push_imm(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *slow_disp = NULL;
  uint8_t *done_disp = NULL;

  if (insn->width != X86_WIDTH_DWORD) return false;

  if (!emit_stack_push_addr_eax(w) ||
      !emit_paged_dtlb_translate_addr_eax(w, insn, X86_WIDTH_DWORD, true,
          &slow_disp) ||
      !emit_mov_r10_rax(w) ||
      !emit_mov_eax_imm32(w, insn->imm) ||
      !emit_store_host_ptr_r10_eax_width(w, X86_WIDTH_DWORD) ||
      !emit_commit_stack_push_esp(w) ||
      !emit_jmp_rel32_placeholder(w, &done_disp)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(slow_disp, slow_native) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  JIT_STAT_INC(native_pmem_stores);
  return patch_rel32(done_disp, w->cur);
}

static bool emit_paged_dtlb_pop_reg(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *slow_disp = NULL;
  uint8_t *done_disp = NULL;

  if (insn->width != X86_WIDTH_DWORD) return false;

  if (!emit_load_reg_eax(w, R_ESP) ||
      !emit_paged_dtlb_translate_addr_eax(w, insn, X86_WIDTH_DWORD, false,
          &slow_disp) ||
      !emit_load_host_ptr_rax_width(w, X86_WIDTH_DWORD) ||
      !emit_store_reg_eax(w, insn->dst)) {
    return false;
  }

  if (insn->dst != R_ESP && !emit_commit_stack_pop_esp(w)) {
    return false;
  }

  if (!emit_jmp_rel32_placeholder(w, &done_disp)) return false;

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(slow_disp, slow_native) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  JIT_STAT_INC(native_pmem_loads);
  return patch_rel32(done_disp, w->cur);
}

static bool emit_paged_dtlb_pop_rm(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *src_slow_disp = NULL;
  uint8_t *dst_slow_disp = NULL;
  uint8_t *done_disp = NULL;

  if (insn->width != X86_WIDTH_DWORD) return false;

  if (!emit_load_reg_eax(w, R_ESP) ||
      !emit_store_dtlb_scratch_eax(w) ||
      !emit_paged_dtlb_translate_addr_eax(w, insn, X86_WIDTH_DWORD, false,
          &src_slow_disp) ||
      !emit_load_host_ptr_rax_width(w, X86_WIDTH_DWORD) ||
      !emit_store_dtlb_value_scratch_eax(w)) {
    return false;
  }

  if (insn->rm_is_reg) {
    if (!emit_load_dtlb_scratch_eax(w) ||
        !emit_add_eax_imm32(w, X86_WIDTH_DWORD) ||
        !emit_store_reg_eax(w, R_ESP) ||
        !emit_load_dtlb_value_scratch_eax(w) ||
        !emit_store_reg_eax(w, insn->rm_reg)) {
      return false;
    }
  }
  else {
    /*
     * Intel specifies that POP r/m computes an ESP-based memory destination
     * after incrementing ESP.  Keep the guest state untouched until both page
     * translations succeed; on a slow edge the helper replays the exact
     * read-increment-write sequence and owns any destination page fault.
     */
    if (!emit_guest_ea_eax(w, &insn->ea) ||
        (insn->ea.base_reg == R_ESP &&
            !emit_add_eax_imm32(w, X86_WIDTH_DWORD)) ||
        !emit_paged_dtlb_translate_addr_eax(w, insn, X86_WIDTH_DWORD, true,
            &dst_slow_disp) ||
        !emit_mov_r10_rax(w) ||
        !emit_load_dtlb_scratch_eax(w) ||
        !emit_add_eax_imm32(w, X86_WIDTH_DWORD) ||
        !emit_store_reg_eax(w, R_ESP) ||
        !emit_load_dtlb_value_scratch_eax(w) ||
        !emit_store_host_ptr_r10_eax_width(w, X86_WIDTH_DWORD)) {
      return false;
    }
  }

  if (!emit_jmp_rel32_placeholder(w, &done_disp)) return false;

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(src_slow_disp, slow_native) ||
      (dst_slow_disp != NULL && !patch_rel32(dst_slow_disp, slow_native)) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  JIT_STAT_INC(native_pmem_loads);
  if (!insn->rm_is_reg) JIT_STAT_INC(native_pmem_stores);
  return patch_rel32(done_disp, w->cur);
}

static bool emit_paged_dtlb_call_rel(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *slow_disp = NULL;
  uint8_t *done_disp = NULL;

  if (insn->width != X86_WIDTH_DWORD) return false;

  if (!emit_stack_push_addr_eax(w) ||
      !emit_paged_dtlb_translate_addr_eax(w, insn, X86_WIDTH_DWORD, true,
          &slow_disp) ||
      !emit_mov_r10_rax(w) ||
      !emit_mov_eax_imm32(w, insn->next_pc) ||
      !emit_store_host_ptr_r10_eax_width(w, X86_WIDTH_DWORD) ||
      !emit_commit_stack_push_esp(w) ||
      !emit_store_pc_imm(w, jit_branch_target(insn)) ||
      !emit_jmp_rel32_placeholder(w, &done_disp)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(slow_disp, slow_native) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  JIT_STAT_INC(native_pmem_stores);
  return patch_rel32(done_disp, w->cur);
}

static bool emit_paged_dtlb_ret(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *slow_disp = NULL;
  uint8_t *done_disp = NULL;

  if (insn->width != X86_WIDTH_DWORD) return false;

  if (!emit_load_reg_eax(w, R_ESP) ||
      !emit_paged_dtlb_translate_addr_eax(w, insn, X86_WIDTH_DWORD, false,
          &slow_disp) ||
      !emit_load_host_ptr_rax_width(w, X86_WIDTH_DWORD) ||
      !emit_store_pc_eax(w) ||
      !emit_commit_stack_pop_esp(w) ||
      !emit_jmp_rel32_placeholder(w, &done_disp)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(slow_disp, slow_native) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  JIT_STAT_INC(native_pmem_loads);
  return patch_rel32(done_disp, w->cur);
}

static bool emit_paged_dtlb_jmp_rm(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *slow_disp = NULL;
  uint8_t *done_disp = NULL;

  if (insn->rm_is_reg || insn->width != X86_WIDTH_DWORD) return false;

  if (!emit_paged_dtlb_load_ea_eax(w, insn, X86_WIDTH_DWORD, false,
          &slow_disp) ||
      !emit_store_pc_eax(w) ||
      !emit_jmp_rel32_placeholder(w, &done_disp)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(slow_disp, slow_native) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  JIT_STAT_INC(native_branch_ops);
  JIT_STAT_INC(native_pmem_loads);
  return patch_rel32(done_disp, w->cur);
}

static bool emit_paged_dtlb_push_rm(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *src_slow_disp = NULL;
  uint8_t *dst_slow_disp = NULL;
  uint8_t *done_disp = NULL;

  if (insn->width != X86_WIDTH_DWORD) return false;

  if (insn->rm_is_reg) {
    if (!emit_load_reg_eax(w, insn->rm_reg) ||
        !emit_store_dtlb_scratch_eax(w)) {
      return false;
    }
  }
  else if (!emit_paged_dtlb_load_ea_eax(w, insn, X86_WIDTH_DWORD, false,
      &src_slow_disp) ||
      !emit_store_dtlb_scratch_eax(w)) {
    return false;
  }

  if (!emit_stack_push_addr_eax(w) ||
      !emit_paged_dtlb_translate_addr_eax(w, insn, X86_WIDTH_DWORD, true,
          &dst_slow_disp) ||
      !emit_mov_r10_rax(w) ||
      !emit_load_dtlb_scratch_eax(w) ||
      !emit_store_host_ptr_r10_eax_width(w, X86_WIDTH_DWORD) ||
      !emit_commit_stack_push_esp(w) ||
      !emit_jmp_rel32_placeholder(w, &done_disp)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if ((src_slow_disp != NULL && !patch_rel32(src_slow_disp, slow_native)) ||
      !patch_rel32(dst_slow_disp, slow_native) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  if (!insn->rm_is_reg) JIT_STAT_INC(native_pmem_loads);
  JIT_STAT_INC(native_pmem_stores);
  return patch_rel32(done_disp, w->cur);
}

static bool emit_paged_dtlb_call_rm(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *src_slow_disp = NULL;
  uint8_t *dst_slow_disp = NULL;
  uint8_t *done_disp = NULL;

  if (insn->width != X86_WIDTH_DWORD) return false;

  if (insn->rm_is_reg) {
    if (!emit_load_reg_eax(w, insn->rm_reg) ||
        !emit_store_dtlb_scratch_eax(w)) {
      return false;
    }
  }
  else if (!emit_paged_dtlb_load_ea_eax(w, insn, X86_WIDTH_DWORD, false,
      &src_slow_disp) ||
      !emit_store_dtlb_scratch_eax(w)) {
    return false;
  }

  if (!emit_stack_push_addr_eax(w) ||
      !emit_paged_dtlb_translate_addr_eax(w, insn, X86_WIDTH_DWORD, true,
          &dst_slow_disp) ||
      !emit_mov_r10_rax(w) ||
      !emit_mov_eax_imm32(w, insn->next_pc) ||
      !emit_store_host_ptr_r10_eax_width(w, X86_WIDTH_DWORD) ||
      !emit_commit_stack_push_esp(w) ||
      !emit_load_dtlb_scratch_eax(w) ||
      !emit_store_pc_eax(w) ||
      !emit_jmp_rel32_placeholder(w, &done_disp)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if ((src_slow_disp != NULL && !patch_rel32(src_slow_disp, slow_native)) ||
      !patch_rel32(dst_slow_disp, slow_native) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  JIT_STAT_INC(native_branch_ops);
  if (!insn->rm_is_reg) JIT_STAT_INC(native_pmem_loads);
  JIT_STAT_INC(native_pmem_stores);
  return patch_rel32(done_disp, w->cur);
}

static bool emit_paged_dtlb_leave(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *slow_disp = NULL;
  uint8_t *done_disp = NULL;

  if (insn->width != X86_WIDTH_DWORD) return false;

  if (!emit_load_reg_eax(w, R_EBP) ||
      !emit_paged_dtlb_translate_addr_eax(w, insn, X86_WIDTH_DWORD, false,
          &slow_disp) ||
      !emit_load_host_ptr_rax_width(w, X86_WIDTH_DWORD) ||
      !emit_store_dtlb_scratch_eax(w) ||
      !emit_load_reg_eax(w, R_EBP) ||
      !emit_add_eax_imm32(w, X86_WIDTH_DWORD) ||
      !emit_store_reg_eax(w, R_ESP) ||
      !emit_load_dtlb_scratch_eax(w) ||
      !emit_store_reg_eax(w, R_EBP) ||
      !emit_jmp_rel32_placeholder(w, &done_disp)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(slow_disp, slow_native) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  JIT_STAT_INC(native_pmem_loads);
  return patch_rel32(done_disp, w->cur);
}

static bool emit_paged_dtlb_incdec_rm(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *slow_disp = NULL;
  uint8_t *done_disp = NULL;
  const uint8_t width = insn->width;

  if (insn->rm_is_reg) return false;
  if (width != X86_WIDTH_BYTE && width != X86_WIDTH_WORD &&
      width != X86_WIDTH_DWORD) {
    return false;
  }
  if (insn->alu_op != X86_ALU_ADD && insn->alu_op != X86_ALU_SUB) {
    return false;
  }

  if (!emit_paged_dtlb_load_ea_eax(w, insn, width, true, &slow_disp) ||
      !emit_alu_eax_imm_width(w, insn->alu_op, width, 1u) ||
      !emit_store_host_ptr_r10_eax_width(w, width) ||
      !emit_capture_status_flags_custom(w, X86_EFLAGS_INCDEC_COPY_MASK, 0) ||
      !emit_jmp_rel32_placeholder(w, &done_disp)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(slow_disp, slow_native) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  JIT_STAT_INC(native_incdec_ops);
  JIT_STAT_INC(native_pmem_loads);
  JIT_STAT_INC(native_pmem_stores);
  return patch_rel32(done_disp, w->cur);
}

static bool emit_paged_dtlb_not_rm(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *slow_disp = NULL;
  uint8_t *done_disp = NULL;
  const uint8_t width = insn->width;

  if (insn->rm_is_reg) return false;
  if (width != X86_WIDTH_BYTE && width != X86_WIDTH_WORD &&
      width != X86_WIDTH_DWORD) {
    return false;
  }

  if (!emit_paged_dtlb_load_ea_eax(w, insn, width, true, &slow_disp) ||
      !emit_not_eax(w) ||
      !emit_store_host_ptr_r10_eax_width(w, width) ||
      !emit_jmp_rel32_placeholder(w, &done_disp)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(slow_disp, slow_native) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  JIT_STAT_INC(native_not_ops);
  JIT_STAT_INC(native_pmem_loads);
  JIT_STAT_INC(native_pmem_stores);
  return patch_rel32(done_disp, w->cur);
}

static bool emit_paged_dtlb_neg_rm(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *slow_disp = NULL;
  uint8_t *done_disp = NULL;
  const uint8_t width = insn->width;

  if (insn->rm_is_reg) return false;
  if (width != X86_WIDTH_BYTE && width != X86_WIDTH_WORD &&
      width != X86_WIDTH_DWORD) {
    return false;
  }

  if (!emit_paged_dtlb_load_ea_eax(w, insn, width, true, &slow_disp) ||
      !emit_neg_eax_width(w, width) ||
      !emit_store_host_ptr_r10_eax_width(w, width) ||
      !emit_capture_status_flags(w, X86_EFLAGS_STATUS_MASK) ||
      !emit_jmp_rel32_placeholder(w, &done_disp)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(slow_disp, slow_native) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  JIT_STAT_INC(native_alu_ops);
  JIT_STAT_INC(native_pmem_loads);
  JIT_STAT_INC(native_pmem_stores);
  return patch_rel32(done_disp, w->cur);
}

static bool emit_paged_dtlb_setcc_rm8(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *slow_disp = NULL;
  uint8_t *done_disp = NULL;

  if (insn->rm_is_reg || insn->width != X86_WIDTH_BYTE) return false;

  if (!emit_guest_ea_eax(w, &insn->ea) ||
      !emit_paged_dtlb_translate_addr_eax(w, insn, X86_WIDTH_BYTE, true,
          &slow_disp) ||
      !emit_mov_r10_rax(w) ||
      !emit_condition_bool_eax(w, insn->cc) ||
      !emit_store_host_ptr_r10_eax_width(w, X86_WIDTH_BYTE) ||
      !emit_jmp_rel32_placeholder(w, &done_disp)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(slow_disp, slow_native) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  JIT_STAT_INC(native_pmem_stores);
  return patch_rel32(done_disp, w->cur);
}

static bool emit_paged_dtlb_imul_reg_rm(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *slow_disp = NULL;
  uint8_t *done_disp = NULL;
  const uint8_t width = insn->width;

  if (insn->rm_is_reg ||
      (width != X86_WIDTH_WORD && width != X86_WIDTH_DWORD)) {
    return false;
  }

  if (!emit_paged_dtlb_load_ea_eax(w, insn, width, false,
          &slow_disp) ||
      !emit_mov_ecx_eax(w) ||
      !emit_load_reg_to_eax_width(w, insn->dst, width)) {
    return false;
  }
  if (width == X86_WIDTH_WORD) {
    if (!emit_imul_ax_cx(w)) return false;
  }
  else if (!emit_imul_eax_ecx(w)) {
    return false;
  }

  if (!((width == X86_WIDTH_WORD) ?
          emit_store_reg_ax_no_flags(w, insn->dst) :
          emit_store_reg_eax(w, insn->dst)) ||
      !emit_capture_status_flags_custom(w, X86_FLAG_CF | X86_FLAG_OF, 0) ||
      !emit_jmp_rel32_placeholder(w, &done_disp)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(slow_disp, slow_native) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  JIT_STAT_INC(native_imul_ops);
  JIT_STAT_INC(native_pmem_loads);
  return patch_rel32(done_disp, w->cur);
}

static bool emit_paged_dtlb_mul_rm(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *slow_disp = NULL;
  uint8_t *done_disp = NULL;
  const uint8_t width = insn->width;

  if (insn->rm_is_reg ||
      (width != X86_WIDTH_BYTE && width != X86_WIDTH_WORD &&
          width != X86_WIDTH_DWORD)) {
    return false;
  }

  if (!emit_paged_dtlb_load_ea_eax(w, insn, width, false,
          &slow_disp) ||
      !emit_mov_ecx_eax(w) ||
      !emit_load_reg_eax(w, R_EAX)) {
    return false;
  }
  if (width == X86_WIDTH_BYTE) {
    if (!emit_mul_cl(w) ||
        !emit_store_reg_ax_no_flags(w, R_EAX)) {
      return false;
    }
  }
  else if (width == X86_WIDTH_WORD) {
    if (!emit_mul_cx(w) ||
        !emit_store_reg_ax_no_flags(w, R_EAX) ||
        !emit_store_reg_dx_no_flags(w, R_EDX)) {
      return false;
    }
  }
  else if (!emit_mul_ecx(w) ||
      !emit_store_edx_eax_pair(w)) {
    return false;
  }

  if (!emit_capture_status_flags_custom(w, X86_FLAG_CF | X86_FLAG_OF, 0) ||
      !emit_jmp_rel32_placeholder(w, &done_disp)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(slow_disp, slow_native) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  JIT_STAT_INC(native_mul_ops);
  JIT_STAT_INC(native_pmem_loads);
  return patch_rel32(done_disp, w->cur);
}

static bool emit_paged_dtlb_imul_acc_rm(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *slow_disp = NULL;
  uint8_t *done_disp = NULL;
  const uint8_t width = insn->width;

  if (insn->rm_is_reg ||
      (width != X86_WIDTH_BYTE && width != X86_WIDTH_WORD &&
          width != X86_WIDTH_DWORD)) {
    return false;
  }

  if (!emit_paged_dtlb_load_ea_eax(w, insn, width, false,
          &slow_disp) ||
      !emit_mov_ecx_eax(w) ||
      !emit_load_reg_eax(w, R_EAX)) {
    return false;
  }
  if (width == X86_WIDTH_BYTE) {
    if (!emit_imul_acc_cl(w) ||
        !emit_store_reg_ax_no_flags(w, R_EAX)) {
      return false;
    }
  }
  else if (width == X86_WIDTH_WORD) {
    if (!emit_imul_acc_cx(w) ||
        !emit_store_reg_ax_no_flags(w, R_EAX) ||
        !emit_store_reg_dx_no_flags(w, R_EDX)) {
      return false;
    }
  }
  else if (!emit_imul_acc_ecx(w) ||
      !emit_store_edx_eax_pair(w)) {
    return false;
  }

  if (!emit_capture_status_flags_custom(w, X86_FLAG_CF | X86_FLAG_OF, 0) ||
      !emit_jmp_rel32_placeholder(w, &done_disp)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(slow_disp, slow_native) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  JIT_STAT_INC(native_imul_ops);
  JIT_STAT_INC(native_pmem_loads);
  return patch_rel32(done_disp, w->cur);
}

static bool emit_paged_dtlb_div_rm(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *src_slow_disp = NULL;
  uint8_t *zero_slow_disp = NULL;
  uint8_t *overflow_slow_disp = NULL;
  uint8_t *done_disp = NULL;

  if (insn->rm_is_reg || insn->width != X86_WIDTH_DWORD) return false;

  /*
   * Host IDIV would raise a host #DE on unsupported dividends.  Keep native
   * execution to the sign-extended EAX subset and send every wider dividend to
   * the helper after the memory read has already had the chance to fault.
   */
  if (!emit_paged_dtlb_load_ea_eax(w, insn, X86_WIDTH_DWORD, false,
          &src_slow_disp) ||
      !emit_mov_ecx_eax(w) ||
      !emit_test_ecx_ecx(w) ||
      !emit_jcc_rel32_placeholder(w, X86_CC_Z, &zero_slow_disp) ||
      !emit_load_reg_eax(w, R_EAX) ||
      !emit_mov_r11d_eax(w) ||
      !emit_load_reg_edx(w, R_EDX) ||
      !emit_cmp_edx_ecx(w) ||
      !emit_jcc_rel32_placeholder(w, X86_CC_AE, &overflow_slow_disp) ||
      !emit_mov_eax_r11d(w) ||
      !emit_div_ecx(w) ||
      !emit_store_edx_eax_pair(w) ||
      !emit_jmp_rel32_placeholder(w, &done_disp)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(src_slow_disp, slow_native) ||
      !patch_rel32(zero_slow_disp, slow_native) ||
      !patch_rel32(overflow_slow_disp, slow_native) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  JIT_STAT_INC(native_div_ops);
  JIT_STAT_INC(native_pmem_loads);
  return patch_rel32(done_disp, w->cur);
}

static bool emit_paged_dtlb_idiv_rm(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *src_slow_disp = NULL;
  uint8_t *zero_slow_disp = NULL;
  uint8_t *wide_slow_disp = NULL;
  uint8_t *not_min_disp = NULL;
  uint8_t *overflow_slow_disp = NULL;
  uint8_t *done_disp = NULL;

  if (insn->rm_is_reg || insn->width != X86_WIDTH_DWORD) return false;

  if (!emit_paged_dtlb_load_ea_eax(w, insn, X86_WIDTH_DWORD, false,
          &src_slow_disp) ||
      !emit_mov_ecx_eax(w) ||
      !emit_store_dtlb_value_scratch_eax(w) ||
      !emit_test_ecx_ecx(w) ||
      !emit_jcc_rel32_placeholder(w, X86_CC_Z, &zero_slow_disp) ||
      !emit_load_reg_eax(w, R_EAX) ||
      !emit_mov_r11d_eax(w) ||
      !emit_cdq_host(w) ||
      !emit_mov_eax_edx(w) ||
      !emit_store_dtlb_scratch_eax(w) ||
      !emit_load_reg_eax(w, R_EDX) ||
      !emit_mov_ecx_eax(w) ||
      !emit_load_dtlb_scratch_eax(w) ||
      !emit_alu_eax_ecx_width(w, X86_ALU_CMP, X86_WIDTH_DWORD) ||
      !emit_jcc_rel32_placeholder(w, X86_CC_NZ, &wide_slow_disp) ||
      !emit_load_dtlb_value_scratch_eax(w) ||
      !emit_mov_ecx_eax(w) ||
      !emit_mov_eax_r11d(w) ||
      !emit_alu_eax_imm32(w, X86_ALU_CMP, 0x80000000u) ||
      !emit_jcc_rel32_placeholder(w, X86_CC_NZ, &not_min_disp) ||
      !emit_cmp_ecx_imm32(w, 0xffffffffu) ||
      !emit_jcc_rel32_placeholder(w, X86_CC_Z, &overflow_slow_disp)) {
    return false;
  }

  uint8_t *native_safe = w->cur;
  if (!patch_rel32(not_min_disp, native_safe) ||
      !emit_idiv_ecx(w) ||
      !emit_store_edx_eax_pair(w) ||
      !emit_jmp_rel32_placeholder(w, &done_disp)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(src_slow_disp, slow_native) ||
      !patch_rel32(zero_slow_disp, slow_native) ||
      !patch_rel32(wide_slow_disp, slow_native) ||
      !patch_rel32(overflow_slow_disp, slow_native) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  JIT_STAT_INC(native_div_ops);
  JIT_STAT_INC(native_pmem_loads);
  return patch_rel32(done_disp, w->cur);
}

static bool emit_store_reg_imm_width(x86_jit_writer_t *w, uint8_t reg,
    uint8_t width, uint32_t imm) {
  uint32_t keep_mask = 0;
  uint32_t value_mask = 0;

  if (width == X86_WIDTH_DWORD) return emit_store_reg_imm(w, reg, imm);
  if (width == X86_WIDTH_BYTE) {
    if (!jit_native_low_byte_reg(reg)) return false;
    keep_mask = 0xffffff00u;
    value_mask = X86_BYTE_MASK;
  }
  else if (width == X86_WIDTH_WORD) {
    keep_mask = 0xffff0000u;
    value_mask = X86_WORD_MASK;
  }
  else {
    return false;
  }

  return emit_load_reg_eax(w, reg) &&
         emit_alu_eax_imm32(w, X86_ALU_AND, keep_mask) &&
         emit_alu_eax_imm32(w, X86_ALU_OR, imm & value_mask) &&
         emit_store_reg_eax(w, reg);
}

static bool emit_alu_eax_ecx_width(x86_jit_writer_t *w, uint8_t alu_op,
    uint8_t width) {
  uint8_t opcode = 0;

  switch (alu_op) {
    case X86_ALU_ADD: opcode = width == X86_WIDTH_BYTE ? 0x00 : 0x01; break;
    case X86_ALU_OR:  opcode = width == X86_WIDTH_BYTE ? 0x08 : 0x09; break;
    case X86_ALU_ADC: opcode = width == X86_WIDTH_BYTE ? 0x10 : 0x11; break;
    case X86_ALU_SBB: opcode = width == X86_WIDTH_BYTE ? 0x18 : 0x19; break;
    case X86_ALU_AND: opcode = width == X86_WIDTH_BYTE ? 0x20 : 0x21; break;
    case X86_ALU_SUB: opcode = width == X86_WIDTH_BYTE ? 0x28 : 0x29; break;
    case X86_ALU_XOR: opcode = width == X86_WIDTH_BYTE ? 0x30 : 0x31; break;
    case X86_ALU_CMP: opcode = width == X86_WIDTH_BYTE ? 0x38 : 0x39; break;
    default: return false;
  }

  if (width == X86_WIDTH_WORD && !emit_u8(w, 0x66)) return false;
  if (width != X86_WIDTH_BYTE && width != X86_WIDTH_WORD &&
      width != X86_WIDTH_DWORD) {
    return false;
  }
  return emit_u8(w, opcode) && emit_u8(w, 0xc8);
}

static bool emit_capture_status_flags_custom(x86_jit_writer_t *w,
    uint32_t copy_mask, uint32_t clear_mask) {
  JIT_STAT_INC(flag_materialisations);
  return emit_pushfq(w) &&
         emit_pop_rax(w) &&
         emit_alu_eax_imm32(w, X86_ALU_AND, copy_mask) &&
         emit_load_eflags_ecx(w) &&
         emit_alu_reg_imm32(w, X86_ALU_AND, R_ECX,
             ~(copy_mask | clear_mask)) &&
         emit_or_eax_ecx(w) &&
         emit_alu_eax_imm32(w, X86_ALU_OR, X86_EFLAGS_FIXED_ONE) &&
         emit_store_eflags_eax(w);
}

static bool emit_capture_status_flags(x86_jit_writer_t *w,
    uint32_t copy_mask) {
  return emit_capture_status_flags_custom(w, copy_mask,
      X86_EFLAGS_STATUS_MASK & ~copy_mask);
}

static bool emit_native_incdec_reg_body(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  if (insn->width != X86_WIDTH_DWORD) return false;
  if (insn->alu_op != X86_ALU_ADD && insn->alu_op != X86_ALU_SUB) {
    return false;
  }

  return emit_load_reg_eax(w, insn->dst) &&
         emit_alu_eax_imm32(w, insn->alu_op, 1u) &&
         emit_store_reg_eax(w, insn->dst);
}

static bool emit_return_completed(x86_jit_writer_t *w, vaddr_t pc,
    uint32_t count) {
  if (jit_fast_chain_runtime_enabled()) {
    return emit_store_pc_imm(w, pc) &&
           emit_mov_eax_esi(w) &&
           emit_add_eax_imm32(w, count) &&
           emit_ret(w);
  }

  return emit_store_pc_imm(w, pc) &&
         emit_load_loop_extra_eax(w) &&
         emit_add_eax_imm32(w, count) &&
         emit_ret(w);
}

/* Emit host code for return loop extra; bytes below are x86-64 encodings. */
static bool emit_return_loop_extra(x86_jit_writer_t *w) {
  if (jit_fast_chain_runtime_enabled()) {
    return emit_mov_eax_esi(w) && emit_ret(w);
  }

  return emit_load_loop_extra_eax(w) && emit_ret(w);
}

/* Increment a 64-bit runtime statistic directly from generated code. */
static bool emit_runtime_counter_inc(x86_jit_writer_t *w,
    volatile uint64_t *counter) {
  return emit_movabs_rdx(w, (uint64_t)(uintptr_t)counter) &&
         emit_add_m64_rdx_imm8(w, 1u);
}

/* Map a slow-chain reason to the runtime counter incremented by its exit stub. */
static volatile uint64_t *jit_chain_slow_reason_counter(
    x86_jit_chain_slow_reason_t reason) {
  switch (reason) {
    case X86_JIT_CHAIN_SLOW_UNLINKED:
      return &jit_chain_exit_unlinked_runtime;
    case X86_JIT_CHAIN_SLOW_COLD_TRACE:
      return &jit_chain_exit_cold_trace_runtime;
    case X86_JIT_CHAIN_SLOW_SIDE_BRANCH:
      return &jit_chain_exit_side_branch_runtime;
    case X86_JIT_CHAIN_SLOW_HELPER:
      return &jit_chain_exit_helper_runtime;
    case X86_JIT_CHAIN_SLOW_UNACCEPTED_SUCCESSOR:
      return &jit_chain_exit_unaccepted_successor_runtime;
    case X86_JIT_CHAIN_SLOW_BLOCK_NOT_CHAINABLE:
      return &jit_chain_exit_block_not_chainable_runtime;
    default:
      return &jit_chain_exit_unlinked_runtime;
  }
}

/* Increment the generic side-exit counter and the reason-specific counter. */
static bool emit_side_exit_counter_inc(x86_jit_writer_t *w,
    volatile uint64_t *counter) {
  if (!jit_stats_enabled) return true;
  return emit_runtime_counter_inc(w, &jit_side_exits_runtime) &&
         emit_runtime_counter_inc(w, counter);
}

/* Emit a side-exit return that reports `count` retired guest instructions. */
static bool emit_ret_count_side_exit(x86_jit_writer_t *w, uint32_t count,
    x86_jit_chain_slow_reason_t reason) {
  return emit_side_exit_counter_inc(w, jit_chain_slow_reason_counter(reason)) &&
         emit_ret_count(w, count);
}

/* Stop a chained run before it would exceed the caller's instruction budget. */
static bool emit_chain_budget_guard(x86_jit_writer_t *w, vaddr_t pc,
    uint8_t **count_imm) {
  uint8_t *ok_disp = NULL;

  if (jit_fast_chain_runtime_enabled()) {
    if (!emit_mov_eax_esi(w) ||
        !emit_add_eax_imm32_placeholder(w, count_imm) ||
        !emit_cmp_eax_edi(w) ||
        !emit_jcc_rel32_placeholder(w, X86_CC_BE, &ok_disp) ||
        !emit_store_pc_imm(w, pc) ||
        !emit_mov_eax_esi(w) ||
        !emit_ret(w)) {
      return false;
    }

    return patch_rel32(ok_disp, w->cur);
  }

  if (!emit_load_loop_extra_eax(w) ||
      !emit_add_eax_imm32_placeholder(w, count_imm) ||
      !emit_cmp_eax_edi(w) ||
      !emit_jcc_rel32_placeholder(w, X86_CC_BE, &ok_disp) ||
      !emit_store_pc_imm(w, pc) ||
      !emit_return_loop_extra(w)) {
    return false;
  }

  return patch_rel32(ok_disp, w->cur);
}

/*
 * Emit a patchable direct-chain exit.
 *
 * State machine:
 *   1. The source block emits a rel32 jump at `patch_site`.
 *   2. Before the successor is known, the jump targets `slow_exit`, which
 *      stores the target PC and returns to the dispatcher.
 *   3. When the successor block is compiled and still current, the jump is
 *      patched to that block's chain entry, optionally through a hit-counter
 *      stub for stats.
 *   4. If the successor is invalidated, `target_generation` lets the JIT patch
 *      the edge back to `slow_exit`.
 */
static bool emit_chain_exit(x86_jit_writer_t *w, x86_jit_block_t *block,
    vaddr_t target_pc, uint32_t count, x86_jit_exit_kind_t kind,
    x86_jit_chain_slow_reason_t slow_reason) {
  uint8_t *budget_exit_disp = NULL;
  uint8_t *abort_exit_disp = NULL;
  uint8_t *chain_disp = NULL;

  if (block->exit_count >= X86_JIT_EXIT_EDGE_LIMIT) return false;

  if (jit_fast_chain_runtime_enabled()) {
    if (!emit_add_esi_imm32(w, count) ||
        !emit_cmp_esi_edi(w) ||
        !emit_jcc_rel32_placeholder(w, X86_CC_AE, &budget_exit_disp)) {
      return false;
    }
    if (jit_chain_abort_check_enabled &&
        (!emit_load_chain_abort_ecx(w) ||
         !emit_test_ecx_ecx(w) ||
         !emit_jcc_rel32_placeholder(w, X86_CC_NZ, &abort_exit_disp))) {
      return false;
    }
    if (!emit_jmp_rel32_placeholder(w, &chain_disp)) return false;

    x86_jit_exit_edge_t *edge = &block->exits[block->exit_count++];
    *edge = (x86_jit_exit_edge_t){
      .valid = true,
      .target_pc = target_pc,
      .patch_site = chain_disp,
      .slow_exit = NULL,
      .hit_stub = NULL,
      .hit_patch_site = NULL,
      .target_generation = 0,
      .kind = (uint8_t)kind,
      .slow_reason = (uint8_t)slow_reason,
    };

    uint8_t *budget_exit = w->cur;
    if (!patch_rel32(budget_exit_disp, budget_exit) ||
        !emit_store_pc_imm(w, target_pc) ||
        !emit_side_exit_counter_inc(w, &jit_chain_exit_budget_runtime) ||
        !emit_mov_eax_esi(w) ||
        !emit_ret(w)) {
      return false;
    }

    uint8_t *slow_exit = w->cur;
    if (jit_chain_abort_check_enabled) {
      if (!patch_rel32(abort_exit_disp, slow_exit) ||
          !emit_store_pc_imm(w, target_pc) ||
          !emit_side_exit_counter_inc(w, &jit_chain_exit_abort_runtime) ||
          !emit_mov_eax_esi(w) ||
          !emit_ret(w)) {
        return false;
      }
      slow_exit = w->cur;
    }

    edge->slow_exit = slow_exit;
    if (!patch_rel32(chain_disp, slow_exit) ||
        !emit_store_pc_imm(w, target_pc) ||
        !emit_side_exit_counter_inc(w,
            jit_chain_slow_reason_counter(slow_reason)) ||
        !emit_mov_eax_esi(w) ||
        !emit_ret(w)) {
      return false;
    }

    if (jit_stats_enabled) {
      edge->hit_stub = w->cur;
      if (!emit_runtime_counter_inc(w, &jit_direct_chain_hits_runtime) ||
          (block->paging &&
              !emit_runtime_counter_inc(w, &jit_stats.paged_chain_hits)) ||
          (slow_reason == X86_JIT_CHAIN_SLOW_COLD_TRACE &&
              jit_trace_sibling_enabled &&
              !emit_runtime_counter_inc(w,
                  &jit_sibling_trace_hits_runtime)) ||
          !emit_jmp_rel32_placeholder(w, &edge->hit_patch_site) ||
          !patch_rel32(edge->hit_patch_site, slow_exit)) {
        return false;
      }
    }

    return true;
  }

  if (!emit_store_pc_imm(w, target_pc) ||
      !emit_load_loop_extra_eax(w) ||
      !emit_add_eax_imm32(w, count) ||
      !emit_store_loop_extra_eax(w) ||
      !emit_cmp_eax_edi(w) ||
      !emit_jcc_rel32_placeholder(w, X86_CC_AE, &budget_exit_disp) ||
      !emit_load_chain_abort_ecx(w) ||
      !emit_test_ecx_ecx(w) ||
      !emit_jcc_rel32_placeholder(w, X86_CC_NZ, &abort_exit_disp) ||
      !emit_jmp_rel32_placeholder(w, &chain_disp)) {
    return false;
  }

  x86_jit_exit_edge_t *edge = &block->exits[block->exit_count++];
  *edge = (x86_jit_exit_edge_t){
    .valid = true,
    .target_pc = target_pc,
    .patch_site = chain_disp,
    .slow_exit = NULL,
    .hit_stub = NULL,
    .hit_patch_site = NULL,
    .target_generation = 0,
    .kind = (uint8_t)kind,
    .slow_reason = (uint8_t)slow_reason,
  };

  uint8_t *budget_exit = w->cur;
  if (!patch_rel32(budget_exit_disp, budget_exit) ||
      !emit_side_exit_counter_inc(w, &jit_chain_exit_budget_runtime) ||
      !emit_ret(w)) {
    return false;
  }

  uint8_t *abort_exit = w->cur;
  if (!patch_rel32(abort_exit_disp, abort_exit) ||
      !emit_side_exit_counter_inc(w, &jit_chain_exit_abort_runtime) ||
      !emit_ret(w)) {
    return false;
  }

  uint8_t *slow_exit = w->cur;
  edge->slow_exit = slow_exit;
  if (!patch_rel32(chain_disp, slow_exit) ||
      !emit_side_exit_counter_inc(w,
          jit_chain_slow_reason_counter(slow_reason)) ||
      !emit_ret(w)) {
    return false;
  }

  if (jit_stats_enabled) {
    edge->hit_stub = w->cur;
    if (!emit_runtime_counter_inc(w, &jit_direct_chain_hits_runtime) ||
        (block->paging &&
            !emit_runtime_counter_inc(w, &jit_stats.paged_chain_hits)) ||
        (slow_reason == X86_JIT_CHAIN_SLOW_COLD_TRACE &&
            jit_trace_sibling_enabled &&
            !emit_runtime_counter_inc(w,
                &jit_sibling_trace_hits_runtime)) ||
        !emit_jmp_rel32_placeholder(w, &edge->hit_patch_site) ||
        !patch_rel32(edge->hit_patch_site, slow_exit)) {
      return false;
    }
  }

  return true;
}

/* Emit a trace loopback to the trace head while respecting budget/abort exits. */
static bool emit_trace_head_loop(x86_jit_writer_t *w, vaddr_t target_pc,
    uint32_t count, const uint8_t *native_target) {
  uint8_t *abort_exit_disp = NULL;
  uint8_t *loop_disp = NULL;

  if (jit_fast_chain_runtime_enabled()) {
    if (!emit_add_esi_imm32(w, count) ||
        !emit_cmp_esi_edi(w) ||
        !emit_jcc_rel32_placeholder(w, X86_CC_AE, &abort_exit_disp) ||
        (jit_stats_enabled &&
            !emit_runtime_counter_inc(w, &jit_trace_loopback_runtime)) ||
        !emit_jmp_rel32_placeholder(w, &loop_disp)) {
      return false;
    }

    uint8_t *budget_native = w->cur;
    return patch_rel32(loop_disp, native_target) &&
           patch_rel32(abort_exit_disp, budget_native) &&
           emit_store_pc_imm(w, target_pc) &&
           emit_side_exit_counter_inc(w, &jit_chain_exit_budget_runtime) &&
           emit_mov_eax_esi(w) &&
           emit_ret(w);
  }

  if (!emit_load_loop_extra_eax(w) ||
      !emit_add_eax_imm32(w, count) ||
      !emit_store_loop_extra_eax(w) ||
      !emit_load_chain_abort_ecx(w) ||
      !emit_test_ecx_ecx(w) ||
      !emit_jcc_rel32_placeholder(w, X86_CC_NZ, &abort_exit_disp) ||
      (jit_stats_enabled &&
          !emit_runtime_counter_inc(w, &jit_trace_loopback_runtime)) ||
      !emit_jmp_rel32_placeholder(w, &loop_disp)) {
    return false;
  }

  uint8_t *abort_native = w->cur;
  return patch_rel32(loop_disp, native_target) &&
         patch_rel32(abort_exit_disp, abort_native) &&
         emit_store_pc_imm(w, target_pc) &&
         emit_return_loop_extra(w);
}

/* Restore a patchable edge so it returns through its slow dispatcher exit. */
static void jit_patch_edge_to_slow(x86_jit_exit_edge_t *edge) {
  if (edge == NULL || !edge->valid || edge->patch_site == NULL ||
      edge->slow_exit == NULL) {
    return;
  }

  (void)patch_rel32(edge->patch_site, edge->slow_exit);
  if (edge->hit_patch_site != NULL) {
    (void)patch_rel32(edge->hit_patch_site, edge->slow_exit);
  }
  edge->target_generation = 0;
}

/* Patch a valid edge to a compiled target block, with generation tracking. */
static void jit_patch_edge_to_target(x86_jit_exit_edge_t *edge,
    const x86_jit_block_t *target) {
  if (edge == NULL || target == NULL || !edge->valid ||
      target->chain_entry == NULL) {
    return;
  }
  if (jit_trace_sibling_enabled &&
      edge->slow_reason == X86_JIT_CHAIN_SLOW_COLD_TRACE &&
      !target->is_trace) {
    jit_patch_edge_to_slow(edge);
    return;
  }

  if (edge->hit_stub != NULL && edge->hit_patch_site != NULL) {
    if (!patch_rel32(edge->hit_patch_site, target->chain_entry) ||
        !patch_rel32(edge->patch_site, edge->hit_stub)) {
      jit_patch_edge_to_slow(edge);
      return;
    }
  }
  else if (!patch_rel32(edge->patch_site, target->chain_entry)) {
    jit_patch_edge_to_slow(edge);
    return;
  }

  edge->target_generation = target->generation;
  JIT_STAT_INC(direct_chain_patches);
  if (target->paging) JIT_STAT_INC(paged_chain_patches);
}

/* Remove every edge that currently jumps into a target block being invalidated. */
static void jit_unpatch_incoming_edges(const x86_jit_block_t *target) {
  if (target == NULL || target->chain_entry == NULL) return;

  for (uint32_t i = 0; i < X86_JIT_CACHE_SIZE; i++) {
    x86_jit_block_t *block = &jit_cache[i];
    for (uint8_t edge_index = 0; edge_index < block->exit_count; edge_index++) {
      x86_jit_exit_edge_t *edge = &block->exits[edge_index];
      if (edge->valid && edge->target_pc == target->pc &&
          edge->target_generation == target->generation) {
        jit_patch_edge_to_slow(edge);
      }
    }
  }
}

/* Return r11d plus an immediate retired-count increment. */
static bool emit_return_r11_plus_imm(x86_jit_writer_t *w, uint32_t count) {
  return emit_mov_eax_r11d(w) &&
         emit_add_eax_imm32(w, count) &&
         emit_ret(w);
}

/* Return the retired-count value already held in r11d. */
static bool emit_return_r11(x86_jit_writer_t *w) {
  return emit_mov_eax_r11d(w) && emit_ret(w);
}

/*
 * Calculate how many loop iterations remain for a resident backedge.  The
 * count parameter is guest instructions per lap; division converts the caller's
 * remaining instruction budget into a lap budget.
 */
static bool emit_resident_lap_budget_r10d(x86_jit_writer_t *w, uint32_t count) {
  if (jit_fast_chain_runtime_enabled()) {
    if (count == 2u) {
      return emit_mov_ecx_edi(w) &&
             emit_sub_ecx_esi(w) &&
             emit_shr_ecx_imm(w, 1u) &&
             emit_mov_r10d_ecx(w);
    }

    return emit_mov_eax_edi(w) &&
           emit_sub_eax_esi(w) &&
           emit_xor_edx_edx(w) &&
           emit_mov_ecx_imm32(w, count) &&
           emit_div_ecx(w) &&
           emit_mov_r10d_eax(w);
  }

  if (count == 2u) {
    return emit_load_entry_budget_arg_ecx(w) &&
           emit_load_loop_extra_eax(w) &&
           emit_sub_ecx_eax(w) &&
           emit_shr_ecx_imm(w, 1u) &&
           emit_mov_r10d_ecx(w);
  }

  return emit_load_entry_budget_arg_eax(w) &&
         emit_load_loop_extra_ecx(w) &&
         emit_sub_eax_ecx(w) &&
         emit_xor_edx_edx(w) &&
         emit_mov_ecx_imm32(w, count) &&
         emit_div_ecx(w) &&
         emit_mov_r10d_eax(w);
}

/* Map single-flag Jcc conditions to `test eflags, mask` plus host JZ/JNZ. */
static bool jit_backedge_flag_test(uint8_t cc, uint32_t *flag,
    uint8_t *host_cc) {
  switch (cc & 0xfu) {
    case X86_CC_O:  *flag = X86_FLAG_OF; *host_cc = X86_CC_NZ; return true;
    case X86_CC_NO: *flag = X86_FLAG_OF; *host_cc = X86_CC_Z;  return true;
    case X86_CC_B:  *flag = X86_FLAG_CF; *host_cc = X86_CC_NZ; return true;
    case X86_CC_AE: *flag = X86_FLAG_CF; *host_cc = X86_CC_Z;  return true;
    case X86_CC_Z:  *flag = X86_FLAG_ZF; *host_cc = X86_CC_NZ; return true;
    case X86_CC_NZ: *flag = X86_FLAG_ZF; *host_cc = X86_CC_Z;  return true;
    case X86_CC_S:  *flag = X86_FLAG_SF; *host_cc = X86_CC_NZ; return true;
    case X86_CC_NS: *flag = X86_FLAG_SF; *host_cc = X86_CC_Z;  return true;
    case X86_CC_P:  *flag = X86_FLAG_PF; *host_cc = X86_CC_NZ; return true;
    case X86_CC_NP: *flag = X86_FLAG_PF; *host_cc = X86_CC_Z;  return true;
    default: return false;
  }
}

/* Map cheap Jcc conditions to a guest-EFLAGS mask and native condition code. */
static bool jit_jcc_fast_test(uint8_t cc, uint32_t *mask, uint8_t *host_cc) {
  if (jit_backedge_flag_test(cc, mask, host_cc)) return true;

  switch (cc & 0xfu) {
    case X86_CC_BE:
      *mask = X86_FLAG_CF | X86_FLAG_ZF;
      *host_cc = X86_CC_NZ;
      return true;
    case X86_CC_A:
      *mask = X86_FLAG_CF | X86_FLAG_ZF;
      *host_cc = X86_CC_Z;
      return true;
    default:
      return false;
  }
}

/* Signed Jcc needs SF xor OF, which cannot be tested with one EFLAGS mask. */
static bool jit_jcc_signed_test(uint8_t cc) {
  switch (cc & 0xfu) {
    case X86_CC_L:
    case X86_CC_GE:
    case X86_CC_LE:
    case X86_CC_G:
      return true;
    default:
      return false;
  }
}

/* Return true when the Jcc condition can be lowered without a helper call. */
static bool jit_jcc_native_supported(uint8_t cc) {
  uint32_t mask = 0;
  uint8_t host_cc = 0;
  return jit_jcc_fast_test(cc, &mask, &host_cc) ||
         jit_jcc_signed_test(cc);
}

/* Conditions safe to reuse immediately after native INC/DEC lowering. */
static bool jit_incdec_jcc_host_cc(uint8_t cc, uint8_t *host_cc) {
  switch (cc & 0xfu) {
    case X86_CC_O:
    case X86_CC_NO:
    case X86_CC_Z:
    case X86_CC_NZ:
    case X86_CC_S:
    case X86_CC_NS:
    case X86_CC_P:
    case X86_CC_NP:
    case X86_CC_L:
    case X86_CC_GE:
    case X86_CC_LE:
    case X86_CC_G:
      *host_cc = cc & 0xfu;
      return true;
    default:
      /*
       * INC/DEC preserve guest CF, while the host ADD/SUB instruction used for
       * lowering updates host CF.  Conditions that read CF must therefore use
       * the normal materialised-EFLAGS path.
       */
      return false;
  }
}

static bool emit_signed_jcc_condition_jump(x86_jit_writer_t *w, uint8_t cc,
    uint8_t **taken_disp) {
  const bool includes_zf = (cc & 0xfu) == X86_CC_LE ||
      (cc & 0xfu) == X86_CC_G;
  uint8_t host_cc = 0;

  switch (cc & 0xfu) {
    case X86_CC_L:
    case X86_CC_LE:
      host_cc = X86_CC_NZ;
      break;
    case X86_CC_GE:
    case X86_CC_G:
      host_cc = X86_CC_Z;
      break;
    default:
      return false;
  }

  if (!emit_load_eflags_eax(w) ||
      !emit_mov_edx_eax(w) ||
      !emit_mov_ecx_edx(w) ||
      !emit_shr_ecx_imm(w, 4u) ||
      !emit_alu_rm32_r32(w, X86_ALU_XOR, R_EAX, R_ECX)) {
    return false;
  }

  /*
   * OF is bit 11 and SF is bit 7.  Shifting a saved copy right by four aligns
   * OF with SF, so the XOR leaves the signed-less-than predicate in SF's bit.
   * JLE/JG add ZF to that predicate.
   */
  if (includes_zf) {
    if (!emit_alu_eax_imm32(w, X86_ALU_AND, X86_FLAG_SF) ||
        !emit_mov_ecx_edx(w) ||
        !emit_and_ecx_imm32(w, X86_FLAG_ZF) ||
        !emit_or_eax_ecx(w)) {
      return false;
    }
  }

  return emit_test_eax_imm32(w, includes_zf ?
             (X86_FLAG_SF | X86_FLAG_ZF) : X86_FLAG_SF) &&
         emit_jcc_rel32_placeholder(w, host_cc, taken_disp);
}

static bool emit_jcc_condition_jump(x86_jit_writer_t *w, uint8_t cc,
    uint8_t **taken_disp) {
  uint32_t mask = 0;
  uint8_t host_cc = 0;
  if (!jit_jcc_fast_test(cc, &mask, &host_cc)) {
    return emit_signed_jcc_condition_jump(w, cc, taken_disp);
  }

  return emit_load_eflags_eax(w) &&
         emit_test_eax_imm32(w, mask) &&
         emit_jcc_rel32_placeholder(w, host_cc, taken_disp);
}

static bool emit_backedge_loop_accounting(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn, const uint8_t *native_target,
    uint32_t count) {
  uint8_t *budget_exit_disp = NULL;
  uint8_t *abort_exit_disp = NULL;
  uint8_t *loop_disp = NULL;

  if (!emit_load_loop_extra_eax(w) ||
      !emit_add_eax_imm32(w, count) ||
      !emit_store_loop_extra_eax(w) ||
      !emit_load_entry_budget_ecx(w) ||
      !emit_mov_edx_eax(w) ||
      !emit_add_edx_imm32(w, count) ||
      !emit_cmp_edx_ecx(w) ||
      !emit_jcc_rel32_placeholder(w, X86_CC_A, &budget_exit_disp) ||
      !emit_load_chain_abort_ecx(w) ||
      !emit_test_ecx_ecx(w) ||
      !emit_jcc_rel32_placeholder(w, X86_CC_NZ, &abort_exit_disp) ||
      !emit_jmp_rel32_placeholder(w, &loop_disp)) {
    return false;
  }

  if (!patch_rel32(loop_disp, native_target)) return false;

  uint8_t *exit_native = w->cur;
  if (!patch_rel32(budget_exit_disp, exit_native) ||
      !patch_rel32(abort_exit_disp, exit_native)) {
    return false;
  }

  return emit_store_pc_imm(w, jit_branch_target(insn)) && emit_ret(w);
}

static bool emit_jcc_backedge(x86_jit_writer_t *w, const x86_jit_insn_t *insn,
    const uint8_t *native_target, uint32_t count) {
  uint8_t *taken_disp = NULL;
  uint32_t flag = 0;
  uint8_t host_cc = 0;

  if (!jit_backedge_flag_test(insn->cc, &flag, &host_cc)) return false;

  if (!emit_load_eflags_eax(w) ||
      !emit_test_eax_imm32(w, flag) ||
      !emit_jcc_rel32_placeholder(w, host_cc, &taken_disp) ||
      !emit_return_completed(w, insn->next_pc, count)) {
    return false;
  }

  uint8_t *taken_native = w->cur;
  if (!patch_rel32(taken_disp, taken_native)) return false;

  return emit_backedge_loop_accounting(w, insn, native_target, count);
}

static bool emit_incdec_jcc_resident_backedge(x86_jit_writer_t *w,
    const x86_jit_insn_t *incdec, const x86_jit_insn_t *jcc,
    uint32_t count) {
  uint8_t *taken_disp = NULL;
  uint8_t *budget_exit_disp = NULL;
  uint8_t *loop_disp = NULL;
  uint8_t host_cc = 0;

  if (count != 2u || !jit_incdec_jcc_host_cc(jcc->cc, &host_cc)) {
    return false;
  }

  if (!((jit_fast_chain_runtime_enabled() ?
          emit_mov_r11d_esi(w) :
          (emit_load_loop_extra_eax(w) && emit_mov_r11d_eax(w)))) ||
      !emit_resident_lap_budget_r10d(w, count) ||
      !emit_load_reg_eax(w, incdec->dst)) {
    return false;
  }

  const uint8_t *loop_native = w->cur;
  if (!emit_alu_eax_imm32(w, incdec->alu_op, 1u) ||
      !emit_jcc_rel32_placeholder(w, host_cc, &taken_disp) ||
      !emit_store_reg_eax(w, incdec->dst) ||
      !emit_capture_status_flags_custom(w, X86_EFLAGS_INCDEC_COPY_MASK, 0) ||
      !emit_store_pc_imm(w, jcc->next_pc) ||
      !emit_return_r11_plus_imm(w, count)) {
    return false;
  }

  uint8_t *taken_native = w->cur;
  if (!patch_rel32(taken_disp, taken_native) ||
      !emit_lea_r11d_r11d_disp8(w, (int8_t)count) ||
      !emit_lea_r10d_r10d_disp8(w, -1) ||
      !emit_mov_ecx_r10d(w) ||
      !emit_jrcxz_rel8_placeholder(w, &budget_exit_disp) ||
      !emit_jmp_rel32_placeholder(w, &loop_disp)) {
    return false;
  }

  uint8_t *budget_exit_native = w->cur;
  if (!patch_rel8(budget_exit_disp, budget_exit_native) ||
      !patch_rel32(loop_disp, loop_native) ||
      !emit_store_reg_eax(w, incdec->dst) ||
      !emit_capture_status_flags_custom(w, X86_EFLAGS_INCDEC_COPY_MASK, 0) ||
      !emit_store_pc_imm(w, jit_branch_target(jcc)) ||
      !emit_return_r11(w)) {
    return false;
  }

  JIT_STAT_INC(native_incdec_ops);
  JIT_STAT_INC(native_incdec_jcc_backedges);
  JIT_STAT_INC(native_incdec_resident_loops);
  return true;
}

static bool emit_cmp_with_resident_eax(x86_jit_writer_t *w,
    const x86_jit_insn_t *cmp, uint8_t resident_reg) {
  if (cmp->width != X86_WIDTH_DWORD || cmp->alu_op != X86_ALU_CMP) {
    return false;
  }

  if (cmp->op == X86_JIT_OP_ALU_IMM_REG && cmp->dst == resident_reg) {
    return emit_alu_eax_imm32(w, X86_ALU_CMP, cmp->imm);
  }

  if (cmp->op != X86_JIT_OP_ALU_REG_REG) return false;

  if (cmp->dst == resident_reg && cmp->src == resident_reg) {
    return emit_alu_rm32_r32(w, X86_ALU_CMP, R_EAX, R_EAX);
  }

  if (cmp->dst == resident_reg) {
    return emit_load_reg_ecx(w, cmp->src) &&
           emit_alu_rm32_r32(w, X86_ALU_CMP, R_EAX, R_ECX);
  }

  if (cmp->src == resident_reg) {
    return emit_load_reg_ecx(w, cmp->dst) &&
           emit_alu_rm32_r32(w, X86_ALU_CMP, R_ECX, R_EAX);
  }

  return false;
}

static bool emit_incdec_cmp_jcc_resident_backedge(x86_jit_writer_t *w,
    const x86_jit_insn_t *incdec, const x86_jit_insn_t *cmp,
    const x86_jit_insn_t *jcc, uint32_t count) {
  uint8_t *taken_disp = NULL;
  uint8_t *budget_exit_disp = NULL;
  uint8_t *loop_disp = NULL;
  const uint8_t host_cc = jcc->cc & 0xfu;

  if (count < 2u || count > INT8_MAX) return false;

  if (!((jit_fast_chain_runtime_enabled() ?
          emit_mov_r11d_esi(w) :
          (emit_load_loop_extra_eax(w) && emit_mov_r11d_eax(w)))) ||
      !emit_resident_lap_budget_r10d(w, count) ||
      !emit_load_reg_eax(w, incdec->dst)) {
    return false;
  }

  const uint8_t *loop_native = w->cur;
  if (!emit_alu_eax_imm32(w, incdec->alu_op, 1u) ||
      !emit_cmp_with_resident_eax(w, cmp, incdec->dst) ||
      !emit_jcc_rel32_placeholder(w, host_cc, &taken_disp) ||
      !emit_store_reg_eax(w, incdec->dst) ||
      !emit_capture_status_flags(w, X86_EFLAGS_STATUS_MASK) ||
      !emit_store_pc_imm(w, jcc->next_pc) ||
      !emit_return_r11_plus_imm(w, count)) {
    return false;
  }

  uint8_t *taken_native = w->cur;
  if (!patch_rel32(taken_disp, taken_native) ||
      !emit_lea_r11d_r11d_disp8(w, (int8_t)count) ||
      !emit_lea_r10d_r10d_disp8(w, -1) ||
      !emit_mov_ecx_r10d(w) ||
      !emit_jrcxz_rel8_placeholder(w, &budget_exit_disp) ||
      !emit_jmp_rel32_placeholder(w, &loop_disp)) {
    return false;
  }

  uint8_t *budget_exit_native = w->cur;
  if (!patch_rel8(budget_exit_disp, budget_exit_native) ||
      !patch_rel32(loop_disp, loop_native) ||
      !emit_store_reg_eax(w, incdec->dst) ||
      !emit_capture_status_flags(w, X86_EFLAGS_STATUS_MASK) ||
      !emit_store_pc_imm(w, jit_branch_target(jcc)) ||
      !emit_return_r11(w)) {
    return false;
  }

  JIT_STAT_INC(native_incdec_ops);
  JIT_STAT_INC(native_alu_ops);
  JIT_STAT_INC(native_incdec_jcc_backedges);
  JIT_STAT_INC(native_incdec_resident_loops);
  return true;
}

static bool emit_native_jmp_rel(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  JIT_STAT_INC(native_branch_ops);
  return emit_store_pc_imm(w, jit_branch_target(insn));
}

static bool emit_chained_jmp_rel(x86_jit_writer_t *w, x86_jit_block_t *block,
    const x86_jit_insn_t *insn, uint32_t count) {
  JIT_STAT_INC(native_branch_ops);
  return emit_chain_exit(w, block, jit_branch_target(insn), count,
      X86_JIT_EXIT_JMP, X86_JIT_CHAIN_SLOW_UNLINKED);
}

static bool emit_native_jmp_rm(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *slow_disp = NULL;
  uint8_t *done_disp = NULL;

  if (insn->width != X86_WIDTH_DWORD) return false;

  if (insn->rm_is_reg) {
    JIT_STAT_INC(native_branch_ops);
    return emit_load_reg_eax(w, insn->rm_reg) && emit_store_pc_eax(w);
  }

  if (!emit_guest_ea_eax(w, &insn->ea) ||
      !emit_mov_edx_eax(w) ||
      !emit_direct_pmem_guard_edx(w, X86_WIDTH_DWORD, &slow_disp) ||
      !emit_movabs_r10(w, (uint64_t)(uintptr_t)guest_to_host(CONFIG_MBASE)) ||
      !emit_mov_eax_m32_r10_rdx(w) ||
      !emit_store_pc_eax(w) ||
      !emit_jmp_rel32_placeholder(w, &done_disp)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(slow_disp, slow_native) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  JIT_STAT_INC(native_branch_ops);
  JIT_STAT_INC(native_pmem_loads);
  return patch_rel32(done_disp, w->cur);
}

static bool emit_native_jcc_rel(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *taken_disp = NULL;
  uint8_t *done_disp = NULL;

  if (!emit_jcc_condition_jump(w, insn->cc, &taken_disp) ||
      !emit_store_pc_imm(w, insn->next_pc) ||
      !emit_jmp_rel32_placeholder(w, &done_disp)) {
    return false;
  }

  uint8_t *taken_native = w->cur;
  if (!patch_rel32(taken_disp, taken_native) ||
      !emit_store_pc_imm(w, jit_branch_target(insn))) {
    return false;
  }

  JIT_STAT_INC(native_branch_ops);
  return patch_rel32(done_disp, w->cur);
}

static bool emit_jcc_edge_exit(x86_jit_writer_t *w, x86_jit_block_t *block,
    vaddr_t target_pc, uint32_t count, x86_jit_exit_kind_t kind,
    bool can_chain) {
  if (can_chain) {
    return emit_chain_exit(w, block, target_pc, count, kind,
        X86_JIT_CHAIN_SLOW_UNLINKED);
  }

  return emit_store_pc_imm(w, target_pc) &&
         emit_ret_count_side_exit(w, count,
             X86_JIT_CHAIN_SLOW_UNACCEPTED_SUCCESSOR);
}

static bool emit_jcc_rel_per_edge(x86_jit_writer_t *w,
    x86_jit_block_t *block, const x86_jit_insn_t *insn, uint32_t count,
    bool fallthrough_can_chain, bool taken_can_chain) {
  uint8_t *taken_disp = NULL;

  if (!emit_jcc_condition_jump(w, insn->cc, &taken_disp) ||
      !emit_jcc_edge_exit(w, block, insn->next_pc, count,
          X86_JIT_EXIT_FALLTHROUGH, fallthrough_can_chain)) {
    return false;
  }

  uint8_t *taken_native = w->cur;
  if (!patch_rel32(taken_disp, taken_native) ||
      !emit_jcc_edge_exit(w, block, jit_branch_target(insn), count,
          X86_JIT_EXIT_TAKEN, taken_can_chain)) {
    return false;
  }

  JIT_STAT_INC(native_branch_ops);
  return true;
}

/* CMP is the only native ALU producer here that writes flags but not a result. */
static bool jit_native_alu_writes_result(uint8_t alu_op) {
  return alu_op != X86_ALU_CMP;
}

/* Only EAX/ECX/EDX/EBX have legacy low-byte names without a REX complication. */
static bool jit_native_low_byte_reg(uint8_t reg) {
  return reg < 4u;
}

/* Select which host status flags are safe to copy after native ALU emission. */
static uint32_t jit_native_alu_flag_copy_mask(uint8_t alu_op) {
  switch (alu_op) {
    case X86_ALU_OR:
    case X86_ALU_AND:
    case X86_ALU_XOR:
      /*
       * Intel leaves AF undefined for logical instructions.  The existing x86
       * interpreter/JIT helper clears AF, so native lowering keeps that local
       * contract instead of copying the host's undefined AF value.
       */
      return X86_EFLAGS_LOGIC_COPY_MASK;
    default:
      return X86_EFLAGS_STATUS_MASK;
  }
}

/* Return true for a Jcc decoded in the native branch IR form. */
static bool jit_is_native_jcc(const x86_jit_insn_t *insn) {
  return insn->op == X86_JIT_OP_JCC_REL;
}

/* Conservative check for helpers that may read/write guest memory or stack. */
static bool jit_helper_may_touch_guest_memory(const x86_jit_insn_t *insn) {
  switch (insn->helper) {
    case X86_JIT_HELPER_MOV_RM_REG:
    case X86_JIT_HELPER_MOV_REG_RM:
    case X86_JIT_HELPER_MOV_IMM_RM:
    case X86_JIT_HELPER_ALU_RM_REG:
    case X86_JIT_HELPER_ALU_REG_RM:
    case X86_JIT_HELPER_ALU_IMM_RM:
    case X86_JIT_HELPER_TEST_RM_REG:
    case X86_JIT_HELPER_JMP_RM:
    case X86_JIT_HELPER_INCDEC_RM:
    case X86_JIT_HELPER_NOT_RM:
    case X86_JIT_HELPER_NEG_RM:
    case X86_JIT_HELPER_TEST_IMM_RM:
    case X86_JIT_HELPER_MUL_RM:
    case X86_JIT_HELPER_IMUL_ACC_RM:
    case X86_JIT_HELPER_DIV_RM:
    case X86_JIT_HELPER_IDIV_RM:
    case X86_JIT_HELPER_SETCC_RM8:
    case X86_JIT_HELPER_MOVZX_REG_RM8:
    case X86_JIT_HELPER_MOVZX_REG_RM16:
    case X86_JIT_HELPER_MOVSX_REG_RM8:
    case X86_JIT_HELPER_MOVSX_REG_RM16:
    case X86_JIT_HELPER_SHIFT_RM:
    case X86_JIT_HELPER_IMUL_REG_RM:
      return !insn->rm_is_reg;
    case X86_JIT_HELPER_MOV_EAX_MOFFS:
    case X86_JIT_HELPER_MOV_MOFFS_EAX:
    case X86_JIT_HELPER_PUSH_REG:
    case X86_JIT_HELPER_PUSH_IMM:
    case X86_JIT_HELPER_PUSH_RM:
    case X86_JIT_HELPER_POP_REG:
    case X86_JIT_HELPER_POP_RM:
    case X86_JIT_HELPER_CALL_REL:
    case X86_JIT_HELPER_CALL_RM:
    case X86_JIT_HELPER_RET:
    case X86_JIT_HELPER_LEAVE:
      return true;
    default:
      return false;
  }
}

/* Return true for native instructions whose host flags can feed a following Jcc. */
static bool jit_is_fusible_flag_producer(const x86_jit_insn_t *insn) {
  switch (insn->op) {
    case X86_JIT_OP_ALU_REG_REG:
    case X86_JIT_OP_ALU_IMM_REG:
      if (jit_alu_reads_carry(insn->alu_op)) return false;
      return insn->width == X86_WIDTH_DWORD;
    case X86_JIT_OP_TEST_REG_REG:
    case X86_JIT_OP_TEST_EAX_IMM:
      return insn->width == X86_WIDTH_DWORD;
    default:
      return false;
  }
}

/* Return the EFLAGS bits produced by a fusible flag-writing instruction. */
static uint32_t jit_flag_producer_copy_mask(const x86_jit_insn_t *insn) {
  if (insn->op == X86_JIT_OP_TEST_REG_REG ||
      insn->op == X86_JIT_OP_TEST_EAX_IMM) {
    return X86_EFLAGS_LOGIC_COPY_MASK;
  }

  return jit_native_alu_flag_copy_mask(insn->alu_op);
}

/* Return true when the next instruction overwrites flags before anyone reads them. */
static bool jit_flags_overwritten_by_next(const x86_jit_insn_t *insns,
    uint32_t index, uint32_t count) {
  return index + 1u < count &&
         jit_is_fusible_flag_producer(&insns[index]) &&
         jit_is_fusible_flag_producer(&insns[index + 1u]);
}

/* Return true when an instruction defines every status flag this JIT models. */
static bool jit_insn_fully_writes_flags(const x86_jit_insn_t *insn) {
  if (insn->op == X86_JIT_OP_TEST_REG_REG ||
      insn->op == X86_JIT_OP_TEST_EAX_IMM) {
    return true;
  }

  if ((insn->op == X86_JIT_OP_ALU_REG_REG ||
      insn->op == X86_JIT_OP_ALU_IMM_REG) &&
      insn->alu_op != X86_ALU_ADC &&
      insn->alu_op != X86_ALU_SBB) {
    return true;
  }

  return false;
}

/* Return true when an instruction consumes the current guest EFLAGS value. */
static bool jit_insn_reads_flags(const x86_jit_insn_t *insn) {
  if (insn->op == X86_JIT_OP_JCC_REL) return true;

  if ((insn->op == X86_JIT_OP_ALU_REG_REG ||
      insn->op == X86_JIT_OP_ALU_IMM_REG) &&
      (insn->alu_op == X86_ALU_ADC || insn->alu_op == X86_ALU_SBB)) {
    return true;
  }

  if (insn->op != X86_JIT_OP_HELPER) return false;

  switch (insn->helper) {
    case X86_JIT_HELPER_JCC_REL:
    case X86_JIT_HELPER_SETCC_RM8:
    case X86_JIT_HELPER_INCDEC_REG:
    case X86_JIT_HELPER_INCDEC_RM:
    case X86_JIT_HELPER_SHIFT_RM:
      return true;
    case X86_JIT_HELPER_ALU_RM_REG:
    case X86_JIT_HELPER_ALU_REG_RM:
    case X86_JIT_HELPER_ALU_IMM_RM:
    case X86_JIT_HELPER_ALU_EAX_IMM:
      return insn->alu_op == X86_ALU_ADC || insn->alu_op == X86_ALU_SBB;
    default:
      return false;
  }
}

/* Probe forward to see whether successor code overwrites flags before reading them. */
static bool jit_successor_flags_dead(vaddr_t pc) {
  if (!jit_lazy_flags_enabled || !jit_flat_segments()) return false;

  x86_jit_reader_t r = { .pc = pc, .cur = pc };
  for (uint32_t i = 0; i < 8u; i++) {
    x86_jit_reader_t probe = r;
    x86_jit_insn_t insn;
    if (!jit_decode_insn(&probe, &insn)) return false;

    if (jit_insn_reads_flags(&insn)) return false;
    if (jit_insn_fully_writes_flags(&insn)) return true;
    if (insn.ends_block) return false;

    r = probe;
    if ((uint32_t)(r.cur - pc) >= X86_JIT_MAX_SOURCE_BYTES) return false;
  }

  return false;
}

static bool emit_capture_status_flags_if_live(x86_jit_writer_t *w,
    uint32_t copy_mask, vaddr_t successor_pc) {
  if (jit_successor_flags_dead(successor_pc)) return true;
  return emit_capture_status_flags(w, copy_mask);
}

static bool jit_native_shift_flag_copy_mask(uint8_t shift_op, uint8_t count,
    uint8_t *host_op, uint32_t *copy_mask) {
  *host_op = shift_op;

  switch (shift_op) {
    case 0:
    case 1:
      *copy_mask = X86_FLAG_CF;
      if (count == 1) *copy_mask |= X86_FLAG_OF;
      return true;
    case 4:
    case 6:
      *host_op = 4;
      *copy_mask = X86_FLAG_CF | X86_FLAG_PF | X86_FLAG_ZF | X86_FLAG_SF;
      if (count == 1) *copy_mask |= X86_FLAG_OF;
      return true;
    case 5:
    case 7:
      *copy_mask = X86_FLAG_CF | X86_FLAG_PF | X86_FLAG_ZF | X86_FLAG_SF;
      if (count == 1) *copy_mask |= X86_FLAG_OF;
      return true;
    default:
      return false;
  }
}

static bool jit_native_shift_count_safe_width(uint8_t shift_op, uint8_t width,
    uint8_t count) {
  if (width == X86_WIDTH_DWORD) return true;
  if (width != X86_WIDTH_BYTE && width != X86_WIDTH_WORD) return false;
  if (count == 0) return true;

  const uint8_t bits = width * X86_BITS_PER_BYTE;
  switch (shift_op) {
    case 0:
    case 1:
      return (count % bits) != 0;
    case 4:
    case 5:
    case 6:
    case 7:
      return count <= bits;
    default:
      return false;
  }
}

static bool emit_native_alu_reg_reg(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  if (!emit_load_reg_eax(w, insn->dst) ||
      !emit_load_reg_ecx(w, insn->src) ||
      (jit_alu_reads_carry(insn->alu_op) &&
          !emit_guest_cf_to_host_cf_r11(w)) ||
      !emit_alu_rm32_r32(w, insn->alu_op, R_EAX, R_ECX)) {
    return false;
  }

  if (jit_native_alu_writes_result(insn->alu_op) &&
      !emit_store_reg_eax(w, insn->dst)) {
    return false;
  }

  JIT_STAT_INC(native_alu_ops);
  return emit_capture_status_flags_if_live(w,
      jit_native_alu_flag_copy_mask(insn->alu_op), insn->next_pc);
}

static bool emit_native_alu_imm_reg(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  if ((insn->width != X86_WIDTH_BYTE && insn->width != X86_WIDTH_WORD &&
          insn->width != X86_WIDTH_DWORD) ||
      (insn->width == X86_WIDTH_BYTE && !jit_native_low_byte_reg(insn->dst))) {
    return false;
  }

  if (!emit_load_reg_to_eax_width(w, insn->dst, insn->width) ||
      (jit_alu_reads_carry(insn->alu_op) &&
          !emit_guest_cf_to_host_cf_r11(w)) ||
      !emit_alu_eax_imm_width(w, insn->alu_op, insn->width, insn->imm)) {
    return false;
  }

  if (jit_native_alu_writes_result(insn->alu_op) &&
      !emit_store_reg_eax_width(w, insn->dst, insn->width)) {
    return false;
  }

  JIT_STAT_INC(native_alu_ops);
  return emit_capture_status_flags_if_live(w,
      jit_native_alu_flag_copy_mask(insn->alu_op), insn->next_pc);
}

static bool emit_native_cdq(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  if (insn->width != X86_WIDTH_DWORD) return false;

  return emit_load_reg_eax(w, R_EAX) &&
         emit_cdq_host(w) &&
         emit_mov_eax_edx(w) &&
         emit_store_reg_eax(w, R_EDX);
}

static bool emit_native_test_reg_reg(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  if (!emit_load_reg_eax(w, insn->dst) ||
      !emit_load_reg_ecx(w, insn->src) ||
      !emit_test_eax_ecx(w)) {
    return false;
  }

  JIT_STAT_INC(native_alu_ops);
  return emit_capture_status_flags_if_live(w, X86_EFLAGS_LOGIC_COPY_MASK,
      insn->next_pc);
}

static bool emit_native_test_eax_imm(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  if (!emit_load_reg_eax(w, R_EAX) ||
      !emit_test_eax_imm_width(w, insn->width, insn->imm)) {
    return false;
  }

  JIT_STAT_INC(native_alu_ops);
  return emit_capture_status_flags_if_live(w, X86_EFLAGS_LOGIC_COPY_MASK,
      insn->next_pc);
}

static bool emit_native_double_shift_reg_imm(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  const uint8_t count = insn->imm & X86_SHIFT_COUNT_MASK;

  if (insn->width != X86_WIDTH_DWORD || insn->count_from_cl) return false;
  if (count == 0) {
    JIT_STAT_INC(native_shift_ops);
    return true;
  }

  uint32_t copy_mask = X86_FLAG_CF | X86_FLAG_PF | X86_FLAG_ZF | X86_FLAG_SF;
  if (count == 1) copy_mask |= X86_FLAG_OF;

  if (!emit_load_reg_eax(w, insn->dst) ||
      !emit_load_reg_ecx(w, insn->src) ||
      !emit_double_shift_eax_ecx_imm(w, insn->alu_op != 0, count) ||
      !emit_store_reg_eax(w, insn->dst)) {
    return false;
  }

  JIT_STAT_INC(native_shift_ops);
  return emit_capture_status_flags_custom(w, copy_mask, 0);
}

static bool emit_flag_producer_no_capture(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  switch (insn->op) {
    case X86_JIT_OP_ALU_REG_REG:
      if (!emit_load_reg_eax(w, insn->dst) ||
          !emit_load_reg_ecx(w, insn->src) ||
          (jit_alu_reads_carry(insn->alu_op) &&
              !emit_guest_cf_to_host_cf_r11(w)) ||
          !emit_alu_rm32_r32(w, insn->alu_op, R_EAX, R_ECX)) {
        return false;
      }
      if (jit_native_alu_writes_result(insn->alu_op) &&
          !emit_store_reg_eax(w, insn->dst)) {
        return false;
      }
      JIT_STAT_INC(native_alu_ops);
      return true;
    case X86_JIT_OP_ALU_IMM_REG:
      if (!emit_load_reg_eax(w, insn->dst) ||
          (jit_alu_reads_carry(insn->alu_op) &&
              !emit_guest_cf_to_host_cf_r11(w)) ||
          !emit_alu_eax_imm32(w, insn->alu_op, insn->imm)) {
        return false;
      }
      if (jit_native_alu_writes_result(insn->alu_op) &&
          !emit_store_reg_eax(w, insn->dst)) {
        return false;
      }
      JIT_STAT_INC(native_alu_ops);
      return true;
    case X86_JIT_OP_TEST_REG_REG:
      if (!emit_load_reg_eax(w, insn->dst) ||
          !emit_load_reg_ecx(w, insn->src) ||
          !emit_test_eax_ecx(w)) {
        return false;
      }
      JIT_STAT_INC(native_alu_ops);
      return true;
    case X86_JIT_OP_TEST_EAX_IMM:
      if (!emit_load_reg_eax(w, R_EAX) ||
          !emit_test_eax_imm_width(w, insn->width, insn->imm)) {
        return false;
      }
      JIT_STAT_INC(native_alu_ops);
      return true;
    default:
      return false;
  }
}

static bool emit_flag_producer_no_capture_regcached(x86_jit_writer_t *w,
    x86_jit_emit_ctx_t *ctx, const x86_jit_insn_t *insn) {
  uint8_t dst_host = 0;
  uint8_t src_host = 0;

  if (!jit_regcache_active(ctx) || !jit_is_fusible_flag_producer(insn)) {
    return jit_regcache_flush_all(w, ctx) &&
           emit_flag_producer_no_capture(w, insn);
  }

  switch (insn->op) {
    case X86_JIT_OP_ALU_REG_REG:
      if (!jit_regcache_get_read(w, ctx, insn->dst, &dst_host, 0) ||
          !jit_regcache_get_read(w, ctx, insn->src, &src_host,
              (uint16_t)(1u << dst_host)) ||
          !emit_alu_host_host(w, insn->alu_op, dst_host, src_host)) {
        return false;
      }
      if (jit_native_alu_writes_result(insn->alu_op)) {
        jit_regcache_mark_dirty(ctx, insn->dst);
      }
      JIT_STAT_INC(native_alu_ops);
      return true;
    case X86_JIT_OP_ALU_IMM_REG:
      if (!jit_regcache_get_read(w, ctx, insn->dst, &dst_host, 0) ||
          !emit_alu_host_imm32(w, insn->alu_op, dst_host, insn->imm)) {
        return false;
      }
      if (jit_native_alu_writes_result(insn->alu_op)) {
        jit_regcache_mark_dirty(ctx, insn->dst);
      }
      JIT_STAT_INC(native_alu_ops);
      return true;
    case X86_JIT_OP_TEST_REG_REG:
      if (!jit_regcache_get_read(w, ctx, insn->dst, &dst_host, 0) ||
          !jit_regcache_get_read(w, ctx, insn->src, &src_host,
              (uint16_t)(1u << dst_host)) ||
          !emit_test_host_host(w, dst_host, src_host)) {
        return false;
      }
      JIT_STAT_INC(native_alu_ops);
      return true;
    case X86_JIT_OP_TEST_EAX_IMM:
      return jit_regcache_flush_all(w, ctx) &&
             emit_flag_producer_no_capture(w, insn);
    default:
      return false;
  }
}

static bool emit_fused_flag_producer_jcc(x86_jit_writer_t *w,
    const x86_jit_insn_t *producer, const x86_jit_insn_t *jcc,
    uint32_t count) {
  uint8_t *taken_disp = NULL;
  const uint32_t copy_mask = jit_flag_producer_copy_mask(producer);

  if (!jit_is_fusible_flag_producer(producer) || !jit_is_native_jcc(jcc)) {
    return false;
  }

  if (!emit_flag_producer_no_capture(w, producer) ||
      !emit_jcc_rel32_placeholder(w, jcc->cc, &taken_disp) ||
      !emit_capture_status_flags_if_live(w, copy_mask, jcc->next_pc) ||
      !emit_store_pc_imm(w, jcc->next_pc) ||
      !emit_ret_count(w, count)) {
    return false;
  }

  uint8_t *taken_native = w->cur;
  if (!patch_rel32(taken_disp, taken_native) ||
      !emit_capture_status_flags_if_live(w, copy_mask, jit_branch_target(jcc)) ||
      !emit_store_pc_imm(w, jit_branch_target(jcc)) ||
      !emit_ret_count(w, count)) {
    return false;
  }

  JIT_STAT_INC(native_alu_jcc_fusions);
  return true;
}

static bool emit_fused_jcc_edge_exit(x86_jit_writer_t *w,
    x86_jit_block_t *block, vaddr_t target_pc, uint32_t count,
    x86_jit_exit_kind_t kind, bool can_chain, uint32_t copy_mask) {
  if (!emit_capture_status_flags_if_live(w, copy_mask, target_pc)) {
    return false;
  }
  if (can_chain) {
    return emit_chain_exit(w, block, target_pc, count, kind,
        X86_JIT_CHAIN_SLOW_UNLINKED);
  }

  return emit_store_pc_imm(w, target_pc) &&
         emit_ret_count_side_exit(w, count,
             X86_JIT_CHAIN_SLOW_UNACCEPTED_SUCCESSOR);
}

static bool emit_fused_flag_producer_jcc_per_edge(x86_jit_writer_t *w,
    x86_jit_block_t *block, const x86_jit_insn_t *producer,
    const x86_jit_insn_t *jcc, uint32_t count,
    bool fallthrough_can_chain, bool taken_can_chain) {
  uint8_t *taken_disp = NULL;
  const uint32_t copy_mask = jit_flag_producer_copy_mask(producer);

  if (!jit_is_fusible_flag_producer(producer) || !jit_is_native_jcc(jcc)) {
    return false;
  }

  if (!emit_flag_producer_no_capture(w, producer) ||
      !emit_jcc_rel32_placeholder(w, jcc->cc, &taken_disp) ||
      !emit_fused_jcc_edge_exit(w, block, jcc->next_pc, count,
          X86_JIT_EXIT_FALLTHROUGH, fallthrough_can_chain, copy_mask)) {
    return false;
  }

  uint8_t *taken_native = w->cur;
  if (!patch_rel32(taken_disp, taken_native) ||
      !emit_fused_jcc_edge_exit(w, block, jit_branch_target(jcc), count,
          X86_JIT_EXIT_TAKEN, taken_can_chain, copy_mask)) {
    return false;
  }

  JIT_STAT_INC(native_alu_jcc_fusions);
  return true;
}

static bool emit_fused_flag_producer_jcc_resident_backedge(
    x86_jit_writer_t *w, const x86_jit_insn_t *producer,
    const x86_jit_insn_t *jcc, uint32_t count) {
  uint8_t *taken_disp = NULL;
  uint8_t *budget_exit_disp = NULL;
  uint8_t *loop_disp = NULL;
  const uint32_t copy_mask = jit_flag_producer_copy_mask(producer);

  if (count != 2u || !jit_is_fusible_flag_producer(producer) ||
      !jit_is_native_jcc(jcc)) {
    return false;
  }

  if (!((jit_fast_chain_runtime_enabled() ?
          emit_mov_r11d_esi(w) :
          (emit_load_loop_extra_eax(w) && emit_mov_r11d_eax(w)))) ||
      !emit_resident_lap_budget_r10d(w, count)) {
    return false;
  }

  const uint8_t *loop_native = w->cur;
  if (!emit_flag_producer_no_capture(w, producer) ||
      !emit_jcc_rel32_placeholder(w, jcc->cc, &taken_disp) ||
      !emit_capture_status_flags_if_live(w, copy_mask, jcc->next_pc) ||
      !emit_store_pc_imm(w, jcc->next_pc) ||
      !emit_return_r11_plus_imm(w, count)) {
    return false;
  }

  uint8_t *taken_native = w->cur;
  if (!patch_rel32(taken_disp, taken_native) ||
      !emit_lea_r11d_r11d_disp8(w, (int8_t)count) ||
      !emit_lea_r10d_r10d_disp8(w, -1) ||
      !emit_mov_ecx_r10d(w) ||
      !emit_jrcxz_rel8_placeholder(w, &budget_exit_disp) ||
      !emit_jmp_rel32_placeholder(w, &loop_disp)) {
    return false;
  }

  uint8_t *budget_exit_native = w->cur;
  if (!patch_rel8(budget_exit_disp, budget_exit_native) ||
      !patch_rel32(loop_disp, loop_native) ||
      !emit_capture_status_flags_if_live(w, copy_mask, jit_branch_target(jcc)) ||
      !emit_store_pc_imm(w, jit_branch_target(jcc)) ||
      !emit_return_r11(w)) {
    return false;
  }

  JIT_STAT_INC(native_alu_jcc_fusions);
  JIT_STAT_INC(native_alu_jcc_resident_loops);
  return true;
}

static bool emit_fused_flag_producer_jcc_backedge(x86_jit_writer_t *w,
    const x86_jit_insn_t *producer, const x86_jit_insn_t *jcc,
    const uint8_t *native_target, uint32_t count) {
  uint8_t *taken_disp = NULL;
  const uint32_t copy_mask = jit_flag_producer_copy_mask(producer);

  if (!jit_is_fusible_flag_producer(producer) || !jit_is_native_jcc(jcc)) {
    return false;
  }

  if (!emit_flag_producer_no_capture(w, producer) ||
      !emit_jcc_rel32_placeholder(w, jcc->cc, &taken_disp) ||
      !emit_capture_status_flags_if_live(w, copy_mask, jcc->next_pc) ||
      !emit_return_completed(w, jcc->next_pc, count)) {
    return false;
  }

  uint8_t *taken_native = w->cur;
  if (!patch_rel32(taken_disp, taken_native) ||
      !emit_capture_status_flags_if_live(w, copy_mask, jit_branch_target(jcc)) ||
      !emit_backedge_loop_accounting(w, jcc, native_target, count)) {
    return false;
  }

  JIT_STAT_INC(native_alu_jcc_fusions);
  return true;
}

static bool emit_native_alu_rm_reg(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *pmem_slow_disp = NULL;
  uint8_t *cross_page_slow_disp = NULL;
  uint8_t *source_page_slow_disp = NULL;
  uint8_t *done_disp = NULL;
  const bool writes_result = jit_native_alu_writes_result(insn->alu_op);
  const uint8_t width = insn->width;

  if (width != X86_WIDTH_BYTE && width != X86_WIDTH_WORD &&
      width != X86_WIDTH_DWORD) {
    return false;
  }

  if (insn->rm_is_reg) {
    if (width == X86_WIDTH_BYTE) {
      if (!emit_load_byte_reg_to_eax(w, insn->src) ||
          !emit_mov_r11d_eax(w) ||
          !emit_load_byte_reg_to_eax(w, insn->rm_reg) ||
          (jit_alu_reads_carry(insn->alu_op) &&
              !emit_guest_cf_to_host_cf_ecx(w)) ||
          !emit_alu_eax_r11_width(w, insn->alu_op, width)) {
        return false;
      }
    }
    else if (!emit_load_reg_r11d(w, insn->src) ||
        !emit_load_reg_eax(w, insn->rm_reg) ||
        (jit_alu_reads_carry(insn->alu_op) &&
            !emit_guest_cf_to_host_cf_ecx(w)) ||
        !emit_alu_eax_r11_width(w, insn->alu_op, width)) {
      return false;
    }

    if (writes_result &&
        !(width == X86_WIDTH_BYTE ?
            emit_store_al_to_byte_reg(w, insn->rm_reg) :
            emit_store_reg_eax(w, insn->rm_reg))) {
      return false;
    }

    JIT_STAT_INC(native_alu_ops);
    return emit_capture_status_flags(w,
        jit_native_alu_flag_copy_mask(insn->alu_op));
  }

  if (!emit_guest_ea_eax(w, &insn->ea) ||
      !emit_mov_edx_eax(w) ||
      !emit_direct_pmem_guard_edx(w, width, &pmem_slow_disp)) {
    return false;
  }

  if (writes_result &&
      !emit_direct_store_source_guard_edx(w, width,
          &cross_page_slow_disp, &source_page_slow_disp)) {
    return false;
  }

  if ((jit_alu_reads_carry(insn->alu_op) &&
          !emit_guest_cf_to_host_cf_r11(w)) ||
      !emit_movabs_r10(w, (uint64_t)(uintptr_t)guest_to_host(CONFIG_MBASE)) ||
      !emit_load_pmem_eax_width(w, width)) {
    return false;
  }
  if (width == X86_WIDTH_BYTE && insn->src >= 4u) {
    if (!emit_mov_ecx_eax(w) ||
        !emit_load_byte_reg_to_eax(w, insn->src) ||
        !emit_mov_r11d_eax(w) ||
        !emit_mov_eax_ecx(w)) {
      return false;
    }
  }
  else if (!emit_load_reg_r11d(w, insn->src)) {
    return false;
  }
  if (!emit_alu_eax_r11_width(w, insn->alu_op, width)) return false;

  if (writes_result && !emit_store_pmem_eax_width(w, width)) return false;

  if (!emit_capture_status_flags(w,
          jit_native_alu_flag_copy_mask(insn->alu_op)) ||
      !emit_jmp_rel32_placeholder(w, &done_disp)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(pmem_slow_disp, slow_native) ||
      (writes_result && !patch_rel32(cross_page_slow_disp, slow_native)) ||
      (writes_result && !patch_rel32(source_page_slow_disp, slow_native)) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  JIT_STAT_INC(native_alu_ops);
  JIT_STAT_INC(native_pmem_loads);
  if (writes_result) JIT_STAT_INC(native_pmem_stores);
  return patch_rel32(done_disp, w->cur);
}

static bool emit_native_alu_imm_rm(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *pmem_slow_disp = NULL;
  uint8_t *cross_page_slow_disp = NULL;
  uint8_t *source_page_slow_disp = NULL;
  uint8_t *done_disp = NULL;
  const bool writes_result = jit_native_alu_writes_result(insn->alu_op);
  const uint8_t width = insn->width;

  if (width != X86_WIDTH_BYTE && width != X86_WIDTH_WORD &&
      width != X86_WIDTH_DWORD) {
    return false;
  }

  if (insn->rm_is_reg) {
    if (width == X86_WIDTH_BYTE) {
      if (!emit_load_byte_reg_to_eax(w, insn->rm_reg) ||
          (jit_alu_reads_carry(insn->alu_op) &&
              !emit_guest_cf_to_host_cf_r11(w)) ||
          !emit_alu_eax_imm_width(w, insn->alu_op, width, insn->imm)) {
        return false;
      }
      if (writes_result &&
          !emit_store_al_to_byte_reg(w, insn->rm_reg)) {
        return false;
      }
    }
    else if (!emit_load_reg_eax(w, insn->rm_reg) ||
        (jit_alu_reads_carry(insn->alu_op) &&
            !emit_guest_cf_to_host_cf_r11(w)) ||
        !emit_alu_eax_imm_width(w, insn->alu_op, width, insn->imm)) {
      return false;
    }
    else if (writes_result && !emit_store_reg_eax(w, insn->rm_reg)) {
      return false;
    }

    JIT_STAT_INC(native_alu_ops);
    return emit_capture_status_flags(w,
        jit_native_alu_flag_copy_mask(insn->alu_op));
  }

  if (!emit_guest_ea_eax(w, &insn->ea) ||
      !emit_mov_edx_eax(w) ||
      !emit_direct_pmem_guard_edx(w, width, &pmem_slow_disp)) {
    return false;
  }

  if (writes_result &&
      !emit_direct_store_source_guard_edx(w, width,
          &cross_page_slow_disp, &source_page_slow_disp)) {
    return false;
  }

  if ((jit_alu_reads_carry(insn->alu_op) &&
          !emit_guest_cf_to_host_cf_r11(w)) ||
      !emit_movabs_r10(w, (uint64_t)(uintptr_t)guest_to_host(CONFIG_MBASE)) ||
      !emit_load_pmem_eax_width(w, width) ||
      !emit_alu_eax_imm_width(w, insn->alu_op, width, insn->imm)) {
    return false;
  }

  if (writes_result && !emit_store_pmem_eax_width(w, width)) return false;

  if (!emit_capture_status_flags(w,
          jit_native_alu_flag_copy_mask(insn->alu_op)) ||
      !emit_jmp_rel32_placeholder(w, &done_disp)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(pmem_slow_disp, slow_native) ||
      (writes_result && !patch_rel32(cross_page_slow_disp, slow_native)) ||
      (writes_result && !patch_rel32(source_page_slow_disp, slow_native)) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  JIT_STAT_INC(native_alu_ops);
  JIT_STAT_INC(native_pmem_loads);
  if (writes_result) JIT_STAT_INC(native_pmem_stores);
  return patch_rel32(done_disp, w->cur);
}

static bool emit_native_test_rm_reg(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *slow_disp = NULL;
  uint8_t *done_disp = NULL;
  const uint8_t width = insn->width;

  if (width != X86_WIDTH_BYTE && width != X86_WIDTH_WORD &&
      width != X86_WIDTH_DWORD) {
    return false;
  }

  if (insn->rm_is_reg) {
    if (width == X86_WIDTH_BYTE) {
      if (!emit_load_byte_reg_to_eax(w, insn->src) ||
          !emit_mov_ecx_eax(w) ||
          !emit_load_byte_reg_to_eax(w, insn->rm_reg) ||
          !emit_test_eax_ecx_width(w, width)) {
        return false;
      }
    }
    else if (!emit_load_reg_eax(w, insn->rm_reg) ||
        !emit_load_reg_ecx(w, insn->src) ||
        !emit_test_eax_ecx_width(w, width)) {
      return false;
    }

    JIT_STAT_INC(native_alu_ops);
    return emit_capture_status_flags(w, X86_EFLAGS_LOGIC_COPY_MASK);
  }

  if (!emit_guest_ea_eax(w, &insn->ea) ||
      !emit_mov_edx_eax(w) ||
      !emit_direct_pmem_guard_edx(w, width, &slow_disp) ||
      !emit_movabs_r10(w, (uint64_t)(uintptr_t)guest_to_host(CONFIG_MBASE)) ||
      !emit_load_pmem_eax_width(w, width)) {
    return false;
  }
  if (width == X86_WIDTH_BYTE && insn->src >= 4u) {
    if (!emit_mov_r11d_eax(w) ||
        !emit_load_byte_reg_to_eax(w, insn->src) ||
        !emit_mov_ecx_eax(w) ||
        !emit_mov_eax_r11d(w)) {
      return false;
    }
  }
  else if (!emit_load_reg_ecx(w, insn->src)) {
    return false;
  }
  if (!emit_test_eax_ecx_width(w, width) ||
      !emit_capture_status_flags(w, X86_EFLAGS_LOGIC_COPY_MASK) ||
      !emit_jmp_rel32_placeholder(w, &done_disp)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(slow_disp, slow_native) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  JIT_STAT_INC(native_alu_ops);
  JIT_STAT_INC(native_pmem_loads);
  return patch_rel32(done_disp, w->cur);
}

static bool emit_native_alu_reg_rm(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *slow_disp = NULL;
  uint8_t *done_disp = NULL;
  const bool writes_result = jit_native_alu_writes_result(insn->alu_op);
  const uint8_t width = insn->width;

  if (width != X86_WIDTH_BYTE && width != X86_WIDTH_WORD &&
      width != X86_WIDTH_DWORD) {
    return false;
  }

  if (insn->rm_is_reg) {
    if (width == X86_WIDTH_BYTE) {
      if (!emit_load_byte_reg_to_eax(w, insn->rm_reg) ||
          !emit_mov_ecx_eax(w) ||
          !emit_load_byte_reg_to_eax(w, insn->dst) ||
          (jit_alu_reads_carry(insn->alu_op) &&
              !emit_guest_cf_to_host_cf_r11(w)) ||
          !emit_alu_eax_ecx_width(w, insn->alu_op, width)) {
        return false;
      }
    }
    else if (!emit_load_reg_eax(w, insn->dst) ||
        !emit_load_reg_ecx(w, insn->rm_reg) ||
        (jit_alu_reads_carry(insn->alu_op) &&
            !emit_guest_cf_to_host_cf_r11(w)) ||
        !emit_alu_eax_ecx_width(w, insn->alu_op, width)) {
      return false;
    }
  }
  else {
    if (!emit_guest_ea_eax(w, &insn->ea) ||
        !emit_mov_edx_eax(w) ||
        !emit_direct_pmem_guard_edx(w, width, &slow_disp) ||
        !emit_movabs_r10(w, (uint64_t)(uintptr_t)guest_to_host(CONFIG_MBASE)) ||
        !emit_load_pmem_ecx_width(w, width) ||
        !(width == X86_WIDTH_BYTE ?
            emit_load_byte_reg_to_eax(w, insn->dst) :
            emit_load_reg_eax(w, insn->dst)) ||
        (jit_alu_reads_carry(insn->alu_op) &&
            !emit_guest_cf_to_host_cf_r11(w)) ||
        !emit_alu_eax_ecx_width(w, insn->alu_op, width)) {
      return false;
    }
  }

  if (writes_result &&
      !(width == X86_WIDTH_BYTE ?
          emit_store_al_to_byte_reg(w, insn->dst) :
          emit_store_reg_eax(w, insn->dst))) {
    return false;
  }

  if (!emit_capture_status_flags(w,
          jit_native_alu_flag_copy_mask(insn->alu_op))) {
    return false;
  }

  if (!insn->rm_is_reg) {
    if (!emit_jmp_rel32_placeholder(w, &done_disp)) return false;
    uint8_t *slow_native = w->cur;
    if (!patch_rel32(slow_disp, slow_native) ||
        !emit_helper_call(w, insn)) {
      return false;
    }
  }

  JIT_STAT_INC(native_alu_ops);
  if (!insn->rm_is_reg) JIT_STAT_INC(native_pmem_loads);
  return done_disp == NULL || patch_rel32(done_disp, w->cur);
}

static bool emit_native_mov_reg_rm_load(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *slow_disp = NULL;
  uint8_t *done_disp = NULL;
  const uint8_t width = insn->width;

  if (width != X86_WIDTH_BYTE && width != X86_WIDTH_WORD &&
      width != X86_WIDTH_DWORD) {
    return false;
  }
  if (width == X86_WIDTH_BYTE &&
      (!jit_native_low_byte_reg(insn->dst) ||
          (insn->rm_is_reg && !jit_native_low_byte_reg(insn->rm_reg)))) {
    return false;
  }

  if (insn->rm_is_reg) {
    if (!emit_load_reg_eax(w, insn->rm_reg) ||
        !emit_store_reg_eax_width(w, insn->dst, width)) {
      return false;
    }
    return true;
  }

  if (!emit_guest_ea_eax(w, &insn->ea) ||
      !emit_mov_edx_eax(w) ||
      !emit_direct_pmem_guard_edx(w, width, &slow_disp) ||
      !emit_movabs_r10(w, (uint64_t)(uintptr_t)guest_to_host(CONFIG_MBASE)) ||
      !emit_load_pmem_eax_width(w, width) ||
      !emit_store_reg_eax_width(w, insn->dst, width) ||
      !emit_jmp_rel32_placeholder(w, &done_disp)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(slow_disp, slow_native) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  JIT_STAT_INC(native_pmem_loads);
  return patch_rel32(done_disp, w->cur);
}

static bool emit_native_mov_rm_reg_store(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *pmem_slow_disp = NULL;
  uint8_t *cross_page_slow_disp = NULL;
  uint8_t *source_page_slow_disp = NULL;
  uint8_t *done_disp = NULL;
  const uint8_t width = insn->width;

  if (width != X86_WIDTH_BYTE && width != X86_WIDTH_WORD &&
      width != X86_WIDTH_DWORD) {
    return false;
  }
  if (width == X86_WIDTH_BYTE && insn->src >= 8u) return false;

  if (insn->rm_is_reg) {
    if (width == X86_WIDTH_BYTE) {
      return emit_load_byte_reg_to_eax(w, insn->src) &&
             emit_store_al_to_byte_reg(w, insn->rm_reg);
    }

    if (!emit_load_reg_eax(w, insn->src) ||
        !emit_store_reg_eax_width(w, insn->rm_reg, width)) {
      return false;
    }
    return true;
  }

  if (!emit_guest_ea_eax(w, &insn->ea) ||
      !emit_mov_edx_eax(w) ||
      !emit_direct_pmem_guard_edx(w, width, &pmem_slow_disp) ||
      !emit_direct_store_source_guard_edx(w, width,
          &cross_page_slow_disp, &source_page_slow_disp)) {
    return false;
  }

  if (width == X86_WIDTH_BYTE) {
    if (!emit_load_byte_reg_to_eax(w, insn->src)) return false;
  }
  else if (!emit_load_reg_r11d(w, insn->src) ||
      !emit_mov_eax_r11d(w)) {
    return false;
  }

  if (!emit_movabs_r10(w, (uint64_t)(uintptr_t)guest_to_host(CONFIG_MBASE)) ||
      !emit_store_pmem_eax_width(w, width) ||
      !emit_jmp_rel32_placeholder(w, &done_disp)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(pmem_slow_disp, slow_native) ||
      !patch_rel32(cross_page_slow_disp, slow_native) ||
      !patch_rel32(source_page_slow_disp, slow_native)) {
    return false;
  }
  if (jit_stats_enabled &&
      !emit_runtime_counter_inc(w, &jit_mov_rm_reg_slow_exits_runtime)) {
    return false;
  }
  if (!emit_helper_call(w, insn)) {
    return false;
  }

  JIT_STAT_INC(native_pmem_stores);
  return patch_rel32(done_disp, w->cur);
}

static bool emit_native_mov_imm_rm(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *pmem_slow_disp = NULL;
  uint8_t *cross_page_slow_disp = NULL;
  uint8_t *source_page_slow_disp = NULL;
  uint8_t *done_disp = NULL;
  const uint8_t width = insn->width;

  if (width != X86_WIDTH_BYTE && width != X86_WIDTH_WORD &&
      width != X86_WIDTH_DWORD) {
    return false;
  }

  if (insn->rm_is_reg) {
    return emit_store_reg_imm_width(w, insn->rm_reg, width, insn->imm);
  }

  if (!emit_guest_ea_eax(w, &insn->ea) ||
      !emit_mov_edx_eax(w) ||
      !emit_direct_pmem_guard_edx(w, width, &pmem_slow_disp) ||
      !emit_direct_store_source_guard_edx(w, width,
          &cross_page_slow_disp, &source_page_slow_disp) ||
      !emit_movabs_r10(w, (uint64_t)(uintptr_t)guest_to_host(CONFIG_MBASE)) ||
      !emit_mov_eax_imm32(w, insn->imm) ||
      !emit_store_pmem_eax_width(w, width) ||
      !emit_jmp_rel32_placeholder(w, &done_disp)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(pmem_slow_disp, slow_native) ||
      !patch_rel32(cross_page_slow_disp, slow_native) ||
      !patch_rel32(source_page_slow_disp, slow_native) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  JIT_STAT_INC(native_pmem_stores);
  return patch_rel32(done_disp, w->cur);
}

static bool emit_native_mov_eax_moffs(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *slow_disp = NULL;
  uint8_t *done_disp = NULL;
  const uint8_t width = insn->width;

  if (width != X86_WIDTH_BYTE && width != X86_WIDTH_WORD &&
      width != X86_WIDTH_DWORD) {
    return false;
  }

  if (!emit_mov_eax_imm32(w, insn->imm) ||
      !emit_mov_edx_eax(w) ||
      !emit_direct_pmem_guard_edx(w, width, &slow_disp) ||
      !emit_movabs_r10(w, (uint64_t)(uintptr_t)guest_to_host(CONFIG_MBASE))) {
    return false;
  }

  if (width == X86_WIDTH_DWORD) {
    if (!emit_mov_eax_m32_r10_rdx(w) ||
        !emit_store_reg_eax(w, R_EAX)) {
      return false;
    }
  }
  else {
    const uint32_t keep_mask = width == X86_WIDTH_BYTE ?
        0xffffff00u : 0xffff0000u;
    if (!emit_load_pmem_ecx_width(w, width) ||
        !emit_load_reg_eax(w, R_EAX) ||
        !emit_alu_eax_imm32(w, X86_ALU_AND, keep_mask) ||
        !emit_or_eax_ecx(w) ||
        !emit_store_reg_eax(w, R_EAX)) {
      return false;
    }
  }

  if (!emit_jmp_rel32_placeholder(w, &done_disp)) return false;

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(slow_disp, slow_native) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  JIT_STAT_INC(native_pmem_loads);
  return patch_rel32(done_disp, w->cur);
}

static bool emit_native_mov_moffs_eax(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *pmem_slow_disp = NULL;
  uint8_t *cross_page_slow_disp = NULL;
  uint8_t *source_page_slow_disp = NULL;
  uint8_t *done_disp = NULL;
  const uint8_t width = insn->width;

  if (width != X86_WIDTH_BYTE && width != X86_WIDTH_WORD &&
      width != X86_WIDTH_DWORD) {
    return false;
  }

  if (!emit_load_reg_r11d(w, R_EAX) ||
      !emit_mov_eax_imm32(w, insn->imm) ||
      !emit_mov_edx_eax(w) ||
      !emit_direct_pmem_guard_edx(w, width, &pmem_slow_disp) ||
      !emit_direct_store_source_guard_edx(w, width,
          &cross_page_slow_disp, &source_page_slow_disp) ||
      !emit_movabs_r10(w, (uint64_t)(uintptr_t)guest_to_host(CONFIG_MBASE)) ||
      !emit_mov_eax_r11d(w) ||
      !emit_store_pmem_eax_width(w, width) ||
      !emit_jmp_rel32_placeholder(w, &done_disp)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(pmem_slow_disp, slow_native) ||
      !patch_rel32(cross_page_slow_disp, slow_native) ||
      !patch_rel32(source_page_slow_disp, slow_native) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  JIT_STAT_INC(native_pmem_stores);
  return patch_rel32(done_disp, w->cur);
}

static bool emit_native_push_reg(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *pmem_slow_disp = NULL;
  uint8_t *cross_page_slow_disp = NULL;
  uint8_t *source_page_slow_disp = NULL;
  uint8_t *done_disp = NULL;

  if (insn->width != X86_WIDTH_DWORD) return false;

  if (!emit_load_reg_eax(w, R_ESP) ||
      !emit_add_eax_imm32(w, 0u - X86_WIDTH_DWORD) ||
      !emit_mov_edx_eax(w) ||
      !emit_direct_pmem_guard_edx(w, X86_WIDTH_DWORD, &pmem_slow_disp) ||
      !emit_direct_store_source_guard_edx(w, X86_WIDTH_DWORD,
          &cross_page_slow_disp, &source_page_slow_disp) ||
      !emit_load_reg_r11d(w, insn->src) ||
      !emit_stack_store_dword_r11d(w) ||
      !emit_store_reg_eax(w, R_ESP) ||
      !emit_jmp_rel32_placeholder(w, &done_disp)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(pmem_slow_disp, slow_native) ||
      !patch_rel32(cross_page_slow_disp, slow_native) ||
      !patch_rel32(source_page_slow_disp, slow_native) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  JIT_STAT_INC(native_pmem_stores);
  return patch_rel32(done_disp, w->cur);
}

static bool emit_native_push_reg_stack_guarded(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  if (insn->width != X86_WIDTH_DWORD) return false;

  if (!emit_load_reg_eax(w, R_ESP) ||
      !emit_add_eax_imm32(w, 0u - X86_WIDTH_DWORD) ||
      !emit_mov_edx_eax(w) ||
      !emit_load_reg_r11d(w, insn->src) ||
      !emit_stack_store_dword_r11d(w) ||
      !emit_store_reg_eax(w, R_ESP)) {
    return false;
  }

  JIT_STAT_INC(native_pmem_stores);
  return true;
}

static bool emit_native_pop_reg(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *slow_disp = NULL;
  uint8_t *done_disp = NULL;

  if (insn->width != X86_WIDTH_DWORD) return false;

  if (!emit_load_reg_eax(w, R_ESP) ||
      !emit_mov_r11d_eax(w) ||
      !emit_mov_edx_eax(w) ||
      !emit_direct_pmem_guard_edx(w, X86_WIDTH_DWORD, &slow_disp) ||
      !emit_stack_load_dword_eax(w) ||
      !emit_store_reg_eax(w, insn->dst)) {
    return false;
  }

  if (insn->dst != R_ESP &&
      (!emit_mov_eax_r11d(w) ||
          !emit_add_eax_imm32(w, X86_WIDTH_DWORD) ||
          !emit_store_reg_eax(w, R_ESP))) {
    return false;
  }

  if (!emit_jmp_rel32_placeholder(w, &done_disp)) return false;

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(slow_disp, slow_native) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  JIT_STAT_INC(native_pmem_loads);
  return patch_rel32(done_disp, w->cur);
}

static bool emit_native_pop_reg_stack_guarded(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  if (insn->width != X86_WIDTH_DWORD || insn->dst == R_ESP) return false;

  if (!emit_load_reg_eax(w, R_ESP) ||
      !emit_mov_r11d_eax(w) ||
      !emit_mov_edx_eax(w) ||
      !emit_stack_load_dword_eax(w) ||
      !emit_store_reg_eax(w, insn->dst) ||
      !emit_mov_eax_r11d(w) ||
      !emit_add_eax_imm32(w, X86_WIDTH_DWORD) ||
      !emit_store_reg_eax(w, R_ESP)) {
    return false;
  }

  JIT_STAT_INC(native_pmem_loads);
  return true;
}

static bool emit_native_call_rel(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *pmem_slow_disp = NULL;
  uint8_t *cross_page_slow_disp = NULL;
  uint8_t *source_page_slow_disp = NULL;
  uint8_t *done_disp = NULL;

  if (insn->width != X86_WIDTH_DWORD) return false;

  if (!emit_load_reg_eax(w, R_ESP) ||
      !emit_add_eax_imm32(w, 0u - X86_WIDTH_DWORD) ||
      !emit_mov_edx_eax(w) ||
      !emit_direct_pmem_guard_edx(w, X86_WIDTH_DWORD, &pmem_slow_disp) ||
      !emit_direct_store_source_guard_edx(w, X86_WIDTH_DWORD,
          &cross_page_slow_disp, &source_page_slow_disp) ||
      !emit_mov_r11d_imm32(w, insn->next_pc) ||
      !emit_stack_store_dword_r11d(w) ||
      !emit_store_reg_eax(w, R_ESP) ||
      !emit_store_pc_imm(w, jit_branch_target(insn)) ||
      !emit_jmp_rel32_placeholder(w, &done_disp)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(pmem_slow_disp, slow_native) ||
      !patch_rel32(cross_page_slow_disp, slow_native) ||
      !patch_rel32(source_page_slow_disp, slow_native) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  JIT_STAT_INC(native_pmem_stores);
  return patch_rel32(done_disp, w->cur);
}

static bool emit_native_call_rel_stack_guarded(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  if (insn->width != X86_WIDTH_DWORD) return false;

  if (!emit_load_reg_eax(w, R_ESP) ||
      !emit_add_eax_imm32(w, 0u - X86_WIDTH_DWORD) ||
      !emit_mov_edx_eax(w) ||
      !emit_mov_r11d_imm32(w, insn->next_pc) ||
      !emit_stack_store_dword_r11d(w) ||
      !emit_store_reg_eax(w, R_ESP) ||
      !emit_store_pc_imm(w, jit_branch_target(insn))) {
    return false;
  }

  JIT_STAT_INC(native_pmem_stores);
  return true;
}

static bool emit_chained_call_rel(x86_jit_writer_t *w, x86_jit_block_t *block,
    const x86_jit_insn_t *insn, uint32_t count) {
  uint8_t *pmem_slow_disp = NULL;
  uint8_t *cross_page_slow_disp = NULL;
  uint8_t *source_page_slow_disp = NULL;

  if (jit_paging_enabled() || insn->width != X86_WIDTH_DWORD) return false;

  if (!emit_load_reg_eax(w, R_ESP) ||
      !emit_add_eax_imm32(w, 0u - X86_WIDTH_DWORD) ||
      !emit_mov_edx_eax(w) ||
      !emit_direct_pmem_guard_edx(w, X86_WIDTH_DWORD, &pmem_slow_disp) ||
      !emit_direct_store_source_guard_edx(w, X86_WIDTH_DWORD,
          &cross_page_slow_disp, &source_page_slow_disp) ||
      !emit_mov_r11d_imm32(w, insn->next_pc) ||
      !emit_stack_store_dword_r11d(w) ||
      !emit_store_reg_eax(w, R_ESP) ||
      !emit_chain_exit(w, block, jit_branch_target(insn), count,
          X86_JIT_EXIT_CALL, X86_JIT_CHAIN_SLOW_UNLINKED)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(pmem_slow_disp, slow_native) ||
      !patch_rel32(cross_page_slow_disp, slow_native) ||
      !patch_rel32(source_page_slow_disp, slow_native) ||
      !emit_helper_call(w, insn) ||
      !emit_ret_count_side_exit(w, count, X86_JIT_CHAIN_SLOW_HELPER)) {
    return false;
  }

  JIT_STAT_INC(native_pmem_stores);
  return true;
}

static bool emit_chained_call_rel_stack_guarded(x86_jit_writer_t *w,
    x86_jit_block_t *block, const x86_jit_insn_t *insn, uint32_t count) {
  if (insn->width != X86_WIDTH_DWORD) return false;

  if (!emit_load_reg_eax(w, R_ESP) ||
      !emit_add_eax_imm32(w, 0u - X86_WIDTH_DWORD) ||
      !emit_mov_edx_eax(w) ||
      !emit_mov_r11d_imm32(w, insn->next_pc) ||
      !emit_stack_store_dword_r11d(w) ||
      !emit_store_reg_eax(w, R_ESP) ||
      !emit_chain_exit(w, block, jit_branch_target(insn), count,
          X86_JIT_EXIT_CALL, X86_JIT_CHAIN_SLOW_UNLINKED)) {
    return false;
  }

  JIT_STAT_INC(native_pmem_stores);
  return true;
}

static bool emit_chained_paged_call_rel(x86_jit_writer_t *w,
    x86_jit_block_t *block, const x86_jit_insn_t *insn, uint32_t count) {
  uint8_t *slow_disp = NULL;

  if (!jit_paging_enabled() || insn->width != X86_WIDTH_DWORD) return false;

  /*
   * Keep the paged CALL commit order identical to emit_paged_dtlb_call_rel():
   * prove the stack write first, store the return address, then publish ESP and
   * enter the callee.  A miss or protected stack page still runs the normal
   * helper before leaving the native chain.
   */
  if (!emit_stack_push_addr_eax(w) ||
      !emit_paged_dtlb_translate_addr_eax(w, insn, X86_WIDTH_DWORD, true,
          &slow_disp) ||
      !emit_mov_r10_rax(w) ||
      !emit_mov_eax_imm32(w, insn->next_pc) ||
      !emit_store_host_ptr_r10_eax_width(w, X86_WIDTH_DWORD) ||
      !emit_commit_stack_push_esp(w) ||
      !emit_chain_exit(w, block, jit_branch_target(insn), count,
          X86_JIT_EXIT_CALL, X86_JIT_CHAIN_SLOW_UNLINKED)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(slow_disp, slow_native) ||
      !emit_helper_call(w, insn) ||
      !emit_ret_count_side_exit(w, count, X86_JIT_CHAIN_SLOW_HELPER)) {
    return false;
  }

  JIT_STAT_INC(native_pmem_stores);
  return true;
}

static bool emit_native_ret(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *slow_disp = NULL;
  uint8_t *done_disp = NULL;

  if (insn->width != X86_WIDTH_DWORD) return false;

  if (!emit_load_reg_eax(w, R_ESP) ||
      !emit_mov_r11d_eax(w) ||
      !emit_mov_edx_eax(w) ||
      !emit_direct_pmem_guard_edx(w, X86_WIDTH_DWORD, &slow_disp) ||
      !emit_stack_load_dword_eax(w) ||
      !emit_store_pc_eax(w) ||
      !emit_mov_eax_r11d(w) ||
      !emit_add_eax_imm32(w, X86_WIDTH_DWORD) ||
      !emit_store_reg_eax(w, R_ESP) ||
      !emit_jmp_rel32_placeholder(w, &done_disp)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(slow_disp, slow_native) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  JIT_STAT_INC(native_pmem_loads);
  return patch_rel32(done_disp, w->cur);
}

static bool emit_native_ret_stack_guarded(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  if (insn->width != X86_WIDTH_DWORD) return false;

  if (!emit_load_reg_eax(w, R_ESP) ||
      !emit_mov_r11d_eax(w) ||
      !emit_mov_edx_eax(w) ||
      !emit_stack_load_dword_eax(w) ||
      !emit_store_pc_eax(w) ||
      !emit_mov_eax_r11d(w) ||
      !emit_add_eax_imm32(w, X86_WIDTH_DWORD) ||
      !emit_store_reg_eax(w, R_ESP)) {
    return false;
  }

  JIT_STAT_INC(native_pmem_loads);
  return true;
}

static bool emit_chained_ret(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn, uint32_t count) {
  uint8_t *pmem_slow_disp = NULL;
  x86_jit_indirect_cache_patches_t patches;

  if (insn->width != X86_WIDTH_DWORD || !jit_fast_chain_runtime_enabled() ||
      !jit_indirect_target_cache_runtime_enabled()) {
    return false;
  }

  if (jit_paging_enabled()) {
    if (!emit_load_reg_eax(w, R_ESP) ||
        !emit_paged_dtlb_translate_addr_eax(w, insn, X86_WIDTH_DWORD, false,
            &pmem_slow_disp) ||
        !emit_load_host_ptr_rax_width(w, X86_WIDTH_DWORD) ||
        !emit_mov_edx_eax(w) ||
        !emit_load_reg_eax(w, R_ESP) ||
        !emit_add_eax_imm32(w, X86_WIDTH_DWORD) ||
        !emit_store_reg_eax(w, R_ESP) ||
        !emit_mov_eax_edx(w) ||
        !emit_indirect_target_cache_jump(w, count, &patches) ||
        !emit_indirect_target_cache_slow_exits(w, &patches)) {
      return false;
    }

    uint8_t *slow_native = w->cur;
    if (!patch_rel32(pmem_slow_disp, slow_native) ||
        !emit_helper_call(w, insn) ||
        !emit_ret_count_side_exit(w, count, X86_JIT_CHAIN_SLOW_HELPER)) {
      return false;
    }

    JIT_STAT_INC(native_pmem_loads);
    return true;
  }

  if (!emit_load_reg_eax(w, R_ESP) ||
      !emit_mov_r11d_eax(w) ||
      !emit_mov_edx_eax(w) ||
      !emit_direct_pmem_guard_edx(w, X86_WIDTH_DWORD, &pmem_slow_disp) ||
      !emit_stack_load_dword_eax(w) ||
      !emit_mov_ecx_eax(w) ||
      !emit_mov_edx_eax(w) ||
      !emit_mov_eax_r11d(w) ||
      !emit_add_eax_imm32(w, X86_WIDTH_DWORD) ||
      !emit_store_reg_eax(w, R_ESP) ||
      !emit_mov_eax_edx(w) ||
      !emit_indirect_target_cache_jump(w, count, &patches) ||
      !emit_indirect_target_cache_slow_exits(w, &patches)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(pmem_slow_disp, slow_native) ||
      !emit_helper_call(w, insn) ||
      !emit_ret_count_side_exit(w, count, X86_JIT_CHAIN_SLOW_HELPER)) {
    return false;
  }

  JIT_STAT_INC(native_pmem_loads);
  return true;
}

static bool emit_chained_ret_stack_guarded(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn, uint32_t count) {
  x86_jit_indirect_cache_patches_t patches;

  if (insn->width != X86_WIDTH_DWORD || !jit_fast_chain_runtime_enabled() ||
      !jit_indirect_target_cache_runtime_enabled()) {
    return false;
  }

  if (!emit_load_reg_eax(w, R_ESP) ||
      !emit_mov_r11d_eax(w) ||
      !emit_mov_edx_eax(w) ||
      !emit_stack_load_dword_eax(w) ||
      !emit_mov_ecx_eax(w) ||
      !emit_mov_edx_eax(w) ||
      !emit_mov_eax_r11d(w) ||
      !emit_add_eax_imm32(w, X86_WIDTH_DWORD) ||
      !emit_store_reg_eax(w, R_ESP) ||
      !emit_mov_eax_edx(w) ||
      !emit_indirect_target_cache_jump(w, count, &patches) ||
      !emit_indirect_target_cache_slow_exits(w, &patches)) {
    return false;
  }

  JIT_STAT_INC(native_pmem_loads);
  return true;
}

static bool patch_optional_rel32(uint8_t *disp, const uint8_t *target) {
  return disp == NULL || patch_rel32(disp, target);
}

static bool emit_paged_ret_cache_meta_guard(x86_jit_writer_t *w,
    x86_jit_indirect_cache_patches_t *patches) {
  const x86_jit_translation_key_t key = jit_current_translation_key();

  return emit_mov_r10_ret_cache_meta_base(w) &&
         emit_mov_r11d_m32_r10_rcx_disp8(w,
             (uint8_t)offsetof(x86_jit_ret_cache_meta_t,
                 translation_key.cr3_key)) &&
         emit_cmp_r11d_imm32(w, key.cr3_key) &&
         emit_jcc_rel32_placeholder(w, X86_CC_NZ,
             &patches->key_cr3_miss_disp) &&
         emit_mov_r11d_m32_r10_rcx_disp8(w,
             (uint8_t)offsetof(x86_jit_ret_cache_meta_t,
                 translation_key.state)) &&
         emit_cmp_r11d_imm32(w, key.state) &&
         emit_jcc_rel32_placeholder(w, X86_CC_NZ,
             &patches->key_state_miss_disp) &&
         emit_mov_r11d_m32_r10_rcx_disp8(w,
             (uint8_t)offsetof(x86_jit_ret_cache_meta_t,
                 translation_key.paging_generation)) &&
         emit_cmp_r11d_imm32(w, key.paging_generation) &&
         emit_jcc_rel32_placeholder(w, X86_CC_NZ,
             &patches->key_generation_miss_disp) &&
         emit_shl_edx_imm(w, 3u) &&
         emit_mov_r10_ret_cache_generation_slot_base(w) &&
         emit_mov_r11_m64_r10_rdx(w) &&
         emit_test_r11_r11(w) &&
         emit_jcc_rel32_placeholder(w, X86_CC_Z,
             &patches->generation_slot_null_disp) &&
         emit_mov_r11d_m32_r11(w) &&
         emit_mov_r10_ret_cache_meta_base(w) &&
         emit_cmp_m32_r10_rcx_disp8_r11d(w,
             (uint8_t)offsetof(x86_jit_ret_cache_meta_t,
                 block_generation)) &&
         emit_jcc_rel32_placeholder(w, X86_CC_NZ,
             &patches->block_generation_miss_disp);
}

static bool emit_indirect_target_cache_jump(x86_jit_writer_t *w,
    uint32_t count, x86_jit_indirect_cache_patches_t *patches) {
  memset(patches, 0, sizeof(*patches));
  const bool paged_probe = jit_paging_enabled();
  if (!jit_indirect_target_cache_runtime_enabled()) return false;

  return emit_add_esi_imm32(w, count) &&
         emit_cmp_esi_edi(w) &&
         emit_jcc_rel32_placeholder(w, X86_CC_AE,
             &patches->budget_exit_disp) &&
         emit_mov_ecx_eax(w) &&
         emit_shr_ecx_imm(w, 4u) &&
         emit_xor_ecx_eax(w) &&
         emit_mov_r11d_ecx(w) &&
         emit_shr_r11d_imm(w, 12u) &&
         emit_xor_ecx_r11d(w) &&
         emit_and_ecx_imm32(w, X86_JIT_RET_CACHE_MASK) &&
         (!paged_probe || emit_mov_edx_ecx(w)) &&
         emit_shl_ecx_imm(w, 4u) &&
         emit_mov_r10_ret_cache_base(w) &&
         emit_mov_r11d_m32_r10_rcx_disp8(w, 0u) &&
         emit_cmp_r11d_eax(w) &&
         emit_jcc_rel32_placeholder(w, X86_CC_NZ,
             &patches->target_miss_disp) &&
         (!paged_probe || emit_paged_ret_cache_meta_guard(w, patches)) &&
         emit_mov_r10_ret_cache_base(w) &&
         emit_mov_r11_m64_r10_rcx_disp8(w, 8u) &&
         emit_test_r11_r11(w) &&
         emit_jcc_rel32_placeholder(w, X86_CC_Z,
             &patches->entry_null_disp) &&
         (!jit_stats_enabled ||
             (emit_runtime_counter_inc(w, &jit_ret_cache_hits_runtime) &&
              (!paged_probe ||
                  emit_runtime_counter_inc(w,
                      &jit_stats.paged_ret_cache_hits)))) &&
         emit_jmp_r11(w);
}

static bool emit_indirect_target_cache_slow_exits(x86_jit_writer_t *w,
    x86_jit_indirect_cache_patches_t *patches) {
  uint8_t *budget_native = w->cur;
  if (!patch_rel32(patches->budget_exit_disp, budget_native) ||
      !emit_store_pc_eax(w) ||
      !emit_side_exit_counter_inc(w, &jit_chain_exit_budget_runtime) ||
      !emit_mov_eax_esi(w) ||
      !emit_ret(w)) {
    return false;
  }

  uint8_t *miss_native = w->cur;
  return patch_optional_rel32(patches->target_miss_disp, miss_native) &&
         patch_optional_rel32(patches->key_cr3_miss_disp, miss_native) &&
         patch_optional_rel32(patches->key_state_miss_disp, miss_native) &&
         patch_optional_rel32(patches->key_generation_miss_disp,
             miss_native) &&
         patch_optional_rel32(patches->generation_slot_null_disp,
             miss_native) &&
         patch_optional_rel32(patches->block_generation_miss_disp,
             miss_native) &&
         patch_optional_rel32(patches->entry_null_disp, miss_native) &&
         (!jit_stats_enabled ||
             (emit_runtime_counter_inc(w, &jit_ret_cache_misses_runtime) &&
              (!jit_paging_enabled() ||
                  emit_runtime_counter_inc(w,
                      &jit_stats.paged_ret_cache_misses)))) &&
         emit_store_pc_eax(w) &&
         emit_side_exit_counter_inc(w,
             &jit_chain_exit_block_not_chainable_runtime) &&
         emit_mov_eax_esi(w) &&
         emit_ret(w);
}

static bool emit_chained_jmp_rm(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn, uint32_t count) {
  uint8_t *pmem_slow_disp = NULL;
  x86_jit_indirect_cache_patches_t patches;

  if (insn->width != X86_WIDTH_DWORD || !jit_fast_chain_runtime_enabled() ||
      !jit_indirect_target_cache_runtime_enabled()) {
    return false;
  }

  if (insn->rm_is_reg) {
    if (!emit_load_reg_eax(w, insn->rm_reg) ||
        !emit_indirect_target_cache_jump(w, count, &patches) ||
        !emit_indirect_target_cache_slow_exits(w, &patches)) {
      return false;
    }
    JIT_STAT_INC(native_branch_ops);
    return true;
  }

  if (jit_paging_enabled()) return false;

  if (!emit_guest_ea_eax(w, &insn->ea) ||
      !emit_mov_edx_eax(w) ||
      !emit_direct_pmem_guard_edx(w, X86_WIDTH_DWORD, &pmem_slow_disp) ||
      !emit_movabs_r10(w, (uint64_t)(uintptr_t)guest_to_host(CONFIG_MBASE)) ||
      !emit_mov_eax_m32_r10_rdx(w) ||
      !emit_indirect_target_cache_jump(w, count, &patches) ||
      !emit_indirect_target_cache_slow_exits(w, &patches)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(pmem_slow_disp, slow_native) ||
      !emit_helper_call(w, insn) ||
      !emit_ret_count_side_exit(w, count, X86_JIT_CHAIN_SLOW_HELPER)) {
    return false;
  }

  JIT_STAT_INC(native_branch_ops);
  JIT_STAT_INC(native_pmem_loads);
  return true;
}

static bool emit_native_neg_rm(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *pmem_slow_disp = NULL;
  uint8_t *cross_page_slow_disp = NULL;
  uint8_t *source_page_slow_disp = NULL;
  uint8_t *done_disp = NULL;
  const uint8_t width = insn->width;

  if (width != X86_WIDTH_BYTE && width != X86_WIDTH_WORD &&
      width != X86_WIDTH_DWORD) {
    return false;
  }
  if (insn->rm_is_reg) {
    if (width == X86_WIDTH_BYTE) {
      if (!emit_load_byte_reg_to_eax(w, insn->rm_reg) ||
          !emit_neg_eax_width(w, width) ||
          !emit_store_al_to_byte_reg(w, insn->rm_reg)) {
        return false;
      }
    }
    else {
      if (!emit_load_reg_eax(w, insn->rm_reg) ||
          !emit_neg_eax_width(w, width) ||
          !emit_store_reg_eax_width(w, insn->rm_reg, width)) {
        return false;
      }
    }
  }
  else {
    if (!emit_guest_ea_eax(w, &insn->ea) ||
        !emit_mov_edx_eax(w) ||
        !emit_direct_pmem_guard_edx(w, width, &pmem_slow_disp) ||
        !emit_direct_store_source_guard_edx(w, width,
            &cross_page_slow_disp, &source_page_slow_disp) ||
        !emit_movabs_r10(w, (uint64_t)(uintptr_t)guest_to_host(CONFIG_MBASE)) ||
        !emit_load_pmem_eax_width(w, width) ||
        !emit_neg_eax_width(w, width) ||
        !emit_store_pmem_eax_width(w, width) ||
        !emit_capture_status_flags(w, X86_EFLAGS_STATUS_MASK) ||
        !emit_jmp_rel32_placeholder(w, &done_disp)) {
      return false;
    }

    uint8_t *slow_native = w->cur;
    if (!patch_rel32(pmem_slow_disp, slow_native) ||
        !patch_rel32(cross_page_slow_disp, slow_native) ||
        !patch_rel32(source_page_slow_disp, slow_native) ||
        !emit_helper_call(w, insn)) {
      return false;
    }

    JIT_STAT_INC(native_alu_ops);
    JIT_STAT_INC(native_pmem_loads);
    JIT_STAT_INC(native_pmem_stores);
    return patch_rel32(done_disp, w->cur);
  }

  JIT_STAT_INC(native_alu_ops);
  return emit_capture_status_flags(w, X86_EFLAGS_STATUS_MASK);
}

static bool emit_native_incdec_rm(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *pmem_slow_disp = NULL;
  uint8_t *cross_page_slow_disp = NULL;
  uint8_t *source_page_slow_disp = NULL;
  uint8_t *done_disp = NULL;
  const uint8_t width = insn->width;

  if (width != X86_WIDTH_BYTE && width != X86_WIDTH_WORD &&
      width != X86_WIDTH_DWORD) {
    return false;
  }
  if (width == X86_WIDTH_BYTE && insn->rm_is_reg &&
      !jit_native_low_byte_reg(insn->rm_reg)) {
    return false;
  }
  if (insn->alu_op != X86_ALU_ADD && insn->alu_op != X86_ALU_SUB) {
    return false;
  }

  if (insn->rm_is_reg) {
    if (!emit_load_reg_eax(w, insn->rm_reg) ||
        !emit_alu_eax_imm_width(w, insn->alu_op, width, 1) ||
        !emit_store_reg_eax(w, insn->rm_reg)) {
      return false;
    }
  }
  else {
    if (!emit_guest_ea_eax(w, &insn->ea) ||
        !emit_mov_edx_eax(w) ||
        !emit_direct_pmem_guard_edx(w, width, &pmem_slow_disp) ||
        !emit_direct_store_source_guard_edx(w, width,
            &cross_page_slow_disp, &source_page_slow_disp) ||
        !emit_movabs_r10(w, (uint64_t)(uintptr_t)guest_to_host(CONFIG_MBASE)) ||
        !emit_load_pmem_eax_width(w, width) ||
        !emit_alu_eax_imm_width(w, insn->alu_op, width, 1) ||
        !emit_store_pmem_eax_width(w, width) ||
        !emit_capture_status_flags_custom(w, X86_EFLAGS_INCDEC_COPY_MASK, 0) ||
        !emit_jmp_rel32_placeholder(w, &done_disp)) {
      return false;
    }

    uint8_t *slow_native = w->cur;
    if (!patch_rel32(pmem_slow_disp, slow_native) ||
        !patch_rel32(cross_page_slow_disp, slow_native) ||
        !patch_rel32(source_page_slow_disp, slow_native) ||
        !emit_helper_call(w, insn)) {
      return false;
    }

    JIT_STAT_INC(native_incdec_ops);
    JIT_STAT_INC(native_pmem_loads);
    JIT_STAT_INC(native_pmem_stores);
    return patch_rel32(done_disp, w->cur);
  }

  JIT_STAT_INC(native_incdec_ops);
  return emit_capture_status_flags_custom(w, X86_EFLAGS_INCDEC_COPY_MASK, 0);
}

static bool emit_native_imul_reg_rm(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *slow_disp = NULL;
  uint8_t *done_disp = NULL;

  if (insn->width != X86_WIDTH_DWORD) return false;

  if (insn->rm_is_reg) {
    if (!emit_load_reg_eax(w, insn->dst) ||
        !emit_load_reg_ecx(w, insn->rm_reg) ||
        !emit_imul_eax_ecx(w) ||
        !emit_store_reg_eax(w, insn->dst)) {
      return false;
    }
  }
  else {
    if (!emit_guest_ea_eax(w, &insn->ea) ||
        !emit_mov_edx_eax(w) ||
        !emit_direct_pmem_guard_edx(w, X86_WIDTH_DWORD, &slow_disp) ||
        !emit_movabs_r10(w, (uint64_t)(uintptr_t)guest_to_host(CONFIG_MBASE)) ||
        !emit_mov_ecx_m32_r10_rdx(w) ||
        !emit_load_reg_eax(w, insn->dst) ||
        !emit_imul_eax_ecx(w) ||
        !emit_store_reg_eax(w, insn->dst) ||
        !emit_capture_status_flags_custom(w, X86_FLAG_CF | X86_FLAG_OF, 0) ||
        !emit_jmp_rel32_placeholder(w, &done_disp)) {
      return false;
    }

    uint8_t *slow_native = w->cur;
    if (!patch_rel32(slow_disp, slow_native) ||
        !emit_helper_call(w, insn)) {
      return false;
    }
  }

  JIT_STAT_INC(native_imul_ops);
  if (insn->rm_is_reg &&
      !emit_capture_status_flags_custom(w, X86_FLAG_CF | X86_FLAG_OF, 0)) {
    return false;
  }

  return done_disp == NULL || patch_rel32(done_disp, w->cur);
}

static bool emit_load_rm_ecx_dword(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn, uint8_t **slow_disp,
    bool *loaded_from_pmem) {
  *loaded_from_pmem = false;

  if (insn->rm_is_reg) {
    return emit_load_reg_ecx(w, insn->rm_reg);
  }

  *loaded_from_pmem = true;
  return emit_guest_ea_eax(w, &insn->ea) &&
         emit_mov_edx_eax(w) &&
         emit_direct_pmem_guard_edx(w, X86_WIDTH_DWORD, slow_disp) &&
         emit_movabs_r10(w, (uint64_t)(uintptr_t)guest_to_host(CONFIG_MBASE)) &&
         emit_mov_ecx_m32_r10_rdx(w);
}

/* Emit host code for store edx eax pair; bytes below are x86-64 encodings. */
static bool emit_store_edx_eax_pair(x86_jit_writer_t *w) {
  return emit_mov_r11d_edx(w) &&
         emit_store_reg_eax(w, R_EAX) &&
         emit_mov_eax_r11d(w) &&
         emit_store_reg_eax(w, R_EDX);
}

static bool emit_native_mul_rm(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *slow_disp = NULL;
  uint8_t *done_disp = NULL;
  bool pmem_load = false;

  if (insn->width != X86_WIDTH_DWORD) return false;

  if (!emit_load_rm_ecx_dword(w, insn, &slow_disp, &pmem_load) ||
      !emit_load_reg_eax(w, R_EAX) ||
      !emit_mul_ecx(w) ||
      !emit_store_edx_eax_pair(w) ||
      !emit_capture_status_flags_custom(w, X86_FLAG_CF | X86_FLAG_OF, 0)) {
    return false;
  }

  if (pmem_load) {
    if (!emit_jmp_rel32_placeholder(w, &done_disp)) return false;
    uint8_t *slow_native = w->cur;
    if (!patch_rel32(slow_disp, slow_native) ||
        !emit_helper_call(w, insn)) {
      return false;
    }
  }

  JIT_STAT_INC(native_mul_ops);
  if (pmem_load) JIT_STAT_INC(native_pmem_loads);
  return done_disp == NULL || patch_rel32(done_disp, w->cur);
}

static bool emit_native_imul_acc_rm(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *slow_disp = NULL;
  uint8_t *done_disp = NULL;
  bool pmem_load = false;

  if (insn->width != X86_WIDTH_DWORD) return false;

  if (!emit_load_rm_ecx_dword(w, insn, &slow_disp, &pmem_load) ||
      !emit_load_reg_eax(w, R_EAX) ||
      !emit_imul_acc_ecx(w) ||
      !emit_store_edx_eax_pair(w) ||
      !emit_capture_status_flags_custom(w, X86_FLAG_CF | X86_FLAG_OF, 0)) {
    return false;
  }

  if (pmem_load) {
    if (!emit_jmp_rel32_placeholder(w, &done_disp)) return false;
    uint8_t *slow_native = w->cur;
    if (!patch_rel32(slow_disp, slow_native) ||
        !emit_helper_call(w, insn)) {
      return false;
    }
  }

  JIT_STAT_INC(native_imul_ops);
  if (pmem_load) JIT_STAT_INC(native_pmem_loads);
  return done_disp == NULL || patch_rel32(done_disp, w->cur);
}

static bool emit_native_div_rm(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *src_slow_disp = NULL;
  uint8_t *zero_slow_disp = NULL;
  uint8_t *overflow_slow_disp = NULL;
  uint8_t *done_disp = NULL;
  bool pmem_load = false;

  if (insn->width != X86_WIDTH_DWORD) return false;

  if (!emit_load_rm_ecx_dword(w, insn, &src_slow_disp, &pmem_load) ||
      !emit_test_ecx_ecx(w) ||
      !emit_jcc_rel32_placeholder(w, X86_CC_Z, &zero_slow_disp) ||
      !emit_load_reg_eax(w, R_EAX) ||
      !emit_mov_r11d_eax(w) ||
      !emit_load_reg_edx(w, R_EDX) ||
      !emit_cmp_edx_ecx(w) ||
      !emit_jcc_rel32_placeholder(w, X86_CC_AE, &overflow_slow_disp) ||
      !emit_mov_eax_r11d(w) ||
      !emit_div_ecx(w) ||
      !emit_store_edx_eax_pair(w) ||
      !emit_jmp_rel32_placeholder(w, &done_disp)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if ((src_slow_disp != NULL && !patch_rel32(src_slow_disp, slow_native)) ||
      !patch_rel32(zero_slow_disp, slow_native) ||
      !patch_rel32(overflow_slow_disp, slow_native) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  JIT_STAT_INC(native_div_ops);
  if (pmem_load) JIT_STAT_INC(native_pmem_loads);
  return patch_rel32(done_disp, w->cur);
}

static bool emit_native_idiv_rm(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *src_slow_disp = NULL;
  uint8_t *zero_slow_disp = NULL;
  uint8_t *wide_slow_disp = NULL;
  uint8_t *not_min_disp = NULL;
  uint8_t *overflow_slow_disp = NULL;
  uint8_t *done_disp = NULL;
  bool pmem_load = false;

  if (!jit_native_idiv_enabled || insn->width != X86_WIDTH_DWORD) return false;

  /*
   * Native x86 IDIV raises a host #DE on zero, overflow, or too-wide
   * EDX:EAX dividends.  Keep the generated path to the compiler's common
   * cdq/idiv case and re-enter the architectural helper for everything else.
   */
  if (!emit_load_rm_ecx_dword(w, insn, &src_slow_disp, &pmem_load) ||
      !emit_test_ecx_ecx(w) ||
      !emit_jcc_rel32_placeholder(w, X86_CC_Z, &zero_slow_disp) ||
      !emit_mov_r10d_ecx(w) ||
      !emit_load_reg_eax(w, R_EAX) ||
      !emit_cdq_host(w) ||
      !emit_load_reg_eax(w, R_EDX) ||
      !emit_cmp_edx_eax(w) ||
      !emit_jcc_rel32_placeholder(w, X86_CC_NZ, &wide_slow_disp) ||
      !emit_mov_ecx_r10d(w) ||
      !emit_load_reg_eax(w, R_EAX) ||
      !emit_alu_eax_imm32(w, X86_ALU_CMP, 0x80000000u) ||
      !emit_jcc_rel32_placeholder(w, X86_CC_NZ, &not_min_disp) ||
      !emit_cmp_ecx_imm32(w, 0xffffffffu) ||
      !emit_jcc_rel32_placeholder(w, X86_CC_Z, &overflow_slow_disp)) {
    return false;
  }

  uint8_t *native_safe = w->cur;
  if (!patch_rel32(not_min_disp, native_safe) ||
      !emit_cdq_host(w) ||
      !emit_idiv_ecx(w) ||
      !emit_store_edx_eax_pair(w) ||
      !emit_jmp_rel32_placeholder(w, &done_disp)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if ((src_slow_disp != NULL && !patch_rel32(src_slow_disp, slow_native)) ||
      !patch_rel32(zero_slow_disp, slow_native) ||
      !patch_rel32(wide_slow_disp, slow_native) ||
      !patch_rel32(overflow_slow_disp, slow_native) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  JIT_STAT_INC(native_div_ops);
  if (pmem_load) JIT_STAT_INC(native_pmem_loads);
  return patch_rel32(done_disp, w->cur);
}

static bool emit_native_shift_reg_imm(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *pmem_slow_disp = NULL;
  uint8_t *cross_page_slow_disp = NULL;
  uint8_t *source_page_slow_disp = NULL;
  uint8_t *done_disp = NULL;
  uint8_t shift_op = insn->alu_op;
  uint32_t copy_mask = 0;
  const uint8_t width = insn->width;

  if (insn->count_from_cl ||
      (width != X86_WIDTH_BYTE && width != X86_WIDTH_WORD &&
          width != X86_WIDTH_DWORD)) {
    return false;
  }

  const uint8_t count = insn->imm & X86_SHIFT_COUNT_MASK;
  if (!jit_native_shift_count_safe_width(insn->alu_op, width, count)) {
    return false;
  }
  if (!jit_native_shift_flag_copy_mask(insn->alu_op, count,
      &shift_op, &copy_mask)) {
    return false;
  }

  if (count == 0) {
    JIT_STAT_INC(native_shift_ops);
    return true;
  }

  if (insn->rm_is_reg) {
    if (!emit_load_reg_to_eax_width(w, insn->rm_reg, width) ||
        !emit_shift_eax_imm_width(w, shift_op, width, count)) {
      return false;
    }
    if (width != X86_WIDTH_DWORD) {
      if (!emit_mov_r11d_eax(w) ||
          !emit_capture_status_flags_custom(w, copy_mask, 0) ||
          !emit_mov_eax_r11d(w) ||
          !emit_store_loaded_rm_to_reg(w, insn->rm_reg, width)) {
        return false;
      }
      JIT_STAT_INC(native_shift_ops);
      return true;
    }
    if (!emit_store_loaded_rm_to_reg(w, insn->rm_reg, width)) return false;
  }
  else {
    if (width != X86_WIDTH_DWORD) return false;
    if (!emit_guest_ea_eax(w, &insn->ea) ||
        !emit_mov_edx_eax(w) ||
        !emit_direct_pmem_guard_edx(w, X86_WIDTH_DWORD, &pmem_slow_disp) ||
        !emit_direct_store_source_guard_edx(w, X86_WIDTH_DWORD,
            &cross_page_slow_disp, &source_page_slow_disp) ||
        !emit_movabs_r10(w, (uint64_t)(uintptr_t)guest_to_host(CONFIG_MBASE)) ||
        !emit_mov_eax_m32_r10_rdx(w) ||
        !emit_shift_eax_imm(w, shift_op, count) ||
        !emit_mov_m32_r10_rdx_eax(w) ||
        !emit_capture_status_flags_custom(w, copy_mask, 0) ||
        !emit_jmp_rel32_placeholder(w, &done_disp)) {
      return false;
    }

    uint8_t *slow_native = w->cur;
    if (!patch_rel32(pmem_slow_disp, slow_native) ||
        !patch_rel32(cross_page_slow_disp, slow_native) ||
        !patch_rel32(source_page_slow_disp, slow_native) ||
        !emit_helper_call(w, insn)) {
      return false;
    }

    JIT_STAT_INC(native_shift_ops);
    JIT_STAT_INC(native_pmem_loads);
    JIT_STAT_INC(native_pmem_stores);
    return patch_rel32(done_disp, w->cur);
  }

  JIT_STAT_INC(native_shift_ops);
  return emit_capture_status_flags_custom(w, copy_mask, 0);
}

static bool emit_shift_rm_cl_body(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn, uint8_t shift_op, uint32_t copy_mask) {
  if (insn->rm_is_reg) {
    if (!emit_load_reg_eax(w, insn->rm_reg) ||
        !emit_shift_eax_cl(w, shift_op) ||
        !emit_store_reg_eax(w, insn->rm_reg)) {
      return false;
    }
  }
  else if (!emit_mov_eax_m32_r10_rdx(w) ||
      !emit_shift_eax_cl(w, shift_op) ||
      !emit_mov_m32_r10_rdx_eax(w)) {
    return false;
  }

  return emit_capture_status_flags_custom(w, copy_mask, 0);
}

static bool emit_native_shift_rm_cl(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *zero_disp = NULL;
  uint8_t *one_disp = NULL;
  uint8_t *many_done_disp = NULL;
  uint8_t *one_done_disp = NULL;
  uint8_t *pmem_slow_disp = NULL;
  uint8_t *cross_page_slow_disp = NULL;
  uint8_t *source_page_slow_disp = NULL;
  uint8_t shift_op = insn->alu_op;
  uint8_t one_shift_op = insn->alu_op;
  uint32_t many_mask = 0;
  uint32_t one_mask = 0;

  if (insn->width != X86_WIDTH_DWORD || !insn->count_from_cl) return false;
  if (!jit_native_shift_flag_copy_mask(insn->alu_op, 2,
          &shift_op, &many_mask) ||
      !jit_native_shift_flag_copy_mask(insn->alu_op, 1,
          &one_shift_op, &one_mask) ||
      shift_op != one_shift_op) {
    return false;
  }

  if (insn->rm_is_reg) {
    if (!emit_load_reg_ecx(w, R_ECX) ||
        !emit_and_ecx_imm32(w, X86_SHIFT_COUNT_MASK) ||
        !emit_jcc_rel32_placeholder(w, X86_CC_Z, &zero_disp) ||
        !emit_cmp_ecx_imm32(w, 1) ||
        !emit_jcc_rel32_placeholder(w, X86_CC_Z, &one_disp) ||
        !emit_shift_rm_cl_body(w, insn, shift_op, many_mask) ||
        !emit_jmp_rel32_placeholder(w, &many_done_disp)) {
      return false;
    }

    uint8_t *one_native = w->cur;
    if (!patch_rel32(one_disp, one_native) ||
        !emit_shift_rm_cl_body(w, insn, shift_op, one_mask)) {
      return false;
    }

    JIT_STAT_INC(native_shift_ops);
    uint8_t *done_native = w->cur;
    return patch_rel32(zero_disp, done_native) &&
           patch_rel32(many_done_disp, done_native);
  }

  if (!emit_guest_ea_eax(w, &insn->ea) ||
      !emit_load_reg_ecx(w, R_ECX) ||
      !emit_and_ecx_imm32(w, X86_SHIFT_COUNT_MASK) ||
      !emit_jcc_rel32_placeholder(w, X86_CC_Z, &zero_disp) ||
      !emit_mov_r11d_ecx(w) ||
      !emit_mov_edx_eax(w) ||
      !emit_direct_pmem_guard_edx(w, X86_WIDTH_DWORD, &pmem_slow_disp) ||
      !emit_direct_store_source_guard_edx(w, X86_WIDTH_DWORD,
          &cross_page_slow_disp, &source_page_slow_disp) ||
      !emit_movabs_r10(w, (uint64_t)(uintptr_t)guest_to_host(CONFIG_MBASE)) ||
      !emit_mov_ecx_r11d(w) ||
      !emit_cmp_ecx_imm32(w, 1) ||
      !emit_jcc_rel32_placeholder(w, X86_CC_Z, &one_disp) ||
      !emit_shift_rm_cl_body(w, insn, shift_op, many_mask) ||
      !emit_jmp_rel32_placeholder(w, &many_done_disp)) {
    return false;
  }

  uint8_t *one_native = w->cur;
  if (!patch_rel32(one_disp, one_native) ||
      !emit_mov_ecx_r11d(w) ||
      !emit_shift_rm_cl_body(w, insn, shift_op, one_mask) ||
      !emit_jmp_rel32_placeholder(w, &one_done_disp)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(pmem_slow_disp, slow_native) ||
      !patch_rel32(cross_page_slow_disp, slow_native) ||
      !patch_rel32(source_page_slow_disp, slow_native) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  JIT_STAT_INC(native_shift_ops);
  JIT_STAT_INC(native_pmem_loads);
  JIT_STAT_INC(native_pmem_stores);
  uint8_t *done_native = w->cur;
  return patch_rel32(zero_disp, done_native) &&
         patch_rel32(many_done_disp, done_native) &&
         patch_rel32(one_done_disp, done_native);
}

static bool emit_paged_dtlb_shift_rm_imm(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *read_slow_disp = NULL;
  uint8_t *write_slow_disp = NULL;
  uint8_t *done_disp = NULL;
  uint8_t shift_op = insn->alu_op;
  uint32_t copy_mask = 0;

  const uint8_t width = insn->width;
  if (insn->rm_is_reg || insn->count_from_cl ||
      (width != X86_WIDTH_BYTE && width != X86_WIDTH_WORD &&
          width != X86_WIDTH_DWORD)) {
    return false;
  }

  const uint8_t count = insn->imm & X86_SHIFT_COUNT_MASK;
  if (!jit_native_shift_count_safe_width(insn->alu_op, width, count)) {
    return false;
  }
  if (!jit_native_shift_flag_copy_mask(insn->alu_op, count,
      &shift_op, &copy_mask)) {
    return false;
  }

  if (count == 0) {
    JIT_STAT_INC(native_shift_ops);
    return true;
  }

  /*
   * Preserve the helper's RMW order: read translation and load first, then
   * write translation, then commit the memory result and flags.
   */
  if (!emit_guest_ea_eax(w, &insn->ea) ||
      !emit_store_dtlb_scratch_eax(w) ||
      !emit_paged_dtlb_translate_addr_eax(w, insn, width, false,
          &read_slow_disp) ||
      !emit_load_host_ptr_rax_width(w, width) ||
      !emit_store_dtlb_value_scratch_eax(w) ||
      !emit_load_dtlb_scratch_eax(w) ||
      !emit_paged_dtlb_translate_addr_eax(w, insn, width, true,
          &write_slow_disp) ||
      !emit_mov_r10_rax(w) ||
      !emit_load_dtlb_value_scratch_eax(w) ||
      !emit_shift_eax_imm_width(w, shift_op, width, count) ||
      !emit_store_host_ptr_r10_eax_width(w, width) ||
      !emit_capture_status_flags_custom(w, copy_mask, 0) ||
      !emit_jmp_rel32_placeholder(w, &done_disp)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(read_slow_disp, slow_native) ||
      !patch_rel32(write_slow_disp, slow_native) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  JIT_STAT_INC(native_shift_ops);
  JIT_STAT_INC(native_pmem_loads);
  JIT_STAT_INC(native_pmem_stores);
  return patch_rel32(done_disp, w->cur);
}

static bool emit_paged_dtlb_shift_rm_cl_small(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *zero_disp = NULL;
  uint8_t *one_disp = NULL;
  uint8_t *read_slow_disp = NULL;
  uint8_t *write_slow_disp = NULL;
  uint8_t *many_done_disp = NULL;
  uint8_t *one_done_disp = NULL;
  uint8_t shift_op = insn->alu_op;
  uint8_t one_shift_op = insn->alu_op;
  uint32_t many_mask = 0;
  uint32_t one_mask = 0;
  const uint8_t width = insn->width;

  if (width != X86_WIDTH_BYTE && width != X86_WIDTH_WORD) return false;
  if (!jit_native_shift_flag_copy_mask(insn->alu_op, 2u,
          &shift_op, &many_mask) ||
      !jit_native_shift_flag_copy_mask(insn->alu_op, 1u,
          &one_shift_op, &one_mask) ||
      shift_op != one_shift_op) {
    return false;
  }

  /*
   * For byte/word CL shifts, host AL/AX shifts give the exact data result and
   * flags for the masked count.  Keep the helper's zero-count behaviour by
   * exiting before EA translation, then reload CL after DTLB helper calls.
   */
  if (!emit_load_reg_ecx(w, R_ECX) ||
      !emit_and_ecx_imm32(w, X86_SHIFT_COUNT_MASK) ||
      !emit_jcc_rel32_placeholder(w, X86_CC_Z, &zero_disp) ||
      !emit_guest_ea_eax(w, &insn->ea) ||
      !emit_store_dtlb_scratch_eax(w) ||
      !emit_paged_dtlb_translate_addr_eax(w, insn, width, false,
          &read_slow_disp) ||
      !emit_load_host_ptr_rax_width(w, width) ||
      !emit_store_dtlb_value_scratch_eax(w) ||
      !emit_load_dtlb_scratch_eax(w) ||
      !emit_paged_dtlb_translate_addr_eax(w, insn, width, true,
          &write_slow_disp) ||
      !emit_mov_r10_rax(w) ||
      !emit_load_dtlb_value_scratch_eax(w) ||
      !emit_mov_r11d_eax(w) ||
      !emit_load_reg_ecx(w, R_ECX) ||
      !emit_and_ecx_imm32(w, X86_SHIFT_COUNT_MASK) ||
      !emit_mov_eax_r11d(w) ||
      !emit_cmp_ecx_imm32(w, 1u) ||
      !emit_jcc_rel32_placeholder(w, X86_CC_Z, &one_disp) ||
      !emit_shift_eax_cl_width(w, shift_op, width) ||
      !emit_store_host_ptr_r10_eax_width(w, width) ||
      !emit_capture_status_flags_custom(w, many_mask, 0) ||
      !emit_jmp_rel32_placeholder(w, &many_done_disp)) {
    return false;
  }

  uint8_t *one_native = w->cur;
  if (!patch_rel32(one_disp, one_native) ||
      !emit_shift_eax_cl_width(w, shift_op, width) ||
      !emit_store_host_ptr_r10_eax_width(w, width) ||
      !emit_capture_status_flags_custom(w, one_mask, 0) ||
      !emit_jmp_rel32_placeholder(w, &one_done_disp)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(read_slow_disp, slow_native) ||
      !patch_rel32(write_slow_disp, slow_native) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  JIT_STAT_INC(native_shift_ops);
  JIT_STAT_INC(native_pmem_loads);
  JIT_STAT_INC(native_pmem_stores);
  uint8_t *done_native = w->cur;
  return patch_rel32(zero_disp, done_native) &&
         patch_rel32(many_done_disp, done_native) &&
         patch_rel32(one_done_disp, done_native);
}

static bool emit_paged_dtlb_shift_rm_cl(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *zero_disp = NULL;
  uint8_t *one_disp = NULL;
  uint8_t *read_slow_disp = NULL;
  uint8_t *write_slow_disp = NULL;
  uint8_t *many_done_disp = NULL;
  uint8_t *one_done_disp = NULL;
  uint8_t shift_op = insn->alu_op;
  uint8_t one_shift_op = insn->alu_op;
  uint32_t many_mask = 0;
  uint32_t one_mask = 0;

  if (insn->rm_is_reg || !insn->count_from_cl) return false;
  if (insn->width == X86_WIDTH_BYTE || insn->width == X86_WIDTH_WORD) {
    return emit_paged_dtlb_shift_rm_cl_small(w, insn);
  }
  if (insn->width != X86_WIDTH_DWORD) return false;
  if (!jit_native_shift_flag_copy_mask(insn->alu_op, 2,
          &shift_op, &many_mask) ||
      !jit_native_shift_flag_copy_mask(insn->alu_op, 1,
          &one_shift_op, &one_mask) ||
      shift_op != one_shift_op) {
    return false;
  }

  /*
   * The NEMU helper returns before touching memory when CL's masked count is
   * zero.  Keep that fast exit before EA calculation or page translation.
   */
  if (!emit_load_reg_eax(w, R_ECX) ||
      !emit_alu_eax_imm32(w, X86_ALU_AND, X86_SHIFT_COUNT_MASK) ||
      !emit_jcc_rel32_placeholder(w, X86_CC_Z, &zero_disp) ||
      !emit_guest_ea_eax(w, &insn->ea) ||
      !emit_store_dtlb_scratch_eax(w) ||
      !emit_paged_dtlb_translate_addr_eax(w, insn, X86_WIDTH_DWORD, false,
          &read_slow_disp) ||
      !emit_load_host_ptr_rax_width(w, X86_WIDTH_DWORD) ||
      !emit_store_dtlb_value_scratch_eax(w) ||
      !emit_load_dtlb_scratch_eax(w) ||
      !emit_paged_dtlb_translate_addr_eax(w, insn, X86_WIDTH_DWORD, true,
          &write_slow_disp) ||
      !emit_mov_r10_rax(w) ||
      !emit_load_dtlb_value_scratch_eax(w) ||
      !emit_mov_r11d_eax(w) ||
      !emit_load_reg_ecx(w, R_ECX) ||
      !emit_and_ecx_imm32(w, X86_SHIFT_COUNT_MASK) ||
      !emit_mov_eax_r11d(w) ||
      !emit_cmp_ecx_imm32(w, 1) ||
      !emit_jcc_rel32_placeholder(w, X86_CC_Z, &one_disp) ||
      !emit_shift_eax_cl(w, shift_op) ||
      !emit_store_host_ptr_r10_eax_width(w, X86_WIDTH_DWORD) ||
      !emit_capture_status_flags_custom(w, many_mask, 0) ||
      !emit_jmp_rel32_placeholder(w, &many_done_disp)) {
    return false;
  }

  uint8_t *one_native = w->cur;
  if (!patch_rel32(one_disp, one_native) ||
      !emit_shift_eax_cl(w, shift_op) ||
      !emit_store_host_ptr_r10_eax_width(w, X86_WIDTH_DWORD) ||
      !emit_capture_status_flags_custom(w, one_mask, 0) ||
      !emit_jmp_rel32_placeholder(w, &one_done_disp)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(read_slow_disp, slow_native) ||
      !patch_rel32(write_slow_disp, slow_native) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  JIT_STAT_INC(native_shift_ops);
  JIT_STAT_INC(native_pmem_loads);
  JIT_STAT_INC(native_pmem_stores);
  uint8_t *done_native = w->cur;
  return patch_rel32(zero_disp, done_native) &&
         patch_rel32(many_done_disp, done_native) &&
         patch_rel32(one_done_disp, done_native);
}

static bool emit_native_not_rm(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *pmem_slow_disp = NULL;
  uint8_t *cross_page_slow_disp = NULL;
  uint8_t *source_page_slow_disp = NULL;
  uint8_t *done_disp = NULL;

  if (insn->width != X86_WIDTH_DWORD) return false;

  if (insn->rm_is_reg) {
    if (!emit_load_reg_eax(w, insn->rm_reg) ||
        !emit_not_eax(w) ||
        !emit_store_reg_eax(w, insn->rm_reg)) {
      return false;
    }
  }
  else {
    if (!emit_guest_ea_eax(w, &insn->ea) ||
        !emit_mov_edx_eax(w) ||
        !emit_direct_pmem_guard_edx(w, X86_WIDTH_DWORD, &pmem_slow_disp) ||
        !emit_direct_store_source_guard_edx(w, X86_WIDTH_DWORD,
            &cross_page_slow_disp, &source_page_slow_disp) ||
        !emit_movabs_r10(w, (uint64_t)(uintptr_t)guest_to_host(CONFIG_MBASE)) ||
        !emit_not_m32_r10_rdx(w) ||
        !emit_jmp_rel32_placeholder(w, &done_disp)) {
      return false;
    }

    uint8_t *slow_native = w->cur;
    if (!patch_rel32(pmem_slow_disp, slow_native) ||
        !patch_rel32(cross_page_slow_disp, slow_native) ||
        !patch_rel32(source_page_slow_disp, slow_native) ||
        !emit_helper_call(w, insn)) {
      return false;
    }
  }

  JIT_STAT_INC(native_not_ops);
  return done_disp == NULL || patch_rel32(done_disp, w->cur);
}

static bool emit_native_movzx_reg_rm(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *slow_disp = NULL;
  uint8_t *done_disp = NULL;
  uint32_t width = 0;
  uint32_t mask = 0;

  if (insn->helper == X86_JIT_HELPER_MOVZX_REG_RM8) {
    width = X86_WIDTH_BYTE;
    mask = X86_BYTE_MASK;
  }
  else if (insn->helper == X86_JIT_HELPER_MOVZX_REG_RM16) {
    width = X86_WIDTH_WORD;
    mask = X86_WORD_MASK;
  }
  else {
    return false;
  }

  if (insn->rm_is_reg) {
    /*
     * IA-32 byte register numbers 4..7 name AH/CH/DH/BH, not SPL/BPL/SIL/DIL.
     * Decode stores the aliased base register plus four. Shift the full base
     * register down before masking so high-byte MOVZX stays on the native path.
     */
    const bool high_byte = width == X86_WIDTH_BYTE && insn->rm_reg >= 4;
    if (!emit_load_reg_eax(w, high_byte ? (insn->rm_reg & 0x3u) : insn->rm_reg) ||
        (high_byte && !emit_shift_eax_imm(w, 5u, 8u)) ||
        !emit_alu_eax_imm32(w, X86_ALU_AND, mask) ||
        !emit_store_reg_eax(w, insn->dst)) {
      return false;
    }
  }
  else {
    if (!emit_guest_ea_eax(w, &insn->ea) ||
        !emit_mov_edx_eax(w) ||
        !emit_direct_pmem_guard_edx(w, width, &slow_disp) ||
        !emit_movabs_r10(w, (uint64_t)(uintptr_t)guest_to_host(CONFIG_MBASE))) {
      return false;
    }

    if (width == X86_WIDTH_BYTE) {
      if (!emit_movzx_eax_m8_r10_rdx(w)) return false;
    }
    else if (!emit_movzx_eax_m16_r10_rdx(w)) {
      return false;
    }

    if (!emit_store_reg_eax(w, insn->dst) ||
        !emit_jmp_rel32_placeholder(w, &done_disp)) {
      return false;
    }

    uint8_t *slow_native = w->cur;
    if (!patch_rel32(slow_disp, slow_native) ||
        !emit_helper_call(w, insn)) {
      return false;
    }
  }

  JIT_STAT_INC(native_movzx_ops);
  return done_disp == NULL || patch_rel32(done_disp, w->cur);
}

static bool emit_native_movsx_reg_rm(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *slow_disp = NULL;
  uint8_t *done_disp = NULL;
  uint32_t width = 0;

  if (insn->width != X86_WIDTH_WORD && insn->width != X86_WIDTH_DWORD) {
    return false;
  }
  if (insn->helper == X86_JIT_HELPER_MOVSX_REG_RM8) {
    width = X86_WIDTH_BYTE;
  }
  else if (insn->helper == X86_JIT_HELPER_MOVSX_REG_RM16) {
    width = X86_WIDTH_WORD;
  }
  else {
    return false;
  }

  if (insn->rm_is_reg) {
    if (width == X86_WIDTH_BYTE) {
      if (!emit_load_byte_reg_to_eax(w, insn->rm_reg)) return false;
      if (!emit_movsx_eax_al(w)) return false;
    }
    else {
      if (!emit_load_reg_eax(w, insn->rm_reg) ||
          !emit_movsx_eax_ax(w)) {
        return false;
      }
    }
    if (!emit_store_reg_eax_width(w, insn->dst, insn->width)) return false;
  }
  else {
    if (!emit_guest_ea_eax(w, &insn->ea) ||
        !emit_mov_edx_eax(w) ||
        !emit_direct_pmem_guard_edx(w, width, &slow_disp) ||
        !emit_movabs_r10(w, (uint64_t)(uintptr_t)guest_to_host(CONFIG_MBASE))) {
      return false;
    }

    if (width == X86_WIDTH_BYTE) {
      if (!emit_movsx_eax_m8_r10_rdx(w)) return false;
    }
    else if (!emit_movsx_eax_m16_r10_rdx(w)) {
      return false;
    }

    if (!emit_store_reg_eax_width(w, insn->dst, insn->width) ||
        !emit_jmp_rel32_placeholder(w, &done_disp)) {
      return false;
    }

    uint8_t *slow_native = w->cur;
    if (!patch_rel32(slow_disp, slow_native) ||
        !emit_helper_call(w, insn)) {
      return false;
    }
  }

  JIT_STAT_INC(native_movsx_ops);
  if (!insn->rm_is_reg) JIT_STAT_INC(native_pmem_loads);
  return done_disp == NULL || patch_rel32(done_disp, w->cur);
}

static bool emit_native_test_imm_rm(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *slow_disp = NULL;
  uint8_t *done_disp = NULL;
  const uint8_t width = insn->width;

  if (width != X86_WIDTH_BYTE && width != X86_WIDTH_WORD &&
      width != X86_WIDTH_DWORD) {
    return false;
  }
  if (insn->rm_is_reg && width == X86_WIDTH_BYTE && insn->rm_reg >= 4u &&
      !jit_native_high_byte_test_enabled) {
    return false;
  }

  if (insn->rm_is_reg) {
    if (!emit_load_reg_to_eax_width(w, insn->rm_reg, width) ||
        !emit_test_eax_imm_width(w, width, insn->imm)) {
      return false;
    }

    JIT_STAT_INC(native_alu_ops);
    return emit_capture_status_flags(w, X86_EFLAGS_LOGIC_COPY_MASK);
  }

  if (!emit_guest_ea_eax(w, &insn->ea) ||
      !emit_mov_edx_eax(w) ||
      !emit_direct_pmem_guard_edx(w, width, &slow_disp) ||
      !emit_movabs_r10(w, (uint64_t)(uintptr_t)guest_to_host(CONFIG_MBASE)) ||
      !emit_load_pmem_eax_width(w, width) ||
      !emit_test_eax_imm_width(w, width, insn->imm) ||
      !emit_capture_status_flags(w, X86_EFLAGS_LOGIC_COPY_MASK) ||
      !emit_jmp_rel32_placeholder(w, &done_disp)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(slow_disp, slow_native) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  JIT_STAT_INC(native_alu_ops);
  JIT_STAT_INC(native_pmem_loads);
  return patch_rel32(done_disp, w->cur);
}

/* Emit host code for condition bool eax; bytes below are x86-64 encodings. */
static bool emit_condition_bool_eax(x86_jit_writer_t *w, uint8_t cc) {
  uint8_t *true_disp = NULL;
  uint8_t *done_disp = NULL;

  if (!emit_jcc_condition_jump(w, cc, &true_disp) ||
      !emit_mov_eax_imm32(w, 0) ||
      !emit_jmp_rel32_placeholder(w, &done_disp)) {
    return false;
  }

  uint8_t *true_native = w->cur;
  return patch_rel32(true_disp, true_native) &&
         emit_mov_eax_imm32(w, 1) &&
         patch_rel32(done_disp, w->cur);
}

static bool emit_native_setcc_rm8(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *pmem_slow_disp = NULL;
  uint8_t *cross_page_slow_disp = NULL;
  uint8_t *source_page_slow_disp = NULL;
  uint8_t *done_disp = NULL;

  if (insn->width != X86_WIDTH_BYTE) return false;

  if (insn->rm_is_reg) {
    if (!jit_native_low_byte_reg(insn->rm_reg)) return false;
    return emit_condition_bool_eax(w, insn->cc) &&
           emit_store_reg_eax_width(w, insn->rm_reg, X86_WIDTH_BYTE);
  }

  if (!emit_condition_bool_eax(w, insn->cc) ||
      !emit_mov_r11d_eax(w) ||
      !emit_guest_ea_eax(w, &insn->ea) ||
      !emit_mov_edx_eax(w) ||
      !emit_direct_pmem_guard_edx(w, X86_WIDTH_BYTE, &pmem_slow_disp) ||
      !emit_direct_store_source_guard_edx(w, X86_WIDTH_BYTE,
          &cross_page_slow_disp, &source_page_slow_disp) ||
      !emit_movabs_r10(w, (uint64_t)(uintptr_t)guest_to_host(CONFIG_MBASE)) ||
      !emit_mov_eax_r11d(w) ||
      !emit_store_pmem_eax_width(w, X86_WIDTH_BYTE) ||
      !emit_jmp_rel32_placeholder(w, &done_disp)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(pmem_slow_disp, slow_native) ||
      !patch_rel32(cross_page_slow_disp, slow_native) ||
      !patch_rel32(source_page_slow_disp, slow_native) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  JIT_STAT_INC(native_pmem_stores);
  return patch_rel32(done_disp, w->cur);
}

static bool emit_native_push_imm(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *pmem_slow_disp = NULL;
  uint8_t *cross_page_slow_disp = NULL;
  uint8_t *source_page_slow_disp = NULL;
  uint8_t *done_disp = NULL;

  if (insn->width != X86_WIDTH_DWORD) return false;

  if (!emit_load_reg_eax(w, R_ESP) ||
      !emit_add_eax_imm32(w, 0u - X86_WIDTH_DWORD) ||
      !emit_mov_edx_eax(w) ||
      !emit_direct_pmem_guard_edx(w, X86_WIDTH_DWORD, &pmem_slow_disp) ||
      !emit_direct_store_source_guard_edx(w, X86_WIDTH_DWORD,
          &cross_page_slow_disp, &source_page_slow_disp) ||
      !emit_mov_r11d_imm32(w, insn->imm) ||
      !emit_movabs_r10(w, (uint64_t)(uintptr_t)guest_to_host(CONFIG_MBASE)) ||
      !emit_mov_m32_r10_rdx_r11d(w) ||
      !emit_store_reg_eax(w, R_ESP) ||
      !emit_jmp_rel32_placeholder(w, &done_disp)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(pmem_slow_disp, slow_native) ||
      !patch_rel32(cross_page_slow_disp, slow_native) ||
      !patch_rel32(source_page_slow_disp, slow_native) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  JIT_STAT_INC(native_pmem_stores);
  return patch_rel32(done_disp, w->cur);
}

static bool emit_native_push_imm_stack_guarded(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  if (insn->width != X86_WIDTH_DWORD) return false;

  if (!emit_load_reg_eax(w, R_ESP) ||
      !emit_add_eax_imm32(w, 0u - X86_WIDTH_DWORD) ||
      !emit_mov_edx_eax(w) ||
      !emit_mov_r11d_imm32(w, insn->imm) ||
      !emit_stack_store_dword_r11d(w) ||
      !emit_store_reg_eax(w, R_ESP)) {
    return false;
  }

  JIT_STAT_INC(native_pmem_stores);
  return true;
}

static bool emit_native_push_rm(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *src_slow_disp = NULL;
  uint8_t *dst_pmem_slow_disp = NULL;
  uint8_t *cross_page_slow_disp = NULL;
  uint8_t *source_page_slow_disp = NULL;
  uint8_t *done_disp = NULL;

  if (insn->width != X86_WIDTH_DWORD) return false;

  if (insn->rm_is_reg) {
    if (!emit_load_reg_r11d(w, insn->rm_reg)) return false;
  }
  else if (!emit_guest_ea_eax(w, &insn->ea) ||
      !emit_mov_edx_eax(w) ||
      !emit_direct_pmem_guard_edx(w, X86_WIDTH_DWORD, &src_slow_disp) ||
      !emit_movabs_r10(w, (uint64_t)(uintptr_t)guest_to_host(CONFIG_MBASE)) ||
      !emit_mov_r11d_m32_r10_rdx(w)) {
    return false;
  }

  if (!emit_load_reg_eax(w, R_ESP) ||
      !emit_add_eax_imm32(w, 0u - X86_WIDTH_DWORD) ||
      !emit_mov_edx_eax(w) ||
      !emit_direct_pmem_guard_edx(w, X86_WIDTH_DWORD, &dst_pmem_slow_disp) ||
      !emit_direct_store_source_guard_edx(w, X86_WIDTH_DWORD,
          &cross_page_slow_disp, &source_page_slow_disp) ||
      !emit_movabs_r10(w, (uint64_t)(uintptr_t)guest_to_host(CONFIG_MBASE)) ||
      !emit_mov_m32_r10_rdx_r11d(w) ||
      !emit_store_reg_eax(w, R_ESP) ||
      !emit_jmp_rel32_placeholder(w, &done_disp)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if ((src_slow_disp != NULL && !patch_rel32(src_slow_disp, slow_native)) ||
      !patch_rel32(dst_pmem_slow_disp, slow_native) ||
      !patch_rel32(cross_page_slow_disp, slow_native) ||
      !patch_rel32(source_page_slow_disp, slow_native) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  if (!insn->rm_is_reg) JIT_STAT_INC(native_pmem_loads);
  JIT_STAT_INC(native_pmem_stores);
  return patch_rel32(done_disp, w->cur);
}

static bool emit_native_push_rm_stack_guarded(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *src_slow_disp = NULL;
  uint8_t *done_disp = NULL;

  if (insn->width != X86_WIDTH_DWORD) return false;

  if (insn->rm_is_reg) {
    if (!emit_load_reg_r11d(w, insn->rm_reg)) return false;
  }
  else if (!emit_guest_ea_eax(w, &insn->ea) ||
      !emit_mov_edx_eax(w) ||
      !emit_direct_pmem_guard_edx(w, X86_WIDTH_DWORD, &src_slow_disp) ||
      !emit_movabs_r10(w, (uint64_t)(uintptr_t)guest_to_host(CONFIG_MBASE)) ||
      !emit_mov_r11d_m32_r10_rdx(w)) {
    return false;
  }

  if (!emit_load_reg_eax(w, R_ESP) ||
      !emit_add_eax_imm32(w, 0u - X86_WIDTH_DWORD) ||
      !emit_mov_edx_eax(w) ||
      !emit_stack_store_dword_r11d(w) ||
      !emit_store_reg_eax(w, R_ESP) ||
      !emit_jmp_rel32_placeholder(w, &done_disp)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if ((src_slow_disp != NULL && !patch_rel32(src_slow_disp, slow_native)) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  if (!insn->rm_is_reg) JIT_STAT_INC(native_pmem_loads);
  JIT_STAT_INC(native_pmem_stores);
  return patch_rel32(done_disp, w->cur);
}

static bool emit_native_leave(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *slow_disp = NULL;
  uint8_t *done_disp = NULL;

  if (insn->width != X86_WIDTH_DWORD) return false;

  if (!emit_load_reg_eax(w, R_EBP) ||
      !emit_mov_edx_eax(w) ||
      !emit_direct_pmem_guard_edx(w, X86_WIDTH_DWORD, &slow_disp) ||
      !emit_movabs_r10(w, (uint64_t)(uintptr_t)guest_to_host(CONFIG_MBASE)) ||
      !emit_mov_r11d_m32_r10_rdx(w) ||
      !emit_add_eax_imm32(w, X86_WIDTH_DWORD) ||
      !emit_store_reg_eax(w, R_ESP) ||
      !emit_mov_eax_r11d(w) ||
      !emit_store_reg_eax(w, R_EBP) ||
      !emit_jmp_rel32_placeholder(w, &done_disp)) {
    return false;
  }

  uint8_t *slow_native = w->cur;
  if (!patch_rel32(slow_disp, slow_native) ||
      !emit_helper_call(w, insn)) {
    return false;
  }

  JIT_STAT_INC(native_pmem_loads);
  return patch_rel32(done_disp, w->cur);
}

static bool emit_native_incdec_reg(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  if (!emit_native_incdec_reg_body(w, insn)) return false;

  JIT_STAT_INC(native_incdec_ops);
  return emit_capture_status_flags_custom(w, X86_EFLAGS_INCDEC_COPY_MASK, 0);
}

static bool emit_insn_regcached(x86_jit_writer_t *w,
    x86_jit_emit_ctx_t *ctx, const x86_jit_insn_t *insn) {
  uint8_t dst_host = 0;
  uint8_t src_host = 0;

  if (!jit_regcache_active(ctx) || insn->width != X86_WIDTH_DWORD) {
    return jit_regcache_flush_all(w, ctx) && emit_insn(w, insn);
  }

  switch (insn->op) {
    case X86_JIT_OP_NOP:
      return true;
    case X86_JIT_OP_MOV_IMM_REG:
      if (!jit_regcache_get_write(w, ctx, insn->dst, false, &dst_host, 0) ||
          !emit_mov_host_imm32(w, dst_host, insn->imm)) {
        return false;
      }
      jit_regcache_mark_dirty(ctx, insn->dst);
      return true;
    case X86_JIT_OP_MOV_REG_REG:
      if (insn->dst == insn->src) return true;
      if (!jit_regcache_get_read(w, ctx, insn->src, &src_host, 0) ||
          !jit_regcache_get_write(w, ctx, insn->dst, false, &dst_host,
              (uint16_t)(1u << src_host)) ||
          !emit_mov_host_host(w, dst_host, src_host)) {
        return false;
      }
      jit_regcache_mark_dirty(ctx, insn->dst);
      return true;
    case X86_JIT_OP_ALU_REG_REG:
      if (insn->alu_op == X86_ALU_ADC || insn->alu_op == X86_ALU_SBB) {
        return jit_regcache_flush_all(w, ctx) && emit_insn(w, insn);
      }
      if (!jit_regcache_get_read(w, ctx, insn->dst, &dst_host, 0) ||
          !jit_regcache_get_read(w, ctx, insn->src, &src_host,
              (uint16_t)(1u << dst_host)) ||
          !emit_alu_host_host(w, insn->alu_op, dst_host, src_host)) {
        return false;
      }
      if (jit_native_alu_writes_result(insn->alu_op)) {
        jit_regcache_mark_dirty(ctx, insn->dst);
      }
      JIT_STAT_INC(native_alu_ops);
      return emit_capture_status_flags_if_live(w,
          jit_native_alu_flag_copy_mask(insn->alu_op), insn->next_pc);
    case X86_JIT_OP_ALU_IMM_REG:
      if (insn->alu_op == X86_ALU_ADC || insn->alu_op == X86_ALU_SBB) {
        return jit_regcache_flush_all(w, ctx) && emit_insn(w, insn);
      }
      if (!jit_regcache_get_read(w, ctx, insn->dst, &dst_host, 0) ||
          !emit_alu_host_imm32(w, insn->alu_op, dst_host, insn->imm)) {
        return false;
      }
      if (jit_native_alu_writes_result(insn->alu_op)) {
        jit_regcache_mark_dirty(ctx, insn->dst);
      }
      JIT_STAT_INC(native_alu_ops);
      return emit_capture_status_flags_if_live(w,
          jit_native_alu_flag_copy_mask(insn->alu_op), insn->next_pc);
    case X86_JIT_OP_TEST_REG_REG:
      if (!jit_regcache_get_read(w, ctx, insn->dst, &dst_host, 0) ||
          !jit_regcache_get_read(w, ctx, insn->src, &src_host,
              (uint16_t)(1u << dst_host)) ||
          !emit_test_host_host(w, dst_host, src_host)) {
        return false;
      }
      JIT_STAT_INC(native_alu_ops);
      return emit_capture_status_flags_if_live(w, X86_EFLAGS_LOGIC_COPY_MASK,
          insn->next_pc);
    default:
      return jit_regcache_flush_all(w, ctx) && emit_insn(w, insn);
  }
}

/* Emit host code for insn; bytes below are x86-64 encodings. */
static bool emit_insn(x86_jit_writer_t *w, const x86_jit_insn_t *insn) {
  switch (insn->op) {
    case X86_JIT_OP_NOP:
      return true;
    case X86_JIT_OP_MOV_IMM_REG:
      return emit_store_reg_imm(w, insn->dst, insn->imm);
    case X86_JIT_OP_MOV_REG_REG:
      return emit_load_reg_eax(w, insn->src) && emit_store_reg_eax(w, insn->dst);
    case X86_JIT_OP_LEA:
      return emit_lea(w, insn);
    case X86_JIT_OP_ALU_REG_REG:
      return emit_native_alu_reg_reg(w, insn);
    case X86_JIT_OP_ALU_IMM_REG:
      return emit_native_alu_imm_reg(w, insn);
    case X86_JIT_OP_TEST_REG_REG:
      return emit_native_test_reg_reg(w, insn);
    case X86_JIT_OP_TEST_EAX_IMM:
      return emit_native_test_eax_imm(w, insn);
    case X86_JIT_OP_DOUBLE_SHIFT_REG_IMM:
      return emit_native_double_shift_reg_imm(w, insn);
    case X86_JIT_OP_CDQ:
      return emit_native_cdq(w, insn);
    case X86_JIT_OP_JMP_REL:
      return emit_native_jmp_rel(w, insn);
    case X86_JIT_OP_JCC_REL:
      return emit_native_jcc_rel(w, insn);
    case X86_JIT_OP_HELPER:
      /*
       * Paged guests need architectural address translation and fault ordering.
       * DTLB helper paths keep misses on the architectural MMU and only let
       * generated code dereference PMEM after page-table, permission, MMIO,
       * and self-modifying-code guards pass.
       */
      if (jit_paging_enabled() && jit_helper_may_touch_guest_memory(insn)) {
        if (insn->helper == X86_JIT_HELPER_MOV_REG_RM &&
            emit_paged_dtlb_mov_reg_rm_load(w, insn)) {
          return true;
        }
        if (insn->helper == X86_JIT_HELPER_MOV_RM_REG &&
            emit_paged_dtlb_mov_rm_reg_store(w, insn)) {
          return true;
        }
        if (insn->helper == X86_JIT_HELPER_MOV_IMM_RM &&
            emit_paged_dtlb_mov_imm_rm(w, insn)) {
          return true;
        }
        if (insn->helper == X86_JIT_HELPER_MOV_EAX_MOFFS &&
            emit_paged_dtlb_mov_eax_moffs(w, insn)) {
          return true;
        }
        if (insn->helper == X86_JIT_HELPER_MOV_MOFFS_EAX &&
            emit_paged_dtlb_mov_moffs_eax(w, insn)) {
          return true;
        }
        if (insn->helper == X86_JIT_HELPER_ALU_RM_REG &&
            emit_paged_dtlb_alu_rm_reg(w, insn)) {
          return true;
        }
        if (insn->helper == X86_JIT_HELPER_ALU_REG_RM &&
            emit_paged_dtlb_alu_reg_rm(w, insn)) {
          return true;
        }
        if (insn->helper == X86_JIT_HELPER_ALU_IMM_RM &&
            emit_paged_dtlb_alu_imm_rm(w, insn)) {
          return true;
        }
        if (insn->helper == X86_JIT_HELPER_TEST_RM_REG &&
            emit_paged_dtlb_test_rm_reg(w, insn)) {
          return true;
        }
        if (insn->helper == X86_JIT_HELPER_TEST_IMM_RM &&
            emit_paged_dtlb_test_imm_rm(w, insn)) {
          return true;
        }
        if ((insn->helper == X86_JIT_HELPER_MOVZX_REG_RM8 ||
                insn->helper == X86_JIT_HELPER_MOVZX_REG_RM16) &&
            emit_paged_dtlb_movzx_reg_rm(w, insn)) {
          return true;
        }
        if ((insn->helper == X86_JIT_HELPER_MOVSX_REG_RM8 ||
                insn->helper == X86_JIT_HELPER_MOVSX_REG_RM16) &&
            emit_paged_dtlb_movsx_reg_rm(w, insn)) {
          return true;
        }
        if (insn->helper == X86_JIT_HELPER_PUSH_REG &&
            emit_paged_dtlb_push_reg(w, insn)) {
          return true;
        }
        if (insn->helper == X86_JIT_HELPER_PUSH_IMM &&
            emit_paged_dtlb_push_imm(w, insn)) {
          return true;
        }
        if (insn->helper == X86_JIT_HELPER_POP_REG &&
            emit_paged_dtlb_pop_reg(w, insn)) {
          return true;
        }
        if (insn->helper == X86_JIT_HELPER_POP_RM &&
            emit_paged_dtlb_pop_rm(w, insn)) {
          return true;
        }
        if (insn->helper == X86_JIT_HELPER_CALL_REL &&
            emit_paged_dtlb_call_rel(w, insn)) {
          return true;
        }
        if (insn->helper == X86_JIT_HELPER_RET &&
            emit_paged_dtlb_ret(w, insn)) {
          return true;
        }
        if (insn->helper == X86_JIT_HELPER_PUSH_RM &&
            emit_paged_dtlb_push_rm(w, insn)) {
          return true;
        }
        if (insn->helper == X86_JIT_HELPER_CALL_RM &&
            emit_paged_dtlb_call_rm(w, insn)) {
          return true;
        }
        if (insn->helper == X86_JIT_HELPER_LEAVE &&
            emit_paged_dtlb_leave(w, insn)) {
          return true;
        }
        if (insn->helper == X86_JIT_HELPER_JMP_RM &&
            emit_paged_dtlb_jmp_rm(w, insn)) {
          return true;
        }
        if (insn->helper == X86_JIT_HELPER_INCDEC_RM &&
            emit_paged_dtlb_incdec_rm(w, insn)) {
          return true;
        }
        if (insn->helper == X86_JIT_HELPER_SHIFT_RM) {
          if (emit_paged_dtlb_shift_rm_cl(w, insn)) return true;
          if (emit_paged_dtlb_shift_rm_imm(w, insn)) return true;
        }
        if (insn->helper == X86_JIT_HELPER_NOT_RM &&
            emit_paged_dtlb_not_rm(w, insn)) {
          return true;
        }
        if (insn->helper == X86_JIT_HELPER_NEG_RM &&
            emit_paged_dtlb_neg_rm(w, insn)) {
          return true;
        }
        if (insn->helper == X86_JIT_HELPER_SETCC_RM8 &&
            emit_paged_dtlb_setcc_rm8(w, insn)) {
          return true;
        }
        if (insn->helper == X86_JIT_HELPER_IMUL_REG_RM &&
            emit_paged_dtlb_imul_reg_rm(w, insn)) {
          return true;
        }
        if (insn->helper == X86_JIT_HELPER_MUL_RM &&
            emit_paged_dtlb_mul_rm(w, insn)) {
          return true;
        }
        if (insn->helper == X86_JIT_HELPER_IMUL_ACC_RM &&
            emit_paged_dtlb_imul_acc_rm(w, insn)) {
          return true;
        }
        if (insn->helper == X86_JIT_HELPER_DIV_RM &&
            emit_paged_dtlb_div_rm(w, insn)) {
          return true;
        }
        if (insn->helper == X86_JIT_HELPER_IDIV_RM &&
            emit_paged_dtlb_idiv_rm(w, insn)) {
          return true;
        }
        return emit_helper_call(w, insn);
      }
      if (insn->helper == X86_JIT_HELPER_MOV_RM_REG) {
        if (emit_native_mov_rm_reg_store(w, insn)) return true;
      }
      if (insn->helper == X86_JIT_HELPER_MOV_REG_RM) {
        if (emit_native_mov_reg_rm_load(w, insn)) return true;
      }
      if (insn->helper == X86_JIT_HELPER_MOV_IMM_RM) {
        if (emit_native_mov_imm_rm(w, insn)) return true;
      }
      if (insn->helper == X86_JIT_HELPER_MOV_EAX_MOFFS) {
        if (emit_native_mov_eax_moffs(w, insn)) return true;
      }
      if (insn->helper == X86_JIT_HELPER_MOV_MOFFS_EAX) {
        if (emit_native_mov_moffs_eax(w, insn)) return true;
      }
      if (insn->helper == X86_JIT_HELPER_ALU_RM_REG) {
        if (emit_native_alu_rm_reg(w, insn)) return true;
      }
      if (insn->helper == X86_JIT_HELPER_ALU_REG_RM) {
        if (emit_native_alu_reg_rm(w, insn)) return true;
      }
      if (insn->helper == X86_JIT_HELPER_ALU_IMM_RM) {
        if (emit_native_alu_imm_rm(w, insn)) return true;
      }
      if (insn->helper == X86_JIT_HELPER_TEST_EAX_IMM) {
        if (emit_native_test_eax_imm(w, insn)) return true;
      }
      if (insn->helper == X86_JIT_HELPER_TEST_RM_REG) {
        if (emit_native_test_rm_reg(w, insn)) return true;
      }
      if (insn->helper == X86_JIT_HELPER_TEST_IMM_RM) {
        if (emit_native_test_imm_rm(w, insn)) return true;
      }
      if (insn->helper == X86_JIT_HELPER_JMP_RM) {
        if (emit_native_jmp_rm(w, insn)) return true;
      }
      if (insn->helper == X86_JIT_HELPER_INCDEC_REG) {
        if (emit_native_incdec_reg(w, insn)) return true;
      }
      if (insn->helper == X86_JIT_HELPER_INCDEC_RM) {
        if (emit_native_incdec_rm(w, insn)) return true;
      }
      if (insn->helper == X86_JIT_HELPER_PUSH_IMM) {
        if (emit_native_push_imm(w, insn)) return true;
      }
      if (insn->helper == X86_JIT_HELPER_PUSH_RM) {
        if (emit_native_push_rm(w, insn)) return true;
      }
      if (insn->helper == X86_JIT_HELPER_PUSH_REG) {
        if (emit_native_push_reg(w, insn)) return true;
      }
      if (insn->helper == X86_JIT_HELPER_POP_REG) {
        if (emit_native_pop_reg(w, insn)) return true;
      }
      if (insn->helper == X86_JIT_HELPER_CALL_REL) {
        if (emit_native_call_rel(w, insn)) return true;
      }
      if (insn->helper == X86_JIT_HELPER_RET) {
        if (emit_native_ret(w, insn)) return true;
      }
      if (insn->helper == X86_JIT_HELPER_LEAVE) {
        if (emit_native_leave(w, insn)) return true;
      }
      if (insn->helper == X86_JIT_HELPER_MUL_RM) {
        if (emit_native_mul_rm(w, insn)) return true;
      }
      if (insn->helper == X86_JIT_HELPER_IMUL_ACC_RM) {
        if (emit_native_imul_acc_rm(w, insn)) return true;
      }
      if (insn->helper == X86_JIT_HELPER_DIV_RM) {
        if (emit_native_div_rm(w, insn)) return true;
      }
      if (insn->helper == X86_JIT_HELPER_IDIV_RM) {
        if (emit_native_idiv_rm(w, insn)) return true;
      }
      if (insn->helper == X86_JIT_HELPER_IMUL_REG_RM) {
        if (emit_native_imul_reg_rm(w, insn)) return true;
      }
      if (insn->helper == X86_JIT_HELPER_SHIFT_RM) {
        if (emit_native_shift_rm_cl(w, insn)) return true;
        if (emit_native_shift_reg_imm(w, insn)) return true;
      }
      if (insn->helper == X86_JIT_HELPER_NOT_RM) {
        if (emit_native_not_rm(w, insn)) return true;
      }
      if (insn->helper == X86_JIT_HELPER_NEG_RM) {
        if (emit_native_neg_rm(w, insn)) return true;
      }
      if (insn->helper == X86_JIT_HELPER_MOVZX_REG_RM8 ||
          insn->helper == X86_JIT_HELPER_MOVZX_REG_RM16) {
        if (emit_native_movzx_reg_rm(w, insn)) return true;
      }
      if (insn->helper == X86_JIT_HELPER_MOVSX_REG_RM8 ||
          insn->helper == X86_JIT_HELPER_MOVSX_REG_RM16) {
        if (emit_native_movsx_reg_rm(w, insn)) return true;
      }
      if (insn->helper == X86_JIT_HELPER_SETCC_RM8) {
        if (emit_native_setcc_rm8(w, insn)) return true;
      }
      return emit_helper_call(w, insn);
    default:
      return false;
  }
}

static bool jit_is_chainable_jcc_backedge(const x86_jit_insn_t *insn,
    vaddr_t block_pc) {
  uint32_t flag = 0;
  uint8_t host_cc = 0;
  const bool is_jcc = (insn->op == X86_JIT_OP_JCC_REL) ||
      (insn->op == X86_JIT_OP_HELPER &&
          insn->helper == X86_JIT_HELPER_JCC_REL);
  return is_jcc &&
         jit_backedge_flag_test(insn->cc, &flag, &host_cc) &&
         jit_branch_target(insn) == block_pc;
}

static bool jit_is_incdec_resident_jcc_backedge(const x86_jit_insn_t *insn,
    vaddr_t block_pc) {
  uint8_t host_cc = 0;
  const bool is_jcc = (insn->op == X86_JIT_OP_JCC_REL) ||
      (insn->op == X86_JIT_OP_HELPER &&
          insn->helper == X86_JIT_HELPER_JCC_REL);
  return is_jcc &&
         jit_incdec_jcc_host_cc(insn->cc, &host_cc) &&
         jit_branch_target(insn) == block_pc;
}

static bool jit_is_any_jcc_backedge(const x86_jit_insn_t *insn,
    vaddr_t block_pc) {
  const bool is_jcc = (insn->op == X86_JIT_OP_JCC_REL) ||
      (insn->op == X86_JIT_OP_HELPER &&
          insn->helper == X86_JIT_HELPER_JCC_REL);
  return is_jcc && jit_branch_target(insn) == block_pc;
}

/* Detect CMP forms that compare against a specific resident loop register. */
static bool jit_is_cmp_with_reg(const x86_jit_insn_t *insn, uint8_t reg) {
  if (insn->width != X86_WIDTH_DWORD || insn->alu_op != X86_ALU_CMP) {
    return false;
  }

  if (insn->op == X86_JIT_OP_ALU_IMM_REG) return insn->dst == reg;
  if (insn->op == X86_JIT_OP_ALU_REG_REG) {
    return insn->dst == reg || insn->src == reg;
  }

  return false;
}

/* Detect register INC/DEC helpers that can be lowered as a resident loop update. */
static bool jit_is_native_incdec_reg(const x86_jit_insn_t *insn) {
  return insn->op == X86_JIT_OP_HELPER &&
         insn->helper == X86_JIT_HELPER_INCDEC_REG &&
         insn->width == X86_WIDTH_DWORD &&
         (insn->alu_op == X86_ALU_ADD || insn->alu_op == X86_ALU_SUB);
}

/* Decode the IA-32 ModR/M byte into mod, reg/opcode, and r/m fields. */
static bool jit_decode_modrm(x86_jit_reader_t *r, uint8_t *mod, uint8_t *reg,
    uint8_t *rm) {
  uint8_t modrm = 0;
  if (!jit_read_u8(r, &modrm)) return false;

  *mod = modrm >> 6;
  *reg = (modrm >> 3) & 0x7u;
  *rm = modrm & 0x7u;
  return true;
}

/*
 * Decode a 32-bit IA-32 effective address.  rm=4 selects a SIB byte; rm=5 with
 * mod=0 means disp32 with no base.  In SIB, index=4 means no index and base=5
 * with mod=0 again means no base plus disp32.
 */
static bool jit_decode_ea32(x86_jit_reader_t *r, uint8_t mod, uint8_t rm,
    x86_jit_ea_t *ea) {
  ea->base_reg = -1;
  ea->index_reg = -1;
  ea->scale = 0;
  ea->disp = 0;

  if (mod == 3) return false;

  if (rm == 4) {
    uint8_t sib = 0;
    if (!jit_read_u8(r, &sib)) return false;

    const uint8_t base = sib & 0x7u;
    const uint8_t index = (sib >> 3) & 0x7u;
    ea->scale = sib >> 6;
    if (index != 4) ea->index_reg = index;
    if (!(mod == 0 && base == 5)) ea->base_reg = base;
  }
  else if (!(mod == 0 && rm == 5)) {
    ea->base_reg = rm;
  }

  if (mod == 0) {
    if (rm == 5 || (rm == 4 && ea->base_reg < 0)) {
      if (!jit_read_u32(r, &ea->disp)) return false;
    }
  }
  else if (mod == 1) {
    int32_t disp = 0;
    if (!jit_read_i8(r, &disp)) return false;
    ea->disp = (uint32_t)disp;
  }
  else if (mod == 2) {
    if (!jit_read_u32(r, &ea->disp)) return false;
  }

  return true;
}

/* Attach either a register r/m operand or a decoded memory effective address. */
static bool jit_decode_rm_operand(x86_jit_reader_t *r, uint8_t mod,
    uint8_t rm, x86_jit_insn_t *out) {
  out->rm_is_reg = mod == 3;
  out->rm_reg = rm;
  if (mod == 3) return true;
  return jit_decode_ea32(r, mod, rm, &out->ea);
}

/* Finish decode by recording the first byte after this instruction. */
static bool jit_finish_decode(x86_jit_reader_t *r, x86_jit_insn_t *out) {
  out->next_pc = r->cur;
  return true;
}

/* Mark a decoded instruction as helper-backed. */
static void jit_mark_helper(x86_jit_insn_t *out, x86_jit_helper_t helper) {
  out->op = X86_JIT_OP_HELPER;
  out->helper = helper;
}

/* Convert the opcode's Group-1 bits 5..3 into the local ALU enum. */
static int jit_alu_from_opcode(uint8_t opcode) {
  switch (opcode & 0x38u) {
    case 0x00: return X86_ALU_ADD;
    case 0x08: return X86_ALU_OR;
    case 0x10: return X86_ALU_ADC;
    case 0x18: return X86_ALU_SBB;
    case 0x20: return X86_ALU_AND;
    case 0x28: return X86_ALU_SUB;
    case 0x30: return X86_ALU_XOR;
    case 0x38: return X86_ALU_CMP;
    default: return -1;
  }
}

/*
 * Decode the supported IA-32 subset into the compact JIT IR.  The decoder keeps
 * unsupported opcodes side-effect free: if it returns false, the interpreter
 * will execute from the original PC.
 */
static bool jit_decode_insn(x86_jit_reader_t *r, x86_jit_insn_t *out) {
  memset(out, 0, sizeof(*out));
  out->pc = r->cur;
  out->width = X86_WIDTH_DWORD;

  uint8_t opcode = 0;
  if (!jit_read_u8(r, &opcode)) return false;

  if (opcode == 0x66) {
    out->width = X86_WIDTH_WORD;
    if (!jit_read_u8(r, &opcode)) return false;
  }

  if (opcode == 0x90) {
    out->op = X86_JIT_OP_NOP;
    return jit_finish_decode(r, out);
  }

  if (opcode >= 0xb8 && opcode <= 0xbf) {
    out->dst = opcode & 0x7u;
    if (out->width == X86_WIDTH_DWORD) {
      out->op = X86_JIT_OP_MOV_IMM_REG;
      return jit_read_u32(r, &out->imm) && jit_finish_decode(r, out);
    }

    out->rm_is_reg = true;
    out->rm_reg = out->dst;
    jit_mark_helper(out, X86_JIT_HELPER_MOV_IMM_RM);
    return jit_read_u16(r, &out->imm) && jit_finish_decode(r, out);
  }

  if (opcode >= 0x40 && opcode <= 0x47) {
    jit_mark_helper(out, X86_JIT_HELPER_INCDEC_REG);
    out->dst = opcode & 0x7u;
    out->alu_op = X86_ALU_ADD;
    return jit_finish_decode(r, out);
  }

  if (opcode >= 0x48 && opcode <= 0x4f) {
    jit_mark_helper(out, X86_JIT_HELPER_INCDEC_REG);
    out->dst = opcode & 0x7u;
    out->alu_op = X86_ALU_SUB;
    return jit_finish_decode(r, out);
  }

  if (opcode >= 0x50 && opcode <= 0x57) {
    if (out->width != X86_WIDTH_DWORD) return false;
    jit_mark_helper(out, X86_JIT_HELPER_PUSH_REG);
    out->src = opcode & 0x7u;
    return jit_finish_decode(r, out);
  }

  if (opcode >= 0x58 && opcode <= 0x5f) {
    if (out->width != X86_WIDTH_DWORD) return false;
    jit_mark_helper(out, X86_JIT_HELPER_POP_REG);
    out->dst = opcode & 0x7u;
    return jit_finish_decode(r, out);
  }

  if (opcode >= 0x70 && opcode <= 0x7f) {
    if (out->width != X86_WIDTH_DWORD) return false;
    int32_t rel = 0;
    if (!jit_read_i8(r, &rel)) return false;
    out->cc = opcode & 0xfu;
    out->rel = rel;
    out->ends_block = true;
    if (jit_jcc_native_supported(out->cc)) {
      out->op = X86_JIT_OP_JCC_REL;
    }
    else {
      jit_mark_helper(out, X86_JIT_HELPER_JCC_REL);
    }
    return jit_finish_decode(r, out);
  }

  if (opcode == 0x88 || opcode == 0x8a) {
    uint8_t mod = 0, reg = 0, rm = 0;
    if (!jit_decode_modrm(r, &mod, &reg, &rm) ||
        !jit_decode_rm_operand(r, mod, rm, out)) {
      return false;
    }

    out->width = X86_WIDTH_BYTE;
    if (opcode == 0x88) {
      out->src = reg;
      jit_mark_helper(out, X86_JIT_HELPER_MOV_RM_REG);
    }
    else {
      out->dst = reg;
      jit_mark_helper(out, X86_JIT_HELPER_MOV_REG_RM);
    }
    return jit_finish_decode(r, out);
  }

  if (opcode == 0x89 || opcode == 0x8b) {
    uint8_t mod = 0, reg = 0, rm = 0;
    if (!jit_decode_modrm(r, &mod, &reg, &rm) ||
        !jit_decode_rm_operand(r, mod, rm, out)) {
      return false;
    }

    if (opcode == 0x89) {
      out->src = reg;
      if (mod == 3 && out->width == X86_WIDTH_DWORD) {
        out->op = X86_JIT_OP_MOV_REG_REG;
        out->dst = rm;
      }
      else {
        jit_mark_helper(out, X86_JIT_HELPER_MOV_RM_REG);
      }
    }
    else {
      out->dst = reg;
      if (mod == 3 && out->width == X86_WIDTH_DWORD) {
        out->op = X86_JIT_OP_MOV_REG_REG;
        out->src = rm;
      }
      else {
        jit_mark_helper(out, X86_JIT_HELPER_MOV_REG_RM);
      }
    }
    return jit_finish_decode(r, out);
  }

  if (opcode == 0x8d) {
    if (out->width != X86_WIDTH_DWORD) return false;
    uint8_t mod = 0, reg = 0, rm = 0;
    if (!jit_decode_modrm(r, &mod, &reg, &rm) ||
        !jit_decode_ea32(r, mod, rm, &out->ea)) {
      return false;
    }

    out->op = X86_JIT_OP_LEA;
    out->dst = reg;
    return jit_finish_decode(r, out);
  }

  if (opcode == 0x8f) {
    if (out->width != X86_WIDTH_DWORD) return false;
    uint8_t mod = 0, reg = 0, rm = 0;
    if (!jit_decode_modrm(r, &mod, &reg, &rm) ||
        reg != 0 ||
        !jit_decode_rm_operand(r, mod, rm, out)) {
      return false;
    }

    jit_mark_helper(out, X86_JIT_HELPER_POP_RM);
    return jit_finish_decode(r, out);
  }

  if (opcode == 0x99) {
    if (out->width != X86_WIDTH_DWORD) return false;
    out->op = X86_JIT_OP_CDQ;
    return jit_finish_decode(r, out);
  }

  const int alu_op = jit_alu_from_opcode(opcode);
  if (opcode < 0x40 && alu_op >= 0 &&
      ((opcode & 0x07u) <= 0x03u)) {
    uint8_t mod = 0, reg = 0, rm = 0;
    if (!jit_decode_modrm(r, &mod, &reg, &rm) ||
        !jit_decode_rm_operand(r, mod, rm, out)) {
      return false;
    }

    out->alu_op = (uint8_t)alu_op;
    const uint8_t form = opcode & 0x07u;
    if (form == 0x00u || form == 0x02u) {
      out->width = X86_WIDTH_BYTE;
    }
    if (form == 0x00u || form == 0x01u) {
      out->src = reg;
      if (mod == 3 && out->width == X86_WIDTH_DWORD) {
        out->op = X86_JIT_OP_ALU_REG_REG;
        out->dst = rm;
      }
      else {
        jit_mark_helper(out, X86_JIT_HELPER_ALU_RM_REG);
      }
    }
    else {
      out->src = rm;
      out->dst = reg;
      if (mod == 3 && out->width == X86_WIDTH_DWORD) {
        out->op = X86_JIT_OP_ALU_REG_REG;
      }
      else {
        out->src = reg;
        jit_mark_helper(out, X86_JIT_HELPER_ALU_REG_RM);
      }
    }
    return jit_finish_decode(r, out);
  }

  if (opcode == 0x83 || opcode == 0x81 || opcode == 0x80) {
    uint8_t mod = 0, reg = 0, rm = 0;
    if (!jit_decode_modrm(r, &mod, &reg, &rm) ||
        !jit_decode_rm_operand(r, mod, rm, out)) {
      return false;
    }

    out->alu_op = reg;
    out->width = opcode == 0x80 ? X86_WIDTH_BYTE : out->width;
    if (opcode == 0x81) {
      if (out->width == X86_WIDTH_DWORD) {
        if (!jit_read_u32(r, &out->imm)) return false;
      }
      else if (!jit_read_u16(r, &out->imm)) {
        return false;
      }
    }
    else if (opcode == 0x83) {
      int32_t imm = 0;
      if (!jit_read_i8(r, &imm)) return false;
      out->imm = (uint32_t)imm;
    }
    else {
      uint8_t imm = 0;
      if (!jit_read_u8(r, &imm)) return false;
      out->imm = imm;
    }

    if (out->width == X86_WIDTH_DWORD && out->rm_is_reg) {
      out->op = X86_JIT_OP_ALU_IMM_REG;
      out->dst = rm;
    }
    else {
      jit_mark_helper(out, X86_JIT_HELPER_ALU_IMM_RM);
    }
    return jit_finish_decode(r, out);
  }

  if (opcode == 0xc0 || opcode == 0xc1 ||
      opcode == 0xd0 || opcode == 0xd1 ||
      opcode == 0xd2 || opcode == 0xd3) {
    uint8_t mod = 0, reg = 0, rm = 0;
    if (!jit_decode_modrm(r, &mod, &reg, &rm) ||
        !jit_decode_rm_operand(r, mod, rm, out)) {
      return false;
    }

    out->width = (opcode == 0xc0 || opcode == 0xd0 || opcode == 0xd2) ?
        X86_WIDTH_BYTE : out->width;
    out->alu_op = reg;
    if (opcode == 0xc0 || opcode == 0xc1) {
      uint8_t imm = 0;
      if (!jit_read_u8(r, &imm)) return false;
      out->imm = imm;
    }
    else if (opcode == 0xd2 || opcode == 0xd3) {
      out->count_from_cl = true;
    }
    else {
      out->imm = 1;
    }
    jit_mark_helper(out, X86_JIT_HELPER_SHIFT_RM);
    return jit_finish_decode(r, out);
  }

  if (opcode == 0xc7 || opcode == 0xc6) {
    uint8_t mod = 0, reg = 0, rm = 0;
    if (!jit_decode_modrm(r, &mod, &reg, &rm) ||
        reg != 0 ||
        !jit_decode_rm_operand(r, mod, rm, out)) {
      return false;
    }

    out->width = opcode == 0xc6 ? X86_WIDTH_BYTE : out->width;
    if (out->width == X86_WIDTH_DWORD) {
      if (!jit_read_u32(r, &out->imm)) return false;
    }
    else if (out->width == X86_WIDTH_WORD) {
      if (!jit_read_u16(r, &out->imm)) return false;
    }
    else {
      uint8_t imm = 0;
      if (!jit_read_u8(r, &imm)) return false;
      out->imm = imm;
    }
    jit_mark_helper(out, X86_JIT_HELPER_MOV_IMM_RM);
    return jit_finish_decode(r, out);
  }

  if (opcode == 0x84 || opcode == 0x85) {
    uint8_t mod = 0, reg = 0, rm = 0;
    if (!jit_decode_modrm(r, &mod, &reg, &rm) ||
        !jit_decode_rm_operand(r, mod, rm, out)) {
      return false;
    }

    out->width = opcode == 0x84 ? X86_WIDTH_BYTE : out->width;
    out->src = reg;
    if (mod == 3 && out->width == X86_WIDTH_DWORD) {
      out->op = X86_JIT_OP_TEST_REG_REG;
      out->dst = rm;
    }
    else {
      jit_mark_helper(out, X86_JIT_HELPER_TEST_RM_REG);
    }
    return jit_finish_decode(r, out);
  }

  if (opcode == 0xa0 || opcode == 0xa1 || opcode == 0xa2 || opcode == 0xa3) {
    if (!jit_read_u32(r, &out->imm)) return false;
    out->width = (opcode == 0xa0 || opcode == 0xa2) ?
        X86_WIDTH_BYTE : out->width;
    jit_mark_helper(out, (opcode == 0xa0 || opcode == 0xa1) ?
        X86_JIT_HELPER_MOV_EAX_MOFFS : X86_JIT_HELPER_MOV_MOFFS_EAX);
    return jit_finish_decode(r, out);
  }

  if (opcode == 0xa8 || opcode == 0xa9) {
    out->width = opcode == 0xa8 ? X86_WIDTH_BYTE : out->width;
    if (out->width == X86_WIDTH_DWORD) {
      if (!jit_read_u32(r, &out->imm)) return false;
    }
    else if (out->width == X86_WIDTH_WORD) {
      if (!jit_read_u16(r, &out->imm)) return false;
    }
    else {
      uint8_t imm = 0;
      if (!jit_read_u8(r, &imm)) return false;
      out->imm = imm;
    }
    if (out->width == X86_WIDTH_DWORD) {
      out->op = X86_JIT_OP_TEST_EAX_IMM;
    }
    else {
      jit_mark_helper(out, X86_JIT_HELPER_TEST_EAX_IMM);
    }
    return jit_finish_decode(r, out);
  }

  if ((opcode & 0xc7u) == 0x05u && jit_alu_from_opcode(opcode) >= 0) {
    if (out->width == X86_WIDTH_DWORD) {
      if (!jit_read_u32(r, &out->imm)) return false;
    }
    else if (!jit_read_u16(r, &out->imm)) {
      return false;
    }
    out->alu_op = (uint8_t)jit_alu_from_opcode(opcode);
    if (out->width == X86_WIDTH_DWORD) {
      out->op = X86_JIT_OP_ALU_IMM_REG;
      out->dst = R_EAX;
    }
    else {
      jit_mark_helper(out, X86_JIT_HELPER_ALU_EAX_IMM);
    }
    return jit_finish_decode(r, out);
  }

  if ((opcode & 0xc7u) == 0x04u && jit_alu_from_opcode(opcode) >= 0) {
    uint8_t imm = 0;
    if (!jit_read_u8(r, &imm)) return false;

    out->width = X86_WIDTH_BYTE;
    out->imm = imm;
    out->alu_op = (uint8_t)jit_alu_from_opcode(opcode);
    out->op = X86_JIT_OP_ALU_IMM_REG;
    out->dst = R_EAX;
    return jit_finish_decode(r, out);
  }

  if (opcode == 0xfe || opcode == 0xff) {
    uint8_t mod = 0, reg = 0, rm = 0;
    if (!jit_decode_modrm(r, &mod, &reg, &rm) ||
        !jit_decode_rm_operand(r, mod, rm, out)) {
      return false;
    }

    out->width = opcode == 0xfe ? X86_WIDTH_BYTE : out->width;
    if (reg == 0 || reg == 1) {
      out->alu_op = reg == 0 ? X86_ALU_ADD : X86_ALU_SUB;
      jit_mark_helper(out, X86_JIT_HELPER_INCDEC_RM);
      return jit_finish_decode(r, out);
    }
    if (opcode == 0xfe) return false;
    if (out->width != X86_WIDTH_DWORD) return false;
    if (reg == 2 || reg == 4) {
      out->ends_block = true;
      jit_mark_helper(out, reg == 2 ?
          X86_JIT_HELPER_CALL_RM : X86_JIT_HELPER_JMP_RM);
      return jit_finish_decode(r, out);
    }
    if (reg == 6) {
      jit_mark_helper(out, X86_JIT_HELPER_PUSH_RM);
      return jit_finish_decode(r, out);
    }
    return false;
  }

  if (opcode == 0xf6 || opcode == 0xf7) {
    uint8_t mod = 0, reg = 0, rm = 0;
    if (!jit_decode_modrm(r, &mod, &reg, &rm) ||
        !jit_decode_rm_operand(r, mod, rm, out)) {
      return false;
    }

    out->width = opcode == 0xf6 ? X86_WIDTH_BYTE : out->width;
    if (reg == 0) {
      if (out->width == X86_WIDTH_DWORD) {
        if (!jit_read_u32(r, &out->imm)) return false;
      }
      else if (out->width == X86_WIDTH_WORD) {
        if (!jit_read_u16(r, &out->imm)) return false;
      }
      else {
        uint8_t imm = 0;
        if (!jit_read_u8(r, &imm)) return false;
        out->imm = imm;
      }
      jit_mark_helper(out, X86_JIT_HELPER_TEST_IMM_RM);
      return jit_finish_decode(r, out);
    }

    if (reg == 2 || reg == 3) {
      jit_mark_helper(out, reg == 2 ?
          X86_JIT_HELPER_NOT_RM : X86_JIT_HELPER_NEG_RM);
      return jit_finish_decode(r, out);
    }
    if (reg == 4 || reg == 5 || reg == 6 || reg == 7) {
      static const x86_jit_helper_t gp3_helpers[] = {
        X86_JIT_HELPER_MUL_RM,
        X86_JIT_HELPER_IMUL_ACC_RM,
        X86_JIT_HELPER_DIV_RM,
        X86_JIT_HELPER_IDIV_RM,
      };
      jit_mark_helper(out, gp3_helpers[reg - 4u]);
      return jit_finish_decode(r, out);
    }
    return false;
  }

  if (opcode == 0x68) {
    if (out->width != X86_WIDTH_DWORD) return false;
    if (!jit_read_u32(r, &out->imm)) return false;
    jit_mark_helper(out, X86_JIT_HELPER_PUSH_IMM);
    return jit_finish_decode(r, out);
  }

  if (opcode == 0x6a) {
    if (out->width != X86_WIDTH_DWORD) return false;
    int32_t imm = 0;
    if (!jit_read_i8(r, &imm)) return false;
    out->imm = (uint32_t)imm;
    jit_mark_helper(out, X86_JIT_HELPER_PUSH_IMM);
    return jit_finish_decode(r, out);
  }

  if (opcode == 0xe8 || opcode == 0xe9) {
    if (out->width != X86_WIDTH_DWORD) return false;
    uint32_t rel = 0;
    if (!jit_read_u32(r, &rel)) return false;
    out->rel = (int32_t)rel;
    out->ends_block = true;
    if (opcode == 0xe9) {
      out->op = X86_JIT_OP_JMP_REL;
    }
    else {
      jit_mark_helper(out, X86_JIT_HELPER_CALL_REL);
    }
    return jit_finish_decode(r, out);
  }

  if (opcode == 0xeb) {
    if (out->width != X86_WIDTH_DWORD) return false;
    int32_t rel = 0;
    if (!jit_read_i8(r, &rel)) return false;
    out->rel = rel;
    out->ends_block = true;
    out->op = X86_JIT_OP_JMP_REL;
    return jit_finish_decode(r, out);
  }

  if (opcode == 0xc3) {
    if (out->width != X86_WIDTH_DWORD) return false;
    out->ends_block = true;
    jit_mark_helper(out, X86_JIT_HELPER_RET);
    return jit_finish_decode(r, out);
  }

  if (opcode == 0xc9) {
    if (out->width != X86_WIDTH_DWORD) return false;
    jit_mark_helper(out, X86_JIT_HELPER_LEAVE);
    return jit_finish_decode(r, out);
  }

  if (opcode == 0x0f) {
    uint8_t opcode2 = 0;
    if (!jit_read_u8(r, &opcode2)) return false;

    if (opcode2 >= 0x80 && opcode2 <= 0x8f) {
      if (out->width != X86_WIDTH_DWORD) return false;
      uint32_t rel = 0;
      if (!jit_read_u32(r, &rel)) return false;
      out->cc = opcode2 & 0xfu;
      out->rel = (int32_t)rel;
      out->ends_block = true;
      if (jit_jcc_native_supported(out->cc)) {
        out->op = X86_JIT_OP_JCC_REL;
      }
      else {
        jit_mark_helper(out, X86_JIT_HELPER_JCC_REL);
      }
      return jit_finish_decode(r, out);
    }

    if (opcode2 >= 0x90 && opcode2 <= 0x9f) {
      uint8_t mod = 0, reg = 0, rm = 0;
      if (!jit_decode_modrm(r, &mod, &reg, &rm) ||
          !jit_decode_rm_operand(r, mod, rm, out)) {
        return false;
      }
      (void)reg;
      out->cc = opcode2 & 0xfu;
      out->width = X86_WIDTH_BYTE;
      jit_mark_helper(out, X86_JIT_HELPER_SETCC_RM8);
      return jit_finish_decode(r, out);
    }

    if (opcode2 == 0xaf) {
      uint8_t mod = 0, reg = 0, rm = 0;
      if (!jit_decode_modrm(r, &mod, &reg, &rm) ||
          !jit_decode_rm_operand(r, mod, rm, out)) {
        return false;
      }
      out->dst = reg;
      jit_mark_helper(out, X86_JIT_HELPER_IMUL_REG_RM);
      return jit_finish_decode(r, out);
    }

    if (opcode2 == 0xa4 || opcode2 == 0xac) {
      if (out->width != X86_WIDTH_DWORD) return false;
      uint8_t mod = 0, reg = 0, rm = 0;
      if (!jit_decode_modrm(r, &mod, &reg, &rm) || mod != 3) {
        return false;
      }
      uint8_t imm = 0;
      if (!jit_read_u8(r, &imm)) return false;
      out->op = X86_JIT_OP_DOUBLE_SHIFT_REG_IMM;
      out->alu_op = opcode2 == 0xac ? 1u : 0u;
      out->dst = rm;
      out->src = reg;
      out->imm = imm;
      return jit_finish_decode(r, out);
    }

    if ((opcode2 == 0xb6 || opcode2 == 0xb7) &&
        out->width == X86_WIDTH_DWORD) {
      uint8_t mod = 0, reg = 0, rm = 0;
      if (!jit_decode_modrm(r, &mod, &reg, &rm) ||
          !jit_decode_rm_operand(r, mod, rm, out)) {
        return false;
      }
      out->dst = reg;
      jit_mark_helper(out, opcode2 == 0xb6 ?
          X86_JIT_HELPER_MOVZX_REG_RM8 : X86_JIT_HELPER_MOVZX_REG_RM16);
      return jit_finish_decode(r, out);
    }

    if ((opcode2 == 0xbe &&
            (out->width == X86_WIDTH_WORD ||
             out->width == X86_WIDTH_DWORD)) ||
        (opcode2 == 0xbf && out->width == X86_WIDTH_DWORD)) {
      uint8_t mod = 0, reg = 0, rm = 0;
      if (!jit_decode_modrm(r, &mod, &reg, &rm) ||
          !jit_decode_rm_operand(r, mod, rm, out)) {
        return false;
      }
      out->dst = reg;
      jit_mark_helper(out, opcode2 == 0xbe ?
          X86_JIT_HELPER_MOVSX_REG_RM8 : X86_JIT_HELPER_MOVSX_REG_RM16);
      return jit_finish_decode(r, out);
    }
  }

  return false;
}

/* Initialise per-block emission state and invalidate every register-cache slot. */
static void jit_emit_ctx_init(x86_jit_emit_ctx_t *ctx) {
  memset(ctx, 0, sizeof(*ctx));
  ctx->valid = true;
  ctx->has_cpu_base = jit_regcache_enabled &&
      (!jit_paging_enabled() || jit_paged_regcache_enabled) &&
      jit_batch_cpu_base_available();
  for (uint32_t i = 0; i < 8u; i++) ctx->guest_to_host[i] = -1;
  for (uint32_t i = 0; i < 16u; i++) ctx->host_to_guest[i] = -1;
  ctx->flags.kind = X86_LAZY_FLAGS_MATERIALISED;
}

/* Decode a straight-line block until unsupported decode, limit, or control flow. */
static bool jit_decode_block(vaddr_t pc, uint32_t max_insns,
    x86_jit_insn_t *insns, uint32_t *count_out, vaddr_t *end_pc_out) {
  x86_jit_reader_t r = { .pc = pc, .cur = pc };
  uint32_t count = 0;

  while (count < X86_JIT_BLOCK_MAX_INSNS && count < max_insns) {
    if ((uint32_t)(r.cur - pc) >= X86_JIT_MAX_SOURCE_BYTES) break;

    x86_jit_reader_t probe = r;
    x86_jit_insn_t insn;
    if (!jit_decode_insn(&probe, &insn)) break;
    if (insn.op == X86_JIT_OP_HELPER && !jit_helper_translation_enabled()) {
      break;
    }
    if ((uint32_t)(probe.cur - pc) > X86_JIT_MAX_SOURCE_BYTES) break;

    insn.ordinal = (uint16_t)(count + 1u);
    insns[count++] = insn;
    r = probe;
    if (insn.ends_block) break;
  }

  *count_out = count;
  *end_pc_out = r.cur;
  return count != 0;
}

/* Summarise helper/memory properties needed before choosing emission fast paths. */
static void jit_analyse_block(const x86_jit_insn_t *insns, uint32_t count,
    x86_jit_emit_ctx_t *ctx) {
  for (uint32_t i = 0; i < count; i++) {
    const x86_jit_insn_t *insn = &insns[i];
    if (insn->op == X86_JIT_OP_HELPER) {
      ctx->may_call_helper = true;
      if (jit_helper_may_touch_guest_memory(insn)) ctx->may_touch_pmem = true;
    }
  }
}

/* Expand the tracked ESP-relative window by one 32-bit load or store slot. */
static void jit_stack_window_note_access(x86_jit_stack_window_t *window,
    int32_t offset, bool is_store) {
  if (!window->valid) {
    window->valid = true;
    window->min_offset = offset;
    window->max_offset = offset;
  }
  else {
    if (offset < window->min_offset) window->min_offset = offset;
    if (offset > window->max_offset) window->max_offset = offset;
  }

  if (is_store) {
    if (!window->has_store) {
      window->has_store = true;
      window->store_min_offset = offset;
      window->store_max_offset = offset;
    }
    else {
      if (offset < window->store_min_offset) window->store_min_offset = offset;
      if (offset > window->store_max_offset) window->store_max_offset = offset;
    }
  }
  else {
    window->has_load = true;
  }
}

/* Return true if an instruction writes ESP in a way the stack-window analysis cannot fold. */
static bool jit_stack_window_guest_esp_write(const x86_jit_insn_t *insn) {
  switch (insn->op) {
    case X86_JIT_OP_MOV_IMM_REG:
    case X86_JIT_OP_MOV_REG_REG:
    case X86_JIT_OP_LEA:
    case X86_JIT_OP_ALU_REG_REG:
    case X86_JIT_OP_ALU_IMM_REG:
    case X86_JIT_OP_DOUBLE_SHIFT_REG_IMM:
      return insn->dst == R_ESP;
    case X86_JIT_OP_HELPER:
      switch (insn->helper) {
        case X86_JIT_HELPER_MOV_REG_RM:
        case X86_JIT_HELPER_MOVZX_REG_RM8:
        case X86_JIT_HELPER_MOVZX_REG_RM16:
        case X86_JIT_HELPER_MOVSX_REG_RM8:
        case X86_JIT_HELPER_MOVSX_REG_RM16:
        case X86_JIT_HELPER_IMUL_REG_RM:
          return insn->dst == R_ESP;
        case X86_JIT_HELPER_POP_RM:
          return true;
        default:
          return false;
      }
    default:
      return false;
  }
}

static bool jit_stack_window_reject(bool paged) {
  if (paged) JIT_STAT_INC(stack_fast_fallbacks);
  return false;
}

static bool jit_analyse_stack_window(const x86_jit_insn_t *insns,
    uint32_t count, x86_jit_stack_window_t *window) {
  memset(window, 0, sizeof(*window));
  const bool paged = jit_paging_enabled();
  if (paged) {
    /*
     * Keep page-mode stack operations on their per-instruction DTLB paths.  A
     * block-entry stack-window guard is not a valid invariant until every
     * accepted PUSH/POP/CALL/RET form can prove its page access at the exact
     * architectural commit point; Nanos-lite stack control flow depends on that
     * ordering.
     */
    if (jit_paged_stack_fast_enabled) JIT_STAT_INC(stack_fast_fallbacks);
    return false;
  }
  else if (!jit_stack_fast_enabled || !jit_fast_chain_runtime_enabled()) {
    return false;
  }

  int32_t esp_delta = 0;
  uint32_t stack_ops = 0;
  for (uint32_t i = 0; i < count; i++) {
    const x86_jit_insn_t *insn = &insns[i];
    if (jit_stack_window_guest_esp_write(insn)) {
      return jit_stack_window_reject(paged);
    }
    if (insn->op != X86_JIT_OP_HELPER) continue;

    switch (insn->helper) {
      case X86_JIT_HELPER_PUSH_REG:
      case X86_JIT_HELPER_PUSH_IMM:
        if (insn->width != X86_WIDTH_DWORD) {
          return jit_stack_window_reject(paged);
        }
        stack_ops++;
        esp_delta -= (int32_t)X86_WIDTH_DWORD;
        jit_stack_window_note_access(window, esp_delta, true);
        break;
      case X86_JIT_HELPER_CALL_REL:
        if (insn->width != X86_WIDTH_DWORD) {
          return jit_stack_window_reject(paged);
        }
        if (paged && (!jit_chain_enabled ||
            !jit_target_probe_accepts_chain(jit_branch_target(insn)))) {
          return jit_stack_window_reject(true);
        }
        stack_ops++;
        esp_delta -= (int32_t)X86_WIDTH_DWORD;
        jit_stack_window_note_access(window, esp_delta, true);
        break;
      case X86_JIT_HELPER_PUSH_RM:
        if (paged) return jit_stack_window_reject(true);
        if (insn->width != X86_WIDTH_DWORD) return false;
        stack_ops++;
        esp_delta -= (int32_t)X86_WIDTH_DWORD;
        jit_stack_window_note_access(window, esp_delta, true);
        break;
      case X86_JIT_HELPER_POP_REG:
        if (insn->width != X86_WIDTH_DWORD || insn->dst == R_ESP) {
          return jit_stack_window_reject(paged);
        }
        stack_ops++;
        jit_stack_window_note_access(window, esp_delta, false);
        esp_delta += (int32_t)X86_WIDTH_DWORD;
        break;
      case X86_JIT_HELPER_RET:
        if (insn->width != X86_WIDTH_DWORD) {
          return jit_stack_window_reject(paged);
        }
        if (paged && (!jit_chain_enabled ||
            !jit_indirect_target_cache_runtime_enabled())) {
          return jit_stack_window_reject(true);
        }
        stack_ops++;
        jit_stack_window_note_access(window, esp_delta, false);
        break;
      case X86_JIT_HELPER_LEAVE:
      case X86_JIT_HELPER_CALL_RM:
        return jit_stack_window_reject(paged);
      default:
        break;
    }
  }

  if (!window->valid) return jit_stack_window_reject(paged);
  if (paged && stack_ops != 1u) {
    return jit_stack_window_reject(true);
  }
  const int64_t access_len64 =
      (int64_t)window->max_offset - (int64_t)window->min_offset +
      (int64_t)X86_WIDTH_DWORD;
  if (access_len64 <= 0 || access_len64 > CONFIG_MSIZE) {
    return jit_stack_window_reject(paged);
  }
  if (window->has_store) {
    const int64_t store_len64 =
        (int64_t)window->store_max_offset -
        (int64_t)window->store_min_offset + (int64_t)X86_WIDTH_DWORD;
    if (store_len64 <= 0 || store_len64 > X86_JIT_SOURCE_PAGE_SIZE) {
      return jit_stack_window_reject(paged);
    }
  }
  return true;
}

/* Align native block starts for friendlier instruction-cache fetch and patching. */
static size_t jit_align_code(size_t value) {
  return (value + X86_JIT_CODE_ALIGN - 1u) & ~(size_t)(X86_JIT_CODE_ALIGN - 1u);
}

static bool jit_block_revalidate_paging_generation(x86_jit_block_t *block) {
  if (!block->paging ||
      block->translation_key.paging_generation == jit_paging_generation) {
    return true;
  }

  /*
   * Page-table writes are common during loader and heap growth.  They only make
   * a block stale if the virtual instruction bytes now resolve to different
   * source bytes; data memory accesses still use the current DTLB at runtime.
   */
  const x86_jit_block_cold_t *cold = jit_block_cold_const(block);
  if (block->is_trace) {
    for (uint16_t i = 0; i < cold->source_span_count; i++) {
      const x86_jit_source_span_t *span = &cold->source_spans[i];
      uint8_t current[X86_JIT_MAX_SOURCE_BYTES];
      if ((uint32_t)span->offset + span->len > block->source_len ||
          span->len > sizeof(current) ||
          !jit_copy_source(span->pc, span->len, current) ||
          memcmp(cold->source + span->offset, current, span->len) != 0) {
        JIT_STAT_INC(paged_source_validation_failures);
        return false;
      }
    }
  }
  else {
    uint8_t current[X86_JIT_MAX_SOURCE_BYTES];
    if (!jit_copy_source(block->pc, block->source_len, current) ||
        memcmp(cold->source, current, block->source_len) != 0) {
      JIT_STAT_INC(paged_source_validation_failures);
      return false;
    }
  }

  block->translation_key.paging_generation = jit_paging_generation;
  return true;
}

/* Validate that cached source bytes still match guest memory for this PC/CR3. */
static bool jit_block_source_matches(x86_jit_block_t *block, vaddr_t pc) {
  if (!block->valid || block->pc != pc || block->source_len == 0) return false;
  if (block->paging != jit_paging_enabled()) return false;
  if (!jit_translation_context_equal(block->translation_key,
      jit_current_translation_key())) {
    JIT_STAT_INC(cr3_or_paging_key_mismatches);
    return false;
  }
  if (!jit_block_revalidate_paging_generation(block)) return false;
  if (!jit_verify_source_enabled) return true;
  const x86_jit_block_cold_t *cold = jit_block_cold_const(block);
  if (block->is_trace) {
    for (uint16_t i = 0; i < cold->source_span_count; i++) {
      const x86_jit_source_span_t *span = &cold->source_spans[i];
      uint8_t current[X86_JIT_MAX_SOURCE_BYTES];
      if ((uint32_t)span->offset + span->len > block->source_len ||
          span->len > sizeof(current) ||
          !jit_copy_source(span->pc, span->len, current) ||
          memcmp(cold->source + span->offset, current, span->len) != 0) {
        if (block->paging) JIT_STAT_INC(paged_source_validation_failures);
        return false;
      }
    }
    return true;
  }
  uint8_t current[X86_JIT_MAX_SOURCE_BYTES];
  if (!jit_copy_source(pc, block->source_len, current)) {
    if (block->paging) JIT_STAT_INC(paged_source_validation_failures);
    return false;
  }
  const bool matches =
      memcmp(cold->source, current, block->source_len) == 0;
  if (!matches && block->paging) {
    JIT_STAT_INC(paged_source_validation_failures);
  }
  return matches;
}

/* Monotonic replacement age; zero is reserved as "never used". */
static uint32_t jit_next_cache_age(void) {
  jit_cache_age_clock++;
  if (jit_cache_age_clock == 0) jit_cache_age_clock = 1;
  return jit_cache_age_clock;
}

static uint32_t jit_translation_key_hash(x86_jit_translation_key_t key) {
  uint32_t hash = key.cr3_key;
  hash ^= key.state * 2246822519u;
  hash ^= hash >> 15;
  return hash;
}

/* Hash a PC/translation key into the set-associative block cache. */
static uint32_t jit_cache_set_key(vaddr_t pc, x86_jit_translation_key_t key) {
  uint32_t hash = pc ^ jit_translation_key_hash(key);
  hash ^= hash >> 4;
  hash ^= hash >> 12;
  hash ^= hash >> 20;
  return hash & (X86_JIT_CACHE_SETS - 1u);
}

/*
 * Hash a PC/CR3 key into the hotness table.  2654435761 is Knuth's 32-bit
 * multiplicative hash constant, used to spread nearby CR3 values before masking.
 */
static uint32_t jit_hot_index(vaddr_t pc, x86_jit_translation_key_t key) {
  uint32_t hash = pc ^ (jit_translation_key_hash(key) * 2654435761u);
  hash ^= hash >> 7;
  hash ^= hash >> 16;
  return hash & (X86_JIT_HOT_TABLE_SIZE - 1u);
}

/* Hash a return target PC into the power-of-two direct RET cache. */
static uint32_t jit_ret_cache_index(vaddr_t pc) {
  uint32_t hash = pc;
  hash ^= hash >> 4;
  hash ^= hash >> 12;
  return hash & X86_JIT_RET_CACHE_MASK;
}

/* Clear all indirect RET chain hints. */
static void jit_ret_cache_clear(void) {
  memset(jit_ret_cache, 0, sizeof(jit_ret_cache));
  memset(jit_ret_cache_meta, 0, sizeof(jit_ret_cache_meta));
  memset(jit_ret_cache_generation_slot, 0,
      sizeof(jit_ret_cache_generation_slot));
}

/* Publish one chainable block as a possible RET target. */
static void jit_ret_cache_publish(const x86_jit_block_t *block) {
  if (block == NULL || !block->valid || block->unsupported ||
      !block->accepts_chain || block->chain_entry == NULL ||
      (block->paging && !jit_paged_retcache_enabled)) {
    return;
  }
  if (block->paging && !jit_block_translation_key_matches(block)) {
    return;
  }

  const uint32_t index = jit_ret_cache_index(block->pc);
  x86_jit_ret_cache_entry_t *entry = &jit_ret_cache[index];
  entry->target_pc = block->pc;
  entry->pad = 0;
  entry->chain_entry = block->chain_entry;
  jit_ret_cache_meta[index] = (x86_jit_ret_cache_meta_t){
    .translation_key = block->translation_key,
    .block_generation = block->generation,
  };
  jit_ret_cache_generation_slot[index] = &block->generation;
}

/* Return or initialise the hotness record for one PC/translation-key pair. */
static x86_jit_hot_info_t *jit_hot_info_for(vaddr_t pc,
    x86_jit_translation_key_t key) {
  x86_jit_hot_info_t *hot = &jit_hot_info[jit_hot_index(pc, key)];
  if (!hot->valid || hot->pc != pc ||
      !jit_translation_key_equal(hot->translation_key, key)) {
    *hot = (x86_jit_hot_info_t){
      .valid = true,
      .pc = pc,
      .cr3_key = key.cr3_key,
      .translation_key = key,
      .trace_index = UINT32_MAX,
    };
  }
  return hot;
}

/* Check whether a hotness record still points to the same compiled trace. */
static bool jit_hot_trace_is_valid(const x86_jit_hot_info_t *hot) {
  if (hot == NULL || !hot->trace_compiled ||
      hot->trace_index >= X86_JIT_CACHE_SIZE) {
    return false;
  }

  const x86_jit_block_t *trace = &jit_cache[hot->trace_index];
  return trace->valid && trace->is_trace &&
         trace->pc == hot->pc &&
         jit_trace_translation_key_matches(hot, trace) &&
         trace->generation == hot->trace_generation;
}

/* Hash the current PC using the current CR3 key. */
static uint32_t jit_cache_set(vaddr_t pc) {
  return jit_cache_set_key(pc, jit_current_translation_key());
}

/* Hash a PC/translation key into the direct-mapped L0 cache. */
static uint32_t jit_l0_index_key(vaddr_t pc, x86_jit_translation_key_t key) {
  uint32_t hash = pc ^ jit_translation_key_hash(key);
  hash ^= hash >> 6;
  hash ^= hash >> 15;
  return hash & (X86_JIT_L0_SIZE - 1u);
}

/* Return one way from the current-CR3 block-cache set. */
static x86_jit_block_t *jit_cache_way(vaddr_t pc, uint32_t way) {
  return &jit_cache[jit_cache_set(pc) * X86_JIT_CACHE_WAYS + way];
}

/* Return one way from a block-cache set for an explicit CR3 key. */
static x86_jit_block_t *jit_cache_way_key(vaddr_t pc, uint32_t way,
    x86_jit_translation_key_t key) {
  return &jit_cache[jit_cache_set_key(pc, key) * X86_JIT_CACHE_WAYS + way];
}

/* Probe the direct-mapped L0 cache before walking all ways of the block cache. */
static x86_jit_block_t *jit_l0_lookup(vaddr_t pc,
    x86_jit_translation_key_t key) {
  if (!jit_l0_cache_enabled || !jit_hot_cold_cache_enabled) return NULL;

  x86_jit_l0_entry_t *l0 = &jit_l0_cache[jit_l0_index_key(pc, key)];
  if (!l0->valid || l0->pc != pc ||
      !jit_translation_context_equal(l0->translation_key, key) ||
      l0->generation != jit_cache_generation ||
      l0->hot_index >= X86_JIT_CACHE_SIZE) {
    return NULL;
  }

  x86_jit_block_t *block = &jit_cache[l0->hot_index];
  if (!block->valid || block->pc != pc || block->source_len == 0 ||
      !jit_block_source_matches(block, pc)) {
    return NULL;
  }
  return block;
}

/* Fill the L0 cache with the hot-cache index and current generation. */
static void jit_l0_fill_key(vaddr_t pc, const x86_jit_block_t *block,
    x86_jit_translation_key_t key) {
  if (!jit_l0_cache_enabled || !jit_hot_cold_cache_enabled || block == NULL) {
    return;
  }

  x86_jit_l0_entry_t *l0 = &jit_l0_cache[jit_l0_index_key(pc, key)];
  *l0 = (x86_jit_l0_entry_t){
    .valid = true,
    .pc = pc,
    .cr3_key = key.cr3_key,
    .translation_key = key,
    .hot_index = jit_block_index(block),
    .generation = jit_cache_generation,
  };
}

/* Fill the L0 cache for the current CR3 key. */
static void jit_l0_fill(vaddr_t pc, const x86_jit_block_t *block) {
  jit_l0_fill_key(pc, block, jit_current_translation_key());
}

/* Lookup a valid block, verifying source bytes if requested. */
static x86_jit_block_t *jit_cache_lookup(vaddr_t pc) {
  const x86_jit_translation_key_t key = jit_current_translation_key();
  x86_jit_block_t *block = jit_l0_lookup(pc, key);
  if (block != NULL) {
    jit_ret_cache_publish(block);
    return block;
  }

  const uint32_t ways = jit_4way_cache_enabled ? X86_JIT_CACHE_WAYS : 1u;
  for (uint32_t way = 0; way < ways; way++) {
    x86_jit_block_t *block = jit_cache_way_key(pc, way, key);
    if (jit_block_source_matches(block, pc)) {
      jit_l0_fill_key(pc, block, key);
      jit_ret_cache_publish(block);
      return block;
    }
    if (jit_verify_source_enabled && block->valid && block->pc == pc &&
        jit_translation_context_equal(block->translation_key, key)) {
      jit_block_invalidate(block);
    }
  }
  return NULL;
}

/* Link all patchable exits of one block to currently compiled successors. */
static void jit_link_block_exits(x86_jit_block_t *block) {
  if (!jit_chain_enabled || block == NULL || !block->valid ||
      block->unsupported) {
    return;
  }

  for (uint8_t i = 0; i < block->exit_count; i++) {
    x86_jit_exit_edge_t *edge = &block->exits[i];
    if (!edge->valid) continue;

    x86_jit_block_t *target = jit_cache_lookup(edge->target_pc);
    if (target != NULL && target->valid && !target->unsupported &&
        target->accepts_chain &&
        (!block->paging || jit_paged_chain_enabled) &&
        block->paging == target->paging &&
        jit_translation_key_equal(block->translation_key,
            target->translation_key) &&
        target->chain_entry != NULL) {
      jit_patch_edge_to_target(edge, target);
    }
    else {
      jit_patch_edge_to_slow(edge);
    }
  }
}

/* Link existing incoming edges after compiling a new target block. */
static void jit_link_edges_to_target(x86_jit_block_t *target) {
  if (!jit_chain_enabled || target == NULL || !target->valid ||
      target->unsupported || !target->accepts_chain ||
      target->chain_entry == NULL) {
    return;
  }
  if (target->paging && !jit_paged_chain_enabled) return;

  for (uint32_t i = 0; i < X86_JIT_CACHE_SIZE; i++) {
    x86_jit_block_t *block = &jit_cache[i];
    if (!block->valid || block->unsupported ||
        block->paging != target->paging ||
        !jit_translation_key_equal(block->translation_key,
            target->translation_key)) {
      continue;
    }
    for (uint8_t edge_index = 0; edge_index < block->exit_count; edge_index++) {
      x86_jit_exit_edge_t *edge = &block->exits[edge_index];
      if (edge->valid && edge->target_pc == target->pc) {
        jit_patch_edge_to_target(edge, target);
      }
    }
  }
}

/* Probe a potential successor to see whether it can accept a direct chain. */
static bool jit_target_probe_accepts_chain(vaddr_t pc) {
  x86_jit_block_t *cached = jit_cache_lookup(pc);
  if (cached != NULL) {
    return cached->valid && !cached->unsupported && cached->accepts_chain;
  }

  const uint32_t block_limit = jit_runtime_block_limit == 0 ?
      X86_JIT_BLOCK_MAX_INSNS : jit_runtime_block_limit;
  x86_jit_insn_t decoded[X86_JIT_BLOCK_MAX_INSNS];
  uint32_t decoded_count = 0;
  vaddr_t end_pc = pc;
  if (!jit_decode_block(pc, block_limit, decoded, &decoded_count, &end_pc)) {
    return false;
  }

  return true;
}

/* Check whether an edge may use a patchable direct chain. */
static bool jit_edge_accepts_chain(bool guarded_block, vaddr_t pc) {
  return guarded_block && jit_target_probe_accepts_chain(pc);
}

/* Classify the possible successors of a block for chain emission decisions. */
static x86_jit_edge_chainability_t jit_block_edge_chainability(
    const x86_jit_insn_t *insns, uint32_t count, vaddr_t end_pc,
    bool guarded_block) {
  x86_jit_edge_chainability_t edges = {0};
  if (!guarded_block || count == 0) return edges;

  const x86_jit_insn_t *last = &insns[count - 1u];
  if (last->op == X86_JIT_OP_JMP_REL) {
    edges.target = jit_edge_accepts_chain(guarded_block,
        jit_branch_target(last));
    return edges;
  }
  if (last->op == X86_JIT_OP_JCC_REL) {
    edges.fallthrough = jit_edge_accepts_chain(guarded_block, last->next_pc);
    edges.taken = jit_edge_accepts_chain(guarded_block,
        jit_branch_target(last));
    return edges;
  }
  if (last->op == X86_JIT_OP_HELPER &&
      last->helper == X86_JIT_HELPER_CALL_REL) {
    edges.target = jit_edge_accepts_chain(guarded_block,
        jit_branch_target(last));
    return edges;
  }
  if (!last->ends_block) {
    edges.fallthrough = jit_edge_accepts_chain(guarded_block, end_pc);
  }

  return edges;
}

/* Return true when at least one successor edge can be directly chained. */
static bool jit_block_has_chainable_edge(x86_jit_edge_chainability_t edges) {
  return edges.fallthrough || edges.taken || edges.target;
}

/* Account why a block did or did not get any chainable outgoing edge. */
static void jit_classify_block_chainability(const x86_jit_insn_t *insns,
    uint32_t count, vaddr_t end_pc, bool guarded_block,
    x86_jit_edge_chainability_t edges) {
  if (!jit_stats_enabled || !guarded_block) return;

  if (jit_block_has_chainable_edge(edges)) {
    jit_stats.blocks_chainable++;
    return;
  }

  jit_stats.blocks_not_chainable++;
  if (count == 0) {
    jit_stats.blocks_not_chainable_unsupported_successor++;
    return;
  }

  const x86_jit_insn_t *last = &insns[count - 1u];
  if (last->op == X86_JIT_OP_JMP_REL) {
    jit_stats.blocks_not_chainable_jmp++;
    if (!jit_target_probe_accepts_chain(jit_branch_target(last))) {
      jit_stats.blocks_not_chainable_unsupported_successor++;
    }
    return;
  }

  if (last->op == X86_JIT_OP_JCC_REL) {
    const bool fallthrough_ok = jit_target_probe_accepts_chain(last->next_pc);
    const bool taken_ok = jit_target_probe_accepts_chain(jit_branch_target(last));
    if (fallthrough_ok != taken_ok) {
      jit_stats.blocks_not_chainable_jcc_one_side++;
    }
    else {
      jit_stats.blocks_not_chainable_jcc_both_sides++;
    }
    if (!fallthrough_ok || !taken_ok) {
      jit_stats.blocks_not_chainable_unsupported_successor++;
    }
    return;
  }

  if (last->op == X86_JIT_OP_HELPER) {
    if (last->helper == X86_JIT_HELPER_CALL_REL) {
      if (!jit_target_probe_accepts_chain(jit_branch_target(last))) {
        jit_stats.blocks_not_chainable_unsupported_successor++;
      }
    }
    else if (last->helper == X86_JIT_HELPER_RET) {
      jit_stats.blocks_not_chainable_ret++;
    }
    else if (last->helper == X86_JIT_HELPER_CALL_RM) {
      jit_stats.blocks_not_chainable_call_rm++;
    }
    else {
      jit_stats.blocks_not_chainable_unsupported_successor++;
    }
    return;
  }

  if (!last->ends_block &&
      !jit_target_probe_accepts_chain(end_pc)) {
    jit_stats.blocks_not_chainable_unsupported_successor++;
  }
}

typedef struct {
  vaddr_t pc;
  vaddr_t end_pc;
  vaddr_t hot_target;
  vaddr_t cold_target;
  uint32_t first_insn;
  uint32_t insn_count;
  uint32_t count_after;
  bool ends_with_jmp;
  bool ends_with_jcc;
  bool hot_is_taken;
  bool is_final;
} x86_jit_trace_part_t;

/* Trace builder treats direct JMP/Jcc as block-ending control instructions. */
static bool jit_trace_insn_is_direct_control(const x86_jit_insn_t *insn) {
  return insn->op == X86_JIT_OP_JMP_REL || insn->op == X86_JIT_OP_JCC_REL;
}

/* Choose the hot Jcc edge from observed counters, falling back to backwards-branch bias. */
static bool jit_trace_choose_taken(vaddr_t pc, const x86_jit_insn_t *jcc) {
  const x86_jit_translation_key_t key = jit_current_translation_key();
  x86_jit_hot_info_t *hot =
      &jit_hot_info[jit_hot_index(pc, key)];
  if (hot->valid && hot->pc == pc &&
      jit_translation_key_equal(hot->translation_key, key) &&
      hot->taken_count != hot->fallthrough_count) {
    return hot->taken_count > hot->fallthrough_count;
  }

  /*
   * In tight loops the backwards branch is usually the dominant edge.  The hot
   * table refines this when an edge has been observed at a C boundary.
   */
  return jit_branch_target(jcc) <= pc;
}

static bool jit_trace_seen_pc(const x86_jit_trace_part_t *parts,
    uint32_t part_count, vaddr_t pc) {
  for (uint32_t i = 0; i < part_count; i++) {
    if (parts[i].pc == pc) return true;
  }
  return false;
}

static bool jit_trace_block_has_resident_backedge(
    const x86_jit_insn_t *insns, uint32_t count, vaddr_t pc) {
  if (count >= 2u &&
      jit_is_native_incdec_reg(&insns[0]) &&
      jit_is_incdec_resident_jcc_backedge(&insns[1], pc)) {
    return true;
  }
  if (count >= 3u &&
      jit_is_native_incdec_reg(&insns[0]) &&
      jit_is_cmp_with_reg(&insns[1], insns[0].dst) &&
      jit_is_any_jcc_backedge(&insns[2], pc)) {
    return true;
  }
  return count >= 2u &&
         jit_is_fusible_flag_producer(&insns[count - 2u]) &&
         jit_is_native_jcc(&insns[count - 1u]) &&
         jit_branch_target(&insns[count - 1u]) == pc;
}

static bool jit_trace_decode(vaddr_t pc, uint32_t max_insns,
    x86_jit_insn_t *trace_insns, uint32_t *trace_count,
    x86_jit_trace_part_t *parts, uint32_t *part_count,
    uint32_t *source_len) {
  uint32_t total = 0;
  uint32_t source_total = 0;
  uint32_t parts_used = 0;
  uint32_t side_exits = 0;
  vaddr_t cur_pc = pc;
  const uint32_t trace_limit = max_insns < X86_JIT_TRACE_MAX_INSNS ?
      max_insns : X86_JIT_TRACE_MAX_INSNS;

  while (parts_used < X86_JIT_TRACE_MAX_BLOCKS && total < trace_limit) {
    if (jit_trace_seen_pc(parts, parts_used, cur_pc)) break;

    x86_jit_insn_t decoded[X86_JIT_BLOCK_MAX_INSNS];
    uint32_t decoded_count = 0;
    vaddr_t end_pc = cur_pc;
    const uint32_t remain = trace_limit - total;
    if (!jit_decode_block(cur_pc, remain, decoded, &decoded_count, &end_pc)) {
      break;
    }

    const x86_jit_insn_t *last = &decoded[decoded_count - 1u];
    if (last->ends_block && !jit_trace_insn_is_direct_control(last)) {
      break;
    }
    if (!jit_trace_loopback_enabled &&
        jit_trace_block_has_resident_backedge(decoded, decoded_count, cur_pc)) {
      break;
    }
    if (last->op == X86_JIT_OP_JCC_REL &&
        side_exits + 2u > X86_JIT_EXIT_EDGE_LIMIT) {
      break;
    }

    const uint32_t span_len = (uint32_t)(end_pc - cur_pc);
    if (span_len == 0 ||
        source_total + span_len > X86_JIT_MAX_SOURCE_BYTES ||
        parts_used >= X86_JIT_TRACE_SOURCE_SPAN_LIMIT) {
      break;
    }

    x86_jit_trace_part_t *part = &parts[parts_used];
    *part = (x86_jit_trace_part_t){
      .pc = cur_pc,
      .end_pc = end_pc,
      .first_insn = total,
      .insn_count = decoded_count,
      .count_after = total + decoded_count,
    };

    for (uint32_t i = 0; i < decoded_count; i++) {
      decoded[i].ordinal = (uint16_t)(total + i + 1u);
      trace_insns[total + i] = decoded[i];
    }

    total += decoded_count;
    source_total += span_len;
    parts_used++;

    vaddr_t next_pc = end_pc;
    if (last->op == X86_JIT_OP_JMP_REL) {
      part->ends_with_jmp = true;
      next_pc = jit_branch_target(last);
      part->hot_target = next_pc;
    }
    else if (last->op == X86_JIT_OP_JCC_REL) {
      part->ends_with_jcc = true;
      part->hot_is_taken = jit_trace_choose_taken(cur_pc, last);
      part->hot_target = part->hot_is_taken ?
          jit_branch_target(last) : last->next_pc;
      part->cold_target = part->hot_is_taken ?
          last->next_pc : jit_branch_target(last);
      next_pc = part->hot_target;
      side_exits++;
    }

    if (part->is_final || next_pc == pc || total >= trace_limit ||
        parts_used >= X86_JIT_TRACE_MAX_BLOCKS ||
        jit_trace_seen_pc(parts, parts_used, next_pc)) {
      part->is_final = true;
      break;
    }

    cur_pc = next_pc;
  }

  if (parts_used != 0) {
    parts[parts_used - 1u].is_final = true;
  }

  *trace_count = total;
  *part_count = parts_used;
  *source_len = source_total;
  if (total < X86_JIT_TRACE_MIN_INSNS) return false;
  if (parts_used >= 2u) return true;
  if (!jit_trace_loopback_enabled || parts_used == 0) return false;

  const x86_jit_trace_part_t *last_part = &parts[parts_used - 1u];
  return last_part->is_final &&
         (last_part->ends_with_jmp || last_part->ends_with_jcc) &&
         last_part->hot_target == pc;
}

static bool jit_trace_jcc_successor_flags_dead(
    const x86_jit_trace_part_t *part) {
  return part != NULL &&
         jit_successor_flags_dead(part->hot_target) &&
         jit_successor_flags_dead(part->cold_target);
}

static bool emit_trace_jcc_side_exit(x86_jit_writer_t *w,
    x86_jit_block_t *block, const x86_jit_insn_t *jcc,
    const x86_jit_trace_part_t *part, x86_jit_emit_ctx_t *ctx) {
  uint8_t *taken_disp = NULL;

  if (!emit_jcc_condition_jump(w, jcc->cc, &taken_disp)) return false;

  if (part->hot_is_taken) {
    x86_jit_emit_ctx_t exit_ctx = *ctx;
    if ((jit_stats_enabled &&
            !emit_runtime_counter_inc(w,
                &jit_trace_side_exit_fallthrough_runtime)) ||
        !jit_regcache_flush_all(w, &exit_ctx) ||
        !emit_chain_exit(w, block, part->cold_target, part->count_after,
        X86_JIT_EXIT_FALLTHROUGH, X86_JIT_CHAIN_SLOW_COLD_TRACE)) {
      return false;
    }
    return patch_rel32(taken_disp, w->cur);
  }

  uint8_t *hot_disp = NULL;
  if (!emit_jmp_rel32_placeholder(w, &hot_disp)) return false;
  uint8_t *cold_native = w->cur;
  x86_jit_emit_ctx_t exit_ctx = *ctx;
  if (!patch_rel32(taken_disp, cold_native) ||
      (jit_stats_enabled &&
          !emit_runtime_counter_inc(w,
              &jit_trace_side_exit_taken_runtime)) ||
      !jit_regcache_flush_all(w, &exit_ctx) ||
      !emit_chain_exit(w, block, part->cold_target, part->count_after,
          X86_JIT_EXIT_TAKEN, X86_JIT_CHAIN_SLOW_COLD_TRACE)) {
    return false;
  }
  return patch_rel32(hot_disp, w->cur);
}

static bool emit_trace_jcc_side_exit_host_flags(x86_jit_writer_t *w,
    x86_jit_block_t *block, const x86_jit_insn_t *jcc,
    const x86_jit_trace_part_t *part, x86_jit_emit_ctx_t *ctx) {
  uint8_t *taken_disp = NULL;

  if (!emit_jcc_rel32_placeholder(w, jcc->cc, &taken_disp)) return false;

  if (part->hot_is_taken) {
    x86_jit_emit_ctx_t exit_ctx = *ctx;
    if ((jit_stats_enabled &&
            !emit_runtime_counter_inc(w,
                &jit_trace_side_exit_fallthrough_runtime)) ||
        !jit_regcache_flush_all(w, &exit_ctx) ||
        !emit_chain_exit(w, block, part->cold_target, part->count_after,
            X86_JIT_EXIT_FALLTHROUGH, X86_JIT_CHAIN_SLOW_COLD_TRACE)) {
      return false;
    }
    return patch_rel32(taken_disp, w->cur);
  }

  uint8_t *hot_disp = NULL;
  if (!emit_jmp_rel32_placeholder(w, &hot_disp)) return false;
  uint8_t *cold_native = w->cur;
  x86_jit_emit_ctx_t exit_ctx = *ctx;
  if (!patch_rel32(taken_disp, cold_native) ||
      (jit_stats_enabled &&
          !emit_runtime_counter_inc(w,
              &jit_trace_side_exit_taken_runtime)) ||
      !jit_regcache_flush_all(w, &exit_ctx) ||
      !emit_chain_exit(w, block, part->cold_target, part->count_after,
          X86_JIT_EXIT_TAKEN, X86_JIT_CHAIN_SLOW_COLD_TRACE)) {
    return false;
  }
  return patch_rel32(hot_disp, w->cur);
}

static bool emit_trace_jcc_final_exit(x86_jit_writer_t *w,
    x86_jit_block_t *block, const x86_jit_insn_t *jcc,
    const x86_jit_trace_part_t *part) {
  uint8_t *taken_disp = NULL;

  if (!emit_jcc_condition_jump(w, jcc->cc, &taken_disp)) return false;

  if (part->hot_is_taken) {
    if ((jit_stats_enabled &&
            !emit_runtime_counter_inc(w,
                &jit_trace_side_exit_fallthrough_runtime)) ||
        !emit_chain_exit(w, block, part->cold_target, part->count_after,
        X86_JIT_EXIT_FALLTHROUGH, X86_JIT_CHAIN_SLOW_COLD_TRACE)) {
      return false;
    }
    uint8_t *hot_native = w->cur;
    if (!patch_rel32(taken_disp, hot_native)) return false;
    if (part->hot_target == block->pc) {
      return emit_trace_head_loop(w, block->pc, part->count_after,
          block->chain_entry);
    }
    return emit_chain_exit(w, block, part->hot_target, part->count_after,
        X86_JIT_EXIT_TAKEN, X86_JIT_CHAIN_SLOW_UNLINKED);
  }

  if (part->hot_target == block->pc) {
    uint8_t *cold_disp = NULL;
    if (!emit_jmp_rel32_placeholder(w, &cold_disp) ||
        !patch_rel32(taken_disp, w->cur) ||
        (jit_stats_enabled &&
            !emit_runtime_counter_inc(w,
                &jit_trace_side_exit_taken_runtime)) ||
        !emit_chain_exit(w, block, part->cold_target, part->count_after,
            X86_JIT_EXIT_TAKEN, X86_JIT_CHAIN_SLOW_COLD_TRACE)) {
      return false;
    }
    uint8_t *hot_native = w->cur;
    return patch_rel32(cold_disp, hot_native) &&
           emit_trace_head_loop(w, block->pc, part->count_after,
               block->chain_entry);
  }

  if (!emit_chain_exit(w, block, part->hot_target, part->count_after,
      X86_JIT_EXIT_FALLTHROUGH, X86_JIT_CHAIN_SLOW_UNLINKED)) {
    return false;
  }
  uint8_t *cold_native = w->cur;
  return patch_rel32(taken_disp, cold_native) &&
         (!jit_stats_enabled ||
          emit_runtime_counter_inc(w, &jit_trace_side_exit_taken_runtime)) &&
         emit_chain_exit(w, block, part->cold_target, part->count_after,
             X86_JIT_EXIT_TAKEN, X86_JIT_CHAIN_SLOW_COLD_TRACE);
}

static bool emit_trace_jcc_final_exit_host_flags(x86_jit_writer_t *w,
    x86_jit_block_t *block, const x86_jit_insn_t *jcc,
    const x86_jit_trace_part_t *part) {
  uint8_t *taken_disp = NULL;

  if (!emit_jcc_rel32_placeholder(w, jcc->cc, &taken_disp)) return false;

  if (part->hot_is_taken) {
    if ((jit_stats_enabled &&
            !emit_runtime_counter_inc(w,
                &jit_trace_side_exit_fallthrough_runtime)) ||
        !emit_chain_exit(w, block, part->cold_target, part->count_after,
            X86_JIT_EXIT_FALLTHROUGH, X86_JIT_CHAIN_SLOW_COLD_TRACE)) {
      return false;
    }
    uint8_t *hot_native = w->cur;
    if (!patch_rel32(taken_disp, hot_native)) return false;
    if (part->hot_target == block->pc) {
      return emit_trace_head_loop(w, block->pc, part->count_after,
          block->chain_entry);
    }
    return emit_chain_exit(w, block, part->hot_target, part->count_after,
        X86_JIT_EXIT_TAKEN, X86_JIT_CHAIN_SLOW_UNLINKED);
  }

  if (part->hot_target == block->pc) {
    uint8_t *cold_disp = NULL;
    if (!emit_jmp_rel32_placeholder(w, &cold_disp) ||
        !patch_rel32(taken_disp, w->cur) ||
        (jit_stats_enabled &&
            !emit_runtime_counter_inc(w,
                &jit_trace_side_exit_taken_runtime)) ||
        !emit_chain_exit(w, block, part->cold_target, part->count_after,
            X86_JIT_EXIT_TAKEN, X86_JIT_CHAIN_SLOW_COLD_TRACE)) {
      return false;
    }
    uint8_t *hot_native = w->cur;
    return patch_rel32(cold_disp, hot_native) &&
           emit_trace_head_loop(w, block->pc, part->count_after,
               block->chain_entry);
  }

  if (!emit_chain_exit(w, block, part->hot_target, part->count_after,
      X86_JIT_EXIT_FALLTHROUGH, X86_JIT_CHAIN_SLOW_UNLINKED)) {
    return false;
  }
  uint8_t *cold_native = w->cur;
  return patch_rel32(taken_disp, cold_native) &&
         (!jit_stats_enabled ||
          emit_runtime_counter_inc(w, &jit_trace_side_exit_taken_runtime)) &&
         emit_chain_exit(w, block, part->cold_target, part->count_after,
             X86_JIT_EXIT_TAKEN, X86_JIT_CHAIN_SLOW_COLD_TRACE);
}

/* Store disjoint trace source spans so later verification can re-read each PC. */
static bool jit_trace_store_source(x86_jit_block_t *block,
    const x86_jit_trace_part_t *parts, uint32_t part_count) {
  x86_jit_block_cold_t *cold = jit_block_cold(block);
  uint32_t offset = 0;
  cold->source_span_count = 0;

  for (uint32_t i = 0; i < part_count; i++) {
    const uint32_t len = (uint32_t)(parts[i].end_pc - parts[i].pc);
    if (len == 0 || offset + len > X86_JIT_MAX_SOURCE_BYTES ||
        i >= X86_JIT_TRACE_SOURCE_SPAN_LIMIT ||
        !jit_copy_source(parts[i].pc, len, cold->source + offset)) {
      return false;
    }

    cold->source_spans[i] = (x86_jit_source_span_t){
      .pc = parts[i].pc,
      .offset = (uint16_t)offset,
      .len = (uint16_t)len,
    };
    cold->source_span_count++;
    offset += len;
  }

  return true;
}

/* Pick an empty way or the oldest way in the current cache set. */
static x86_jit_block_t *jit_cache_select_victim(vaddr_t pc) {
  x86_jit_block_t *victim = NULL;

  const uint32_t ways = jit_4way_cache_enabled ? X86_JIT_CACHE_WAYS : 1u;
  for (uint32_t way = 0; way < ways; way++) {
    x86_jit_block_t *block = jit_cache_way(pc, way);
    if (!block->valid) {
      victim = block;
      break;
    }
    if (victim == NULL || block->cache_age < victim->cache_age) {
      victim = block;
    }
  }

  Assert(victim != NULL, "x86 JIT cache victim missing");
  jit_block_discard(victim);
  x86_jit_block_cold_t *cold = jit_block_cold(victim);
  memset(cold, 0, sizeof(*cold));
  victim->cache_age = jit_next_cache_age();
  return victim;
}

/* Publish a negative cache entry for an opcode this JIT currently cannot translate. */
static void jit_publish_unsupported(vaddr_t pc) {
  x86_jit_block_t *block = jit_cache_select_victim(pc);
  x86_jit_block_cold_t *cold = jit_block_cold(block);
  uint8_t opcode = 0;
  uint8_t opcode2 = 0;
  memset(block, 0, sizeof(*block));
  memset(cold, 0, sizeof(*cold));
  block->valid = true;
  block->unsupported = true;
  block->pc = pc;
  block->cr3_key = jit_cr3_key();
  block->paging = jit_paging_enabled();
  block->translation_key = jit_current_translation_key();
  block->source_len = 1;
  block->cold_index = jit_block_index(block);
  if (jit_copy_source(pc, block->source_len, cold->source)) {
    opcode = cold->source[0];
  }
  if (opcode == 0x0f && jit_vaddr_read_u8(pc + 1u, &opcode2)) {
    block->unsupported_opcode2 = opcode2;
  }
  block->cache_age = jit_next_cache_age();
  if (!jit_mark_source_pages(block, pc, block->source_len)) {
    block->valid = false;
    return;
  }
  block->cr3_key = jit_cr3_key();
  block->translation_key = jit_current_translation_key();
  jit_l0_fill(pc, block);
  JIT_STAT_INC(blocks_unsupported);
  if (jit_stats_enabled) {
    jit_stats.unsupported_by_opcode[opcode]++;
    if (opcode == 0x0f) jit_stats.unsupported_0f_by_opcode[opcode2]++;
  }
}

/* Compile a hot multi-block trace following the observed branch direction. */
static x86_jit_block_t *jit_compile_trace(vaddr_t pc, uint32_t max_insns,
    x86_jit_hot_info_t *hot) {
  if (!jit_trace_enabled || !jit_chain_enabled || !jit_flat_segments()) {
    return NULL;
  }
  if (jit_paging_enabled() &&
      (!jit_paged_fastpath_enabled || !jit_paged_trace_enabled)) {
    return NULL;
  }
  if (jit_paging_enabled() && !jit_paged_fastpath_mode_ready()) {
    return NULL;
  }

  x86_jit_insn_t decoded[X86_JIT_TRACE_MAX_INSNS];
  x86_jit_trace_part_t parts[X86_JIT_TRACE_MAX_BLOCKS];
  uint32_t decoded_count = 0;
  uint32_t part_count = 0;
  uint32_t source_len = 0;
  memset(parts, 0, sizeof(parts));

  if (!jit_trace_decode(pc, max_insns, decoded, &decoded_count, parts,
      &part_count, &source_len)) {
    if (hot != NULL) hot->trace_failed = true;
    JIT_STAT_INC(trace_compile_failures);
    return NULL;
  }

  if (jit_code_used + X86_JIT_BLOCK_CODE_HEADROOM > X86_JIT_CODE_SIZE) {
    jit_reset_arena();
  }

  const size_t start_used = jit_code_used;
  x86_jit_writer_t w = {
    .start = jit_code + start_used,
    .cur = jit_code + start_used,
    .end = jit_code + X86_JIT_CODE_SIZE,
  };

  x86_jit_block_t *block = jit_cache_select_victim(pc);
  x86_jit_block_cold_t *cold = jit_block_cold(block);
  x86_jit_emit_ctx_t trace_ctx;
  jit_emit_ctx_init(&trace_ctx);
  trace_ctx.trace_mode = true;
  const bool trace_uses_regcache =
      jit_trace_regcache_enabled && jit_regcache_active(&trace_ctx);
  memcpy(cold->insns, decoded, decoded_count * sizeof(decoded[0]));
  block->exit_count = 0;
  block->chain_entry = w.cur;
  block->chain_guard_count_imm = NULL;
  block->accepts_chain = false;
  block->is_trace = true;
  if (jit_stats_enabled) {
    if (trace_uses_regcache) {
      jit_stats.traces_using_regcache++;
    }
    else {
      jit_stats.traces_not_using_regcache++;
    }
  }

  if (!emit_chain_budget_guard(&w, pc, &block->chain_guard_count_imm)) {
    jit_code_used = start_used;
    return NULL;
  }
  if (jit_stats_enabled &&
      !emit_runtime_counter_inc(&w, &jit_trace_hits_runtime)) {
    jit_code_used = start_used;
    return NULL;
  }

  for (uint32_t part_index = 0; part_index < part_count; part_index++) {
    const x86_jit_trace_part_t *part = &parts[part_index];
    for (uint32_t i = 0; i < part->insn_count; i++) {
      const uint32_t insn_index = part->first_insn + i;
      const x86_jit_insn_t *insn = &cold->insns[insn_index];
      const bool is_last = i + 1u == part->insn_count;

      if (!is_last && i + 2u == part->insn_count && part->ends_with_jcc) {
        const x86_jit_insn_t *jcc = &cold->insns[insn_index + 1u];
        if (jit_lazy_flags_enabled &&
            jit_is_fusible_flag_producer(insn) &&
            jit_is_native_jcc(jcc) &&
            jit_trace_jcc_successor_flags_dead(part)) {
          const bool emitted_producer = trace_uses_regcache ?
              emit_flag_producer_no_capture_regcached(&w, &trace_ctx, insn) :
              emit_flag_producer_no_capture(&w, insn);
          if (!emitted_producer) {
            jit_code_used = start_used;
            return NULL;
          }
          trace_ctx.flags.kind = X86_LAZY_FLAGS_HOST_VALID;
          trace_ctx.flags.copy_mask = jit_flag_producer_copy_mask(insn);
          trace_ctx.flags.clear_mask =
              X86_EFLAGS_STATUS_MASK & ~trace_ctx.flags.copy_mask;
          if (part->is_final) {
            if (!jit_regcache_flush_all(&w, &trace_ctx) ||
                !emit_trace_jcc_final_exit_host_flags(&w, block, jcc, part)) {
              jit_code_used = start_used;
              return NULL;
            }
          }
          else if (!emit_trace_jcc_side_exit_host_flags(&w, block, jcc, part,
              &trace_ctx)) {
            jit_code_used = start_used;
            return NULL;
          }
          i++;
          continue;
        }
      }

      if (is_last && part->ends_with_jmp) {
        if (part->is_final) {
          bool ok = false;
          if (part->hot_target == block->pc) {
            ok = jit_regcache_flush_all(&w, &trace_ctx) &&
                emit_trace_head_loop(&w, block->pc, part->count_after,
                    block->chain_entry);
          }
          else {
            ok = jit_regcache_flush_all(&w, &trace_ctx) &&
                emit_chain_exit(&w, block, part->hot_target,
                part->count_after, X86_JIT_EXIT_JMP,
                X86_JIT_CHAIN_SLOW_UNLINKED);
          }
          if (!ok) {
            jit_code_used = start_used;
            return NULL;
          }
        }
        continue;
      }

      if (is_last && part->ends_with_jcc) {
        if (part->is_final) {
          if (!jit_regcache_flush_all(&w, &trace_ctx)) {
            jit_code_used = start_used;
            return NULL;
          }
          if (!emit_trace_jcc_final_exit(&w, block, insn, part)) {
            jit_code_used = start_used;
            return NULL;
          }
        }
        else if (!emit_trace_jcc_side_exit(&w, block, insn, part,
            &trace_ctx)) {
          jit_code_used = start_used;
          return NULL;
        }
        continue;
      }

      if (!(trace_uses_regcache ?
              emit_insn_regcached(&w, &trace_ctx, insn) :
              emit_insn(&w, insn))) {
        jit_code_used = start_used;
        return NULL;
      }
    }

    if (part->is_final && !part->ends_with_jmp && !part->ends_with_jcc &&
        (!jit_regcache_flush_all(&w, &trace_ctx) ||
         !emit_chain_exit(&w, block, part->end_pc, part->count_after,
             X86_JIT_EXIT_FALLTHROUGH, X86_JIT_CHAIN_SLOW_UNLINKED))) {
      jit_code_used = start_used;
      return NULL;
    }
  }

  if (block->chain_guard_count_imm != NULL) {
    patch_u32(block->chain_guard_count_imm, decoded_count);
  }

  block->valid = true;
  block->pc = pc;
  block->cr3_key = jit_cr3_key();
  block->paging = jit_paging_enabled();
  block->translation_key = jit_current_translation_key();
  block->source_len = source_len;
  block->c_entry = (x86_jit_entry_t)block->chain_entry;
  block->generation = jit_cache_generation;
  block->accepts_chain = !block->paging || jit_paged_chain_enabled;
  block->guest_insns = decoded_count;
  block->cache_age = jit_next_cache_age();
  block->cold_index = jit_block_index(block);
  block->uses_loop_accounting = false;
  block->uses_global_loop_accounting = false;
  if (!jit_trace_store_source(block, parts, part_count)) {
    block->valid = false;
    jit_code_used = start_used;
    if (hot != NULL) hot->trace_failed = true;
    JIT_STAT_INC(trace_compile_failures);
    return NULL;
  }
  if (!jit_mark_trace_source_pages(block)) {
    block->valid = false;
    jit_code_used = start_used;
    if (hot != NULL) hot->trace_failed = true;
    JIT_STAT_INC(trace_compile_failures);
    return NULL;
  }
  block->cr3_key = jit_cr3_key();
  block->translation_key = jit_current_translation_key();
  block->generation = jit_cache_generation;
  jit_l0_fill(pc, block);
  jit_ret_cache_publish(block);

  __builtin___clear_cache((char *)w.start, (char *)w.cur);
  jit_code_used = jit_align_code((size_t)(w.cur - jit_code));
  jit_link_block_exits(block);
  jit_link_edges_to_target(block);

  if (hot != NULL) {
    hot->trace_compiled = true;
    hot->trace_index = jit_block_index(block);
    hot->trace_generation = block->generation;
  }
  JIT_STAT_INC(blocks_compiled);
  JIT_STAT_INC(traces_compiled);
  if (block->paging) {
    JIT_STAT_INC(paged_blocks_compiled);
    JIT_STAT_INC(paged_traces_compiled);
  }
  JIT_STAT_ADD(compiled_insns, decoded_count);
  return block;
}

/* Allocate the executable code arena on first use. */
static bool jit_ensure_code_cache(void) {
  if (jit_code != NULL) return true;

  void *mem = mmap(NULL, X86_JIT_CODE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) {
    Log("jit: mmap failed, disable x86 JIT");
    return false;
  }

  jit_code = mem;
  jit_code_used = 0;
  isa_jit_invalidation_active = true;
  Log("jit: x86 x86-64 code cache enabled, size = %zu bytes",
      (size_t)X86_JIT_CODE_SIZE);
  return true;
}

/* Drop all generated code and side metadata while keeping the mmap allocation. */
static void jit_reset_arena(void) {
  memset(jit_cache, 0, sizeof(jit_cache));
  memset(jit_cache_cold, 0, sizeof(jit_cache_cold));
  memset(jit_l0_cache, 0, sizeof(jit_l0_cache));
  memset(jit_hot_info, 0, sizeof(jit_hot_info));
  jit_ret_cache_clear();
  memset(jit_source_page_has_code, 0, sizeof(jit_source_page_has_code));
  memset(jit_source_page_blocks, 0, sizeof(jit_source_page_blocks));
  jit_dtlb_flush();
  jit_cache_age_clock = 1;
  jit_cache_bump_generation();
  jit_code_used = 0;
  JIT_STAT_INC(arena_resets);
}

/* Decode, analyse, emit, validate, and publish one native block. */
static x86_jit_block_t *jit_compile_block(vaddr_t pc, uint32_t max_insns) {
  if (!jit_flat_segments()) {
    return NULL;
  }
  if (jit_paging_enabled() && !jit_paged_fastpath_enabled) {
    return NULL;
  }
  if (jit_paging_enabled() && !jit_paged_fastpath_mode_ready()) {
    return NULL;
  }

  x86_jit_insn_t decoded[X86_JIT_BLOCK_MAX_INSNS];
  uint32_t decoded_count = 0;
  vaddr_t end_pc = pc;
  if (!jit_decode_block(pc, max_insns, decoded, &decoded_count, &end_pc)) {
    jit_publish_unsupported(pc);
    return NULL;
  }

  if (jit_code_used + X86_JIT_BLOCK_CODE_HEADROOM > X86_JIT_CODE_SIZE) {
    jit_reset_arena();
  }

  const size_t start_used = jit_code_used;
  x86_jit_writer_t w = {
    .start = jit_code + start_used,
    .cur = jit_code + start_used,
    .end = jit_code + X86_JIT_CODE_SIZE,
  };
  uint32_t emitted_count = 0;
  bool ends_with_control = false;
  bool ends_with_chained_control = false;
  x86_jit_emit_ctx_t ctx;
  jit_emit_ctx_init(&ctx);
  jit_analyse_block(decoded, decoded_count, &ctx);
  x86_jit_stack_window_t stack_window;
  const bool stack_window_fast =
      jit_analyse_stack_window(decoded, decoded_count, &stack_window);
  const bool paged_block = jit_paging_enabled();
  const bool guarded_block =
      jit_chain_enabled && (!paged_block || jit_paged_chain_enabled);
  const x86_jit_edge_chainability_t block_edges =
      jit_block_edge_chainability(decoded, decoded_count, end_pc,
          guarded_block);
  const bool chainable_block = jit_block_has_chainable_edge(block_edges);
  const bool fast_chain_block = jit_fast_chain_runtime_enabled();
  jit_classify_block_chainability(decoded, decoded_count, end_pc,
      guarded_block, block_edges);

  x86_jit_block_t *block = jit_cache_select_victim(pc);
  x86_jit_block_cold_t *cold = jit_block_cold(block);
  memcpy(cold->insns, decoded, decoded_count * sizeof(decoded[0]));
  block->exit_count = 0;
  block->chain_entry = w.cur;
  block->chain_guard_count_imm = NULL;
  block->accepts_chain = false;
  if (guarded_block &&
      !emit_chain_budget_guard(&w, pc, &block->chain_guard_count_imm)) {
    jit_code_used = start_used;
    return NULL;
  }
  if (stack_window_fast &&
      !emit_stack_window_guard(&w, pc, &stack_window, &cold->insns[0])) {
    jit_code_used = start_used;
    return NULL;
  }

  for (uint32_t i = 0; i < decoded_count; i++) {
    if (i == 0 && decoded_count >= 2u &&
        jit_is_native_incdec_reg(&cold->insns[i]) &&
        jit_is_incdec_resident_jcc_backedge(&cold->insns[i + 1u], pc)) {
      if (!jit_regcache_flush_all(&w, &ctx) ||
          !emit_incdec_jcc_resident_backedge(&w, &cold->insns[i],
          &cold->insns[i + 1u], 2u)) {
        jit_code_used = start_used;
        return NULL;
      }
      emitted_count = 2u;
      ends_with_control = true;
      ends_with_chained_control = true;
      ctx.uses_loop_accounting = true;
      break;
    }

    if (i == 0 && decoded_count >= 3u &&
        jit_is_native_incdec_reg(&cold->insns[i]) &&
        jit_is_cmp_with_reg(&cold->insns[i + 1u], cold->insns[i].dst) &&
        jit_is_any_jcc_backedge(&cold->insns[i + 2u], pc)) {
      if (!jit_regcache_flush_all(&w, &ctx) ||
          !emit_incdec_cmp_jcc_resident_backedge(&w, &cold->insns[i],
          &cold->insns[i + 1u], &cold->insns[i + 2u], 3u)) {
        jit_code_used = start_used;
        return NULL;
      }
      emitted_count = 3u;
      ends_with_control = true;
      ends_with_chained_control = true;
      ctx.uses_loop_accounting = true;
      break;
    }

    if (jit_is_fusible_flag_producer(&cold->insns[i]) &&
        i + 1u < decoded_count && jit_is_native_jcc(&cold->insns[i + 1u])) {
      const uint32_t fused_count = i + 2u;
      if (jit_branch_target(&cold->insns[i + 1u]) == pc &&
          (i == 0 || !fast_chain_block)) {
        if (i == 0) {
          if (!jit_regcache_flush_all(&w, &ctx) ||
              !emit_fused_flag_producer_jcc_resident_backedge(&w,
              &cold->insns[i], &cold->insns[i + 1u], 2u)) {
            jit_code_used = start_used;
            return NULL;
          }
        }
        else {
          if (!jit_regcache_flush_all(&w, &ctx) ||
              !emit_fused_flag_producer_jcc_backedge(&w,
              &cold->insns[i], &cold->insns[i + 1u], w.start,
              fused_count)) {
            jit_code_used = start_used;
            return NULL;
          }
          ctx.uses_global_loop_accounting = true;
        }
        emitted_count = fused_count;
        ends_with_control = true;
        ends_with_chained_control = true;
        ctx.uses_loop_accounting = true;
        break;
      }
      const bool fallthrough_can_chain =
          jit_edge_accepts_chain(guarded_block, cold->insns[i + 1u].next_pc);
      const bool taken_can_chain =
          jit_edge_accepts_chain(guarded_block,
              jit_branch_target(&cold->insns[i + 1u]));
      if (fallthrough_can_chain || taken_can_chain) {
        if (!jit_regcache_flush_all(&w, &ctx) ||
            !emit_fused_flag_producer_jcc_per_edge(&w, block,
            &cold->insns[i], &cold->insns[i + 1u], fused_count,
            fallthrough_can_chain, taken_can_chain)) {
          jit_code_used = start_used;
          return NULL;
        }
      }
      else if (!jit_regcache_flush_all(&w, &ctx) ||
          !emit_fused_flag_producer_jcc(&w, &cold->insns[i],
          &cold->insns[i + 1u], fused_count)) {
        jit_code_used = start_used;
        return NULL;
      }
      emitted_count = fused_count;
      ends_with_control = true;
      ends_with_chained_control = true;
      break;
    }

    const uint32_t insn_count = i + 1u;
    if (jit_lazy_flags_enabled &&
        jit_flags_overwritten_by_next(cold->insns, i, decoded_count)) {
      if (!jit_regcache_flush_all(&w, &ctx) ||
          !emit_flag_producer_no_capture(&w, &cold->insns[i])) {
        jit_code_used = start_used;
        return NULL;
      }
      emitted_count = insn_count;
      ctx.flags.kind = X86_LAZY_FLAGS_HOST_VALID;
      continue;
    }

    if (!fast_chain_block && jit_is_chainable_jcc_backedge(&cold->insns[i], pc)) {
      if (!jit_regcache_flush_all(&w, &ctx) ||
          !emit_jcc_backedge(&w, &cold->insns[i], w.start, insn_count)) {
        jit_code_used = start_used;
        return NULL;
      }
      emitted_count = insn_count;
      ends_with_chained_control = true;
      ctx.uses_loop_accounting = true;
      ctx.uses_global_loop_accounting = true;
    }
    else if (cold->insns[i].op == X86_JIT_OP_JMP_REL &&
        jit_edge_accepts_chain(guarded_block,
            jit_branch_target(&cold->insns[i]))) {
      if (!jit_regcache_flush_all(&w, &ctx) ||
          !emit_chained_jmp_rel(&w, block, &cold->insns[i], insn_count)) {
        jit_code_used = start_used;
        return NULL;
      }
      ends_with_chained_control = true;
    }
    else if (cold->insns[i].op == X86_JIT_OP_JMP_REL && guarded_block) {
      if (!jit_regcache_flush_all(&w, &ctx) ||
          !emit_store_pc_imm(&w, jit_branch_target(&cold->insns[i])) ||
          !emit_ret_count_side_exit(&w, insn_count,
              X86_JIT_CHAIN_SLOW_UNACCEPTED_SUCCESSOR)) {
        jit_code_used = start_used;
        return NULL;
      }
      ends_with_chained_control = true;
      ends_with_control = true;
    }
    else if (cold->insns[i].op == X86_JIT_OP_JCC_REL) {
      const bool fallthrough_can_chain =
          jit_edge_accepts_chain(guarded_block, cold->insns[i].next_pc);
      const bool taken_can_chain =
          jit_edge_accepts_chain(guarded_block,
              jit_branch_target(&cold->insns[i]));
      if (!fallthrough_can_chain && !taken_can_chain) {
        if (!emit_insn_regcached(&w, &ctx, &cold->insns[i])) {
          jit_code_used = start_used;
          return NULL;
        }
        emitted_count = insn_count;
        ends_with_control = true;
        break;
      }
      if (!jit_regcache_flush_all(&w, &ctx) ||
          !emit_jcc_rel_per_edge(&w, block, &cold->insns[i], insn_count,
              fallthrough_can_chain, taken_can_chain)) {
        jit_code_used = start_used;
        return NULL;
      }
      ends_with_chained_control = true;
    }
    else if (stack_window_fast && cold->insns[i].op == X86_JIT_OP_HELPER &&
        cold->insns[i].helper == X86_JIT_HELPER_PUSH_REG) {
      if (!jit_regcache_flush_all(&w, &ctx) ||
          !emit_native_push_reg_stack_guarded(&w, &cold->insns[i])) {
        jit_code_used = start_used;
        return NULL;
      }
    }
    else if (stack_window_fast && cold->insns[i].op == X86_JIT_OP_HELPER &&
        cold->insns[i].helper == X86_JIT_HELPER_PUSH_IMM) {
      if (!jit_regcache_flush_all(&w, &ctx) ||
          !emit_native_push_imm_stack_guarded(&w, &cold->insns[i])) {
        jit_code_used = start_used;
        return NULL;
      }
    }
    else if (stack_window_fast && cold->insns[i].op == X86_JIT_OP_HELPER &&
        cold->insns[i].helper == X86_JIT_HELPER_PUSH_RM) {
      if (!jit_regcache_flush_all(&w, &ctx) ||
          !emit_native_push_rm_stack_guarded(&w, &cold->insns[i])) {
        jit_code_used = start_used;
        return NULL;
      }
    }
    else if (stack_window_fast && cold->insns[i].op == X86_JIT_OP_HELPER &&
        cold->insns[i].helper == X86_JIT_HELPER_POP_REG) {
      if (!jit_regcache_flush_all(&w, &ctx) ||
          !emit_native_pop_reg_stack_guarded(&w, &cold->insns[i])) {
        jit_code_used = start_used;
        return NULL;
      }
    }
    else if (jit_chain_enabled && (!paged_block || stack_window_fast) &&
        cold->insns[i].op == X86_JIT_OP_HELPER &&
        cold->insns[i].helper == X86_JIT_HELPER_CALL_REL &&
        jit_target_probe_accepts_chain(jit_branch_target(&cold->insns[i]))) {
      if (!jit_regcache_flush_all(&w, &ctx) ||
          !(stack_window_fast ?
              emit_chained_call_rel_stack_guarded(&w, block, &cold->insns[i],
                  insn_count) :
              emit_chained_call_rel(&w, block, &cold->insns[i],
                  insn_count))) {
        jit_code_used = start_used;
        return NULL;
      }
      ends_with_chained_control = true;
      ends_with_control = true;
    }
    else if (jit_chain_enabled && paged_block &&
        cold->insns[i].op == X86_JIT_OP_HELPER &&
        cold->insns[i].helper == X86_JIT_HELPER_CALL_REL &&
        jit_target_probe_accepts_chain(jit_branch_target(&cold->insns[i]))) {
      if (!jit_regcache_flush_all(&w, &ctx) ||
          !emit_chained_paged_call_rel(&w, block, &cold->insns[i],
              insn_count)) {
        jit_code_used = start_used;
        return NULL;
      }
      ends_with_chained_control = true;
      ends_with_control = true;
    }
    else if (stack_window_fast && cold->insns[i].op == X86_JIT_OP_HELPER &&
        cold->insns[i].helper == X86_JIT_HELPER_CALL_REL) {
      if (!jit_regcache_flush_all(&w, &ctx) ||
          !emit_native_call_rel_stack_guarded(&w, &cold->insns[i])) {
        jit_code_used = start_used;
        return NULL;
      }
      ends_with_chained_control = true;
      ends_with_control = true;
    }
    else if (jit_chain_enabled && fast_chain_block &&
        jit_indirect_target_cache_runtime_enabled() &&
        cold->insns[i].op == X86_JIT_OP_HELPER &&
        cold->insns[i].helper == X86_JIT_HELPER_JMP_RM &&
        (!paged_block || cold->insns[i].rm_is_reg)) {
      if (!jit_regcache_flush_all(&w, &ctx) ||
          !emit_chained_jmp_rm(&w, &cold->insns[i], insn_count)) {
        jit_code_used = start_used;
        return NULL;
      }
      ends_with_chained_control = true;
      ends_with_control = true;
    }
    else if (jit_chain_enabled && fast_chain_block &&
        jit_indirect_target_cache_runtime_enabled() &&
        cold->insns[i].op == X86_JIT_OP_HELPER &&
        cold->insns[i].helper == X86_JIT_HELPER_RET &&
        cold->insns[i].width == X86_WIDTH_DWORD) {
      if (!jit_regcache_flush_all(&w, &ctx) ||
          !(stack_window_fast ?
              emit_chained_ret_stack_guarded(&w, &cold->insns[i],
                  insn_count) :
              emit_chained_ret(&w, &cold->insns[i], insn_count))) {
        jit_code_used = start_used;
        return NULL;
      }
      ends_with_chained_control = true;
      ends_with_control = true;
    }
    else if (stack_window_fast && cold->insns[i].op == X86_JIT_OP_HELPER &&
        cold->insns[i].helper == X86_JIT_HELPER_RET &&
        cold->insns[i].width == X86_WIDTH_DWORD) {
      if (!jit_regcache_flush_all(&w, &ctx) ||
          !emit_native_ret_stack_guarded(&w, &cold->insns[i])) {
        jit_code_used = start_used;
        return NULL;
      }
      ends_with_chained_control = true;
      ends_with_control = true;
    }
    else if (!emit_insn_regcached(&w, &ctx, &cold->insns[i])) {
      jit_code_used = start_used;
      return NULL;
    }

    emitted_count = insn_count;
    if (cold->insns[i].ends_block) {
      ends_with_control = true;
      break;
    }
  }

  if (!jit_regcache_flush_all(&w, &ctx)) {
    jit_code_used = start_used;
    return NULL;
  }

  if (!ends_with_chained_control) {
    const bool fallthrough_can_chain = !ends_with_control &&
        jit_edge_accepts_chain(guarded_block, end_pc);
    if (fallthrough_can_chain) {
      if (!emit_chain_exit(&w, block, end_pc, emitted_count,
          X86_JIT_EXIT_FALLTHROUGH, X86_JIT_CHAIN_SLOW_UNLINKED)) {
        jit_code_used = start_used;
        return NULL;
      }
    }
    else if ((!ends_with_control && !emit_store_pc_imm(&w, end_pc)) ||
        !emit_ret_count_side_exit(&w, emitted_count,
            guarded_block ? X86_JIT_CHAIN_SLOW_UNACCEPTED_SUCCESSOR :
                X86_JIT_CHAIN_SLOW_BLOCK_NOT_CHAINABLE)) {
      jit_code_used = start_used;
      return NULL;
    }
  }

  if (guarded_block && block->chain_guard_count_imm != NULL) {
    patch_u32(block->chain_guard_count_imm, emitted_count);
  }

  const uint32_t source_len = (uint32_t)(end_pc - pc);
  block->valid = true;
  block->pc = pc;
  block->cr3_key = jit_cr3_key();
  block->paging = jit_paging_enabled();
  block->translation_key = jit_current_translation_key();
  block->source_len = source_len;
  block->c_entry = (x86_jit_entry_t)block->chain_entry;
  block->generation = jit_cache_generation;
  block->accepts_chain = guarded_block;
  block->guest_insns = emitted_count;
  block->cache_age = jit_next_cache_age();
  block->cold_index = jit_block_index(block);
  block->uses_loop_accounting = ctx.uses_loop_accounting;
  block->uses_global_loop_accounting = ctx.uses_global_loop_accounting;
  if (!jit_copy_source(pc, source_len, cold->source)) {
    block->valid = false;
    jit_code_used = start_used;
    return NULL;
  }
  if (!jit_mark_source_pages(block, pc, source_len)) {
    block->valid = false;
    jit_code_used = start_used;
    return NULL;
  }
  block->cr3_key = jit_cr3_key();
  block->translation_key = jit_current_translation_key();
  block->generation = jit_cache_generation;
  jit_l0_fill(pc, block);
  jit_ret_cache_publish(block);

  __builtin___clear_cache((char *)w.start, (char *)w.cur);
  jit_code_used = jit_align_code((size_t)(w.cur - jit_code));
  if (chainable_block || block->exit_count != 0) {
    jit_link_block_exits(block);
    jit_link_edges_to_target(block);
  }
  JIT_STAT_INC(blocks_compiled);
  if (block->paging) JIT_STAT_INC(paged_blocks_compiled);
  JIT_STAT_ADD(compiled_insns, emitted_count);
  return block;
}

/* Public hook: report whether the x86 JIT can be used in this process. */
bool isa_jit_available(void) {
  if (jit_runtime_disabled()) return false;
  return jit_ensure_code_cache();
}

/*
 * Public hook: run a bounded JIT batch.  The caller supplies the instruction
 * budget and device deadline; `executed` receives the retired guest count.
 */
bool isa_jit_exec(uint64_t remaining, uint32_t device_budget, uint32_t *executed) {
  *executed = 0;
  if (remaining == 0 || device_budget == 0 || !isa_jit_available()) return false;
  if (!jit_flat_segments()) return false;

  JIT_STAT_INC(exec_requests);

  uint64_t budget = remaining < X86_JIT_BATCH_MAX_INSNS ?
      remaining : X86_JIT_BATCH_MAX_INSNS;
  if (budget > device_budget) budget = device_budget;

  while (*executed < budget) {
    const uint32_t remaining_budget = (uint32_t)(budget - *executed);
    const uint32_t block_limit = jit_runtime_block_limit == 0 ?
        X86_JIT_BLOCK_MAX_INSNS : jit_runtime_block_limit;
    const uint32_t block_budget = remaining_budget > block_limit ?
        block_limit : remaining_budget;
    x86_jit_block_t *block = jit_cache_lookup(cpu.pc);
    if (block != NULL) {
      JIT_STAT_INC(cache_hits);
    }
    else {
      JIT_STAT_INC(cache_misses);
      block = jit_compile_block(cpu.pc, block_budget);
    }

    if (block == NULL || block->unsupported) {
      JIT_STAT_INC(unsupported_hits);
      if (jit_stats_enabled && block != NULL && block->source_len != 0) {
        const x86_jit_block_cold_t *cold = jit_block_cold_const(block);
        const uint8_t opcode = cold->source[0];
        jit_stats.unsupported_hits_by_opcode[opcode]++;
        if (opcode == 0x0f) {
          jit_stats.unsupported_0f_hits_by_opcode[block->unsupported_opcode2]++;
        }
      }
      return *executed > 0;
    }

    x86_jit_hot_info_t *hot = NULL;
    if (jit_trace_enabled && jit_chain_enabled && !block->is_trace &&
        (!block->paging || jit_paged_trace_enabled)) {
      hot = jit_hot_info_for(block->pc, block->translation_key);
      if (hot->exec_count != UINT32_MAX) hot->exec_count++;
      if (!hot->trace_failed && !jit_hot_trace_is_valid(hot) &&
          hot->exec_count >= jit_trace_hot_threshold) {
        const uint32_t trace_budget = remaining_budget > X86_JIT_TRACE_MAX_INSNS ?
            X86_JIT_TRACE_MAX_INSNS : remaining_budget;
        x86_jit_block_t *trace = jit_compile_trace(block->pc, trace_budget, hot);
        if (trace != NULL) block = trace;
      }
      else if (jit_hot_trace_is_valid(hot)) {
        block = &jit_cache[hot->trace_index];
      }
    }

    if (block->guest_insns > remaining_budget) {
      return *executed > 0;
    }

    if (block->paging && block->is_trace) {
      JIT_STAT_INC(paged_trace_hits);
    }

    if (block->uses_global_loop_accounting) {
      jit_entry_budget = remaining_budget;
    }
    if (jit_chain_enabled || block->uses_global_loop_accounting || block->paging) {
      jit_loop_extra = 0;
      jit_chain_abort = 0;
    }

    jit_fault_guest_count = 0;
    uint32_t ran = 0;
    if (block->paging) {
      x86_exception_env_valid = true;
      if (setjmp(x86_exception_env) == 0) {
        ran = jit_paged_batch_enabled ?
            jit_batch_enter(block->c_entry, remaining_budget) :
            block->c_entry(remaining_budget);
        x86_exception_env_valid = false;
      }
      else {
        x86_mmu_clear_cpl_override();
        x86_exception_env_valid = false;
        cpu.pc = x86_exception_target;
        ran = jit_fault_guest_count;
        if (ran == 0) ran = 1;
      }
    }
    else {
      ran = jit_batch_enter(block->c_entry, remaining_budget);
    }

    if (ran == 0) return *executed > 0;
    Assert(ran > 0 && ran <= budget - *executed,
        "x86 JIT block returned invalid count %u", ran);
    if (hot != NULL && !block->is_trace && ran == block->guest_insns) {
      const x86_jit_block_cold_t *cold = jit_block_cold_const(block);
      if (block->guest_insns != 0) {
        const x86_jit_insn_t *last = &cold->insns[block->guest_insns - 1u];
        if (last->op == X86_JIT_OP_JCC_REL) {
          if (cpu.pc == jit_branch_target(last) &&
              hot->taken_count != UINT32_MAX) {
            hot->taken_count++;
          }
          else if (cpu.pc == last->next_pc &&
              hot->fallthrough_count != UINT32_MAX) {
            hot->fallthrough_count++;
          }
        }
      }
    }
    *executed += ran;
    JIT_STAT_INC(blocks_executed);
    JIT_STAT_ADD(executed_insns, ran);
  }

  return *executed > 0;
}

/* Public hook: discard every compiled block and every JIT-owned lookup table. */
void isa_jit_flush_all(void) {
  memset(jit_cache, 0, sizeof(jit_cache));
  memset(jit_cache_cold, 0, sizeof(jit_cache_cold));
  memset(jit_l0_cache, 0, sizeof(jit_l0_cache));
  memset(jit_hot_info, 0, sizeof(jit_hot_info));
  jit_ret_cache_clear();
  memset(jit_source_page_has_code, 0, sizeof(jit_source_page_has_code));
  memset(jit_source_page_blocks, 0, sizeof(jit_source_page_blocks));
  jit_dtlb_flush();
  jit_cache_age_clock = 1;
  jit_cache_bump_generation();
  jit_code_used = 0;
}

/* Public hook: discard only the private data-translation cache. */
void isa_jit_flush_data_tlb(void) {
  jit_dtlb_flush();
  jit_ret_cache_clear();
  jit_chain_abort = 1;
}

/* Public hook: cheap pre-check for PMEM writes that may stale JIT state. */
bool isa_jit_may_invalidate_paddr(paddr_t addr, int len) {
  return jit_range_may_touch_source_pages(addr, len) ||
         jit_range_may_touch_page_table_pages(addr, len);
}

/* Public hook: react to PMEM writes that can stale native code or data TLBs. */
void isa_jit_invalidate_paddr(paddr_t addr, int len) {
  if (len <= 0) return;

  JIT_STAT_INC(invalidation_requests);
  const bool touches_page_table =
      jit_range_may_touch_page_table_pages(addr, len);
  const bool touches_source = jit_range_may_touch_source_pages(addr, len);

  if (touches_page_table) {
    jit_dtlb_flush();
    jit_paging_bump_generation();
    JIT_STAT_INC(page_table_write_invalidations);
  }

  if (!touches_source) {
    JIT_STAT_INC(invalidation_page_skips);
    return;
  }

  size_t first = 0;
  size_t last = 0;
  if (!jit_source_page_range(addr, len, &first, &last)) {
    JIT_STAT_INC(invalidation_page_skips);
    return;
  }
  const paddr_t end = addr + (paddr_t)len - 1u;

  JIT_STAT_INC(precise_invalidation_scans);

  bool needs_full_scan = false;
  for (size_t page = first; page <= last; page++) {
    if (jit_source_page_blocks[page].overflow) {
      needs_full_scan = true;
      break;
    }
  }

  bool invalidated_source = false;
  if (needs_full_scan) {
    for (uint32_t i = 0; i < X86_JIT_CACHE_SIZE; i++) {
      x86_jit_block_t *block = &jit_cache[i];
      if (!block->valid || block->source_len == 0) continue;
      if (!jit_block_touches_source_page_range(block, first, last)) continue;
      if (!jit_block_source_overlaps_paddr_range(block, addr, end)) continue;
      jit_block_invalidate(block);
      jit_chain_abort = 1;
      invalidated_source = true;
      JIT_STAT_INC(invalidated_blocks);
      JIT_STAT_INC(source_alias_invalidations);
    }
    if (invalidated_source) {
      jit_dtlb_clear_entries();
      memset(jit_l0_cache, 0, sizeof(jit_l0_cache));
      memset(jit_hot_info, 0, sizeof(jit_hot_info));
    }
    return;
  }

  for (size_t page = first; page <= last; page++) {
    x86_jit_source_page_blocks_t *blocks = &jit_source_page_blocks[page];
    const uint16_t count = blocks->count;
    uint32_t indices[X86_JIT_SOURCE_PAGE_BLOCK_LIMIT];
    memcpy(indices, blocks->block_indices, count * sizeof(indices[0]));

    for (uint16_t i = 0; i < count; i++) {
      if (indices[i] >= X86_JIT_CACHE_SIZE) continue;
      x86_jit_block_t *block = &jit_cache[indices[i]];
      if (!block->valid || block->source_len == 0) continue;
      if (!jit_block_source_overlaps_paddr_range(block, addr, end)) continue;
      jit_block_invalidate(block);
      jit_chain_abort = 1;
      invalidated_source = true;
      JIT_STAT_INC(invalidated_blocks);
      JIT_STAT_INC(source_alias_invalidations);
    }
  }

  if (invalidated_source) {
    jit_dtlb_clear_entries();
    memset(jit_l0_cache, 0, sizeof(jit_l0_cache));
    memset(jit_hot_info, 0, sizeof(jit_hot_info));
  }
}

/* Public hook: print optional JIT counters collected during execution. */
void isa_jit_dump_stats(void) {
  jit_init_runtime_options();

  if (jit_runtime_disabled()) {
    if (jit_stats_enabled) {
      if (jit_env_force_disable) {
        Log("jit: disabled by NEMU_DISABLE_JIT=1");
      }
      else {
        Log("jit: disabled by NEMU_X86_JIT=0");
      }
    }
    return;
  }

#if X86_JIT_STATS
  if (!jit_stats_enabled || (jit_code == NULL && jit_stats.exec_requests == 0)) {
    return;
  }

  const uint64_t cache_total = jit_stats.cache_hits + jit_stats.cache_misses;
  const uint64_t cache_hit_pct =
      jit_percent_x100(jit_stats.cache_hits, cache_total);
  const uint64_t avg_compile_len =
      jit_ratio_x100(jit_stats.compiled_insns, jit_stats.blocks_compiled);
  const uint64_t avg_exec_len =
      jit_ratio_x100(jit_stats.executed_insns, jit_stats.blocks_executed);
  const uint64_t direct_chain_hits = jit_direct_chain_hits_runtime;
  const uint64_t trace_hits = jit_trace_hits_runtime;
  const uint64_t side_exits = jit_side_exits_runtime;
  const uint64_t chain_exit_budget = jit_chain_exit_budget_runtime;
  const uint64_t chain_exit_abort = jit_chain_exit_abort_runtime;
  const uint64_t chain_exit_unlinked = jit_chain_exit_unlinked_runtime;
  const uint64_t chain_exit_cold_trace = jit_chain_exit_cold_trace_runtime;
  const uint64_t chain_exit_side_branch = jit_chain_exit_side_branch_runtime;
  const uint64_t chain_exit_helper = jit_chain_exit_helper_runtime;
  const uint64_t chain_exit_unaccepted_successor =
      jit_chain_exit_unaccepted_successor_runtime;
  const uint64_t chain_exit_block_not_chainable =
      jit_chain_exit_block_not_chainable_runtime;
  const uint64_t trace_side_exit_taken = jit_trace_side_exit_taken_runtime;
  const uint64_t trace_side_exit_fallthrough =
      jit_trace_side_exit_fallthrough_runtime;
  const uint64_t trace_loopback = jit_trace_loopback_runtime;
  const uint64_t smc_invalidation_exits = jit_smc_invalidation_exits_runtime;
  const uint64_t mov_rm_reg_slow_exits =
      jit_mov_rm_reg_slow_exits_runtime;
  const uint64_t ret_cache_hits = jit_ret_cache_hits_runtime;
  const uint64_t ret_cache_misses = jit_ret_cache_misses_runtime;
  const uint64_t sibling_trace_hits = jit_sibling_trace_hits_runtime;

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
      ", paged blocks = %" PRIu64
      ", unsupported blocks = %" PRIu64
      ", avg compiled length = %" PRIu64 ".%02" PRIu64 " insn",
      jit_stats.blocks_compiled,
      jit_stats.paged_blocks_compiled,
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
  Log("jit: native ALU ops = %" PRIu64
      ", native ALU/Jcc fusions = %" PRIu64
      ", native ALU/Jcc resident loops = %" PRIu64
      ", native inc/dec ops = %" PRIu64
      ", native inc/dec Jcc backedges = %" PRIu64
      ", native inc/dec resident loops = %" PRIu64
      ", native branch ops = %" PRIu64
      ", native PMEM loads = %" PRIu64
      ", native PMEM stores = %" PRIu64
      ", native mul ops = %" PRIu64
      ", native imul ops = %" PRIu64
      ", native div ops = %" PRIu64
      ", native shift/rotate ops = %" PRIu64
      ", native not ops = %" PRIu64
      ", native movzx ops = %" PRIu64
      ", native movsx ops = %" PRIu64,
      jit_stats.native_alu_ops,
      jit_stats.native_alu_jcc_fusions,
      jit_stats.native_alu_jcc_resident_loops,
      jit_stats.native_incdec_ops,
      jit_stats.native_incdec_jcc_backedges,
      jit_stats.native_incdec_resident_loops,
      jit_stats.native_branch_ops,
      jit_stats.native_pmem_loads,
      jit_stats.native_pmem_stores,
      jit_stats.native_mul_ops,
      jit_stats.native_imul_ops,
      jit_stats.native_div_ops,
      jit_stats.native_shift_ops,
      jit_stats.native_not_ops,
      jit_stats.native_movzx_ops,
      jit_stats.native_movsx_ops);
  Log("jit: DTLB read hits = %" PRIu64
      " (paged = %" PRIu64 ")",
      jit_stats.dtlb_read_hits,
      jit_stats.paged_dtlb_read_hits);
  Log("jit: DTLB write hits = %" PRIu64
      " (paged = %" PRIu64 ")",
      jit_stats.dtlb_write_hits,
      jit_stats.paged_dtlb_write_hits);
  Log("jit: DTLB fills = %" PRIu64, jit_stats.dtlb_fills);
  Log("jit: DTLB fallbacks = %" PRIu64
      " (paged = %" PRIu64 ")",
      jit_stats.dtlb_fallbacks,
      jit_stats.paged_dtlb_fallbacks);
  Log("jit: DTLB flushes = %" PRIu64, jit_stats.dtlb_flushes);
  Log("jit: flag materialisations emitted = %" PRIu64
      ", guest GPR memory loads emitted = %" PRIu64
      ", guest GPR memory stores emitted = %" PRIu64
      ", native PMEM guards emitted = %" PRIu64,
      jit_stats.flag_materialisations,
      jit_stats.guest_gpr_loads_emitted,
      jit_stats.guest_gpr_stores_emitted,
      jit_stats.native_pmem_guards_emitted);
  Log("jit: direct chain hits = %" PRIu64
      ", direct chain patches = %" PRIu64
      " (paged patches = %" PRIu64
      ", paged hits = %" PRIu64 ")"
      ", batch entries = %" PRIu64
      ", traces compiled = %" PRIu64
      " (paged = %" PRIu64 ")"
      ", trace compile failures = %" PRIu64
      ", trace hits = %" PRIu64
      " (paged = %" PRIu64 ")"
      ", side exits = %" PRIu64
      ", SMC invalidation exits = %" PRIu64,
      direct_chain_hits,
      jit_stats.direct_chain_patches,
      jit_stats.paged_chain_patches,
      jit_stats.paged_chain_hits,
      jit_stats.batch_entries,
      jit_stats.traces_compiled,
      jit_stats.paged_traces_compiled,
      jit_stats.trace_compile_failures,
      trace_hits,
      jit_stats.paged_trace_hits,
      side_exits,
      smc_invalidation_exits);
  Log("jit: helper calls = %" PRIu64
      ", helper inc/dec calls = %" PRIu64
      ", helper inc/dec register calls = %" PRIu64
      ", helper inc/dec r/m calls = %" PRIu64,
      jit_stats.helper_calls,
      jit_stats.helper_incdec_calls,
      jit_stats.helper_incdec_reg_calls,
      jit_stats.helper_incdec_rm_calls);
  Log("jit: native mov-rm-reg slow exits = %" PRIu64,
      mov_rm_reg_slow_exits);
  Log("jit: indirect target cache hits = %" PRIu64
      ", misses = %" PRIu64
      ", paged hits = %" PRIu64
      ", paged misses = %" PRIu64,
      ret_cache_hits,
      ret_cache_misses,
      jit_stats.paged_ret_cache_hits,
      jit_stats.paged_ret_cache_misses);
  Log("jit: sibling trace hits = %" PRIu64, sibling_trace_hits);
  Log("jit: chain exit reasons budget = %" PRIu64
      ", abort = %" PRIu64
      ", unlinked = %" PRIu64
      ", cold trace = %" PRIu64
      ", side branch = %" PRIu64
      ", helper = %" PRIu64
      ", unaccepted successor = %" PRIu64
      ", block not chainable = %" PRIu64,
      chain_exit_budget,
      chain_exit_abort,
      chain_exit_unlinked,
      chain_exit_cold_trace,
      chain_exit_side_branch,
      chain_exit_helper,
      chain_exit_unaccepted_successor,
      chain_exit_block_not_chainable);
  Log("jit: trace side exits taken = %" PRIu64
      ", fallthrough = %" PRIu64
      ", trace loopbacks = %" PRIu64,
      trace_side_exit_taken,
      trace_side_exit_fallthrough,
      trace_loopback);
  Log("jit: chainability blocks chainable = %" PRIu64
      ", not chainable = %" PRIu64
      ", jmp = %" PRIu64
      ", jcc one side = %" PRIu64
      ", jcc both sides = %" PRIu64
      ", ret = %" PRIu64
      ", call-rm = %" PRIu64
      ", unsupported successor = %" PRIu64,
      jit_stats.blocks_chainable,
      jit_stats.blocks_not_chainable,
      jit_stats.blocks_not_chainable_jmp,
      jit_stats.blocks_not_chainable_jcc_one_side,
      jit_stats.blocks_not_chainable_jcc_both_sides,
      jit_stats.blocks_not_chainable_ret,
      jit_stats.blocks_not_chainable_call_rm,
      jit_stats.blocks_not_chainable_unsupported_successor);
  Log("jit: traces using regcache = %" PRIu64
      ", traces not using regcache = %" PRIu64,
      jit_stats.traces_using_regcache,
      jit_stats.traces_not_using_regcache);
  Log("jit: toggles fast chain = %u"
      ", edge pc store = %u"
      ", chain abort check = %u"
      ", trace regcache = %u"
      ", trace sibling = %u"
      ", trace loopback = %u"
      ", regcache wide = %u"
      ", stack fast = %u"
      ", paged trace = %u"
      ", paged chain = %u"
      ", paged retcache = %u"
      ", paged batch = %u"
      ", paged regcache = %u"
      ", paged stack fast = %u"
      ", paged aggressive = %u"
      ", native idiv = %u"
      ", high-byte test = %u",
      jit_fast_chain_enabled ? 1u : 0u,
      jit_edge_pc_store_enabled ? 1u : 0u,
      jit_chain_abort_check_enabled ? 1u : 0u,
      jit_trace_regcache_enabled ? 1u : 0u,
      jit_trace_sibling_enabled ? 1u : 0u,
      jit_trace_loopback_enabled ? 1u : 0u,
      jit_regcache_wide_enabled ? 1u : 0u,
      jit_stack_fast_enabled ? 1u : 0u,
      jit_paged_trace_enabled ? 1u : 0u,
      jit_paged_chain_enabled ? 1u : 0u,
      jit_paged_retcache_enabled ? 1u : 0u,
      jit_paged_batch_enabled ? 1u : 0u,
      jit_paged_regcache_enabled ? 1u : 0u,
      jit_paged_stack_fast_enabled ? 1u : 0u,
      jit_paged_aggressive_enabled ? 1u : 0u,
      jit_native_idiv_enabled ? 1u : 0u,
      jit_native_high_byte_test_enabled ? 1u : 0u);
  for (uint32_t helper = 1; helper < X86_JIT_HELPER_COUNT; helper++) {
    if (jit_stats.helper_by_kind[helper] != 0) {
      const char *name = jit_helper_names[helper] != NULL ?
          jit_helper_names[helper] : "unknown";
      Log("jit: helper profile %-16s calls = %" PRIu64,
          name, jit_stats.helper_by_kind[helper]);
    }
  }
  if (jit_stats.helper_by_kind[X86_JIT_HELPER_SHIFT_RM] != 0) {
    Log("jit: helper shift-rm forms reg = %" PRIu64
        ", mem = %" PRIu64
        ", cl = %" PRIu64
        ", imm = %" PRIu64,
        jit_stats.helper_shift_rm_reg,
        jit_stats.helper_shift_rm_mem,
        jit_stats.helper_shift_rm_cl,
        jit_stats.helper_shift_rm_imm);
    Log("jit: helper shift-rm widths byte = %" PRIu64
        ", word = %" PRIu64
        ", dword = %" PRIu64,
        jit_stats.helper_shift_rm_width[X86_WIDTH_BYTE],
        jit_stats.helper_shift_rm_width[X86_WIDTH_WORD],
        jit_stats.helper_shift_rm_width[X86_WIDTH_DWORD]);
    for (uint32_t op = 0; op < 8; op++) {
      if (jit_stats.helper_shift_rm_op[op] != 0) {
        Log("jit: helper shift-rm op %u calls = %" PRIu64,
            op, jit_stats.helper_shift_rm_op[op]);
      }
    }
  }
  Log("jit: invalidation requests = %" PRIu64
      ", page skips = %" PRIu64
      ", precise invalidation scans = %" PRIu64
      ", invalidated blocks = %" PRIu64
      ", page-table write invalidations = %" PRIu64
      ", source alias invalidations = %" PRIu64
      ", arena resets = %" PRIu64,
      jit_stats.invalidation_requests,
      jit_stats.invalidation_page_skips,
      jit_stats.precise_invalidation_scans,
      jit_stats.invalidated_blocks,
      jit_stats.page_table_write_invalidations,
      jit_stats.source_alias_invalidations,
      jit_stats.arena_resets);
  Log("jit: paged fallback causes source validation = %" PRIu64
      ", key mismatches = %" PRIu64
      ", cross-page = %" PRIu64
      ", mmio = %" PRIu64
      ", stack fast = %" PRIu64
      ", large-page = %" PRIu64
      ", unsupported mode = %" PRIu64,
      jit_stats.paged_source_validation_failures,
      jit_stats.cr3_or_paging_key_mismatches,
      jit_stats.cross_page_fallbacks,
      jit_stats.mmio_fallbacks,
      jit_stats.stack_fast_fallbacks,
      jit_stats.paged_large_page_fallbacks,
      jit_stats.unsupported_paging_mode_fallbacks);
  for (uint32_t opcode = 0; opcode < 256u; opcode++) {
    if (jit_stats.unsupported_by_opcode[opcode] != 0) {
      Log("jit: unsupported opcode 0x%02x = %" PRIu64,
          opcode, jit_stats.unsupported_by_opcode[opcode]);
    }
  }
  for (uint32_t opcode = 0; opcode < 256u; opcode++) {
    if (jit_stats.unsupported_hits_by_opcode[opcode] != 0) {
      Log("jit: unsupported-hit opcode 0x%02x = %" PRIu64,
          opcode, jit_stats.unsupported_hits_by_opcode[opcode]);
    }
  }
  for (uint32_t opcode = 0; opcode < 256u; opcode++) {
    if (jit_stats.unsupported_0f_by_opcode[opcode] != 0) {
      Log("jit: unsupported 0f opcode 0x%02x = %" PRIu64,
          opcode, jit_stats.unsupported_0f_by_opcode[opcode]);
    }
  }
  for (uint32_t opcode = 0; opcode < 256u; opcode++) {
    if (jit_stats.unsupported_0f_hits_by_opcode[opcode] != 0) {
      Log("jit: unsupported-hit 0f opcode 0x%02x = %" PRIu64,
          opcode, jit_stats.unsupported_0f_hits_by_opcode[opcode]);
    }
  }
#else
  if (jit_stats_enabled) {
    Log("jit: stats requested, but this binary was built without X86_JIT_STATS=1");
  }
#endif
}

#else

/* Stub hook used when this binary cannot host the x86 JIT. */
bool isa_jit_available(void) {
  return false;
}

/* Stub hook: no native execution is attempted when the JIT is compiled out. */
bool isa_jit_exec(uint64_t remaining, uint32_t device_budget, uint32_t *executed) {
  (void)remaining;
  (void)device_budget;
  *executed = 0;
  return false;
}

/* Stub hook: no generated state exists to flush. */
void isa_jit_flush_all(void) {
}

/* Stub hook: no private data TLB exists to flush. */
void isa_jit_flush_data_tlb(void) {
}

/* Stub hook: compiled-out JIT state is never invalidated by PMEM writes. */
bool isa_jit_may_invalidate_paddr(paddr_t addr, int len) {
  (void)addr;
  (void)len;
  return false;
}

/* Stub hook: ignore PMEM invalidation notifications when no JIT state exists. */
void isa_jit_invalidate_paddr(paddr_t addr, int len) {
  (void)addr;
  (void)len;
}

/* Stub hook: no JIT statistics exist when the JIT is compiled out. */
void isa_jit_dump_stats(void) {
}

#endif
