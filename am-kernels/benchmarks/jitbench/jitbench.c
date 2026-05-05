#include <am.h>
#include <klib.h>
#include <klib-macros.h>
#include <stdint.h>

#define ALU_ITERS 2500000u
#define MEM_ITERS 1000000u
#define MEM_WORDS 1024u

static uint32_t mem_buf[MEM_WORDS];

static uint64_t uptime_us(void)
{
  return io_read(AM_TIMER_UPTIME).us;
}

static uint32_t rotl32(uint32_t value, unsigned shift)
{
  return (value << shift) | (value >> (32u - shift));
}

static void print_ms(const char *name, uint64_t usec)
{
  printf("%s: %d.%03d ms\n", name, (int)(usec / 1000u),
      (int)(usec % 1000u));
}

/*
 * This loop is intentionally made from JIT-friendly RV32I/RV32M operations:
 * ALU immediates, register ALU operations and a predictable branch. It gives a
 * compact hot loop that should spend most of its time in translated code.
 */
__attribute__((noinline))
static uint32_t alu_hot_loop(uint32_t count)
{
  uint32_t a = 0x12345678u;
  uint32_t b = 0x9e3779b9u;
  uint32_t c = 0x243f6a88u;

  for (uint32_t i = 0; i < count; i++)
  {
    a += b ^ (i * 0x045d9f3bu);
    b = rotl32(b + c + i, 7);
    c ^= a + rotl32(b, 11);
    if ((i & 15u) == 0)
    {
      a ^= c >> 3;
    }
    else
    {
      b += a | 1u;
    }
  }

  return a ^ b ^ c;
}

/*
 * The memory loop keeps accesses inside a small RAM window. It is useful for
 * checking that the JIT direct-PMEM helper path stays hot without depending on
 * MMIO, paging, syscalls or file-system work.
 */
__attribute__((noinline))
static uint32_t memory_hot_loop(uint32_t count)
{
  for (uint32_t i = 0; i < MEM_WORDS; i++)
  {
    mem_buf[i] = 0x9e3779b9u ^ (i * 0x01020304u);
  }

  uint32_t acc = 0xdeadbeefu;
  for (uint32_t i = 0; i < count; i++)
  {
    uint32_t idx = (i * 17u + (acc >> 24)) & (MEM_WORDS - 1u);
    uint32_t value = mem_buf[idx];
    value += rotl32(acc ^ i, 5);
    mem_buf[idx] = value;
    acc ^= value + idx * 0x045d9f3bu;
    acc = rotl32(acc, 3);
  }

  return acc ^ mem_buf[(acc >> 8) & (MEM_WORDS - 1u)];
}

int main(const char *args)
{
  (void)args;
  ioe_init();

  printf("======= Running JITBench =======\n");

  uint64_t start = uptime_us();
  uint32_t alu = alu_hot_loop(ALU_ITERS);
  uint64_t after_alu = uptime_us();
  uint32_t mem = memory_hot_loop(MEM_ITERS);
  uint64_t after_mem = uptime_us();

  uint32_t checksum = alu ^ rotl32(mem, 13) ^ 0xa5a55a5au;
  int pass = checksum == 0x5d10403fu;

  print_ms("ALU hot loop", after_alu - start);
  print_ms("Memory hot loop", after_mem - after_alu);
  printf("checksum: 0x%x\n", checksum);
  printf("JITBench %s\n", pass ? "PASS" : "FAIL");

  return pass ? 0 : 1;
}
