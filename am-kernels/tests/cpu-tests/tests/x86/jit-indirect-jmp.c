#include "trap.h"

#define INDIRECT_JMP_ITERS 5000u

static uint32_t indirect_target_slot;

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

static uint32_t indirect_jmp_mem_target(void) {
  uint32_t result = 0;

  /*
   * Use a memory r/m32 target as well as the hot register-target loop above.
   * This covers the guarded direct-PMEM path for FF /4 without changing the
   * loop's count-sensitive helper profile.
   */
  asm volatile(
      "movl $1f, %[slot]\n"
      "movl %[slot_addr], %%edx\n"
      "jmp *(%%edx)\n"
      "movl $0xbad0bad0, %%eax\n"
      "1:\n"
      "movl $0x13572468, %%eax\n"
      : "=&a"(result), [slot] "=m"(indirect_target_slot)
      : [slot_addr] "r"(&indirect_target_slot)
      : "edx", "memory");

  return result;
}

int main() {
  check(indirect_jmp_loop(INDIRECT_JMP_ITERS) ==
      expected_result(INDIRECT_JMP_ITERS));
  check(indirect_jmp_mem_target() == 0x13572468u);
  return 0;
}
