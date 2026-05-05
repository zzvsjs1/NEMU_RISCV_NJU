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

typedef uint32_t (*rv32_jit_entry_t)(void);

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

bool isa_jit_exec(uint64_t remaining, uint32_t device_budget, uint32_t *executed)
{
  (void)remaining;
  (void)device_budget;
  *executed = 0;
  return false;
}
