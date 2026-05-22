#include "trap.h"

#define INDIRECT_JMP_ITERS 5000u

static uint32_t expected_result(uint32_t iters) {
  uint32_t acc = 0;

  while (iters-- > 0) {
    acc += 3u;
    acc ^= 0x5au;
  }

  return acc;
}

static uint32_t indirect_jmp_loop(uint32_t iters) {
  uint32_t result;

  /*
   * Exercise near absolute JMP r/m32 (FF /4).  The labels make the target
   * values normal IA-32 code addresses, and every loop lap must pass through
   * an indirect jump before continuing.
   */
  asm volatile(
      "movl %[iters], %%ecx\n"
      "xorl %%eax, %%eax\n"
      "movl $1f, %%edx\n"
      "jmp *%%edx\n"
      "1:\n"
      "addl $3, %%eax\n"
      "movl $2f, %%edx\n"
      "jmp *%%edx\n"
      "2:\n"
      "xorl $0x5a, %%eax\n"
      "decl %%ecx\n"
      "movl $1b, %%edx\n"
      "jnz 3f\n"
      "movl $4f, %%edx\n"
      "3:\n"
      "jmp *%%edx\n"
      "4:\n"
      : "=&a"(result)
      : [iters] "r"(iters)
      : "ecx", "edx", "cc", "memory");

  return result;
}

int main() {
  check(indirect_jmp_loop(INDIRECT_JMP_ITERS) ==
      expected_result(INDIRECT_JMP_ITERS));
  return 0;
}
