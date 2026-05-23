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
 * MMU and key blocks by CR3.  Paged guest memory operations deliberately stay on
 * translated C helpers until the native memory emitters grow page-table guards;
 * this keeps the first paged mode fast path local to x86 and preserves the
 * RISC-V interpreter/JIT paths.
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

#define X86_CR0_PG 0x80000000u
#define X86_DWORD_LIMIT 0xffffffffu
#define X86_BYTE_MASK 0xffu
#define X86_WORD_MASK 0xffffu
#define X86_EFLAGS_FIXED_ONE (1u << 1)
#define X86_AUX_CARRY_BIT 0x10u
#define X86_PARITY_FOLD_NIBBLE_MASK 0xfu
#define X86_PARITY_FOLD_TABLE 0x6996u

#define X86_FLAG_CF (1u << 0)
#define X86_FLAG_PF (1u << 2)
#define X86_FLAG_AF (1u << 4)
#define X86_FLAG_ZF (1u << 6)
#define X86_FLAG_SF (1u << 7)
#define X86_FLAG_OF (1u << 11)
#define X86_EFLAGS_STATUS_MASK \
  (X86_FLAG_CF | X86_FLAG_PF | X86_FLAG_AF | \
      X86_FLAG_ZF | X86_FLAG_SF | X86_FLAG_OF)
#define X86_EFLAGS_LOGIC_COPY_MASK (X86_FLAG_PF | X86_FLAG_ZF | X86_FLAG_SF)
#define X86_EFLAGS_INCDEC_COPY_MASK \
  (X86_FLAG_PF | X86_FLAG_AF | X86_FLAG_ZF | X86_FLAG_SF | X86_FLAG_OF)

#define X86_WIDTH_BYTE 1u
#define X86_WIDTH_WORD 2u
#define X86_WIDTH_DWORD 4u
#define X86_SHIFT_COUNT_MASK 0x1fu
#define X86_BITS_PER_BYTE 8u
#define X86_WORD_BITS 16u
#define X86_DWORD_BITS 32u
#define X86_DWORD_BASE 0x100000000ll

#define X86_JIT_BLOCK_MAX_INSNS 64u
#define X86_JIT_BATCH_MAX_INSNS 65536u
#define X86_JIT_CACHE_SETS 4096u
#define X86_JIT_CACHE_WAYS 4u
#define X86_JIT_CACHE_SIZE (X86_JIT_CACHE_SETS * X86_JIT_CACHE_WAYS)
#define X86_JIT_L0_SIZE 4096u
#define X86_JIT_DEFAULT_BLOCK_LIMIT 32u
#define X86_JIT_CODE_SIZE (16u * 1024u * 1024u)
#define X86_JIT_BLOCK_CODE_HEADROOM 16384u
#define X86_JIT_CODE_ALIGN 16u
#define X86_JIT_MAX_SOURCE_BYTES 512u
#define X86_JIT_SOURCE_PAGE_SHIFT 12u
#define X86_JIT_SOURCE_PAGE_SIZE (1u << X86_JIT_SOURCE_PAGE_SHIFT)
#define X86_JIT_SOURCE_PAGE_COUNT \
  (((size_t)CONFIG_MSIZE + X86_JIT_SOURCE_PAGE_SIZE - 1u) / \
      X86_JIT_SOURCE_PAGE_SIZE)
#define X86_JIT_SOURCE_PAGE_BLOCK_LIMIT 16u
#define X86_JIT_BLOCK_SOURCE_PAGE_LIMIT 4u
#define X86_JIT_BLOCK_SOURCE_RANGE_LIMIT 4u

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
} x86_jit_emit_ctx_t;

typedef enum {
  X86_JIT_OP_NOP,
  X86_JIT_OP_MOV_IMM_REG,
  X86_JIT_OP_MOV_REG_REG,
  X86_JIT_OP_LEA,
  X86_JIT_OP_ALU_REG_REG,
  X86_JIT_OP_ALU_IMM_REG,
  X86_JIT_OP_TEST_REG_REG,
  X86_JIT_OP_TEST_EAX_IMM,
  X86_JIT_OP_JMP_REL,
  X86_JIT_OP_JCC_REL,
  X86_JIT_OP_HELPER,
} x86_jit_op_t;

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

typedef struct {
  paddr_t start;
  paddr_t end;
} x86_jit_source_range_t;

typedef struct {
  bool valid;
  bool unsupported;
  bool paging;
  vaddr_t pc;
  uint32_t cr3_key;
  uint32_t source_len;
  uint8_t unsupported_opcode2;
  x86_jit_entry_t entry;
  uint32_t guest_insns;
  uint32_t cache_age;
  bool uses_loop_accounting;
  bool uses_global_loop_accounting;
  uint32_t cold_index;
} x86_jit_block_t;

typedef struct {
  uint8_t source[X86_JIT_MAX_SOURCE_BYTES];
  uint16_t source_page_count;
  bool source_page_overflow;
  size_t source_pages[X86_JIT_BLOCK_SOURCE_PAGE_LIMIT];
  uint16_t source_range_count;
  bool source_range_overflow;
  x86_jit_source_range_t source_ranges[X86_JIT_BLOCK_SOURCE_RANGE_LIMIT];
  x86_jit_insn_t insns[X86_JIT_BLOCK_MAX_INSNS];
} x86_jit_block_cold_t;

typedef struct {
  uint32_t block_indices[X86_JIT_SOURCE_PAGE_BLOCK_LIMIT];
  uint16_t count;
  bool overflow;
} x86_jit_source_page_blocks_t;

typedef struct {
  bool valid;
  vaddr_t pc;
  uint32_t cr3_key;
  uint32_t hot_index;
  uint32_t generation;
} x86_jit_l0_entry_t;

typedef struct {
  uint64_t exec_requests;
  uint64_t cache_hits;
  uint64_t cache_misses;
  uint64_t blocks_compiled;
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
  uint64_t helper_by_kind[X86_JIT_HELPER_COUNT];
  uint64_t invalidation_requests;
  uint64_t invalidation_page_skips;
  uint64_t precise_invalidation_scans;
  uint64_t invalidated_blocks;
  uint64_t arena_resets;
  uint64_t unsupported_by_opcode[256];
  uint64_t unsupported_hits_by_opcode[256];
  uint64_t unsupported_0f_by_opcode[256];
  uint64_t unsupported_0f_hits_by_opcode[256];
} x86_jit_stats_t;

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

enum {
  X86_CC_O  = 0x0,
  X86_CC_NO = 0x1,
  X86_CC_B  = 0x2,
  X86_CC_AE = 0x3,
  X86_CC_Z  = 0x4,
  X86_CC_NZ = 0x5,
  X86_CC_BE = 0x6,
  X86_CC_A  = 0x7,
  X86_CC_S  = 0x8,
  X86_CC_NS = 0x9,
  X86_CC_P  = 0xa,
  X86_CC_NP = 0xb,
  X86_CC_L  = 0xc,
  X86_CC_GE = 0xd,
  X86_CC_LE = 0xe,
  X86_CC_G  = 0xf,
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
static uint32_t jit_runtime_block_limit = X86_JIT_DEFAULT_BLOCK_LIMIT;
static x86_jit_block_cold_t jit_cache_cold[X86_JIT_CACHE_SIZE];
static x86_jit_l0_entry_t jit_l0_cache[X86_JIT_L0_SIZE];
static volatile uint32_t jit_entry_budget = 0;
static volatile uint32_t jit_loop_extra = 0;
static volatile uint32_t jit_chain_abort = 0;
static volatile uint32_t jit_fault_guest_count = 0;
static bool jit_source_page_has_code[X86_JIT_SOURCE_PAGE_COUNT];
static x86_jit_source_page_blocks_t
    jit_source_page_blocks[X86_JIT_SOURCE_PAGE_COUNT];
static uint32_t jit_cache_age_clock = 1;
static uint32_t jit_cache_generation = 1;
static x86_jit_stats_t jit_stats;

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

#define JIT_STAT_INC(field) \
  do { \
    if (__builtin_expect(jit_stats_enabled, false)) jit_stats.field++; \
  } while (0)

#define JIT_STAT_ADD(field, value) \
  do { \
    if (__builtin_expect(jit_stats_enabled, false)) jit_stats.field += (value); \
  } while (0)

static bool jit_env_flag_enabled(const char *name) {
  const char *value = getenv(name);
  return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
}

static bool jit_env_flag_disabled(const char *name) {
  const char *value = getenv(name);
  return value != NULL && value[0] != '\0' && strcmp(value, "0") == 0;
}

static bool jit_env_flag_default_enabled(const char *name) {
  return !jit_env_flag_disabled(name);
}

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
    jit_runtime_block_limit = jit_env_u32("NEMU_X86_JIT_BLOCK_LIMIT",
        X86_JIT_DEFAULT_BLOCK_LIMIT, 1u, X86_JIT_BLOCK_MAX_INSNS);
    jit_runtime_options_init = true;
  }
}

static bool jit_runtime_disabled(void) {
  jit_init_runtime_options();
  return jit_env_force_disable || !jit_env_enable;
}

static bool jit_helper_translation_enabled(void) {
  jit_init_runtime_options();
  return jit_helpers_enabled;
}

static uint64_t jit_ratio_x100(uint64_t numerator, uint64_t denominator) {
  if (denominator == 0) return 0;
  return (numerator * 100u + denominator / 2u) / denominator;
}

static uint64_t jit_percent_x100(uint64_t numerator, uint64_t denominator) {
  if (denominator == 0) return 0;
  return (numerator * 10000u + denominator / 2u) / denominator;
}

static bool jit_paging_enabled(void) {
  return (cpu.cr0 & X86_CR0_PG) != 0;
}

static uint32_t jit_cr3_key(void) {
  return jit_paging_enabled() ? (uint32_t)(cpu.cr3 & ~(uint32_t)PAGE_MASK) : 0u;
}

static bool jit_flat_segments(void) {
  for (uint32_t i = 0; i < 4; i++) {
    if (cpu.seg_cache[i].base != 0 || cpu.seg_cache[i].limit != X86_DWORD_LIMIT) {
      return false;
    }
  }

  return true;
}

static bool jit_flat_direct_fetch(vaddr_t pc, uint32_t len,
    const uint8_t **host) {
  if (!jit_flat_source_enabled || host == NULL || len == 0) return false;
  if (jit_paging_enabled() || !jit_flat_segments()) return false;
  if (!in_pmem_range((paddr_t)pc, (int)len)) return false;

  *host = guest_to_host((paddr_t)pc);
  return true;
}

static int jit_translate_status(paddr_t ret) {
  return (int)(ret & (paddr_t)PAGE_MASK);
}

static paddr_t jit_translate_page(paddr_t ret) {
  return ret & ~(paddr_t)PAGE_MASK;
}

static bool jit_vaddr_to_paddr(vaddr_t addr, uint32_t len, int type,
    paddr_t *pa) {
  if (len == 0) return false;

  const int mmu = isa_mmu_check(addr, (int)len, type);
  if (mmu == MMU_DIRECT) {
    *pa = (paddr_t)addr;
    return in_pmem_range(*pa, (int)len);
  }

  if (mmu != MMU_TRANSLATE) return false;

  const paddr_t ret = isa_mmu_translate(addr, (int)len, type);
  if (jit_translate_status(ret) != MEM_RET_OK) return false;

  *pa = jit_translate_page(ret) | (paddr_t)(addr & PAGE_MASK);
  return in_pmem_range(*pa, (int)len);
}

static bool jit_vaddr_read_u8(vaddr_t addr, uint8_t *value) {
  const uint8_t *host = NULL;
  if (jit_flat_direct_fetch(addr, 1u, &host)) {
    *value = *host;
    return true;
  }

  paddr_t pa = 0;
  if (!jit_vaddr_to_paddr(addr, 1u, MEM_TYPE_IFETCH, &pa)) return false;
  *value = host_read(guest_to_host(pa), 1);
  return true;
}

static bool jit_read_u8(x86_jit_reader_t *r, uint8_t *value) {
  if (!jit_vaddr_read_u8(r->cur, value)) return false;
  r->cur++;
  return true;
}

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

static bool jit_read_i8(x86_jit_reader_t *r, int32_t *value) {
  uint8_t raw = 0;
  if (!jit_read_u8(r, &raw)) return false;
  *value = (int8_t)raw;
  return true;
}

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

static bool jit_paddr_source_page(paddr_t addr, size_t *page) {
  if (!in_pmem(addr)) return false;

  const paddr_t offset = addr - (paddr_t)CONFIG_MBASE;
  const size_t idx = (size_t)(offset >> X86_JIT_SOURCE_PAGE_SHIFT);
  if (idx >= X86_JIT_SOURCE_PAGE_COUNT) return false;

  *page = idx;
  return true;
}

static uint32_t jit_block_index(const x86_jit_block_t *block) {
  return (uint32_t)(block - jit_cache);
}

static x86_jit_block_cold_t *jit_block_cold(x86_jit_block_t *block) {
  return &jit_cache_cold[jit_block_index(block)];
}

static const x86_jit_block_cold_t *jit_block_cold_const(
    const x86_jit_block_t *block) {
  return &jit_cache_cold[jit_block_index(block)];
}

static void jit_cache_bump_generation(void) {
  jit_cache_generation++;
  if (jit_cache_generation == 0) jit_cache_generation = 1;
}

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

static void jit_block_unregister_source_pages(x86_jit_block_t *block) {
  x86_jit_block_cold_t *cold = jit_block_cold(block);
  if (cold->source_page_count == 0) return;

  const uint32_t block_index = jit_block_index(block);
  for (uint16_t i = 0; i < cold->source_page_count; i++) {
    jit_source_page_remove_block(cold->source_pages[i], block_index);
  }
  cold->source_page_count = 0;
}

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

static void jit_mark_source_pages(x86_jit_block_t *block, vaddr_t pc,
    uint32_t len) {
  x86_jit_block_cold_t *cold = jit_block_cold(block);
  cold->source_page_count = 0;
  cold->source_page_overflow = false;
  cold->source_range_count = 0;
  cold->source_range_overflow = false;

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
      return;
    }

    for (size_t page = first; page <= last; page++) {
      jit_block_register_source_page(block, page);
    }

    cold->source_ranges[0] =
        (x86_jit_source_range_t){ .start = start, .end = end };
    cold->source_range_count = 1;
    return;
  }

  for (uint32_t i = 0; i < len; i++) {
    paddr_t pa = 0;
    size_t page = 0;
    if (jit_vaddr_to_paddr(pc + i, 1u, MEM_TYPE_IFETCH, &pa) &&
        jit_paddr_source_page(pa, &page)) {
      jit_block_register_source_page(block, page);
      jit_block_register_source_range(block, pa);
    }
  }
}

static void jit_block_invalidate(x86_jit_block_t *block) {
  if (block->valid) jit_block_unregister_source_pages(block);
  block->valid = false;
  jit_cache_bump_generation();
}

static void jit_block_discard(x86_jit_block_t *block) {
  jit_block_invalidate(block);
  memset(block, 0, sizeof(*block));
}

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

static uint32_t jit_width_mask(uint8_t width) {
  return width == X86_WIDTH_DWORD ? X86_DWORD_LIMIT :
      ((1u << (width * 8u)) - 1u);
}

static uint32_t jit_sign_bit(uint8_t width) {
  return 1u << (width * 8u - 1u);
}

static uint32_t jit_mask_width(uint32_t val, uint8_t width) {
  return val & jit_width_mask(width);
}

static uint32_t jit_sign_extend(uint32_t val, uint8_t width) {
  switch (width) {
    case X86_WIDTH_BYTE: return (uint32_t)(int32_t)(int8_t)val;
    case X86_WIDTH_WORD: return (uint32_t)(int32_t)(int16_t)val;
    case X86_WIDTH_DWORD: return val;
    default: panic("x86 JIT helper bad sign-extension width %u", width);
  }
}

static int64_t jit_signed_width(uint32_t val, uint8_t width) {
  switch (width) {
    case X86_WIDTH_BYTE: return (int8_t)val;
    case X86_WIDTH_WORD: return (int16_t)val;
    case X86_WIDTH_DWORD: return (int32_t)val;
    default: panic("x86 JIT helper bad signed width %u", width);
  }
}

static bool jit_flag_get(uint32_t flag) {
  return (cpu.eflags & flag) != 0;
}

static void jit_flag_set(uint32_t flag, bool val) {
  if (val) cpu.eflags |= flag;
  else cpu.eflags &= ~flag;
  cpu.eflags |= X86_EFLAGS_FIXED_ONE;
}

static bool jit_parity_even(uint8_t val) {
  val ^= val >> 4;
  val &= X86_PARITY_FOLD_NIBBLE_MASK;
  return ((X86_PARITY_FOLD_TABLE >> val) & 1u) == 0;
}

static void jit_set_zsp_flags(uint32_t result, uint8_t width) {
  result = jit_mask_width(result, width);
  jit_flag_set(X86_FLAG_ZF, result == 0);
  jit_flag_set(X86_FLAG_SF, (result & jit_sign_bit(width)) != 0);
  jit_flag_set(X86_FLAG_PF, jit_parity_even(result & X86_BYTE_MASK));
}

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
    case X86_ALU_ADC:
    case X86_ALU_SBB:
      panic("x86 JIT helper reached unsupported carry-dependent ALU op %u", op);
      break;
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

static uint32_t jit_reg_read(uint8_t reg, uint8_t width) {
  switch (width) {
    case X86_WIDTH_BYTE: return reg_b(reg);
    case X86_WIDTH_WORD: return reg_w(reg);
    case X86_WIDTH_DWORD: return reg_l(reg);
    default: panic("x86 JIT helper bad register width %u", width);
  }
}

static void jit_reg_write(uint8_t reg, uint8_t width, uint32_t data) {
  switch (width) {
    case X86_WIDTH_BYTE: reg_b(reg) = data; return;
    case X86_WIDTH_WORD: reg_w(reg) = data; return;
    case X86_WIDTH_DWORD: reg_l(reg) = data; return;
    default: panic("x86 JIT helper bad register width %u", width);
  }
}

static uint32_t jit_mem_read(vaddr_t addr, uint8_t width) {
  return vaddr_read(addr, width);
}

static void jit_mem_write(vaddr_t addr, uint8_t width, uint32_t data) {
  vaddr_write(addr, width, data);
}

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

static uint32_t jit_rm_read(const x86_jit_insn_t *insn, uint8_t width) {
  if (insn->rm_is_reg) {
    return jit_reg_read(insn->rm_reg, width);
  }

  return jit_mem_read(jit_ea_addr(insn), width);
}

static void jit_rm_write(const x86_jit_insn_t *insn, uint8_t width,
    uint32_t data) {
  if (insn->rm_is_reg) {
    jit_reg_write(insn->rm_reg, width, data);
  }
  else {
    jit_mem_write(jit_ea_addr(insn), width, data);
  }
}

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

static void jit_push32(uint32_t data) {
  cpu.esp -= X86_WIDTH_DWORD;
  jit_mem_write(cpu.esp, X86_WIDTH_DWORD, data);
}

static uint32_t jit_pop32(void) {
  uint32_t data = jit_mem_read(cpu.esp, X86_WIDTH_DWORD);
  cpu.esp += X86_WIDTH_DWORD;
  return data;
}

static uint32_t jit_branch_target(const x86_jit_insn_t *insn) {
  return (uint32_t)(insn->next_pc + insn->rel);
}

static void jit_helper_exec(const x86_jit_insn_t *insn) {
  uint32_t lhs = 0, rhs = 0, result = 0;
  uint32_t old_eflags = 0, new_eflags = 0;

  cpu.pc = insn->pc;
  jit_fault_guest_count = jit_loop_extra + insn->ordinal;
  JIT_STAT_INC(helper_calls);
  if (__builtin_expect(jit_stats_enabled, false) &&
      insn->helper < X86_JIT_HELPER_COUNT) {
    jit_stats.helper_by_kind[insn->helper]++;
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
      jit_reg_write(insn->dst, X86_WIDTH_DWORD,
          jit_sign_extend(jit_rm_read(insn, X86_WIDTH_BYTE), X86_WIDTH_BYTE));
      return;
    case X86_JIT_HELPER_MOVSX_REG_RM16:
      jit_reg_write(insn->dst, X86_WIDTH_DWORD,
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

static bool emit_u8(x86_jit_writer_t *w, uint8_t value) {
  if (w->cur >= w->end) return false;
  *w->cur++ = value;
  return true;
}

static bool emit_u32(x86_jit_writer_t *w, uint32_t value) {
  for (uint32_t i = 0; i < 4; i++) {
    if (!emit_u8(w, (uint8_t)(value >> (i * 8)))) return false;
  }
  return true;
}

static bool emit_u64(x86_jit_writer_t *w, uint64_t value) {
  for (uint32_t i = 0; i < 8; i++) {
    if (!emit_u8(w, (uint8_t)(value >> (i * 8)))) return false;
  }
  return true;
}

static bool emit_movabs_rdx(x86_jit_writer_t *w, uint64_t value) {
  return emit_u8(w, 0x48) && emit_u8(w, 0xba) && emit_u64(w, value);
}

static bool emit_movabs_rax(x86_jit_writer_t *w, uint64_t value) {
  return emit_u8(w, 0x48) && emit_u8(w, 0xb8) && emit_u64(w, value);
}

static bool emit_mov_eax_moffs64(x86_jit_writer_t *w, uint64_t addr) {
  return emit_u8(w, 0xa1) && emit_u64(w, addr);
}

static bool emit_mov_moffs64_eax(x86_jit_writer_t *w, uint64_t addr) {
  return emit_u8(w, 0xa3) && emit_u64(w, addr);
}

static bool emit_movabs_rdi(x86_jit_writer_t *w, uint64_t value) {
  return emit_u8(w, 0x48) && emit_u8(w, 0xbf) && emit_u64(w, value);
}

static bool emit_movabs_r10(x86_jit_writer_t *w, uint64_t value) {
  return emit_u8(w, 0x49) && emit_u8(w, 0xba) && emit_u64(w, value);
}

static bool emit_movabs_r11(x86_jit_writer_t *w, uint64_t value) {
  return emit_u8(w, 0x49) && emit_u8(w, 0xbb) && emit_u64(w, value);
}

static bool emit_sub_rsp_imm8(x86_jit_writer_t *w, uint8_t value) {
  return emit_u8(w, 0x48) && emit_u8(w, 0x83) &&
         emit_u8(w, 0xec) && emit_u8(w, value);
}

static bool emit_add_rsp_imm8(x86_jit_writer_t *w, uint8_t value) {
  return emit_u8(w, 0x48) && emit_u8(w, 0x83) &&
         emit_u8(w, 0xc4) && emit_u8(w, value);
}

static bool emit_call_rax(x86_jit_writer_t *w) {
  return emit_u8(w, 0xff) && emit_u8(w, 0xd0);
}

static bool emit_mov_eax_imm32(x86_jit_writer_t *w, uint32_t value) {
  return emit_u8(w, 0xb8) && emit_u32(w, value);
}

static bool emit_mov_ecx_imm32(x86_jit_writer_t *w, uint32_t value) {
  return emit_u8(w, 0xb9) && emit_u32(w, value);
}

static bool emit_mov_r11d_imm32(x86_jit_writer_t *w, uint32_t value) {
  return emit_u8(w, 0x41) && emit_u8(w, 0xbb) && emit_u32(w, value);
}

static bool emit_xor_r11d_r11d(x86_jit_writer_t *w) {
  return emit_u8(w, 0x45) && emit_u8(w, 0x31) && emit_u8(w, 0xdb);
}

static bool emit_xor_edx_edx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x31) && emit_u8(w, 0xd2);
}

static bool emit_mov_m32_rdx_imm32(x86_jit_writer_t *w, uint32_t value) {
  return emit_u8(w, 0xc7) && emit_u8(w, 0x02) && emit_u32(w, value);
}

static bool emit_mov_ecx_m32_rdx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x8b) && emit_u8(w, 0x0a);
}

static bool emit_mov_eax_m32_r10_rdx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x41) && emit_u8(w, 0x8b) &&
         emit_u8(w, 0x04) && emit_u8(w, 0x12);
}

static bool emit_mov_ecx_m32_r10_rdx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x41) && emit_u8(w, 0x8b) &&
         emit_u8(w, 0x0c) && emit_u8(w, 0x12);
}

static bool emit_movzx_eax_m8_r10_rdx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x41) && emit_u8(w, 0x0f) &&
         emit_u8(w, 0xb6) && emit_u8(w, 0x04) && emit_u8(w, 0x12);
}

static bool emit_movzx_eax_m16_r10_rdx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x41) && emit_u8(w, 0x0f) &&
         emit_u8(w, 0xb7) && emit_u8(w, 0x04) && emit_u8(w, 0x12);
}

static bool emit_movsx_eax_m8_r10_rdx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x41) && emit_u8(w, 0x0f) &&
         emit_u8(w, 0xbe) && emit_u8(w, 0x04) && emit_u8(w, 0x12);
}

static bool emit_movsx_eax_m16_r10_rdx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x41) && emit_u8(w, 0x0f) &&
         emit_u8(w, 0xbf) && emit_u8(w, 0x04) && emit_u8(w, 0x12);
}

static bool emit_movsx_eax_al(x86_jit_writer_t *w) {
  return emit_u8(w, 0x0f) && emit_u8(w, 0xbe) && emit_u8(w, 0xc0);
}

static bool emit_movsx_eax_ax(x86_jit_writer_t *w) {
  return emit_u8(w, 0x0f) && emit_u8(w, 0xbf) && emit_u8(w, 0xc0);
}

static bool emit_movzx_ecx_m8_r10_rdx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x41) && emit_u8(w, 0x0f) &&
         emit_u8(w, 0xb6) && emit_u8(w, 0x0c) && emit_u8(w, 0x12);
}

static bool emit_movzx_ecx_m16_r10_rdx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x41) && emit_u8(w, 0x0f) &&
         emit_u8(w, 0xb7) && emit_u8(w, 0x0c) && emit_u8(w, 0x12);
}

static bool emit_mov_m8_r10_rdx_al(x86_jit_writer_t *w) {
  return emit_u8(w, 0x41) && emit_u8(w, 0x88) &&
         emit_u8(w, 0x04) && emit_u8(w, 0x12);
}

static bool emit_mov_m16_r10_rdx_ax(x86_jit_writer_t *w) {
  return emit_u8(w, 0x66) && emit_u8(w, 0x41) && emit_u8(w, 0x89) &&
         emit_u8(w, 0x04) && emit_u8(w, 0x12);
}

static bool emit_mov_m32_r10_rdx_r11d(x86_jit_writer_t *w) {
  return emit_u8(w, 0x45) && emit_u8(w, 0x89) &&
         emit_u8(w, 0x1c) && emit_u8(w, 0x12);
}

static bool emit_mov_r11d_m32_r10_rdx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x45) && emit_u8(w, 0x8b) &&
         emit_u8(w, 0x1c) && emit_u8(w, 0x12);
}

static bool emit_mov_m32_r10_rdx_eax(x86_jit_writer_t *w) {
  return emit_u8(w, 0x41) && emit_u8(w, 0x89) &&
         emit_u8(w, 0x04) && emit_u8(w, 0x12);
}

static bool emit_not_m32_r10_rdx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x41) && emit_u8(w, 0xf7) &&
         emit_u8(w, 0x14) && emit_u8(w, 0x12);
}

static bool emit_mov_r11d_m32_r11(x86_jit_writer_t *w) {
  return emit_u8(w, 0x45) && emit_u8(w, 0x8b) && emit_u8(w, 0x1b);
}

static bool emit_add_eax_m32_rdx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x03) && emit_u8(w, 0x02);
}

static bool emit_shl_ecx_imm(x86_jit_writer_t *w, uint8_t value) {
  return value == 0 || (emit_u8(w, 0xc1) && emit_u8(w, 0xe1) && emit_u8(w, value));
}

static bool emit_add_eax_ecx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x01) && emit_u8(w, 0xc8);
}

static bool emit_add_eax_imm32(x86_jit_writer_t *w, uint32_t value) {
  return emit_u8(w, 0x05) && emit_u32(w, value);
}

static bool emit_add_edx_imm32(x86_jit_writer_t *w, uint32_t value) {
  return emit_u8(w, 0x81) && emit_u8(w, 0xc2) && emit_u32(w, value);
}

static bool emit_cmp_edx_imm32(x86_jit_writer_t *w, uint32_t value) {
  return emit_u8(w, 0x81) && emit_u8(w, 0xfa) && emit_u32(w, value);
}

static bool emit_mov_edx_eax(x86_jit_writer_t *w) {
  return emit_u8(w, 0x89) && emit_u8(w, 0xc2);
}

static bool emit_mov_eax_r11d(x86_jit_writer_t *w) {
  return emit_u8(w, 0x44) && emit_u8(w, 0x89) && emit_u8(w, 0xd8);
}

static bool emit_mov_r10d_eax(x86_jit_writer_t *w) {
  return emit_u8(w, 0x41) && emit_u8(w, 0x89) && emit_u8(w, 0xc2);
}

static bool emit_mov_r10d_ecx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x41) && emit_u8(w, 0x89) && emit_u8(w, 0xca);
}

static bool emit_mov_ecx_r10d(x86_jit_writer_t *w) {
  return emit_u8(w, 0x44) && emit_u8(w, 0x89) && emit_u8(w, 0xd1);
}

static bool emit_mov_r11d_ecx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x41) && emit_u8(w, 0x89) && emit_u8(w, 0xcb);
}

static bool emit_mov_r11d_eax(x86_jit_writer_t *w) {
  return emit_u8(w, 0x41) && emit_u8(w, 0x89) && emit_u8(w, 0xc3);
}

static bool emit_mov_r11d_edx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x41) && emit_u8(w, 0x89) && emit_u8(w, 0xd3);
}

static bool emit_mov_ecx_r11d(x86_jit_writer_t *w) {
  return emit_u8(w, 0x44) && emit_u8(w, 0x89) && emit_u8(w, 0xd9);
}

static bool emit_mov_ecx_edx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x89) && emit_u8(w, 0xd1);
}

static bool emit_mov_eax_edi(x86_jit_writer_t *w) {
  return emit_u8(w, 0x89) && emit_u8(w, 0xf8);
}

static bool emit_mov_ecx_edi(x86_jit_writer_t *w) {
  return emit_u8(w, 0x89) && emit_u8(w, 0xf9);
}

static bool emit_cmp_edx_ecx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x39) && emit_u8(w, 0xca);
}

static bool emit_cmp_ecx_imm32(x86_jit_writer_t *w, uint32_t value) {
  return emit_u8(w, 0x81) && emit_u8(w, 0xf9) && emit_u32(w, value);
}

static bool emit_test_ecx_ecx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x85) && emit_u8(w, 0xc9);
}

static bool emit_and_ecx_imm32(x86_jit_writer_t *w, uint32_t value) {
  return emit_u8(w, 0x81) && emit_u8(w, 0xe1) && emit_u32(w, value);
}

static bool emit_shr_ecx_imm(x86_jit_writer_t *w, uint8_t value) {
  return value == 0 || (emit_u8(w, 0xc1) && emit_u8(w, 0xe9) && emit_u8(w, value));
}

static bool emit_lea_r11d_r11d_disp8(x86_jit_writer_t *w, int8_t disp) {
  return emit_u8(w, 0x45) && emit_u8(w, 0x8d) &&
         emit_u8(w, 0x5b) && emit_u8(w, (uint8_t)disp);
}

static bool emit_lea_r10d_r10d_disp8(x86_jit_writer_t *w, int8_t disp) {
  return emit_u8(w, 0x45) && emit_u8(w, 0x8d) &&
         emit_u8(w, 0x52) && emit_u8(w, (uint8_t)disp);
}

static bool emit_movzx_ecx_m8_r10_rcx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x41) && emit_u8(w, 0x0f) &&
         emit_u8(w, 0xb6) && emit_u8(w, 0x0c) && emit_u8(w, 0x0a);
}

static bool emit_pushfq(x86_jit_writer_t *w) {
  return emit_u8(w, 0x9c);
}

static bool emit_pop_rax(x86_jit_writer_t *w) {
  return emit_u8(w, 0x58);
}

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

static bool emit_test_eax_ecx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x85) && emit_u8(w, 0xc8);
}

static bool emit_test_ax_cx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x66) && emit_u8(w, 0x85) && emit_u8(w, 0xc8);
}

static bool emit_test_al_cl(x86_jit_writer_t *w) {
  return emit_u8(w, 0x84) && emit_u8(w, 0xc8);
}

static bool emit_or_eax_ecx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x09) && emit_u8(w, 0xc8);
}

static bool emit_imul_eax_ecx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x0f) && emit_u8(w, 0xaf) && emit_u8(w, 0xc1);
}

static bool emit_mul_ecx(x86_jit_writer_t *w) {
  return emit_u8(w, 0xf7) && emit_u8(w, 0xe1);
}

static bool emit_imul_acc_ecx(x86_jit_writer_t *w) {
  return emit_u8(w, 0xf7) && emit_u8(w, 0xe9);
}

static bool emit_shift_eax_imm(x86_jit_writer_t *w, uint8_t shift_op,
    uint8_t count) {
  return emit_u8(w, 0xc1) &&
         emit_u8(w, (uint8_t)(0xc0u | ((shift_op & 0x7u) << 3))) &&
         emit_u8(w, count);
}

static bool emit_shift_eax_cl(x86_jit_writer_t *w, uint8_t shift_op) {
  return emit_u8(w, 0xd3) &&
         emit_u8(w, (uint8_t)(0xc0u | ((shift_op & 0x7u) << 3)));
}

static bool emit_not_eax(x86_jit_writer_t *w) {
  return emit_u8(w, 0xf7) && emit_u8(w, 0xd0);
}

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

static bool emit_div_ecx(x86_jit_writer_t *w) {
  return emit_u8(w, 0xf7) && emit_u8(w, 0xf1);
}

static bool emit_ret(x86_jit_writer_t *w) {
  return emit_u8(w, 0xc3);
}

static bool emit_jcc_rel32_placeholder(x86_jit_writer_t *w, uint8_t cc,
    uint8_t **disp) {
  if (!emit_u8(w, 0x0f) || !emit_u8(w, 0x80u | (cc & 0xfu))) return false;
  *disp = w->cur;
  return emit_u32(w, 0);
}

static bool emit_jmp_rel32_placeholder(x86_jit_writer_t *w, uint8_t **disp) {
  if (!emit_u8(w, 0xe9)) return false;
  *disp = w->cur;
  return emit_u32(w, 0);
}

static bool emit_jrcxz_rel8_placeholder(x86_jit_writer_t *w, uint8_t **disp) {
  if (!emit_u8(w, 0xe3)) return false;
  *disp = w->cur;
  return emit_u8(w, 0);
}

static bool patch_rel8(uint8_t *disp, const uint8_t *target) {
  const int64_t rel = (int64_t)(target - (disp + 1));
  if (rel < INT8_MIN || rel > INT8_MAX) return false;

  *disp = (uint8_t)(int8_t)rel;
  return true;
}

static bool patch_rel32(uint8_t *disp, const uint8_t *target) {
  const int64_t rel = (int64_t)(target - (disp + 4));
  if (rel < INT32_MIN || rel > INT32_MAX) return false;

  const uint32_t encoded = (uint32_t)(int32_t)rel;
  for (uint32_t i = 0; i < 4; i++) {
    disp[i] = (uint8_t)(encoded >> (i * 8));
  }
  return true;
}

static bool emit_ret_count(x86_jit_writer_t *w, uint32_t count) {
  return emit_mov_eax_imm32(w, count) && emit_u8(w, 0xc3);
}

static uintptr_t jit_gpr_addr(uint8_t reg) {
  Assert(reg < 8, "bad x86 JIT register %u", reg);
  return (uintptr_t)&cpu.gpr[reg]._32;
}

static bool emit_store_reg_imm(x86_jit_writer_t *w, uint8_t reg, uint32_t value) {
  return emit_movabs_rdx(w, jit_gpr_addr(reg)) &&
         emit_mov_m32_rdx_imm32(w, value);
}

static bool emit_load_reg_eax(x86_jit_writer_t *w, uint8_t reg) {
  return emit_mov_eax_moffs64(w, jit_gpr_addr(reg));
}

static bool emit_load_reg_ecx(x86_jit_writer_t *w, uint8_t reg) {
  return emit_movabs_rdx(w, jit_gpr_addr(reg)) &&
         emit_mov_ecx_m32_rdx(w);
}

static bool emit_load_reg_edx(x86_jit_writer_t *w, uint8_t reg) {
  return emit_load_reg_eax(w, reg) && emit_mov_edx_eax(w);
}

static bool emit_load_reg_r11d(x86_jit_writer_t *w, uint8_t reg) {
  return emit_movabs_r11(w, jit_gpr_addr(reg)) &&
         emit_mov_r11d_m32_r11(w);
}

static bool emit_store_reg_eax(x86_jit_writer_t *w, uint8_t reg) {
  return emit_mov_moffs64_eax(w, jit_gpr_addr(reg));
}

static bool emit_store_pc_imm(x86_jit_writer_t *w, vaddr_t pc) {
  return emit_movabs_rdx(w, (uintptr_t)&cpu.pc) &&
         emit_mov_m32_rdx_imm32(w, pc);
}

static bool emit_store_pc_eax(x86_jit_writer_t *w) {
  return emit_mov_moffs64_eax(w, (uintptr_t)&cpu.pc);
}

static bool emit_add_reg_to_eax(x86_jit_writer_t *w, uint8_t reg) {
  return emit_movabs_rdx(w, jit_gpr_addr(reg)) &&
         emit_add_eax_m32_rdx(w);
}

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

static bool emit_lea(x86_jit_writer_t *w, const x86_jit_insn_t *insn) {
  if (!emit_guest_ea_eax(w, &insn->ea)) return false;
  return emit_store_reg_eax(w, insn->dst);
}

static bool emit_helper_call(x86_jit_writer_t *w, const x86_jit_insn_t *insn) {
  return emit_sub_rsp_imm8(w, 8) &&
         emit_movabs_rdi(w, (uintptr_t)insn) &&
         emit_movabs_rax(w, (uintptr_t)jit_helper_exec) &&
         emit_call_rax(w) &&
         emit_add_rsp_imm8(w, 8);
}

static bool emit_direct_pmem_guard_edx(x86_jit_writer_t *w, uint32_t len,
    uint8_t **slow_disp) {
  if (len == 0 || len > CONFIG_MSIZE) return false;

  return emit_add_edx_imm32(w, 0u - (uint32_t)CONFIG_MBASE) &&
         emit_cmp_edx_imm32(w, (uint32_t)CONFIG_MSIZE - len) &&
         emit_jcc_rel32_placeholder(w, X86_CC_A, slow_disp);
}

static bool emit_direct_store_source_guard_edx(x86_jit_writer_t *w,
    uint32_t len, uint8_t **cross_page_slow_disp,
    uint8_t **source_page_slow_disp) {
  if (len == 0 || len > X86_JIT_SOURCE_PAGE_SIZE) return false;

  return emit_mov_ecx_edx(w) &&
         emit_and_ecx_imm32(w, X86_JIT_SOURCE_PAGE_SIZE - 1u) &&
         emit_cmp_ecx_imm32(w, X86_JIT_SOURCE_PAGE_SIZE - len) &&
         emit_jcc_rel32_placeholder(w, X86_CC_A, cross_page_slow_disp) &&
         emit_mov_ecx_edx(w) &&
         emit_shr_ecx_imm(w, X86_JIT_SOURCE_PAGE_SHIFT) &&
         emit_movabs_r10(w, (uint64_t)(uintptr_t)jit_source_page_has_code) &&
         emit_movzx_ecx_m8_r10_rcx(w) &&
         emit_test_ecx_ecx(w) &&
         emit_jcc_rel32_placeholder(w, X86_CC_NZ, source_page_slow_disp);
}

static bool emit_load_loop_extra_eax(x86_jit_writer_t *w) {
  return emit_mov_eax_moffs64(w, (uintptr_t)&jit_loop_extra);
}

static bool emit_store_loop_extra_eax(x86_jit_writer_t *w) {
  return emit_mov_moffs64_eax(w, (uintptr_t)&jit_loop_extra);
}

static bool emit_load_entry_budget_ecx(x86_jit_writer_t *w) {
  return emit_movabs_rdx(w, (uintptr_t)&jit_entry_budget) &&
         emit_mov_ecx_m32_rdx(w);
}

static bool emit_load_entry_budget_arg_ecx(x86_jit_writer_t *w) {
  return emit_mov_ecx_edi(w);
}

static bool emit_load_entry_budget_arg_eax(x86_jit_writer_t *w) {
  return emit_mov_eax_edi(w);
}

static bool emit_load_chain_abort_ecx(x86_jit_writer_t *w) {
  return emit_movabs_rdx(w, (uintptr_t)&jit_chain_abort) &&
         emit_mov_ecx_m32_rdx(w);
}

static bool emit_load_eflags_eax(x86_jit_writer_t *w) {
  return emit_mov_eax_moffs64(w, (uintptr_t)&cpu.eflags);
}

static bool emit_load_eflags_ecx(x86_jit_writer_t *w) {
  return emit_movabs_rdx(w, (uintptr_t)&cpu.eflags) &&
         emit_mov_ecx_m32_rdx(w);
}

static bool emit_store_eflags_eax(x86_jit_writer_t *w) {
  return emit_mov_moffs64_eax(w, (uintptr_t)&cpu.eflags);
}

static bool emit_alu_rm32_r32(x86_jit_writer_t *w, uint8_t alu_op,
    uint8_t rm, uint8_t reg) {
  uint8_t opcode = 0;
  switch (alu_op) {
    case X86_ALU_ADD: opcode = 0x01; break;
    case X86_ALU_OR:  opcode = 0x09; break;
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
  if (alu_op == X86_ALU_ADC || alu_op == X86_ALU_SBB) return false;
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
  if (alu_op == X86_ALU_ADC || alu_op == X86_ALU_SBB) return false;

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
  return emit_store_pc_imm(w, pc) &&
         emit_load_loop_extra_eax(w) &&
         emit_add_eax_imm32(w, count) &&
         emit_ret(w);
}

static bool emit_return_r11_plus_imm(x86_jit_writer_t *w, uint32_t count) {
  return emit_mov_eax_r11d(w) &&
         emit_add_eax_imm32(w, count) &&
         emit_ret(w);
}

static bool emit_return_r11(x86_jit_writer_t *w) {
  return emit_mov_eax_r11d(w) && emit_ret(w);
}

static bool emit_resident_lap_budget_r10d(x86_jit_writer_t *w, uint32_t count) {
  if (count == 2u) {
    return emit_load_entry_budget_arg_ecx(w) &&
           emit_shr_ecx_imm(w, 1u) &&
           emit_mov_r10d_ecx(w);
  }

  return emit_load_entry_budget_arg_eax(w) &&
         emit_xor_edx_edx(w) &&
         emit_mov_ecx_imm32(w, count) &&
         emit_div_ecx(w) &&
         emit_mov_r10d_eax(w);
}

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

static bool jit_jcc_native_supported(uint8_t cc) {
  uint32_t mask = 0;
  uint8_t host_cc = 0;
  return jit_jcc_fast_test(cc, &mask, &host_cc) ||
         jit_jcc_signed_test(cc);
}

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

  if (!emit_xor_r11d_r11d(w) ||
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

  if (!emit_xor_r11d_r11d(w) ||
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

static bool jit_native_alu_writes_result(uint8_t alu_op) {
  return alu_op != X86_ALU_CMP;
}

static bool jit_native_low_byte_reg(uint8_t reg) {
  return reg < 4u;
}

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

static bool jit_is_native_jcc(const x86_jit_insn_t *insn) {
  return insn->op == X86_JIT_OP_JCC_REL;
}

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
    case X86_JIT_HELPER_CALL_REL:
    case X86_JIT_HELPER_CALL_RM:
    case X86_JIT_HELPER_RET:
    case X86_JIT_HELPER_LEAVE:
      return true;
    default:
      return false;
  }
}

static bool jit_is_fusible_flag_producer(const x86_jit_insn_t *insn) {
  switch (insn->op) {
    case X86_JIT_OP_ALU_REG_REG:
    case X86_JIT_OP_ALU_IMM_REG:
    case X86_JIT_OP_TEST_REG_REG:
    case X86_JIT_OP_TEST_EAX_IMM:
      return insn->width == X86_WIDTH_DWORD;
    default:
      return false;
  }
}

static uint32_t jit_flag_producer_copy_mask(const x86_jit_insn_t *insn) {
  if (insn->op == X86_JIT_OP_TEST_REG_REG ||
      insn->op == X86_JIT_OP_TEST_EAX_IMM) {
    return X86_EFLAGS_LOGIC_COPY_MASK;
  }

  return jit_native_alu_flag_copy_mask(insn->alu_op);
}

static bool jit_flags_overwritten_by_next(const x86_jit_insn_t *insns,
    uint32_t index, uint32_t count) {
  return index + 1u < count &&
         jit_is_fusible_flag_producer(&insns[index]) &&
         jit_is_fusible_flag_producer(&insns[index + 1u]);
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

static bool emit_native_alu_reg_reg(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  if (!emit_load_reg_eax(w, insn->dst) ||
      !emit_load_reg_ecx(w, insn->src) ||
      !emit_alu_rm32_r32(w, insn->alu_op, R_EAX, R_ECX)) {
    return false;
  }

  if (jit_native_alu_writes_result(insn->alu_op) &&
      !emit_store_reg_eax(w, insn->dst)) {
    return false;
  }

  JIT_STAT_INC(native_alu_ops);
  return emit_capture_status_flags(w,
      jit_native_alu_flag_copy_mask(insn->alu_op));
}

static bool emit_native_alu_imm_reg(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  if (!emit_load_reg_eax(w, insn->dst) ||
      !emit_alu_eax_imm32(w, insn->alu_op, insn->imm)) {
    return false;
  }

  if (jit_native_alu_writes_result(insn->alu_op) &&
      !emit_store_reg_eax(w, insn->dst)) {
    return false;
  }

  JIT_STAT_INC(native_alu_ops);
  return emit_capture_status_flags(w,
      jit_native_alu_flag_copy_mask(insn->alu_op));
}

static bool emit_native_test_reg_reg(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  if (!emit_load_reg_eax(w, insn->dst) ||
      !emit_load_reg_ecx(w, insn->src) ||
      !emit_test_eax_ecx(w)) {
    return false;
  }

  JIT_STAT_INC(native_alu_ops);
  return emit_capture_status_flags(w, X86_EFLAGS_LOGIC_COPY_MASK);
}

static bool emit_native_test_eax_imm(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  if (!emit_load_reg_eax(w, R_EAX) ||
      !emit_test_eax_imm_width(w, insn->width, insn->imm)) {
    return false;
  }

  JIT_STAT_INC(native_alu_ops);
  return emit_capture_status_flags(w, X86_EFLAGS_LOGIC_COPY_MASK);
}

static bool emit_flag_producer_no_capture(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  switch (insn->op) {
    case X86_JIT_OP_ALU_REG_REG:
      if (!emit_load_reg_eax(w, insn->dst) ||
          !emit_load_reg_ecx(w, insn->src) ||
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
      !emit_capture_status_flags(w, copy_mask) ||
      !emit_store_pc_imm(w, jcc->next_pc) ||
      !emit_ret_count(w, count)) {
    return false;
  }

  uint8_t *taken_native = w->cur;
  if (!patch_rel32(taken_disp, taken_native) ||
      !emit_capture_status_flags(w, copy_mask) ||
      !emit_store_pc_imm(w, jit_branch_target(jcc)) ||
      !emit_ret_count(w, count)) {
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

  if (!emit_xor_r11d_r11d(w) ||
      !emit_resident_lap_budget_r10d(w, count)) {
    return false;
  }

  const uint8_t *loop_native = w->cur;
  if (!emit_flag_producer_no_capture(w, producer) ||
      !emit_jcc_rel32_placeholder(w, jcc->cc, &taken_disp) ||
      !emit_capture_status_flags(w, copy_mask) ||
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
      !emit_capture_status_flags(w, copy_mask) ||
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
      !emit_capture_status_flags(w, copy_mask) ||
      !emit_return_completed(w, jcc->next_pc, count)) {
    return false;
  }

  uint8_t *taken_native = w->cur;
  if (!patch_rel32(taken_disp, taken_native) ||
      !emit_capture_status_flags(w, copy_mask) ||
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
  if (width == X86_WIDTH_BYTE &&
      (!jit_native_low_byte_reg(insn->src) ||
          (insn->rm_is_reg && !jit_native_low_byte_reg(insn->rm_reg)))) {
    return false;
  }

  if (insn->rm_is_reg) {
    if (!emit_load_reg_r11d(w, insn->src) ||
        !emit_load_reg_eax(w, insn->rm_reg) ||
        !emit_alu_eax_r11_width(w, insn->alu_op, width)) {
      return false;
    }

    if (writes_result && !emit_store_reg_eax(w, insn->rm_reg)) {
      return false;
    }

    JIT_STAT_INC(native_alu_ops);
    return emit_capture_status_flags(w,
        jit_native_alu_flag_copy_mask(insn->alu_op));
  }

  if (!emit_load_reg_r11d(w, insn->src) ||
      !emit_guest_ea_eax(w, &insn->ea) ||
      !emit_mov_edx_eax(w) ||
      !emit_direct_pmem_guard_edx(w, width, &pmem_slow_disp)) {
    return false;
  }

  if (writes_result &&
      !emit_direct_store_source_guard_edx(w, width,
          &cross_page_slow_disp, &source_page_slow_disp)) {
    return false;
  }

  if (!emit_movabs_r10(w, (uint64_t)(uintptr_t)guest_to_host(CONFIG_MBASE)) ||
      !emit_load_pmem_eax_width(w, width) ||
      !emit_alu_eax_r11_width(w, insn->alu_op, width)) {
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
    if (width == X86_WIDTH_BYTE &&
        !jit_native_low_byte_reg(insn->rm_reg)) {
      return false;
    }

    if (!emit_load_reg_eax(w, insn->rm_reg) ||
        !emit_alu_eax_imm_width(w, insn->alu_op, width, insn->imm)) {
      return false;
    }

    if (writes_result && !emit_store_reg_eax(w, insn->rm_reg)) {
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

  if (!emit_movabs_r10(w, (uint64_t)(uintptr_t)guest_to_host(CONFIG_MBASE)) ||
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
  if (width == X86_WIDTH_BYTE &&
      (!jit_native_low_byte_reg(insn->src) ||
          (insn->rm_is_reg && !jit_native_low_byte_reg(insn->rm_reg)))) {
    return false;
  }

  if (insn->rm_is_reg) {
    if (!emit_load_reg_eax(w, insn->rm_reg) ||
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
      !emit_load_pmem_eax_width(w, width) ||
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
  if (width == X86_WIDTH_BYTE &&
      (!jit_native_low_byte_reg(insn->dst) ||
          (insn->rm_is_reg && !jit_native_low_byte_reg(insn->rm_reg)))) {
    return false;
  }

  if (insn->rm_is_reg) {
    if (!emit_load_reg_eax(w, insn->dst) ||
        !emit_load_reg_ecx(w, insn->rm_reg) ||
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
        !emit_load_reg_eax(w, insn->dst) ||
        !emit_alu_eax_ecx_width(w, insn->alu_op, width)) {
      return false;
    }
  }

  if (writes_result && !emit_store_reg_eax(w, insn->dst)) return false;

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
  if (width == X86_WIDTH_BYTE &&
      (!jit_native_low_byte_reg(insn->src) ||
          (insn->rm_is_reg && !jit_native_low_byte_reg(insn->rm_reg)))) {
    return false;
  }

  if (insn->rm_is_reg) {
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
          &cross_page_slow_disp, &source_page_slow_disp) ||
      !emit_load_reg_r11d(w, insn->src) ||
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

static bool emit_native_pop_reg(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *slow_disp = NULL;
  uint8_t *done_disp = NULL;

  if (insn->width != X86_WIDTH_DWORD) return false;

  if (!emit_load_reg_eax(w, R_ESP) ||
      !emit_mov_edx_eax(w) ||
      !emit_direct_pmem_guard_edx(w, X86_WIDTH_DWORD, &slow_disp) ||
      !emit_movabs_r10(w, (uint64_t)(uintptr_t)guest_to_host(CONFIG_MBASE)) ||
      !emit_mov_eax_m32_r10_rdx(w) ||
      !emit_store_reg_eax(w, insn->dst)) {
    return false;
  }

  if (insn->dst != R_ESP &&
      (!emit_load_reg_eax(w, R_ESP) ||
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
      !emit_movabs_r10(w, (uint64_t)(uintptr_t)guest_to_host(CONFIG_MBASE)) ||
      !emit_mov_m32_r10_rdx_r11d(w) ||
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

static bool emit_native_ret(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *slow_disp = NULL;
  uint8_t *done_disp = NULL;

  if (insn->width != X86_WIDTH_DWORD) return false;

  if (!emit_load_reg_eax(w, R_ESP) ||
      !emit_mov_edx_eax(w) ||
      !emit_direct_pmem_guard_edx(w, X86_WIDTH_DWORD, &slow_disp) ||
      !emit_movabs_r10(w, (uint64_t)(uintptr_t)guest_to_host(CONFIG_MBASE)) ||
      !emit_mov_eax_m32_r10_rdx(w) ||
      !emit_store_pc_eax(w) ||
      !emit_load_reg_eax(w, R_ESP) ||
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
  if (width == X86_WIDTH_BYTE && insn->rm_is_reg &&
      !jit_native_low_byte_reg(insn->rm_reg)) {
    return false;
  }

  if (insn->rm_is_reg) {
    if (!emit_load_reg_eax(w, insn->rm_reg) ||
        !emit_neg_eax_width(w, width) ||
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

static bool emit_native_shift_reg_imm(x86_jit_writer_t *w,
    const x86_jit_insn_t *insn) {
  uint8_t *pmem_slow_disp = NULL;
  uint8_t *cross_page_slow_disp = NULL;
  uint8_t *source_page_slow_disp = NULL;
  uint8_t *done_disp = NULL;
  uint8_t shift_op = insn->alu_op;
  uint32_t copy_mask = 0;

  if (insn->width != X86_WIDTH_DWORD || insn->count_from_cl) {
    return false;
  }

  const uint8_t count = insn->imm & X86_SHIFT_COUNT_MASK;
  if (!jit_native_shift_flag_copy_mask(insn->alu_op, count,
      &shift_op, &copy_mask)) {
    return false;
  }

  if (count == 0) {
    JIT_STAT_INC(native_shift_ops);
    return true;
  }

  if (insn->rm_is_reg) {
    if (!emit_load_reg_eax(w, insn->rm_reg) ||
        !emit_shift_eax_imm(w, shift_op, count) ||
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
     * Loading the whole 32-bit register and masking is only valid for the low
     * byte registers.
     */
    if (width == X86_WIDTH_BYTE && insn->rm_reg >= 4) return false;

    if (!emit_load_reg_eax(w, insn->rm_reg) ||
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
    /*
     * Byte register numbers 4..7 are AH/CH/DH/BH.  Loading the whole 32-bit
     * register and sign-extending AL is only correct for AL/CL/DL/BL.
     */
    if (width == X86_WIDTH_BYTE && insn->rm_reg >= 4) return false;

    if (!emit_load_reg_eax(w, insn->rm_reg)) return false;
    if (width == X86_WIDTH_BYTE) {
      if (!emit_movsx_eax_al(w)) return false;
    }
    else if (!emit_movsx_eax_ax(w)) {
      return false;
    }
    if (!emit_store_reg_eax(w, insn->dst)) return false;
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
  if (width == X86_WIDTH_BYTE && insn->rm_is_reg &&
      !jit_native_low_byte_reg(insn->rm_reg)) {
    return false;
  }

  if (insn->rm_is_reg) {
    if (!emit_load_reg_eax(w, insn->rm_reg) ||
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
    case X86_JIT_OP_JMP_REL:
      return emit_native_jmp_rel(w, insn);
    case X86_JIT_OP_JCC_REL:
      return emit_native_jcc_rel(w, insn);
    case X86_JIT_OP_HELPER:
      /*
       * Paged guests need architectural address translation and fault ordering.
       * Until the native PMEM paths grow inline page-table guards, let helpers
       * handle translated memory through vaddr_read()/vaddr_write().
       */
      if (jit_paging_enabled() && jit_helper_may_touch_guest_memory(insn)) {
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

static bool jit_is_native_incdec_reg(const x86_jit_insn_t *insn) {
  return insn->op == X86_JIT_OP_HELPER &&
         insn->helper == X86_JIT_HELPER_INCDEC_REG &&
         insn->width == X86_WIDTH_DWORD &&
         (insn->alu_op == X86_ALU_ADD || insn->alu_op == X86_ALU_SUB);
}

static bool jit_decode_modrm(x86_jit_reader_t *r, uint8_t *mod, uint8_t *reg,
    uint8_t *rm) {
  uint8_t modrm = 0;
  if (!jit_read_u8(r, &modrm)) return false;

  *mod = modrm >> 6;
  *reg = (modrm >> 3) & 0x7u;
  *rm = modrm & 0x7u;
  return true;
}

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

static bool jit_decode_rm_operand(x86_jit_reader_t *r, uint8_t mod,
    uint8_t rm, x86_jit_insn_t *out) {
  out->rm_is_reg = mod == 3;
  out->rm_reg = rm;
  if (mod == 3) return true;
  return jit_decode_ea32(r, mod, rm, &out->ea);
}

static bool jit_finish_decode(x86_jit_reader_t *r, x86_jit_insn_t *out) {
  out->next_pc = r->cur;
  return true;
}

static void jit_mark_helper(x86_jit_insn_t *out, x86_jit_helper_t helper) {
  out->op = X86_JIT_OP_HELPER;
  out->helper = helper;
}

static int jit_alu_from_opcode(uint8_t opcode) {
  switch (opcode & 0x38u) {
    case 0x00: return X86_ALU_ADD;
    case 0x08: return X86_ALU_OR;
    case 0x20: return X86_ALU_AND;
    case 0x28: return X86_ALU_SUB;
    case 0x30: return X86_ALU_XOR;
    case 0x38: return X86_ALU_CMP;
    default: return -1;
  }
}

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

    if (reg == X86_ALU_ADC || reg == X86_ALU_SBB) return false;

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

    if ((opcode2 == 0xbe || opcode2 == 0xbf) &&
        out->width == X86_WIDTH_DWORD) {
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

static void jit_emit_ctx_init(x86_jit_emit_ctx_t *ctx) {
  memset(ctx, 0, sizeof(*ctx));
  ctx->valid = true;
  ctx->has_cpu_base = jit_regcache_enabled;
  for (uint32_t i = 0; i < 8u; i++) ctx->guest_to_host[i] = -1;
  for (uint32_t i = 0; i < 16u; i++) ctx->host_to_guest[i] = -1;
  ctx->flags.kind = X86_LAZY_FLAGS_MATERIALISED;
}

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

static size_t jit_align_code(size_t value) {
  return (value + X86_JIT_CODE_ALIGN - 1u) & ~(size_t)(X86_JIT_CODE_ALIGN - 1u);
}

static bool jit_block_source_matches(const x86_jit_block_t *block, vaddr_t pc) {
  if (!block->valid || block->pc != pc || block->source_len == 0) return false;
  if (block->cr3_key != jit_cr3_key()) return false;
  if (!jit_verify_source_enabled) return true;
  const x86_jit_block_cold_t *cold = jit_block_cold_const(block);
  uint8_t current[X86_JIT_MAX_SOURCE_BYTES];
  if (!jit_copy_source(pc, block->source_len, current)) return false;
  return memcmp(cold->source, current, block->source_len) == 0;
}

static uint32_t jit_next_cache_age(void) {
  jit_cache_age_clock++;
  if (jit_cache_age_clock == 0) jit_cache_age_clock = 1;
  return jit_cache_age_clock;
}

static uint32_t jit_cache_set_key(vaddr_t pc, uint32_t cr3_key) {
  uint32_t hash = pc ^ cr3_key;
  hash ^= hash >> 4;
  hash ^= hash >> 12;
  hash ^= hash >> 20;
  return hash & (X86_JIT_CACHE_SETS - 1u);
}

static uint32_t jit_cache_set(vaddr_t pc) {
  return jit_cache_set_key(pc, jit_cr3_key());
}

static uint32_t jit_l0_index_key(vaddr_t pc, uint32_t cr3_key) {
  uint32_t hash = pc ^ cr3_key;
  hash ^= hash >> 6;
  hash ^= hash >> 15;
  return hash & (X86_JIT_L0_SIZE - 1u);
}

static x86_jit_block_t *jit_cache_way(vaddr_t pc, uint32_t way) {
  return &jit_cache[jit_cache_set(pc) * X86_JIT_CACHE_WAYS + way];
}

static x86_jit_block_t *jit_cache_way_key(vaddr_t pc, uint32_t way,
    uint32_t cr3_key) {
  return &jit_cache[jit_cache_set_key(pc, cr3_key) * X86_JIT_CACHE_WAYS + way];
}

static x86_jit_block_t *jit_l0_lookup(vaddr_t pc, uint32_t cr3_key) {
  if (!jit_l0_cache_enabled || !jit_hot_cold_cache_enabled) return NULL;

  x86_jit_l0_entry_t *l0 = &jit_l0_cache[jit_l0_index_key(pc, cr3_key)];
  if (!l0->valid || l0->pc != pc || l0->cr3_key != cr3_key ||
      l0->generation != jit_cache_generation ||
      l0->hot_index >= X86_JIT_CACHE_SIZE) {
    return NULL;
  }

  x86_jit_block_t *block = &jit_cache[l0->hot_index];
  if (!block->valid || block->pc != pc || block->source_len == 0 ||
      block->cr3_key != l0->cr3_key) {
    return NULL;
  }
  if (jit_verify_source_enabled && !jit_block_source_matches(block, pc)) {
    return NULL;
  }
  return block;
}

static void jit_l0_fill_key(vaddr_t pc, const x86_jit_block_t *block,
    uint32_t cr3_key) {
  if (!jit_l0_cache_enabled || !jit_hot_cold_cache_enabled || block == NULL) {
    return;
  }

  x86_jit_l0_entry_t *l0 = &jit_l0_cache[jit_l0_index_key(pc, cr3_key)];
  *l0 = (x86_jit_l0_entry_t){
    .valid = true,
    .pc = pc,
    .cr3_key = cr3_key,
    .hot_index = jit_block_index(block),
    .generation = jit_cache_generation,
  };
}

static void jit_l0_fill(vaddr_t pc, const x86_jit_block_t *block) {
  jit_l0_fill_key(pc, block, jit_cr3_key());
}

static x86_jit_block_t *jit_cache_lookup(vaddr_t pc) {
  const uint32_t cr3_key = jit_cr3_key();
  x86_jit_block_t *block = jit_l0_lookup(pc, cr3_key);
  if (block != NULL) return block;

  const uint32_t ways = jit_4way_cache_enabled ? X86_JIT_CACHE_WAYS : 1u;
  for (uint32_t way = 0; way < ways; way++) {
    x86_jit_block_t *block = jit_cache_way_key(pc, way, cr3_key);
    if (jit_block_source_matches(block, pc)) {
      jit_l0_fill_key(pc, block, cr3_key);
      return block;
    }
  }
  return NULL;
}

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
  block->source_len = 1;
  block->cold_index = jit_block_index(block);
  if (jit_copy_source(pc, block->source_len, cold->source)) {
    opcode = cold->source[0];
  }
  if (opcode == 0x0f && jit_vaddr_read_u8(pc + 1u, &opcode2)) {
    block->unsupported_opcode2 = opcode2;
  }
  block->cache_age = jit_next_cache_age();
  jit_mark_source_pages(block, pc, block->source_len);
  jit_l0_fill(pc, block);
  JIT_STAT_INC(blocks_unsupported);
  if (jit_stats_enabled) {
    jit_stats.unsupported_by_opcode[opcode]++;
    if (opcode == 0x0f) jit_stats.unsupported_0f_by_opcode[opcode2]++;
  }
}

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

static void jit_reset_arena(void) {
  memset(jit_cache, 0, sizeof(jit_cache));
  memset(jit_cache_cold, 0, sizeof(jit_cache_cold));
  memset(jit_l0_cache, 0, sizeof(jit_l0_cache));
  memset(jit_source_page_has_code, 0, sizeof(jit_source_page_has_code));
  memset(jit_source_page_blocks, 0, sizeof(jit_source_page_blocks));
  jit_cache_age_clock = 1;
  jit_cache_bump_generation();
  jit_code_used = 0;
  JIT_STAT_INC(arena_resets);
}

static x86_jit_block_t *jit_compile_block(vaddr_t pc, uint32_t max_insns) {
  if (!jit_flat_segments()) {
    return NULL;
  }
  if (jit_paging_enabled() && !jit_paged_fastpath_enabled) {
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

  x86_jit_block_t *block = jit_cache_select_victim(pc);
  x86_jit_block_cold_t *cold = jit_block_cold(block);
  memcpy(cold->insns, decoded, decoded_count * sizeof(decoded[0]));

  for (uint32_t i = 0; i < decoded_count; i++) {
    if (i == 0 && decoded_count >= 2u &&
        jit_is_native_incdec_reg(&cold->insns[i]) &&
        jit_is_incdec_resident_jcc_backedge(&cold->insns[i + 1u], pc)) {
      if (!emit_incdec_jcc_resident_backedge(&w, &cold->insns[i],
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
      if (!emit_incdec_cmp_jcc_resident_backedge(&w, &cold->insns[i],
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
      if (jit_branch_target(&cold->insns[i + 1u]) == pc) {
        if (i == 0) {
          if (!emit_fused_flag_producer_jcc_resident_backedge(&w,
              &cold->insns[i], &cold->insns[i + 1u], 2u)) {
            jit_code_used = start_used;
            return NULL;
          }
        }
        else {
          if (!emit_fused_flag_producer_jcc_backedge(&w,
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
      if (!emit_fused_flag_producer_jcc(&w, &cold->insns[i],
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
      if (!emit_flag_producer_no_capture(&w, &cold->insns[i])) {
        jit_code_used = start_used;
        return NULL;
      }
      emitted_count = insn_count;
      ctx.flags.kind = X86_LAZY_FLAGS_HOST_VALID;
      continue;
    }

    if (jit_is_chainable_jcc_backedge(&cold->insns[i], pc)) {
      if (!emit_jcc_backedge(&w, &cold->insns[i], w.start, insn_count)) {
        jit_code_used = start_used;
        return NULL;
      }
      emitted_count = insn_count;
      ends_with_chained_control = true;
      ctx.uses_loop_accounting = true;
      ctx.uses_global_loop_accounting = true;
    }
    else if (!emit_insn(&w, &cold->insns[i])) {
      jit_code_used = start_used;
      return NULL;
    }

    emitted_count = insn_count;
    if (cold->insns[i].ends_block) {
      ends_with_control = true;
      break;
    }
  }

  if (!ends_with_chained_control &&
      ((!ends_with_control && !emit_store_pc_imm(&w, end_pc)) ||
       !emit_ret_count(&w, emitted_count))) {
    jit_code_used = start_used;
    return NULL;
  }

  const uint32_t source_len = (uint32_t)(end_pc - pc);
  block->valid = true;
  block->pc = pc;
  block->cr3_key = jit_cr3_key();
  block->paging = jit_paging_enabled();
  block->source_len = source_len;
  block->entry = (x86_jit_entry_t)w.start;
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
  jit_mark_source_pages(block, pc, source_len);
  jit_l0_fill(pc, block);

  __builtin___clear_cache((char *)w.start, (char *)w.cur);
  jit_code_used = jit_align_code((size_t)(w.cur - jit_code));
  JIT_STAT_INC(blocks_compiled);
  JIT_STAT_ADD(compiled_insns, emitted_count);
  return block;
}

bool isa_jit_available(void) {
  if (jit_runtime_disabled()) return false;
  return jit_ensure_code_cache();
}

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

    if (block->guest_insns > remaining_budget) {
      return *executed > 0;
    }

    if (block->uses_global_loop_accounting) {
      jit_entry_budget = remaining_budget;
    }
    if (block->uses_global_loop_accounting || block->paging) {
      jit_loop_extra = 0;
      jit_chain_abort = 0;
    }

    jit_fault_guest_count = 0;
    uint32_t ran = 0;
    if (block->paging) {
      x86_exception_env_valid = true;
      if (setjmp(x86_exception_env) == 0) {
        ran = block->entry(remaining_budget);
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
      ran = block->entry(remaining_budget);
    }

    if (ran == 0) return *executed > 0;
    Assert(ran > 0 && ran <= budget - *executed,
        "x86 JIT block returned invalid count %u", ran);
    *executed += ran;
    JIT_STAT_INC(blocks_executed);
    JIT_STAT_ADD(executed_insns, ran);
  }

  return *executed > 0;
}

void isa_jit_flush_all(void) {
  memset(jit_cache, 0, sizeof(jit_cache));
  memset(jit_cache_cold, 0, sizeof(jit_cache_cold));
  memset(jit_l0_cache, 0, sizeof(jit_l0_cache));
  memset(jit_source_page_has_code, 0, sizeof(jit_source_page_has_code));
  memset(jit_source_page_blocks, 0, sizeof(jit_source_page_blocks));
  jit_cache_age_clock = 1;
  jit_cache_bump_generation();
  jit_code_used = 0;
}

void isa_jit_flush_data_tlb(void) {
  isa_jit_flush_all();
}

bool isa_jit_may_invalidate_paddr(paddr_t addr, int len) {
  return jit_range_may_touch_source_pages(addr, len);
}

void isa_jit_invalidate_paddr(paddr_t addr, int len) {
  if (len <= 0) return;

  JIT_STAT_INC(invalidation_requests);
  if (!jit_range_may_touch_source_pages(addr, len)) {
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

  if (needs_full_scan) {
    for (uint32_t i = 0; i < X86_JIT_CACHE_SIZE; i++) {
      x86_jit_block_t *block = &jit_cache[i];
      if (!block->valid || block->source_len == 0) continue;
      if (!jit_block_touches_source_page_range(block, first, last)) continue;
      if (!jit_block_source_overlaps_paddr_range(block, addr, end)) continue;
      jit_block_invalidate(block);
      jit_chain_abort = 1;
      JIT_STAT_INC(invalidated_blocks);
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
      JIT_STAT_INC(invalidated_blocks);
    }
  }
}

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
  Log("jit: helper calls = %" PRIu64
      ", helper inc/dec calls = %" PRIu64
      ", helper inc/dec register calls = %" PRIu64
      ", helper inc/dec r/m calls = %" PRIu64,
      jit_stats.helper_calls,
      jit_stats.helper_incdec_calls,
      jit_stats.helper_incdec_reg_calls,
      jit_stats.helper_incdec_rm_calls);
  for (uint32_t helper = 1; helper < X86_JIT_HELPER_COUNT; helper++) {
    if (jit_stats.helper_by_kind[helper] != 0) {
      const char *name = jit_helper_names[helper] != NULL ?
          jit_helper_names[helper] : "unknown";
      Log("jit: helper profile %-16s calls = %" PRIu64,
          name, jit_stats.helper_by_kind[helper]);
    }
  }
  Log("jit: invalidation requests = %" PRIu64
      ", page skips = %" PRIu64
      ", precise invalidation scans = %" PRIu64
      ", invalidated blocks = %" PRIu64
      ", arena resets = %" PRIu64,
      jit_stats.invalidation_requests,
      jit_stats.invalidation_page_skips,
      jit_stats.precise_invalidation_scans,
      jit_stats.invalidated_blocks,
      jit_stats.arena_resets);
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

bool isa_jit_available(void) {
  return false;
}

bool isa_jit_exec(uint64_t remaining, uint32_t device_budget, uint32_t *executed) {
  (void)remaining;
  (void)device_budget;
  *executed = 0;
  return false;
}

void isa_jit_flush_all(void) {
}

void isa_jit_flush_data_tlb(void) {
}

bool isa_jit_may_invalidate_paddr(paddr_t addr, int len) {
  (void)addr;
  (void)len;
  return false;
}

void isa_jit_invalidate_paddr(paddr_t addr, int len) {
  (void)addr;
  (void)len;
}

void isa_jit_dump_stats(void) {
}

#endif
