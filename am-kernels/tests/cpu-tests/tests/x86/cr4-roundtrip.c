#include "trap.h"

#define X86_CR4_PSE 0x00000010u

static uint32_t read_cr4(void) {
  uint32_t value;
  asm volatile("movl %%cr4, %0" : "=r"(value));
  return value;
}

static void write_cr4(uint32_t value) {
  asm volatile("movl %0, %%cr4" : : "r"(value) : "memory");
}

int main() {
  uint32_t old_cr4 = read_cr4();

  /*
   * NEMU only needs the CR4.PSE bit for 32-bit paging, but MOV CR4 still has to
   * preserve that architectural state so later page-walk code can decide whether
   * PDE.PS is legal.
   */
  write_cr4(old_cr4 | X86_CR4_PSE);
  check((read_cr4() & X86_CR4_PSE) != 0);

  write_cr4(old_cr4 & ~X86_CR4_PSE);
  check((read_cr4() & X86_CR4_PSE) == 0);

  write_cr4(old_cr4);
  return 0;
}
