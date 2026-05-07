#include <isa-jit.h>
#include <isa.h>
#include <memory/host.h>
#include <memory/vaddr.h>
#include <utils.h>
#include "local-include/reg.h"

#include <stddef.h>
#include <stdlib.h>

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
 * One native block may still be very short because stores, branches and
 * unsupported instructions end translation early. A bounded batch lets one
 * isa_jit_exec() call consume several cached blocks before returning to the
 * outer CPU loop. The limit is intentionally the same scale as the device
 * update interval, so timer/device work and interrupt checks are not delayed
 * by an unbounded translated-code run.
 */
#define RV32_JIT_BATCH_MAX_INSNS 256u
#define RV32_JIT_CACHE_SIZE 4096u
#define RV32_JIT_CODE_SIZE (4u * 1024u * 1024u)
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
#ifdef CONFIG_RV32_JIT_STATS
#define RV32_JIT_STATS 1
#else
#define RV32_JIT_STATS 0
#endif

typedef uint32_t (*rv32_jit_entry_t)(void);

typedef struct
{
  uint8_t *start;
  uint8_t *cur;
  uint8_t *end;
} rv32_jit_writer_t;

typedef struct
{
  bool valid;
  vaddr_t pc;
  word_t satp;
  paddr_t paddr_start;
  uint32_t source_len;
  uint32_t insn_count;
  rv32_jit_entry_t entry;
} rv32_jit_block_t;

static rv32_jit_block_t jit_cache[RV32_JIT_CACHE_SIZE];
static uint16_t jit_source_chunk_refs[RV32_JIT_PMEM_CHUNK_COUNT];
static uint8_t *jit_code = NULL;
static size_t jit_code_used = 0;
static bool jit_disabled = false;
static bool jit_env_disable = false;
static bool jit_runtime_options_ready = false;
static bool jit_stats_enabled = false;

#if RV32_JIT_STATS
typedef struct
{
  uint64_t exec_requests;
  uint64_t cache_hits;
  uint64_t cache_misses;
  uint64_t unsupported_hits;
  uint64_t blocks_executed;
  uint64_t executed_insns;

  uint64_t compile_requests;
  uint64_t blocks_compiled;
  uint64_t compiled_insns;
  uint64_t blocks_unsupported;
  uint64_t arena_resets;

  uint64_t invalidation_requests;
  uint64_t invalidation_page_skips;
  uint64_t invalidated_blocks;

  uint64_t helper_loads;
  uint64_t helper_load_direct;
  uint64_t helper_load_slow;
  uint64_t helper_stores;
  uint64_t helper_store_direct;
  uint64_t helper_store_slow;
  uint64_t helper_complex_ops;
} rv32_jit_stats_t;

static rv32_jit_stats_t jit_stats;

#define JIT_STAT_INC(field) \
  do { \
    jit_stats.field++; \
  } while (0)

#define JIT_STAT_ADD(field, value) \
  do { \
    jit_stats.field += (value); \
  } while (0)
#else
#define JIT_STAT_INC(field) do { } while (0)
#define JIT_STAT_ADD(field, value) do { } while (0)
#endif

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

static int32_t imm_i(uint32_t instr)
{
  return sext(bits(instr, 31, 20), 12);
}

static int32_t imm_s(uint32_t instr)
{
  return sext(bits(instr, 11, 7) | (bits(instr, 31, 25) << 5), 12);
}

static int32_t imm_b(uint32_t instr)
{
  const uint32_t imm = (bits(instr, 11, 8) << 1)
      | (bits(instr, 30, 25) << 5)
      | (bits(instr, 7, 7) << 11)
      | (bits(instr, 31, 31) << 12);
  return sext(imm, 13);
}

static uint32_t imm_u(uint32_t instr)
{
  return instr & 0xfffff000u;
}

static int32_t imm_j(uint32_t instr)
{
  const uint32_t imm = (bits(instr, 19, 12) << 12)
      | (bits(instr, 20, 20) << 11)
      | (bits(instr, 30, 21) << 1)
      | (bits(instr, 31, 31) << 20);
  return sext(imm, 21);
}

static bool jit_direct_pmem_range(vaddr_t addr, uint32_t len, paddr_t *paddr)
{
  if ((cpu.csr.satp & 0x80000000u) != 0 || len == 0)
  {
    return false;
  }

  const paddr_t start = (paddr_t)addr;
  const paddr_t end = start + (paddr_t)len - 1u;
  if (end < start || !in_pmem(start) || !in_pmem(end))
  {
    return false;
  }

  *paddr = start;
  return true;
}

static bool jit_env_flag_enabled(const char *name)
{
  const char *value = getenv(name);
  return value != NULL && value[0] != '\0' &&
      !(value[0] == '0' && value[1] == '\0');
}

static void jit_init_runtime_options(void)
{
  if (!jit_runtime_options_ready)
  {
    jit_env_disable = jit_env_flag_enabled("NEMU_DISABLE_JIT");
    jit_stats_enabled = jit_env_flag_enabled("NEMU_JIT_STATS");
    jit_runtime_options_ready = true;
  }
}

static bool jit_runtime_disabled(void)
{
  jit_init_runtime_options();
  return jit_env_disable;
}

static uint32_t jit_load_raw(vaddr_t addr, uint32_t len)
{
  JIT_STAT_INC(helper_loads);

  paddr_t paddr = 0;
  uint32_t value = 0;
  if (jit_direct_pmem_range(addr, len, &paddr))
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

static uint32_t jit_load_i8(vaddr_t addr)
{
  return (uint32_t)(int32_t)(int8_t)jit_load_raw(addr, 1);
}

static uint32_t jit_load_i16(vaddr_t addr)
{
  return (uint32_t)(int32_t)(int16_t)jit_load_raw(addr, 2);
}

static uint32_t jit_load_u32(vaddr_t addr)
{
  return jit_load_raw(addr, 4);
}

static uint32_t jit_load_u8(vaddr_t addr)
{
  return jit_load_raw(addr, 1);
}

static uint32_t jit_load_u16(vaddr_t addr)
{
  return jit_load_raw(addr, 2);
}

static void jit_store_raw(vaddr_t addr, uint32_t len, uint32_t data)
{
  JIT_STAT_INC(helper_stores);

  paddr_t paddr = 0;
  if (jit_direct_pmem_range(addr, len, &paddr))
  {
    JIT_STAT_INC(helper_store_direct);
    host_write(guest_to_host(paddr), (int)len, data);
    isa_jit_invalidate_paddr(paddr, (int)len);
    return;
  }

  JIT_STAT_INC(helper_store_slow);
  vaddr_write(addr, (int)len, data);
}

static void jit_store_u8(vaddr_t addr, uint32_t data)
{
  jit_store_raw(addr, 1, data);
}

static void jit_store_u16(vaddr_t addr, uint32_t data)
{
  jit_store_raw(addr, 2, data);
}

static void jit_store_u32(vaddr_t addr, uint32_t data)
{
  jit_store_raw(addr, 4, data);
}

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
          (int64_t)(int32_t)rhs) >> 32);
      break;
    case 0x00a:
      /*
       * MULHSU is signed(rs1) * unsigned(rs2). The product still fits in a
       * signed 64-bit value because both operands are 32-bit wide.
       */
      out = (uint32_t)(((int64_t)(int32_t)lhs *
          (int64_t)(uint64_t)rhs) >> 32);
      break;
    case 0x00b:
      out = (uint32_t)(((uint64_t)lhs * (uint64_t)rhs) >> 32);
      break;
    case 0x00c:
      out = (rhs == 0) ? UINT32_MAX :
          ((int32_t)lhs == INT32_MIN && (int32_t)rhs == -1
              ? lhs : (uint32_t)((int32_t)lhs / (int32_t)rhs));
      break;
    case 0x00d:
      out = rhs == 0 ? UINT32_MAX : lhs / rhs;
      break;
    case 0x00e:
      out = (rhs == 0) ? lhs :
          ((int32_t)lhs == INT32_MIN && (int32_t)rhs == -1
              ? 0 : (uint32_t)((int32_t)lhs % (int32_t)rhs));
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

static size_t jit_align_up(size_t value, size_t align)
{
  return (value + align - 1u) & ~(align - 1u);
}

static uint32_t jit_hash(vaddr_t pc, word_t satp)
{
  return ((pc >> 2) ^ satp ^ (satp >> 12)) & (RV32_JIT_CACHE_SIZE - 1u);
}

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

static void jit_source_chunks_ref(paddr_t addr, uint32_t len)
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
    Assert(jit_source_chunk_refs[i] != UINT16_MAX,
        "jit: too many source blocks in PMEM source chunk %zu", i);
    jit_source_chunk_refs[i]++;
  }
}

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

static bool jit_write_may_touch_source_chunk(paddr_t addr, int len)
{
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

static void jit_block_discard(rv32_jit_block_t *block)
{
  if (!block->valid)
  {
    return;
  }

  if (block->entry != NULL && block->source_len != 0)
  {
    jit_source_chunks_unref(block->paddr_start, block->source_len);
  }

  block->valid = false;
  block->entry = NULL;
  block->source_len = 0;
  block->insn_count = 0;
}

static void jit_cache_clear(void)
{
  memset(jit_cache, 0, sizeof(jit_cache));
  memset(jit_source_chunk_refs, 0, sizeof(jit_source_chunk_refs));
}

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

static void jit_arena_reset(void)
{
  JIT_STAT_INC(arena_resets);
  jit_code_used = 0;
  jit_cache_clear();
}

static bool emit_u8(rv32_jit_writer_t *w, uint8_t value)
{
  if (w->cur >= w->end)
  {
    return false;
  }

  *w->cur++ = value;
  return true;
}

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

static bool emit_movabs_r11(rv32_jit_writer_t *w, uint64_t value)
{
  /* movabs r11, imm64 */
  return emit_u8(w, 0x49) && emit_u8(w, 0xbb) && emit_u64(w, value);
}

static bool emit_load_cpu_base(rv32_jit_writer_t *w)
{
  return emit_movabs_r11(w, (uint64_t)(uintptr_t)&cpu);
}

static bool emit_load_gpr_eax(rv32_jit_writer_t *w, uint32_t reg)
{
  const uint32_t off = (uint32_t)offsetof(CPU_state, gpr)
      + reg * sizeof(cpu.gpr[0]);

  /* mov eax, dword ptr [r11 + off]. r11 holds &cpu inside native blocks. */
  return emit_u8(w, 0x41) && emit_u8(w, 0x8b) && emit_u8(w, 0x83)
      && emit_u32(w, off);
}

static bool emit_load_gpr_ecx(rv32_jit_writer_t *w, uint32_t reg)
{
  const uint32_t off = (uint32_t)offsetof(CPU_state, gpr)
      + reg * sizeof(cpu.gpr[0]);

  /* mov ecx, dword ptr [r11 + off]. r11 holds &cpu inside native blocks. */
  return emit_u8(w, 0x41) && emit_u8(w, 0x8b) && emit_u8(w, 0x8b)
      && emit_u32(w, off);
}

static bool emit_store_gpr_eax(rv32_jit_writer_t *w, uint32_t reg)
{
  if (reg == 0)
  {
    return true;
  }

  const uint32_t off = (uint32_t)offsetof(CPU_state, gpr)
      + reg * sizeof(cpu.gpr[0]);

  /* mov dword ptr [r11 + off], eax. Writes to x0 are ignored above. */
  return emit_u8(w, 0x41) && emit_u8(w, 0x89) && emit_u8(w, 0x83)
      && emit_u32(w, off);
}

static bool emit_store_pc_eax(rv32_jit_writer_t *w)
{
  const uint32_t off = (uint32_t)offsetof(CPU_state, pc);

  return emit_u8(w, 0x41) && emit_u8(w, 0x89) && emit_u8(w, 0x83)
      && emit_u32(w, off);
}

static bool emit_set_pc_imm(rv32_jit_writer_t *w, vaddr_t pc)
{
  const uint32_t off = (uint32_t)offsetof(CPU_state, pc);

  /* mov dword ptr [r11 + pc_off], imm32 */
  return emit_u8(w, 0x41) && emit_u8(w, 0xc7) && emit_u8(w, 0x83)
      && emit_u32(w, off) && emit_u32(w, pc);
}

static bool emit_mov_eax_imm(rv32_jit_writer_t *w, uint32_t value)
{
  return emit_u8(w, 0xb8) && emit_u32(w, value);
}

static bool emit_cmp_eax_imm(rv32_jit_writer_t *w, uint32_t value)
{
  return emit_u8(w, 0x3d) && emit_u32(w, value);
}

static bool emit_cmp_eax_ecx(rv32_jit_writer_t *w)
{
  return emit_u8(w, 0x39) && emit_u8(w, 0xc8);
}

static bool emit_setcc_eax(rv32_jit_writer_t *w, uint8_t setcc_opcode)
{
  /* setcc al; movzx eax, al */
  return emit_u8(w, 0x0f) && emit_u8(w, setcc_opcode) && emit_u8(w, 0xc0)
      && emit_u8(w, 0x0f) && emit_u8(w, 0xb6) && emit_u8(w, 0xc0);
}

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

static bool emit_jmp_rel32_placeholder(rv32_jit_writer_t *w, uint8_t **disp)
{
  if (!emit_u8(w, 0xe9))
  {
    return false;
  }

  *disp = w->cur;
  return emit_u32(w, 0);
}

static void patch_rel32(uint8_t *disp, const uint8_t *target)
{
  const int64_t rel = target - (disp + 4);
  Assert(rel >= INT32_MIN && rel <= INT32_MAX,
      "jit: x86 branch displacement out of range");
  const int32_t rel32 = (int32_t)rel;
  memcpy(disp, &rel32, sizeof(rel32));
}

static bool emit_call_abs(rv32_jit_writer_t *w, uintptr_t func)
{
  /* movabs rax, func; call rax */
  return emit_u8(w, 0x48) && emit_u8(w, 0xb8)
      && emit_u64(w, (uint64_t)func)
      && emit_u8(w, 0xff) && emit_u8(w, 0xd0);
}

typedef struct
{
  uint8_t *satp_slow_disp;
  uint8_t *range_slow_disp;
} rv32_jit_pmem_guard_patch_t;

static bool emit_movabs_r9(rv32_jit_writer_t *w, uint64_t value)
{
  /* movabs r9, imm64 */
  return emit_u8(w, 0x49) && emit_u8(w, 0xb9) && emit_u64(w, value);
}

static bool emit_movabs_r10(rv32_jit_writer_t *w, uint64_t value)
{
  /* movabs r10, imm64 */
  return emit_u8(w, 0x49) && emit_u8(w, 0xba) && emit_u64(w, value);
}

static bool emit_mov_edx_eax(rv32_jit_writer_t *w)
{
  return emit_u8(w, 0x89) && emit_u8(w, 0xc2);
}

static bool emit_mov_r8d_edx(rv32_jit_writer_t *w)
{
  return emit_u8(w, 0x41) && emit_u8(w, 0x89) && emit_u8(w, 0xd0);
}

static bool emit_sub_edx_imm(rv32_jit_writer_t *w, uint32_t value)
{
  return emit_u8(w, 0x81) && emit_u8(w, 0xea) && emit_u32(w, value);
}

static bool emit_cmp_edx_imm(rv32_jit_writer_t *w, uint32_t value)
{
  return emit_u8(w, 0x81) && emit_u8(w, 0xfa) && emit_u32(w, value);
}

static bool emit_and_r8d_imm(rv32_jit_writer_t *w, uint32_t value)
{
  return emit_u8(w, 0x41) && emit_u8(w, 0x81) && emit_u8(w, 0xe0)
      && emit_u32(w, value);
}

static bool emit_cmp_r8d_imm(rv32_jit_writer_t *w, uint32_t value)
{
  return emit_u8(w, 0x41) && emit_u8(w, 0x81) && emit_u8(w, 0xf8)
      && emit_u32(w, value);
}

static bool emit_shr_r8d_imm(rv32_jit_writer_t *w, uint8_t value)
{
  return emit_u8(w, 0x41) && emit_u8(w, 0xc1) && emit_u8(w, 0xe8)
      && emit_u8(w, value);
}

static bool emit_cmp_satp_zero(rv32_jit_writer_t *w)
{
  const uint32_t off = (uint32_t)offsetof(CPU_state, csr.satp);

  /* cmp dword ptr [r11 + satp_off], 0 */
  return emit_u8(w, 0x41) && emit_u8(w, 0x83) && emit_u8(w, 0xbb)
      && emit_u32(w, off) && emit_u8(w, 0x00);
}

static bool emit_cmp_source_chunk_ref_zero(rv32_jit_writer_t *w)
{
  /* cmp word ptr [r9 + r8 * 2], 0 */
  return emit_u8(w, 0x66) && emit_u8(w, 0x43) && emit_u8(w, 0x83)
      && emit_u8(w, 0x3c) && emit_u8(w, 0x41) && emit_u8(w, 0x00);
}

static bool emit_direct_pmem_guard(rv32_jit_writer_t *w, uint32_t len,
    rv32_jit_pmem_guard_patch_t *patch)
{
  Assert(len >= 1 && len <= 4, "jit: unsupported direct PMEM width %u", len);

  /*
   * Keep the guard stricter than paddr_read(): it only accepts a complete
   * in-PMEM byte range. Any boundary, MMIO, paging, or wraparound case falls
   * back to the existing helper path.
   */
  return emit_cmp_satp_zero(w)
      && emit_jcc_rel32_placeholder(w, 0x85, &patch->satp_slow_disp)
      && emit_mov_edx_eax(w)
      && emit_sub_edx_imm(w, (uint32_t)CONFIG_MBASE)
      && emit_cmp_edx_imm(w, (uint32_t)CONFIG_MSIZE - len)
      && emit_jcc_rel32_placeholder(w, 0x87, &patch->range_slow_disp);
}

static void patch_direct_pmem_guard(const rv32_jit_pmem_guard_patch_t *patch,
    const uint8_t *slow_path)
{
  patch_rel32(patch->satp_slow_disp, slow_path);
  patch_rel32(patch->range_slow_disp, slow_path);
}

static bool emit_direct_pmem_load_eax(rv32_jit_writer_t *w, uint32_t funct3)
{
  if (!emit_movabs_r10(w, (uint64_t)(uintptr_t)guest_to_host(CONFIG_MBASE)))
  {
    return false;
  }

  switch (funct3)
  {
    case 0x0:
      /* movsx eax, byte ptr [r10 + rdx] */
      return emit_u8(w, 0x41) && emit_u8(w, 0x0f) && emit_u8(w, 0xbe)
          && emit_u8(w, 0x04) && emit_u8(w, 0x12);
    case 0x1:
      /* movsx eax, word ptr [r10 + rdx] */
      return emit_u8(w, 0x41) && emit_u8(w, 0x0f) && emit_u8(w, 0xbf)
          && emit_u8(w, 0x04) && emit_u8(w, 0x12);
    case 0x2:
      /* mov eax, dword ptr [r10 + rdx] */
      return emit_u8(w, 0x41) && emit_u8(w, 0x8b)
          && emit_u8(w, 0x04) && emit_u8(w, 0x12);
    case 0x4:
      /* movzx eax, byte ptr [r10 + rdx] */
      return emit_u8(w, 0x41) && emit_u8(w, 0x0f) && emit_u8(w, 0xb6)
          && emit_u8(w, 0x04) && emit_u8(w, 0x12);
    case 0x5:
      /* movzx eax, word ptr [r10 + rdx] */
      return emit_u8(w, 0x41) && emit_u8(w, 0x0f) && emit_u8(w, 0xb7)
          && emit_u8(w, 0x04) && emit_u8(w, 0x12);
    default:
      return false;
  }
}

static bool emit_direct_pmem_store_from_ecx(rv32_jit_writer_t *w, uint32_t len)
{
  if (!emit_movabs_r10(w, (uint64_t)(uintptr_t)guest_to_host(CONFIG_MBASE)))
  {
    return false;
  }

  switch (len)
  {
    case 1:
      /* mov byte ptr [r10 + rdx], cl */
      return emit_u8(w, 0x41) && emit_u8(w, 0x88)
          && emit_u8(w, 0x0c) && emit_u8(w, 0x12);
    case 2:
      /* mov word ptr [r10 + rdx], cx */
      return emit_u8(w, 0x66) && emit_u8(w, 0x41) && emit_u8(w, 0x89)
          && emit_u8(w, 0x0c) && emit_u8(w, 0x12);
    case 4:
      /* mov dword ptr [r10 + rdx], ecx */
      return emit_u8(w, 0x41) && emit_u8(w, 0x89)
          && emit_u8(w, 0x0c) && emit_u8(w, 0x12);
    default:
      return false;
  }
}

static bool emit_store_source_chunk_guard(rv32_jit_writer_t *w, uint32_t len,
    uint8_t **cross_chunk_disp, uint8_t **source_chunk_disp)
{
  Assert(len >= 1 && len <= 4, "jit: unsupported direct store width %u", len);

  /*
   * Direct continuing stores only handle one source-tracking chunk. Crossing a
   * chunk boundary is rare for byte/halfword/word stores, and the helper path
   * remains the conservative choice because it can perform exact invalidation
   * and return to cpu_exec() before the next guest fetch.
   */
  return emit_mov_r8d_edx(w)
      && emit_and_r8d_imm(w, RV32_JIT_SOURCE_CHUNK_MASK)
      && emit_cmp_r8d_imm(w, RV32_JIT_SOURCE_CHUNK_SIZE - len)
      && emit_jcc_rel32_placeholder(w, 0x87, cross_chunk_disp)
      && emit_mov_r8d_edx(w)
      && emit_shr_r8d_imm(w, RV32_JIT_SOURCE_CHUNK_SHIFT)
      && emit_movabs_r9(w, (uint64_t)(uintptr_t)jit_source_chunk_refs)
      && emit_cmp_source_chunk_ref_zero(w)
      && emit_jcc_rel32_placeholder(w, 0x85, source_chunk_disp);
}

static bool emit_prologue(rv32_jit_writer_t *w)
{
  /*
   * System V enters this generated function with rsp % 16 == 8. Subtracting 8
   * aligns the stack before any helper call made inside the block.
   */
  return emit_u8(w, 0x48) && emit_u8(w, 0x83) && emit_u8(w, 0xec)
      && emit_u8(w, 0x08) && emit_load_cpu_base(w);
}

static bool emit_epilogue_return_count(rv32_jit_writer_t *w, uint32_t count)
{
  /* mov eax, count; add rsp, 8; ret */
  return emit_u8(w, 0xb8) && emit_u32(w, count)
      && emit_u8(w, 0x48) && emit_u8(w, 0x83) && emit_u8(w, 0xc4)
      && emit_u8(w, 0x08) && emit_u8(w, 0xc3);
}

static bool emit_addr_eax_from_rs1_imm(rv32_jit_writer_t *w, uint32_t rs1,
    int32_t imm)
{
  return emit_load_gpr_eax(w, rs1)
      && emit_u8(w, 0x05) && emit_u32(w, (uint32_t)imm);
}

static bool emit_load_instr(rv32_jit_writer_t *w, uint32_t instr, vaddr_t cur_pc)
{
  const uint32_t rd = bits(instr, 11, 7);
  const uint32_t funct3 = bits(instr, 14, 12);
  const uint32_t rs1 = bits(instr, 19, 15);

  uintptr_t helper = 0;
  uint32_t len = 0;
  switch (funct3)
  {
    case 0x0: helper = (uintptr_t)jit_load_i8; len = 1; break;
    case 0x1: helper = (uintptr_t)jit_load_i16; len = 2; break;
    case 0x2: helper = (uintptr_t)jit_load_u32; len = 4; break;
    case 0x4: helper = (uintptr_t)jit_load_u8; len = 1; break;
    case 0x5: helper = (uintptr_t)jit_load_u16; len = 2; break;
    default: return false;
  }

  rv32_jit_pmem_guard_patch_t guard = {0};
  uint8_t *done_disp = NULL;
  if (!emit_addr_eax_from_rs1_imm(w, rs1, imm_i(instr)) ||
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
  if (!emit_set_pc_imm(w, cur_pc) ||
      !emit_u8(w, 0x89) || !emit_u8(w, 0xc7) ||
      !emit_call_abs(w, helper) ||
      !emit_load_cpu_base(w))
  {
    return false;
  }

  patch_rel32(done_disp, w->cur);
  return emit_store_gpr_eax(w, rd);
}

static bool emit_store_instr(rv32_jit_writer_t *w, uint32_t instr,
    vaddr_t cur_pc, vaddr_t next_pc, uint32_t exit_count)
{
  const uint32_t funct3 = bits(instr, 14, 12);
  const uint32_t rs1 = bits(instr, 19, 15);
  const uint32_t rs2 = bits(instr, 24, 20);

  uintptr_t helper = 0;
  uint32_t len = 0;
  switch (funct3)
  {
    case 0x0: helper = (uintptr_t)jit_store_u8; len = 1; break;
    case 0x1: helper = (uintptr_t)jit_store_u16; len = 2; break;
    case 0x2: helper = (uintptr_t)jit_store_u32; len = 4; break;
    default: return false;
  }

  rv32_jit_pmem_guard_patch_t guard = {0};
  uint8_t *cross_chunk_disp = NULL;
  uint8_t *source_chunk_disp = NULL;
  uint8_t *done_disp = NULL;
  if (!emit_addr_eax_from_rs1_imm(w, rs1, imm_s(instr)) ||
      !emit_load_gpr_ecx(w, rs2) ||
      !emit_direct_pmem_guard(w, len, &guard) ||
      !emit_store_source_chunk_guard(w, len, &cross_chunk_disp,
          &source_chunk_disp) ||
      !emit_direct_pmem_store_from_ecx(w, len) ||
      !emit_jmp_rel32_placeholder(w, &done_disp))
  {
    return false;
  }

  const uint8_t *slow_path = w->cur;
  patch_direct_pmem_guard(&guard, slow_path);
  patch_rel32(cross_chunk_disp, slow_path);
  patch_rel32(source_chunk_disp, slow_path);

  /*
   * The helper path handles MMIO, paging, cross-chunk direct stores, and source
   * code invalidation. Set cpu.pc to the store itself before the call so faults
   * and MMIO diagnostics identify the correct guest instruction. After a
   * successful helper return, advance cpu.pc and leave the native block; the JIT
   * dispatcher may run another block, but it will start from the post-store PC.
   */
  if (!emit_set_pc_imm(w, cur_pc) ||
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

static bool emit_load_store_instr(rv32_jit_writer_t *w, uint32_t instr,
    vaddr_t cur_pc, uint32_t exit_count)
{
  const uint32_t opcode = instr & 0x7fu;

  if (opcode == 0x03)
  {
    return emit_load_instr(w, instr, cur_pc);
  }

  if (opcode == 0x23)
  {
    return emit_store_instr(w, instr, cur_pc, cur_pc + 4u, exit_count);
  }

  return false;
}

static bool jit_instr_is_control_flow(uint32_t instr)
{
  const uint32_t opcode = instr & 0x7fu;
  return opcode == 0x63 || opcode == 0x6f || opcode == 0x67;
}

static bool emit_branch_instr(rv32_jit_writer_t *w, uint32_t instr, vaddr_t pc)
{
  const uint32_t funct3 = bits(instr, 14, 12);
  const uint32_t rs1 = bits(instr, 19, 15);
  const uint32_t rs2 = bits(instr, 24, 20);
  uint8_t jcc = 0;

  switch (funct3)
  {
    case 0x0: jcc = 0x84; break; /* JE  */
    case 0x1: jcc = 0x85; break; /* JNE */
    case 0x4: jcc = 0x8c; break; /* JL, signed */
    case 0x5: jcc = 0x8d; break; /* JGE, signed */
    case 0x6: jcc = 0x82; break; /* JB, unsigned */
    case 0x7: jcc = 0x83; break; /* JAE, unsigned */
    default: return false;
  }

  uint8_t *taken_disp = NULL;
  uint8_t *end_disp = NULL;
  const vaddr_t fallthrough = pc + 4u;
  const vaddr_t target = pc + imm_b(instr);

  if (!emit_load_gpr_eax(w, rs1) || !emit_load_gpr_ecx(w, rs2) ||
      !emit_cmp_eax_ecx(w) || !emit_jcc_rel32_placeholder(w, jcc, &taken_disp) ||
      !emit_set_pc_imm(w, fallthrough) ||
      !emit_jmp_rel32_placeholder(w, &end_disp))
  {
    return false;
  }

  patch_rel32(taken_disp, w->cur);
  if (!emit_set_pc_imm(w, target))
  {
    return false;
  }

  patch_rel32(end_disp, w->cur);
  return true;
}

static bool emit_control_flow_instr(rv32_jit_writer_t *w, uint32_t instr,
    vaddr_t pc)
{
  const uint32_t opcode = instr & 0x7fu;
  const uint32_t rd = bits(instr, 11, 7);
  const uint32_t funct3 = bits(instr, 14, 12);
  const uint32_t rs1 = bits(instr, 19, 15);

  if (opcode == 0x63)
  {
    return emit_branch_instr(w, instr, pc);
  }

  if (opcode == 0x6f)
  {
    return emit_mov_eax_imm(w, pc + 4u)
        && emit_store_gpr_eax(w, rd)
        && emit_set_pc_imm(w, pc + imm_j(instr));
  }

  if (opcode == 0x67 && funct3 == 0)
  {
    /*
     * JALR computes the target from the old rs1 value, then writes the link
     * register. Storing cpu.pc before rd preserves rd == rs1 behaviour.
     */
    return emit_load_gpr_eax(w, rs1)
        && emit_u8(w, 0x05) && emit_u32(w, (uint32_t)imm_i(instr))
        && emit_u8(w, 0x25) && emit_u32(w, 0xfffffffeu)
        && emit_store_pc_eax(w)
        && emit_mov_eax_imm(w, pc + 4u)
        && emit_store_gpr_eax(w, rd);
  }

  return false;
}

static bool emit_alu_instr(rv32_jit_writer_t *w, uint32_t instr, vaddr_t cur_pc)
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
    return emit_mov_eax_imm(w, imm_u(instr)) && emit_store_gpr_eax(w, rd);
  }

  if (opcode == 0x17)
  {
    return emit_mov_eax_imm(w, cur_pc + imm_u(instr))
        && emit_store_gpr_eax(w, rd);
  }

  if (opcode == 0x13)
  {
    if (!emit_load_gpr_eax(w, rs1))
    {
      return false;
    }

    const uint32_t imm = (uint32_t)imm_i(instr);
    switch (funct3)
    {
      case 0x0: return emit_u8(w, 0x05) && emit_u32(w, imm) && emit_store_gpr_eax(w, rd);
      case 0x1:
        if (bits(instr, 31, 25) != 0x00)
        {
          return false;
        }
        return emit_u8(w, 0xc1) && emit_u8(w, 0xe0)
            && emit_u8(w, bits(instr, 24, 20)) && emit_store_gpr_eax(w, rd);
      case 0x2:
        return emit_cmp_eax_imm(w, imm) && emit_setcc_eax(w, 0x9c)
            && emit_store_gpr_eax(w, rd);
      case 0x3:
        return emit_cmp_eax_imm(w, imm) && emit_setcc_eax(w, 0x92)
            && emit_store_gpr_eax(w, rd);
      case 0x4: return emit_u8(w, 0x35) && emit_u32(w, imm) && emit_store_gpr_eax(w, rd);
      case 0x5:
        if (bits(instr, 31, 25) == 0x00)
        {
          return emit_u8(w, 0xc1) && emit_u8(w, 0xe8)
              && emit_u8(w, bits(instr, 24, 20)) && emit_store_gpr_eax(w, rd);
        }
        if (bits(instr, 31, 25) == 0x20)
        {
          return emit_u8(w, 0xc1) && emit_u8(w, 0xf8)
              && emit_u8(w, bits(instr, 24, 20)) && emit_store_gpr_eax(w, rd);
        }
        return false;
      case 0x6: return emit_u8(w, 0x0d) && emit_u32(w, imm) && emit_store_gpr_eax(w, rd);
      case 0x7: return emit_u8(w, 0x25) && emit_u32(w, imm) && emit_store_gpr_eax(w, rd);
      default: return false;
    }
  }

  if (opcode == 0x33)
  {
    if (!emit_load_gpr_eax(w, rs1) || !emit_load_gpr_ecx(w, rs2))
    {
      return false;
    }

    const uint32_t key = (funct7 << 3) | funct3;
    switch (key)
    {
      case 0x000: return emit_u8(w, 0x01) && emit_u8(w, 0xc8) && emit_store_gpr_eax(w, rd);
      case 0x100: return emit_u8(w, 0x29) && emit_u8(w, 0xc8) && emit_store_gpr_eax(w, rd);
      case 0x001: return emit_u8(w, 0xd3) && emit_u8(w, 0xe0) && emit_store_gpr_eax(w, rd);
      case 0x002: return emit_cmp_eax_ecx(w) && emit_setcc_eax(w, 0x9c) && emit_store_gpr_eax(w, rd);
      case 0x003: return emit_cmp_eax_ecx(w) && emit_setcc_eax(w, 0x92) && emit_store_gpr_eax(w, rd);
      case 0x004: return emit_u8(w, 0x31) && emit_u8(w, 0xc8) && emit_store_gpr_eax(w, rd);
      case 0x005: return emit_u8(w, 0xd3) && emit_u8(w, 0xe8) && emit_store_gpr_eax(w, rd);
      case 0x105: return emit_u8(w, 0xd3) && emit_u8(w, 0xf8) && emit_store_gpr_eax(w, rd);
      case 0x006: return emit_u8(w, 0x09) && emit_u8(w, 0xc8) && emit_store_gpr_eax(w, rd);
      case 0x007: return emit_u8(w, 0x21) && emit_u8(w, 0xc8) && emit_store_gpr_eax(w, rd);
      case 0x008: return emit_u8(w, 0x0f) && emit_u8(w, 0xaf) && emit_u8(w, 0xc1)
          && emit_store_gpr_eax(w, rd);
      case 0x009:
      case 0x00a:
      case 0x00b:
      case 0x00c:
      case 0x00d:
      case 0x00e:
      case 0x00f:
        return emit_u8(w, 0xbf) && emit_u32(w, instr)
            && emit_call_abs(w, (uintptr_t)jit_op_complex)
            && emit_load_cpu_base(w);
      default: return false;
    }
  }

  return false;
}

bool isa_jit_available(void)
{
  return !jit_runtime_disabled() && RV32_JIT_ENABLED && !jit_disabled &&
      jit_code_init();
}

void isa_jit_flush_all(void)
{
  if (jit_code != NULL)
  {
    jit_arena_reset();
  }
}

void isa_jit_invalidate_paddr(paddr_t addr, int len)
{
  JIT_STAT_INC(invalidation_requests);

  if (len <= 0)
  {
    return;
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
 * Convert the current instruction-fetch virtual address into the physical
 * address that backs the translated source bytes. Blocks are invalidated by
 * physical PMEM writes, so every cache entry records this physical range.
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

static rv32_jit_block_t *jit_cache_slot(vaddr_t pc)
{
  return &jit_cache[jit_hash(pc, cpu.csr.satp)];
}

static void jit_mark_unsupported(vaddr_t pc, paddr_t paddr, uint32_t source_len)
{
  JIT_STAT_INC(blocks_unsupported);

  rv32_jit_block_t *block = jit_cache_slot(pc);
  jit_block_discard(block);
  *block = (rv32_jit_block_t) {
    .valid = true,
    .pc = pc,
    .satp = cpu.csr.satp,
    .paddr_start = paddr,
    .source_len = source_len,
    .insn_count = 0,
    .entry = NULL,
  };
}

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
  if (!emit_prologue(&w))
  {
    return NULL;
  }

  vaddr_t cur_pc = pc;
  uint32_t count = 0;
  uint32_t source_len = 0;
  bool block_sets_pc = false;

  while (count < max_insns && count < RV32_JIT_BLOCK_MAX_INSNS)
  {
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
    bool end_block = false;
    if (jit_instr_is_control_flow(instr))
    {
      if (!emit_control_flow_instr(&w, instr, cur_pc))
      {
        w.cur = instr_start;
        break;
      }
      block_sets_pc = true;
      end_block = true;
    }
    else if (!emit_alu_instr(&w, instr, cur_pc) &&
        !emit_load_store_instr(&w, instr, cur_pc, count + 1u))
    {
      /*
       * Emitters may fail after writing a prefix of an x86 instruction. Roll
       * back to the last complete native instruction before falling back.
       */
      w.cur = instr_start;
      break;
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

  if ((!block_sets_pc && !emit_set_pc_imm(&w, cur_pc)) ||
      !emit_epilogue_return_count(&w, count))
  {
    return NULL;
  }

  __builtin___clear_cache((char *)w.start, (char *)w.cur);

  rv32_jit_block_t *block = jit_cache_slot(pc);
  jit_block_discard(block);
  jit_source_chunks_ref(first_paddr, source_len);
  JIT_STAT_INC(blocks_compiled);
  JIT_STAT_ADD(compiled_insns, count);
  *block = (rv32_jit_block_t) {
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
      ? RV32_JIT_BATCH_MAX_INSNS : (uint32_t)remaining;
  if (batch_budget > device_budget)
  {
    batch_budget = device_budget;
  }

  uint32_t total = 0;
  while (total < batch_budget)
  {
    uint32_t block_budget = batch_budget - total;
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

    const uint32_t ran = block->entry();
    Assert(ran > 0 && ran <= block_budget,
        "jit: invalid executed count %u", ran);
    JIT_STAT_INC(blocks_executed);
    JIT_STAT_ADD(executed_insns, ran);
    total += ran;
  }

  *executed = total;
  return total > 0;
}

#if RV32_JIT_STATS
static uint64_t jit_ratio_x100(uint64_t numerator, uint64_t denominator)
{
  if (denominator == 0)
  {
    return 0;
  }
  return (numerator * 100u + denominator / 2u) / denominator;
}

static uint64_t jit_percent_x100(uint64_t numerator, uint64_t denominator)
{
  if (denominator == 0)
  {
    return 0;
  }
  return (numerator * 10000u + denominator / 2u) / denominator;
}
#endif

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
