#include "trap.h"

static uint32_t read_cr3(void) {
  uint32_t value;
  asm volatile("movl %%cr3, %0" : "=r"(value));
  return value;
}

static void write_cr3(uint32_t value) {
  asm volatile("movl %0, %%cr3" : : "r"(value) : "memory");
}

int main() {
  uint32_t old_cr3 = read_cr3();
  uint32_t base = old_cr3 & ~0xfffu;
  uint32_t test_cr3 = base | 0x18u;

  write_cr3(test_cr3);
  uint32_t got = read_cr3();
  write_cr3(old_cr3);

  // In 32-bit paging, CR3 keeps PWT and PCD in bits 3 and 4.
  check((got & ~0xfffu) == base);
  check((got & 0x18u) == 0x18u);
  check((got & 0xfe7u) == 0);

  return 0;
}
