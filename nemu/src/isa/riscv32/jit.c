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
  /*
   * The writer owns a half-open native-code interval [start, end). cur always
   * points at the next byte to fill, so failed emitters can report overflow
   * without publishing a partial block into the cache.
   */
  uint8_t *start;
  uint8_t *cur;
  uint8_t *end;
} rv32_jit_writer_t;

typedef struct
{
  /*
   * valid means this slot is meaningful for its pc/satp tag. entry == NULL is
   * a deliberate negative-cache marker for unsupported instructions, not a
   * corrupt block; it prevents repeatedly trying to compile the same slow path.
   * Such markers are still checked against the current mapping before reuse, but
   * they do not own source refs because there is no native code to invalidate.
   */
  bool valid;
  vaddr_t pc;
  word_t satp;
  /*
   * Native code is invalidated by physical writes, so the translated source is
   * recorded as one contiguous PMEM range. The compiler stops a block before
   * the next instruction if virtual translation would make this range split.
   */
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
  /*
   * Helpers may be called from generated code where the fast inline PMEM guard
   * rejected the access. Re-check here rather than trusting the emitter: paging,
   * wrap-around, and MMIO must all fall through to the normal memory path. This
   * keeps the direct-PMEM optimisation a proof of the ordinary RAM case, not a
   * second memory model with subtly different fault or device behaviour.
   */
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

  /*
   * The direct helper path is still semantically a memory access by the guest:
   * it is allowed only for Bare-mode PMEM. Anything that might involve Sv32,
   * devices, or exception reporting delegates to vaddr_read().
   */
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

  /*
   * A direct helper store has to do the invalidation normally performed by
   * paddr_write(). Generated code may keep running after ordinary data stores,
   * so this is the point where self-modifying code is noticed before any later
   * block lookup can reuse stale native code.
   */
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
  /*
   * Generated blocks keep &cpu in r11 across straight-line code. Helper calls
   * use the host ABI and may clobber caller-saved registers, so call sites
   * reload r11 before continuing to access guest state.
   */
  return emit_movabs_r11(w, (uint64_t)(uintptr_t)&cpu);
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
    return emit_u8(w, 0x83) && emit_u8(w, 0xc0)
        && emit_u8(w, (uint8_t)signed_value);
  }

  return emit_u8(w, 0x05) && emit_u32(w, value);
}

static bool emit_cmp_eax_imm(rv32_jit_writer_t *w, uint32_t value)
{
  return emit_u8(w, 0x3d) && emit_u32(w, value);
}

static bool emit_cmp_eax_ecx(rv32_jit_writer_t *w)
{
  return emit_u8(w, 0x39) && emit_u8(w, 0xc8);
}

static bool emit_cmp_ecx_imm8(rv32_jit_writer_t *w, uint8_t value)
{
  return emit_u8(w, 0x83) && emit_u8(w, 0xf9) && emit_u8(w, value);
}

static bool emit_test_ecx_ecx(rv32_jit_writer_t *w)
{
  return emit_u8(w, 0x85) && emit_u8(w, 0xc9);
}

static bool emit_xor_edx_edx(rv32_jit_writer_t *w)
{
  return emit_u8(w, 0x31) && emit_u8(w, 0xd2);
}

static bool emit_cdq(rv32_jit_writer_t *w)
{
  return emit_u8(w, 0x99);
}

static bool emit_mov_eax_edx(rv32_jit_writer_t *w)
{
  return emit_u8(w, 0x89) && emit_u8(w, 0xd0);
}

static bool emit_mul_ecx(rv32_jit_writer_t *w)
{
  /* Unsigned edx:eax = eax * ecx. */
  return emit_u8(w, 0xf7) && emit_u8(w, 0xe1);
}

static bool emit_imul_ecx(rv32_jit_writer_t *w)
{
  /* Signed edx:eax = eax * ecx. */
  return emit_u8(w, 0xf7) && emit_u8(w, 0xe9);
}

static bool emit_div_ecx(rv32_jit_writer_t *w)
{
  /* Unsigned edx:eax / ecx, quotient in eax, remainder in edx. */
  return emit_u8(w, 0xf7) && emit_u8(w, 0xf1);
}

static bool emit_idiv_ecx(rv32_jit_writer_t *w)
{
  /* Signed edx:eax / ecx, quotient in eax, remainder in edx. */
  return emit_u8(w, 0xf7) && emit_u8(w, 0xf9);
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

typedef enum
{
  RV32_JIT_HREG_RBX = 0,
  RV32_JIT_HREG_R12,
  RV32_JIT_HREG_R13,
  RV32_JIT_HREG_R14,
  RV32_JIT_HREG_R15,
  RV32_JIT_HREG_COUNT,
} rv32_jit_hreg_t;

typedef struct
{
  bool valid;
  bool loaded;
  bool dirty;
  uint32_t guest_reg;
  uint32_t age;
  rv32_jit_hreg_t hreg;
} rv32_jit_reg_slot_t;

typedef struct
{
  rv32_jit_reg_slot_t slots[RV32_JIT_HREG_COUNT];
  uint32_t next_age;
  bool source_refs_loaded;
} rv32_jit_reg_cache_t;

static uint8_t jit_hreg_x86_reg(rv32_jit_hreg_t hreg)
{
  switch (hreg)
  {
    case RV32_JIT_HREG_RBX: return 3;
    case RV32_JIT_HREG_R12: return 12;
    case RV32_JIT_HREG_R13: return 13;
    case RV32_JIT_HREG_R14: return 14;
    case RV32_JIT_HREG_R15: return 15;
    default: Assert(0, "jit: invalid host register slot %d", hreg);
  }
  return 3;
}

static uint8_t jit_modrm(uint8_t mod, uint8_t reg, uint8_t rm)
{
  return (uint8_t)((mod << 6) | ((reg & 7u) << 3) | (rm & 7u));
}

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

static bool emit_push_saved_hregs(rv32_jit_writer_t *w)
{
  return emit_u8(w, 0x53)
      && emit_u8(w, 0x41) && emit_u8(w, 0x54)
      && emit_u8(w, 0x41) && emit_u8(w, 0x55)
      && emit_u8(w, 0x41) && emit_u8(w, 0x56)
      && emit_u8(w, 0x41) && emit_u8(w, 0x57);
}

static bool emit_pop_saved_hregs(rv32_jit_writer_t *w)
{
  return emit_u8(w, 0x41) && emit_u8(w, 0x5f)
      && emit_u8(w, 0x41) && emit_u8(w, 0x5e)
      && emit_u8(w, 0x41) && emit_u8(w, 0x5d)
      && emit_u8(w, 0x41) && emit_u8(w, 0x5c)
      && emit_u8(w, 0x5b);
}

static bool emit_load_gpr_hreg(rv32_jit_writer_t *w, rv32_jit_hreg_t hreg,
    uint32_t reg)
{
  const uint32_t off = (uint32_t)offsetof(CPU_state, gpr)
      + reg * sizeof(cpu.gpr[0]);
  const uint8_t dst = jit_hreg_x86_reg(hreg);
  const uint8_t base = 11;

  /* mov hreg32, dword ptr [r11 + off] */
  return emit_rex32_if_needed(w, dst, base)
      && emit_u8(w, 0x8b)
      && emit_u8(w, jit_modrm(2, dst, base))
      && emit_u32(w, off);
}

static bool emit_store_gpr_hreg(rv32_jit_writer_t *w, uint32_t reg,
    rv32_jit_hreg_t hreg)
{
  const uint32_t off = (uint32_t)offsetof(CPU_state, gpr)
      + reg * sizeof(cpu.gpr[0]);
  const uint8_t src = jit_hreg_x86_reg(hreg);
  const uint8_t base = 11;

  /* mov dword ptr [r11 + off], hreg32 */
  return emit_rex32_if_needed(w, src, base)
      && emit_u8(w, 0x89)
      && emit_u8(w, jit_modrm(2, src, base))
      && emit_u32(w, off);
}

static bool emit_mov_eax_hreg(rv32_jit_writer_t *w, rv32_jit_hreg_t hreg)
{
  const uint8_t src = jit_hreg_x86_reg(hreg);

  /* mov eax, hreg32 */
  return emit_rex32_if_needed(w, src, 0)
      && emit_u8(w, 0x89)
      && emit_u8(w, jit_modrm(3, src, 0));
}

static bool emit_mov_ecx_hreg(rv32_jit_writer_t *w, rv32_jit_hreg_t hreg)
{
  const uint8_t src = jit_hreg_x86_reg(hreg);

  /* mov ecx, hreg32 */
  return emit_rex32_if_needed(w, src, 1)
      && emit_u8(w, 0x89)
      && emit_u8(w, jit_modrm(3, src, 1));
}

static bool emit_mov_hreg_eax(rv32_jit_writer_t *w, rv32_jit_hreg_t hreg)
{
  const uint8_t dst = jit_hreg_x86_reg(hreg);

  /* mov hreg32, eax */
  return emit_rex32_if_needed(w, 0, dst)
      && emit_u8(w, 0x89)
      && emit_u8(w, jit_modrm(3, 0, dst));
}

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
  return emit_rex32_if_needed(w, src_reg, dst_reg)
      && emit_u8(w, 0x89)
      && emit_u8(w, jit_modrm(3, src_reg, dst_reg));
}

static bool emit_mov_hreg_imm(rv32_jit_writer_t *w, rv32_jit_hreg_t hreg,
    uint32_t value)
{
  const uint8_t dst = jit_hreg_x86_reg(hreg);

  /* mov hreg32, imm32 */
  return emit_rex32_if_needed(w, 0, dst)
      && emit_u8(w, 0xc7)
      && emit_u8(w, jit_modrm(3, 0, dst))
      && emit_u32(w, value);
}

static bool __attribute__((unused)) emit_mov_hreg_ecx(rv32_jit_writer_t *w,
    rv32_jit_hreg_t hreg)
{
  const uint8_t dst = jit_hreg_x86_reg(hreg);

  /* mov hreg32, ecx */
  return emit_rex32_if_needed(w, 1, dst)
      && emit_u8(w, 0x89)
      && emit_u8(w, jit_modrm(3, 1, dst));
}

static void jit_reg_cache_init(rv32_jit_reg_cache_t *regs)
{
  regs->next_age = 1;
  regs->source_refs_loaded = false;
  for (uint32_t i = 0; i < RV32_JIT_HREG_COUNT; i++)
  {
    regs->slots[i] = (rv32_jit_reg_slot_t) {
      .valid = false,
      .loaded = false,
      .dirty = false,
      .guest_reg = 0,
      .age = 0,
      .hreg = (rv32_jit_hreg_t)i,
    };
  }
}

static void jit_reg_cache_restore(rv32_jit_reg_cache_t *regs,
    const rv32_jit_reg_cache_t *snapshot)
{
  *regs = *snapshot;
}

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

static bool jit_reg_emit_flush_slot(rv32_jit_writer_t *w,
    const rv32_jit_reg_slot_t *slot)
{
  if (!slot->valid || !slot->loaded || !slot->dirty || slot->guest_reg == 0)
  {
    return true;
  }

  return emit_store_gpr_hreg(w, slot->guest_reg, slot->hreg);
}

static bool jit_reg_flush_slot(rv32_jit_writer_t *w, rv32_jit_reg_slot_t *slot)
{
  if (!jit_reg_emit_flush_slot(w, slot))
  {
    return false;
  }

  slot->dirty = false;
  return true;
}

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

static void jit_reg_mark_hreg_dirty(rv32_jit_reg_cache_t *regs,
    rv32_jit_reg_slot_t *slot)
{
  slot->loaded = true;
  slot->dirty = true;
  slot->age = regs->next_age++;
}

static bool emit_hreg_binop_hreg(rv32_jit_writer_t *w, uint8_t opcode,
    rv32_jit_hreg_t dst, rv32_jit_hreg_t src)
{
  const uint8_t dst_reg = jit_hreg_x86_reg(dst);
  const uint8_t src_reg = jit_hreg_x86_reg(src);

  /* opcode dst, src */
  return emit_rex32_if_needed(w, src_reg, dst_reg)
      && emit_u8(w, opcode)
      && emit_u8(w, jit_modrm(3, src_reg, dst_reg));
}

static bool emit_hreg_alu_imm(rv32_jit_writer_t *w, rv32_jit_hreg_t hreg,
    uint8_t subop, uint32_t imm)
{
  const uint8_t dst = jit_hreg_x86_reg(hreg);
  const int32_t simm = (int32_t)imm;
  if (simm >= INT8_MIN && simm <= INT8_MAX)
  {
    /* 83 /subop ib sign-extends the immediate, matching these RV32 values. */
    return emit_rex32_if_needed(w, subop, dst)
        && emit_u8(w, 0x83)
        && emit_u8(w, jit_modrm(3, subop, dst))
        && emit_u8(w, (uint8_t)simm);
  }

  /* 81 /subop id against the cached host register. */
  return emit_rex32_if_needed(w, subop, dst)
      && emit_u8(w, 0x81)
      && emit_u8(w, jit_modrm(3, subop, dst))
      && emit_u32(w, imm);
}

static bool emit_hreg_shift_imm(rv32_jit_writer_t *w, rv32_jit_hreg_t hreg,
    uint8_t subop, uint8_t amount)
{
  const uint8_t dst = jit_hreg_x86_reg(hreg);
  return emit_rex32_if_needed(w, subop, dst)
      && emit_u8(w, 0xc1)
      && emit_u8(w, jit_modrm(3, subop, dst))
      && emit_u8(w, amount);
}

static bool emit_hreg_shift_cl(rv32_jit_writer_t *w, rv32_jit_hreg_t hreg,
    uint8_t subop)
{
  const uint8_t dst = jit_hreg_x86_reg(hreg);
  return emit_rex32_if_needed(w, subop, dst)
      && emit_u8(w, 0xd3)
      && emit_u8(w, jit_modrm(3, subop, dst));
}

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

static bool emit_rv32_mul_high(rv32_jit_writer_t *w,
    rv32_jit_reg_cache_t *regs, uint32_t rd, bool is_signed)
{
  return (is_signed ? emit_imul_ecx(w) : emit_mul_ecx(w))
      && emit_mov_eax_edx(w)
      && jit_reg_write_eax(w, regs, rd);
}

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

static bool emit_load_pmem_base(rv32_jit_writer_t *w)
{
  /*
   * Direct-PMEM fast paths are common enough that loading this once per native
   * block is cheaper than repeating a movabs before every translated load or
   * store. r10 is caller-saved, so helper calls that rejoin the block reload it.
   */
  return emit_movabs_r10(w, (uint64_t)(uintptr_t)guest_to_host(CONFIG_MBASE));
}

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

static bool emit_lea_edx_eax_imm(rv32_jit_writer_t *w, uint32_t value)
{
  /*
   * lea edx, [rax + disp32] computes the low RV32 address bits in one
   * instruction. With disp32 = -CONFIG_MBASE it replaces mov edx,eax; sub edx,
   * CONFIG_MBASE in the direct-PMEM guard.
   */
  return emit_u8(w, 0x8d) && emit_u8(w, 0x90) && emit_u32(w, value);
}

static bool emit_mov_r8d_edx(rv32_jit_writer_t *w)
{
  return emit_u8(w, 0x41) && emit_u8(w, 0x89) && emit_u8(w, 0xd0);
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

  return emit_lea_edx_eax_imm(w, 0u - (uint32_t)CONFIG_MBASE)
      && emit_cmp_edx_imm(w, (uint32_t)CONFIG_MSIZE - len)
      && emit_jcc_rel32_placeholder(w, 0x87, &patch->range_slow_disp);
}

static void patch_direct_pmem_guard(const rv32_jit_pmem_guard_patch_t *patch,
    const uint8_t *slow_path)
{
  if (patch->satp_slow_disp != NULL)
  {
    patch_rel32(patch->satp_slow_disp, slow_path);
  }
  patch_rel32(patch->range_slow_disp, slow_path);
}

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
  /*
   * Stores use the low part of ECX so SB/SH truncate in the same way host_write()
   * does. The caller has already proved Bare-mode PMEM and checked source-chunk
   * refs before taking this continuation path.
   */
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
  return emit_mov_r8d_edx(w)
      && emit_and_r8d_imm(w, RV32_JIT_SOURCE_CHUNK_MASK)
      && emit_cmp_r8d_imm(w, RV32_JIT_SOURCE_CHUNK_SIZE - len)
      && emit_jcc_rel32_placeholder(w, 0x87, cross_chunk_disp)
      && emit_mov_r8d_edx(w)
      && emit_shr_r8d_imm(w, RV32_JIT_SOURCE_CHUNK_SHIFT)
      && jit_reg_ensure_source_refs_base(w, regs)
      && emit_cmp_source_chunk_ref_zero(w)
      && emit_jcc_rel32_placeholder(w, 0x85, source_chunk_disp);
}

static bool emit_prologue(rv32_jit_writer_t *w)
{
  /*
   * System V enters generated code with rsp % 16 == 8. Five callee-saved
   * pushes align the stack before helper calls and provide the guest register
   * cache slots.
   */
  return emit_push_saved_hregs(w) && emit_load_cpu_base(w)
      && emit_load_pmem_base(w);
}

static bool emit_epilogue_return_count(rv32_jit_writer_t *w, uint32_t count)
{
  /* mov eax, count; pop saved cache registers; ret */
  return emit_u8(w, 0xb8) && emit_u32(w, count)
      && emit_pop_saved_hregs(w)
      && emit_u8(w, 0xc3);
}

static bool emit_load_instr(rv32_jit_writer_t *w, rv32_jit_reg_cache_t *regs,
    uint32_t instr, vaddr_t cur_pc)
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

static bool emit_store_instr(rv32_jit_writer_t *w, rv32_jit_reg_cache_t *regs,
    uint32_t instr, vaddr_t cur_pc, vaddr_t next_pc, uint32_t exit_count)
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

static bool emit_load_store_instr(rv32_jit_writer_t *w,
    rv32_jit_reg_cache_t *regs, uint32_t instr, vaddr_t cur_pc,
    uint32_t exit_count)
{
  const uint32_t opcode = instr & 0x7fu;

  if (opcode == 0x03)
  {
    return emit_load_instr(w, regs, instr, cur_pc);
  }

  if (opcode == 0x23)
  {
    return emit_store_instr(w, regs, instr, cur_pc, cur_pc + 4u, exit_count);
  }

  return false;
}

static bool jit_instr_is_control_flow(uint32_t instr)
{
  const uint32_t opcode = instr & 0x7fu;
  return opcode == 0x63 || opcode == 0x6f || opcode == 0x67;
}

static bool emit_branch_instr(rv32_jit_writer_t *w, rv32_jit_reg_cache_t *regs,
    uint32_t instr, vaddr_t pc, uint32_t exit_count)
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

  uint8_t *fallthrough_disp = NULL;
  const vaddr_t target = pc + imm_b(instr);

  /*
   * Conditional branches are the first control-flow case that can keep useful
   * cached registers alive. The untaken path stays in this native block, while
   * the taken path materialises the same register state into cpu.gpr[] and
   * returns to the dispatcher at the branch target.
   */
  if (!jit_reg_read_eax(w, regs, rs1) || !jit_reg_read_ecx(w, regs, rs2) ||
      !emit_cmp_eax_ecx(w) ||
      !emit_jcc_rel32_placeholder(w, (uint8_t)(jcc ^ 1u),
          &fallthrough_disp) ||
      !jit_reg_emit_flush_all_dirty(w, regs) ||
      !emit_set_pc_imm(w, target) ||
      !emit_epilogue_return_count(w, exit_count))
  {
    return false;
  }

  patch_rel32(fallthrough_disp, w->cur);
  return true;
}

static bool emit_control_flow_instr(rv32_jit_writer_t *w,
    rv32_jit_reg_cache_t *regs, uint32_t instr, vaddr_t pc)
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
    return emit_mov_eax_imm(w, pc + 4u)
        && jit_reg_write_eax(w, regs, rd)
        && jit_reg_emit_flush_all_dirty(w, regs)
        && emit_set_pc_imm(w, pc + imm_j(instr));
  }

  if (opcode == 0x67 && funct3 == 0)
  {
    /*
     * JALR computes the target from the old rs1 value, then writes the link
     * register. Storing cpu.pc before rd preserves rd == rs1 behaviour.
     */
    return jit_reg_read_eax(w, regs, rs1)
        && emit_add_eax_imm(w, (uint32_t)imm_i(instr))
        && emit_u8(w, 0x25) && emit_u32(w, 0xfffffffeu)
        && emit_store_pc_eax(w)
        && emit_mov_eax_imm(w, pc + 4u)
        && jit_reg_write_eax(w, regs, rd)
        && jit_reg_emit_flush_all_dirty(w, regs);
  }

  return false;
}

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
        case 0x0: return jit_reg_write_imm(w, regs, rd, imm);
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
        case 0x4: return jit_reg_write_imm(w, regs, rd, imm);
        case 0x5:
          if (bits(instr, 31, 25) == 0x00 || bits(instr, 31, 25) == 0x20)
          {
            return jit_reg_write_imm(w, regs, rd, 0);
          }
          return false;
        case 0x6: return jit_reg_write_imm(w, regs, rd, imm);
        case 0x7: return jit_reg_write_imm(w, regs, rd, 0);
        default: return false;
      }
    }

    if (rd != 0 && rd == rs1)
    {
      switch (funct3)
      {
        case 0x0: return imm == 0 ? true :
            jit_reg_apply_imm(w, regs, rd, 0, imm);
        case 0x1:
          if (bits(instr, 31, 25) != 0x00)
          {
            return false;
          }
          return jit_reg_apply_shift_imm(w, regs, rd, 4,
              (uint8_t)bits(instr, 24, 20));
        case 0x4: return imm == 0 ? true :
            jit_reg_apply_imm(w, regs, rd, 6, imm);
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
        case 0x6: return imm == 0 ? true :
            jit_reg_apply_imm(w, regs, rd, 1, imm);
        case 0x7: return jit_reg_apply_imm(w, regs, rd, 4, imm);
        default: break;
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
        default: break;
      }
    }

    if (!jit_reg_read_eax(w, regs, rs1))
    {
      return false;
    }

    switch (funct3)
    {
      case 0x0: return emit_add_eax_imm(w, imm)
          && jit_reg_write_eax(w, regs, rd);
      case 0x1:
        if (bits(instr, 31, 25) != 0x00)
        {
          return false;
        }
        return emit_u8(w, 0xc1) && emit_u8(w, 0xe0)
            && emit_u8(w, bits(instr, 24, 20))
            && jit_reg_write_eax(w, regs, rd);
      case 0x2:
        return emit_cmp_eax_imm(w, imm) && emit_setcc_eax(w, 0x9c)
            && jit_reg_write_eax(w, regs, rd);
      case 0x3:
        return emit_cmp_eax_imm(w, imm) && emit_setcc_eax(w, 0x92)
            && jit_reg_write_eax(w, regs, rd);
      case 0x4: return emit_u8(w, 0x35) && emit_u32(w, imm)
          && jit_reg_write_eax(w, regs, rd);
      case 0x5:
        if (bits(instr, 31, 25) == 0x00)
        {
          return emit_u8(w, 0xc1) && emit_u8(w, 0xe8)
              && emit_u8(w, bits(instr, 24, 20))
              && jit_reg_write_eax(w, regs, rd);
        }
        if (bits(instr, 31, 25) == 0x20)
        {
          return emit_u8(w, 0xc1) && emit_u8(w, 0xf8)
              && emit_u8(w, bits(instr, 24, 20))
              && jit_reg_write_eax(w, regs, rd);
        }
        return false;
      case 0x6: return emit_u8(w, 0x0d) && emit_u32(w, imm)
          && jit_reg_write_eax(w, regs, rd);
      case 0x7: return emit_u8(w, 0x25) && emit_u32(w, imm)
          && jit_reg_write_eax(w, regs, rd);
      default: return false;
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
        default: break;
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
            return rs1 == rs2 ? jit_reg_write_imm(w, regs, rd, 0) :
                jit_reg_copy(w, regs, rd, rs1);
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
        default: break;
      }
    }

    if (!jit_reg_read_eax(w, regs, rs1) ||
        !jit_reg_read_ecx(w, regs, rs2))
    {
      return false;
    }

    switch (key)
    {
      case 0x000: return emit_u8(w, 0x01) && emit_u8(w, 0xc8)
          && jit_reg_write_eax(w, regs, rd);
      case 0x100: return emit_u8(w, 0x29) && emit_u8(w, 0xc8)
          && jit_reg_write_eax(w, regs, rd);
      case 0x001: return emit_u8(w, 0xd3) && emit_u8(w, 0xe0)
          && jit_reg_write_eax(w, regs, rd);
      case 0x002: return emit_cmp_eax_ecx(w) && emit_setcc_eax(w, 0x9c)
          && jit_reg_write_eax(w, regs, rd);
      case 0x003: return emit_cmp_eax_ecx(w) && emit_setcc_eax(w, 0x92)
          && jit_reg_write_eax(w, regs, rd);
      case 0x004: return emit_u8(w, 0x31) && emit_u8(w, 0xc8)
          && jit_reg_write_eax(w, regs, rd);
      case 0x005: return emit_u8(w, 0xd3) && emit_u8(w, 0xe8)
          && jit_reg_write_eax(w, regs, rd);
      case 0x105: return emit_u8(w, 0xd3) && emit_u8(w, 0xf8)
          && jit_reg_write_eax(w, regs, rd);
      case 0x006: return emit_u8(w, 0x09) && emit_u8(w, 0xc8)
          && jit_reg_write_eax(w, regs, rd);
      case 0x007: return emit_u8(w, 0x21) && emit_u8(w, 0xc8)
          && jit_reg_write_eax(w, regs, rd);
      case 0x008: return emit_u8(w, 0x0f) && emit_u8(w, 0xaf) && emit_u8(w, 0xc1)
          && jit_reg_write_eax(w, regs, rd);
      case 0x009: return emit_rv32_mul_high(w, regs, rd, true);
      case 0x00b: return emit_rv32_mul_high(w, regs, rd, false);
      case 0x00c: return emit_rv32_div(w, regs, rd);
      case 0x00d: return emit_rv32_divu(w, regs, rd);
      case 0x00e: return emit_rv32_rem(w, regs, rd);
      case 0x00f: return emit_rv32_remu(w, regs, rd);
      case 0x00a:
        return jit_reg_flush_all_dirty(w, regs)
            && emit_u8(w, 0xbf) && emit_u32(w, instr)
            && emit_call_abs(w, (uintptr_t)jit_op_complex)
            && emit_load_cpu_base(w)
            && emit_load_pmem_base(w)
            && (!regs->source_refs_loaded || emit_load_source_refs_base(w))
            && (jit_reg_invalidate_all(regs), true)
            && jit_reg_write_eax(w, regs, rd);
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
  /*
   * A full flush drops native code and source refs together. This is used when
   * an address-space change makes virtual PCs difficult to relate to the old
   * physical source ranges cheaply.
   */
  if (jit_code != NULL)
  {
    jit_arena_reset();
  }
}

void isa_jit_invalidate_paddr(paddr_t addr, int len)
{
  JIT_STAT_INC(invalidation_requests);

  /*
   * Invalidations are keyed by physical bytes because writes can arrive through
   * the interpreter, JIT helper stores, or devices. The half-open write interval
   * below is compared with each compiled block's physical source interval.
   */
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
  rv32_jit_reg_cache_t regs;
  jit_reg_cache_init(&regs);

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
      if (!emit_branch_instr(&w, &regs, instr, cur_pc, count + 1u))
      {
        w.cur = instr_start;
        jit_reg_cache_restore(&regs, &regs_start);
        break;
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
      !emit_epilogue_return_count(&w, count))
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
    /*
     * Each native block reports how many guest instructions it completed. The
     * dispatcher uses that count, rather than assuming a fixed block length, so
     * helper exits and control-flow terminators keep device timing bounded.
     */
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
