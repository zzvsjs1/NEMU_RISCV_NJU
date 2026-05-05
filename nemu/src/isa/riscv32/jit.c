#include <isa-jit.h>
#include <isa.h>
#include <memory/vaddr.h>
#include <utils.h>
#include "local-include/reg.h"

#include <stddef.h>

#if defined(__x86_64__) && defined(CONFIG_ISA_riscv32) && \
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
#define RV32_JIT_CACHE_SIZE 4096u
#define RV32_JIT_CODE_SIZE (4u * 1024u * 1024u)
#define RV32_JIT_CODE_ALIGN 16u

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
static uint8_t *jit_code = NULL;
static size_t jit_code_used = 0;
static bool jit_disabled = false;

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

static uint32_t imm_u(uint32_t instr)
{
  return instr & 0xfffff000u;
}

static uint32_t jit_load(vaddr_t addr, uint32_t len, uint32_t sign_ext)
{
  uint32_t value = vaddr_read(addr, (int)len);
  if (sign_ext && len == 1)
  {
    value = (uint32_t)(int32_t)(int8_t)value;
  }
  else if (sign_ext && len == 2)
  {
    value = (uint32_t)(int32_t)(int16_t)value;
  }

  return value;
}

static void jit_store(vaddr_t addr, uint32_t len, uint32_t data)
{
  vaddr_write(addr, (int)len, data);
}

static size_t jit_align_up(size_t value, size_t align)
{
  return (value + align - 1u) & ~(align - 1u);
}

static uint32_t jit_hash(vaddr_t pc, word_t satp)
{
  return ((pc >> 2) ^ satp ^ (satp >> 12)) & (RV32_JIT_CACHE_SIZE - 1u);
}

static void jit_cache_clear(void)
{
  memset(jit_cache, 0, sizeof(jit_cache));
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

static bool emit_load_gpr_eax(rv32_jit_writer_t *w, uint32_t reg)
{
  const uint32_t off = (uint32_t)offsetof(CPU_state, gpr)
      + reg * sizeof(cpu.gpr[0]);

  /* mov eax, dword ptr [r11 + off], with r11 loaded from &cpu first. */
  return emit_movabs_r11(w, (uint64_t)(uintptr_t)&cpu)
      && emit_u8(w, 0x41) && emit_u8(w, 0x8b) && emit_u8(w, 0x83)
      && emit_u32(w, off);
}

static bool emit_load_gpr_ecx(rv32_jit_writer_t *w, uint32_t reg)
{
  const uint32_t off = (uint32_t)offsetof(CPU_state, gpr)
      + reg * sizeof(cpu.gpr[0]);

  /* mov ecx, dword ptr [r11 + off], with r11 loaded from &cpu first. */
  return emit_movabs_r11(w, (uint64_t)(uintptr_t)&cpu)
      && emit_u8(w, 0x41) && emit_u8(w, 0x8b) && emit_u8(w, 0x8b)
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
  return emit_movabs_r11(w, (uint64_t)(uintptr_t)&cpu)
      && emit_u8(w, 0x41) && emit_u8(w, 0x89) && emit_u8(w, 0x83)
      && emit_u32(w, off);
}

static bool emit_set_pc_imm(rv32_jit_writer_t *w, vaddr_t pc)
{
  const uint32_t off = (uint32_t)offsetof(CPU_state, pc);

  /* mov dword ptr [r11 + pc_off], imm32 */
  return emit_movabs_r11(w, (uint64_t)(uintptr_t)&cpu)
      && emit_u8(w, 0x41) && emit_u8(w, 0xc7) && emit_u8(w, 0x83)
      && emit_u32(w, off) && emit_u32(w, pc);
}

static bool emit_call_abs(rv32_jit_writer_t *w, uintptr_t func)
{
  /* movabs rax, func; call rax */
  return emit_u8(w, 0x48) && emit_u8(w, 0xb8)
      && emit_u64(w, (uint64_t)func)
      && emit_u8(w, 0xff) && emit_u8(w, 0xd0);
}

static bool emit_prologue(rv32_jit_writer_t *w)
{
  /*
   * System V enters this generated function with rsp % 16 == 8. Subtracting 8
   * aligns the stack before any helper call made inside the block.
   */
  return emit_u8(w, 0x48) && emit_u8(w, 0x83) && emit_u8(w, 0xec)
      && emit_u8(w, 0x08);
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

static bool emit_load_store_instr(rv32_jit_writer_t *w, uint32_t instr)
{
  const uint32_t opcode = instr & 0x7fu;
  const uint32_t rd = bits(instr, 11, 7);
  const uint32_t funct3 = bits(instr, 14, 12);
  const uint32_t rs1 = bits(instr, 19, 15);
  const uint32_t rs2 = bits(instr, 24, 20);

  if (opcode == 0x03)
  {
    uint32_t len = 0;
    uint32_t sign_ext = 0;
    switch (funct3)
    {
      case 0x0: len = 1; sign_ext = 1; break;
      case 0x1: len = 2; sign_ext = 1; break;
      case 0x2: len = 4; sign_ext = 0; break;
      case 0x4: len = 1; sign_ext = 0; break;
      case 0x5: len = 2; sign_ext = 0; break;
      default: return false;
    }

    /*
     * C helper arguments follow the x86-64 SysV ABI:
     *   edi = guest virtual address, esi = width, edx = sign-extension flag.
     */
    return emit_addr_eax_from_rs1_imm(w, rs1, imm_i(instr))
        && emit_u8(w, 0x89) && emit_u8(w, 0xc7)
        && emit_u8(w, 0xbe) && emit_u32(w, len)
        && emit_u8(w, 0xba) && emit_u32(w, sign_ext)
        && emit_call_abs(w, (uintptr_t)jit_load)
        && emit_store_gpr_eax(w, rd);
  }

  if (opcode == 0x23)
  {
    uint32_t len = 0;
    switch (funct3)
    {
      case 0x0: len = 1; break;
      case 0x1: len = 2; break;
      case 0x2: len = 4; break;
      default: return false;
    }

    /*
     * Stores go through vaddr_write(), so MMU, MMIO and code-cache
     * invalidation stay in the existing memory system.
     */
    return emit_addr_eax_from_rs1_imm(w, rs1, imm_s(instr))
        && emit_u8(w, 0x89) && emit_u8(w, 0xc7)
        && emit_u8(w, 0xbe) && emit_u32(w, len)
        && emit_load_gpr_eax(w, rs2)
        && emit_u8(w, 0x89) && emit_u8(w, 0xc2)
        && emit_call_abs(w, (uintptr_t)jit_store);
  }

  return false;
}

static bool jit_instr_is_store(uint32_t instr)
{
  return (instr & 0x7fu) == 0x23;
}

static bool emit_alu_instr(rv32_jit_writer_t *w, uint32_t instr)
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
    return emit_u8(w, 0xb8) && emit_u32(w, imm_u(instr))
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
      case 0x4: return emit_u8(w, 0x35) && emit_u32(w, imm) && emit_store_gpr_eax(w, rd);
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
      case 0x004: return emit_u8(w, 0x31) && emit_u8(w, 0xc8) && emit_store_gpr_eax(w, rd);
      case 0x006: return emit_u8(w, 0x09) && emit_u8(w, 0xc8) && emit_store_gpr_eax(w, rd);
      case 0x007: return emit_u8(w, 0x21) && emit_u8(w, 0xc8) && emit_store_gpr_eax(w, rd);
      default: return false;
    }
  }

  return false;
}

bool isa_jit_available(void)
{
  return RV32_JIT_ENABLED && !jit_disabled && jit_code_init();
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
  if (len <= 0)
  {
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
      block->valid = false;
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

static rv32_jit_block_t *jit_cache_slot(vaddr_t pc)
{
  return &jit_cache[jit_hash(pc, cpu.csr.satp)];
}

static void jit_mark_unsupported(vaddr_t pc, paddr_t paddr, uint32_t source_len)
{
  rv32_jit_block_t *block = jit_cache_slot(pc);
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

  while (count < max_insns && count < RV32_JIT_BLOCK_MAX_INSNS)
  {
    paddr_t cur_paddr = 0;
    if (!jit_translate_ifetch(cur_pc, &cur_paddr) || !in_pmem(cur_paddr))
    {
      break;
    }

    if ((cur_paddr & ~(paddr_t)PAGE_MASK) !=
        (first_paddr & ~(paddr_t)PAGE_MASK))
    {
      break;
    }

    const uint32_t instr = vaddr_ifetch(cur_pc, 4);
    uint8_t *instr_start = w.cur;
    if (!emit_alu_instr(&w, instr) && !emit_load_store_instr(&w, instr))
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
    if (jit_instr_is_store(instr))
    {
      /*
       * A store can modify code bytes or page-table memory. Ending the native
       * block immediately after the store lets invalidation take effect before
       * the next guest fetch, preserving self-modifying-code behaviour.
       */
      break;
    }
  }

  if (count == 0)
  {
    jit_mark_unsupported(pc, first_paddr, 4);
    return NULL;
  }

  if (!emit_set_pc_imm(&w, cur_pc) || !emit_epilogue_return_count(&w, count))
  {
    return NULL;
  }

  __builtin___clear_cache((char *)w.start, (char *)w.cur);

  rv32_jit_block_t *block = jit_cache_slot(pc);
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
  if (!isa_jit_available() || remaining == 0 || device_budget == 0)
  {
    return false;
  }

  uint32_t max_insns = remaining > RV32_JIT_BLOCK_MAX_INSNS
      ? RV32_JIT_BLOCK_MAX_INSNS : (uint32_t)remaining;
  if (max_insns > device_budget)
  {
    max_insns = device_budget;
  }

  rv32_jit_block_t *block = jit_cache_slot(cpu.pc);
  if (!block->valid || block->pc != cpu.pc || block->satp != cpu.csr.satp ||
      block->insn_count > max_insns)
  {
    block = jit_compile_block(cpu.pc, max_insns);
  }

  if (block == NULL || !block->valid || block->entry == NULL)
  {
    return false;
  }

  const uint32_t ran = block->entry();
  Assert(ran > 0 && ran <= max_insns, "jit: invalid executed count %u", ran);
  *executed = ran;
  return true;
}
