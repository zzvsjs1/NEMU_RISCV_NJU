#include <generated/autoconf.h>

#include <isa-jit.h>
#include <isa.h>
#include <memory/host.h>
#include <memory/paddr.h>
#include <utils.h>

#include <inttypes.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/*
 * First x86 JIT milestone.
 *
 * The interpreter in inst.c remains the architectural reference.  This file
 * only translates a tiny fault-free IA-32 subset into x86-64 code:
 *
 *   - MOV r32, imm32
 *   - MOV r32, r32
 *   - LEA r32, m
 *   - NOP
 *
 * Everything else returns to the interpreter before the unsupported
 * instruction commits.  This keeps the initial native path useful for proving
 * the code-cache, CPU-loop, statistics, and invalidation contracts without
 * taking correctness risks around flags, memory faults, segmentation, or
 * paging.  Later milestones can widen the subset behind the same fallback
 * boundary.
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

#define X86_JIT_BLOCK_MAX_INSNS 16u
#define X86_JIT_CACHE_SIZE 4096u
#define X86_JIT_CODE_SIZE (16u * 1024u * 1024u)
#define X86_JIT_BLOCK_CODE_HEADROOM 4096u
#define X86_JIT_CODE_ALIGN 16u
#define X86_JIT_MAX_SOURCE_BYTES 128u

typedef uint32_t (*x86_jit_entry_t)(void);

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
  X86_JIT_OP_NOP,
  X86_JIT_OP_MOV_IMM_REG,
  X86_JIT_OP_MOV_REG_REG,
  X86_JIT_OP_LEA,
} x86_jit_op_t;

typedef struct {
  x86_jit_op_t op;
  uint8_t dst;
  uint8_t src;
  uint32_t imm;
  x86_jit_ea_t ea;
} x86_jit_insn_t;

typedef struct {
  bool valid;
  bool unsupported;
  vaddr_t pc;
  uint32_t source_len;
  uint8_t source[X86_JIT_MAX_SOURCE_BYTES];
  x86_jit_entry_t entry;
  uint32_t guest_insns;
} x86_jit_block_t;

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
  uint64_t invalidation_requests;
  uint64_t invalidated_blocks;
  uint64_t arena_resets;
} x86_jit_stats_t;

bool isa_jit_invalidation_active = false;

#if X86_JIT_ENABLED

static x86_jit_block_t jit_cache[X86_JIT_CACHE_SIZE];
static uint8_t *jit_code = NULL;
static size_t jit_code_used = 0;
static bool jit_runtime_options_init = false;
static bool jit_env_disable = false;
static bool jit_stats_enabled = false;
static x86_jit_stats_t jit_stats;

static bool jit_env_flag_enabled(const char *name) {
  const char *value = getenv(name);
  return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
}

static void jit_init_runtime_options(void) {
  if (!jit_runtime_options_init) {
    jit_env_disable = jit_env_flag_enabled("NEMU_DISABLE_JIT");
    jit_stats_enabled = jit_env_flag_enabled("NEMU_JIT_STATS");
    jit_runtime_options_init = true;
  }
}

static bool jit_runtime_disabled(void) {
  jit_init_runtime_options();
  return jit_env_disable;
}

static uint64_t jit_ratio_x100(uint64_t numerator, uint64_t denominator) {
  if (denominator == 0) return 0;
  return (numerator * 100u + denominator / 2u) / denominator;
}

static uint64_t jit_percent_x100(uint64_t numerator, uint64_t denominator) {
  if (denominator == 0) return 0;
  return (numerator * 10000u + denominator / 2u) / denominator;
}

static bool jit_flat_mode(void) {
  if ((cpu.cr0 & X86_CR0_PG) != 0) return false;

  for (uint32_t i = 0; i < 4; i++) {
    if (cpu.seg_cache[i].base != 0 || cpu.seg_cache[i].limit != X86_DWORD_LIMIT) {
      return false;
    }
  }

  return true;
}

static bool jit_linear_in_pmem(vaddr_t addr, uint32_t len) {
  return in_pmem_range((paddr_t)addr, (int)len);
}

static bool jit_read_u8(x86_jit_reader_t *r, uint8_t *value) {
  if (!jit_linear_in_pmem(r->cur, 1)) return false;
  *value = host_read(guest_to_host((paddr_t)r->cur), 1);
  r->cur++;
  return true;
}

static bool jit_read_u32(x86_jit_reader_t *r, uint32_t *value) {
  if (!jit_linear_in_pmem(r->cur, 4)) return false;
  *value = host_read(guest_to_host((paddr_t)r->cur), 4);
  r->cur += 4;
  return true;
}

static bool jit_read_i8(x86_jit_reader_t *r, int32_t *value) {
  uint8_t raw = 0;
  if (!jit_read_u8(r, &raw)) return false;
  *value = (int8_t)raw;
  return true;
}

static bool jit_copy_source(vaddr_t pc, uint32_t len, uint8_t *dst) {
  if (len > X86_JIT_MAX_SOURCE_BYTES || !jit_linear_in_pmem(pc, len)) return false;
  memcpy(dst, guest_to_host((paddr_t)pc), len);
  return true;
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

static bool emit_mov_eax_imm32(x86_jit_writer_t *w, uint32_t value) {
  return emit_u8(w, 0xb8) && emit_u32(w, value);
}

static bool emit_mov_m32_rdx_imm32(x86_jit_writer_t *w, uint32_t value) {
  return emit_u8(w, 0xc7) && emit_u8(w, 0x02) && emit_u32(w, value);
}

static bool emit_mov_eax_m32_rdx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x8b) && emit_u8(w, 0x02);
}

static bool emit_mov_ecx_m32_rdx(x86_jit_writer_t *w) {
  return emit_u8(w, 0x8b) && emit_u8(w, 0x0a);
}

static bool emit_mov_m32_rdx_eax(x86_jit_writer_t *w) {
  return emit_u8(w, 0x89) && emit_u8(w, 0x02);
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
  return emit_movabs_rdx(w, jit_gpr_addr(reg)) &&
         emit_mov_eax_m32_rdx(w);
}

static bool emit_load_reg_ecx(x86_jit_writer_t *w, uint8_t reg) {
  return emit_movabs_rdx(w, jit_gpr_addr(reg)) &&
         emit_mov_ecx_m32_rdx(w);
}

static bool emit_store_reg_eax(x86_jit_writer_t *w, uint8_t reg) {
  return emit_movabs_rdx(w, jit_gpr_addr(reg)) &&
         emit_mov_m32_rdx_eax(w);
}

static bool emit_store_pc_imm(x86_jit_writer_t *w, vaddr_t pc) {
  return emit_movabs_rdx(w, (uintptr_t)&cpu.pc) &&
         emit_mov_m32_rdx_imm32(w, pc);
}

static bool emit_add_reg_to_eax(x86_jit_writer_t *w, uint8_t reg) {
  return emit_movabs_rdx(w, jit_gpr_addr(reg)) &&
         emit_add_eax_m32_rdx(w);
}

static bool emit_lea(x86_jit_writer_t *w, const x86_jit_insn_t *insn) {
  if (!emit_mov_eax_imm32(w, insn->ea.disp)) return false;

  if (insn->ea.base_reg >= 0 &&
      !emit_add_reg_to_eax(w, (uint8_t)insn->ea.base_reg)) {
    return false;
  }

  if (insn->ea.index_reg >= 0) {
    if (!emit_load_reg_ecx(w, (uint8_t)insn->ea.index_reg) ||
        !emit_shl_ecx_imm(w, insn->ea.scale) ||
        !emit_add_eax_ecx(w)) {
      return false;
    }
  }

  return emit_store_reg_eax(w, insn->dst);
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
    default:
      return false;
  }
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

static bool jit_decode_insn(x86_jit_reader_t *r, x86_jit_insn_t *out) {
  uint8_t opcode = 0;
  if (!jit_read_u8(r, &opcode)) return false;

  memset(out, 0, sizeof(*out));

  if (opcode == 0x90) {
    out->op = X86_JIT_OP_NOP;
    return true;
  }

  if (opcode >= 0xb8 && opcode <= 0xbf) {
    out->op = X86_JIT_OP_MOV_IMM_REG;
    out->dst = opcode & 0x7u;
    return jit_read_u32(r, &out->imm);
  }

  if (opcode == 0x89 || opcode == 0x8b) {
    uint8_t mod = 0, reg = 0, rm = 0;
    if (!jit_decode_modrm(r, &mod, &reg, &rm) || mod != 3) return false;

    out->op = X86_JIT_OP_MOV_REG_REG;
    if (opcode == 0x89) {
      out->dst = rm;
      out->src = reg;
    }
    else {
      out->dst = reg;
      out->src = rm;
    }
    return true;
  }

  if (opcode == 0x8d) {
    uint8_t mod = 0, reg = 0, rm = 0;
    if (!jit_decode_modrm(r, &mod, &reg, &rm) ||
        !jit_decode_ea32(r, mod, rm, &out->ea)) {
      return false;
    }

    out->op = X86_JIT_OP_LEA;
    out->dst = reg;
    return true;
  }

  return false;
}

static size_t jit_align_code(size_t value) {
  return (value + X86_JIT_CODE_ALIGN - 1u) & ~(size_t)(X86_JIT_CODE_ALIGN - 1u);
}

static bool jit_block_source_matches(const x86_jit_block_t *block, vaddr_t pc) {
  if (!block->valid || block->pc != pc || block->source_len == 0) return false;
  if (!jit_flat_mode()) return false;
  if (!jit_linear_in_pmem(pc, block->source_len)) return false;
  return memcmp(block->source, guest_to_host((paddr_t)pc), block->source_len) == 0;
}

static x86_jit_block_t *jit_cache_slot(vaddr_t pc) {
  return &jit_cache[(pc >> 1) & (X86_JIT_CACHE_SIZE - 1u)];
}

static void jit_publish_unsupported(vaddr_t pc) {
  x86_jit_block_t *block = jit_cache_slot(pc);
  memset(block, 0, sizeof(*block));
  block->valid = true;
  block->unsupported = true;
  block->pc = pc;
  block->source_len = 1;
  (void)jit_copy_source(pc, block->source_len, block->source);
  jit_stats.blocks_unsupported++;
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
  jit_code_used = 0;
  jit_stats.arena_resets++;
}

static x86_jit_block_t *jit_compile_block(vaddr_t pc, uint32_t max_insns) {
  if (!jit_flat_mode()) {
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
  x86_jit_reader_t r = { .pc = pc, .cur = pc };
  uint32_t count = 0;

  while (count < X86_JIT_BLOCK_MAX_INSNS && count < max_insns) {
    if ((uint32_t)(r.cur - pc) >= X86_JIT_MAX_SOURCE_BYTES) break;

    x86_jit_reader_t probe = r;
    x86_jit_insn_t insn;
    if (!jit_decode_insn(&probe, &insn)) break;
    if ((uint32_t)(probe.cur - pc) > X86_JIT_MAX_SOURCE_BYTES) break;
    if (!emit_insn(&w, &insn)) {
      jit_code_used = start_used;
      return NULL;
    }

    r = probe;
    count++;
  }

  if (count == 0) {
    jit_publish_unsupported(pc);
    return NULL;
  }

  if (!emit_store_pc_imm(&w, r.cur) || !emit_ret_count(&w, count)) {
    jit_code_used = start_used;
    return NULL;
  }

  const uint32_t source_len = (uint32_t)(r.cur - pc);
  x86_jit_block_t *block = jit_cache_slot(pc);
  memset(block, 0, sizeof(*block));
  block->valid = true;
  block->pc = pc;
  block->source_len = source_len;
  block->entry = (x86_jit_entry_t)w.start;
  block->guest_insns = count;
  if (!jit_copy_source(pc, source_len, block->source)) {
    block->valid = false;
    jit_code_used = start_used;
    return NULL;
  }

  __builtin___clear_cache((char *)w.start, (char *)w.cur);
  jit_code_used = jit_align_code((size_t)(w.cur - jit_code));
  jit_stats.blocks_compiled++;
  jit_stats.compiled_insns += count;
  return block;
}

bool isa_jit_available(void) {
  if (jit_runtime_disabled()) return false;
  return jit_ensure_code_cache();
}

bool isa_jit_exec(uint64_t remaining, uint32_t device_budget, uint32_t *executed) {
  *executed = 0;
  if (remaining == 0 || device_budget == 0 || !isa_jit_available()) return false;

  jit_stats.exec_requests++;

  uint64_t budget = remaining < device_budget ? remaining : device_budget;
  if (budget > X86_JIT_BLOCK_MAX_INSNS) budget = X86_JIT_BLOCK_MAX_INSNS;

  while (*executed < budget) {
    x86_jit_block_t *block = jit_cache_slot(cpu.pc);
    if (jit_block_source_matches(block, cpu.pc)) {
      jit_stats.cache_hits++;
    }
    else {
      jit_stats.cache_misses++;
      block = jit_compile_block(cpu.pc, (uint32_t)(budget - *executed));
    }

    if (block == NULL || block->unsupported) {
      jit_stats.unsupported_hits++;
      return *executed > 0;
    }

    const uint32_t ran = block->entry();
    Assert(ran > 0 && ran <= budget - *executed,
        "x86 JIT block returned invalid count %u", ran);
    *executed += ran;
    jit_stats.blocks_executed++;
    jit_stats.executed_insns += ran;
  }

  return *executed > 0;
}

void isa_jit_flush_all(void) {
  memset(jit_cache, 0, sizeof(jit_cache));
  jit_code_used = 0;
}

void isa_jit_flush_data_tlb(void) {
}

void isa_jit_invalidate_paddr(paddr_t addr, int len) {
  if (len <= 0) return;

  jit_stats.invalidation_requests++;
  const paddr_t end = addr + (paddr_t)len - 1u;
  for (uint32_t i = 0; i < X86_JIT_CACHE_SIZE; i++) {
    x86_jit_block_t *block = &jit_cache[i];
    if (!block->valid || block->source_len == 0) continue;

    const paddr_t block_start = (paddr_t)block->pc;
    const paddr_t block_end = block_start + block->source_len - 1u;
    if (addr <= block_end && end >= block_start) {
      block->valid = false;
      jit_stats.invalidated_blocks++;
    }
  }
}

void isa_jit_dump_stats(void) {
  jit_init_runtime_options();

  if (jit_runtime_disabled()) {
    Log("jit: disabled by NEMU_DISABLE_JIT=1");
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
  Log("jit: invalidation requests = %" PRIu64
      ", invalidated blocks = %" PRIu64
      ", arena resets = %" PRIu64,
      jit_stats.invalidation_requests,
      jit_stats.invalidated_blocks,
      jit_stats.arena_resets);
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

void isa_jit_invalidate_paddr(paddr_t addr, int len) {
  (void)addr;
  (void)len;
}

void isa_jit_dump_stats(void) {
}

#endif
