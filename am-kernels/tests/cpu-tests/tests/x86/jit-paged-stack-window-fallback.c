#include "trap.h"

#define X86_CR0_PG 0x80000000u
#define X86_PTE_P 0x001u
#define X86_PTE_W 0x002u
#define ITERATIONS 4096u

static uint32_t page_dir[1024] __attribute__((aligned(4096)));
static uint32_t page_table[1024] __attribute__((aligned(4096)));
static volatile uint32_t call_count;
static volatile uint32_t checksum;

static uint32_t read_cr0(void) {
  uint32_t value;
  asm volatile("movl %%cr0, %0" : "=r"(value));
  return value;
}

static void write_cr0(uint32_t value) {
  asm volatile("movl %0, %%cr0" : : "r"(value) : "memory");
}

static uint32_t read_cr3(void) {
  uint32_t value;
  asm volatile("movl %%cr3, %0" : "=r"(value));
  return value;
}

static void write_cr3(uint32_t value) {
  asm volatile("movl %0, %%cr3" : : "r"(value) : "memory");
}

static void install_identity_4k_paging(void) {
  for (uint32_t i = 0; i < 1024u; i++) {
    page_dir[i] = 0;
    page_table[i] = (i << 12) | X86_PTE_P | X86_PTE_W;
  }

  page_dir[0] = (uint32_t)page_table | X86_PTE_P | X86_PTE_W;
}

static uint32_t rol32(uint32_t value, uint32_t count) {
  count &= 31u;
  return count == 0 ? value : (value << count) | (value >> (32u - count));
}

static uint32_t target_value(uint32_t index, uint32_t count) {
  return (index * 0x9e3779b1u) ^ rol32(count + 0x13579bdfu, index & 15u);
}

__attribute__((noinline)) static uint32_t stack_target(uint32_t index) {
  uint32_t count = call_count + 1u;
  uint32_t value = target_value(index, count);

  call_count = count;
  return value;
}

static uint32_t fold_call(uint32_t acc, uint32_t value) {
  return rol32(acc ^ value, 7u) + 0x6d2b79f5u;
}

__attribute__((noinline)) static uint32_t run_paged_call_loop(void) {
  uint32_t acc = 0x2468ace0u;

  /*
   * This loop is intentionally bigger than the trace hot threshold so paged
   * batch/chaining has a chance to exercise CALL_REL stack handling.
   */
  for (uint32_t i = 0; i < ITERATIONS; i++) {
    acc = fold_call(acc, stack_target(i));
  }

  checksum = acc;
  return acc;
}

static uint32_t expected_checksum(void) {
  uint32_t acc = 0x2468ace0u;

  for (uint32_t i = 0; i < ITERATIONS; i++) {
    acc = fold_call(acc, target_value(i, i + 1u));
  }

  return acc;
}

int main() {
  uint32_t old_cr0 = read_cr0();
  uint32_t old_cr3 = read_cr3();
  uint32_t expected = expected_checksum();

  install_identity_4k_paging();
  call_count = 0;
  checksum = 0;

  write_cr3((uint32_t)page_dir);
  write_cr0(old_cr0 | X86_CR0_PG);
  uint32_t got = run_paged_call_loop();
  write_cr0(old_cr0);
  write_cr3(old_cr3);

  check(call_count == ITERATIONS);
  check(got == expected);
  check(checksum == expected);
  return 0;
}
