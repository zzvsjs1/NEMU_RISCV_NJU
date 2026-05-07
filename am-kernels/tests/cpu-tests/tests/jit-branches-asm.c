#include "trap.h"
#include <stdint.h>

static uint32_t branch_score(int32_t lhs, int32_t rhs)
{
  uint32_t score = 0;
  asm volatile(
    "beq  %[lhs], %[rhs], 1f\n"
    "addi %[score], %[score], 1\n"
    "1:\n"
    "bne  %[lhs], %[rhs], 2f\n"
    "addi %[score], %[score], 2\n"
    "2:\n"
    "blt  %[lhs], %[rhs], 3f\n"
    "addi %[score], %[score], 4\n"
    "3:\n"
    "bge  %[lhs], %[rhs], 4f\n"
    "addi %[score], %[score], 8\n"
    "4:\n"
    "bltu %[lhs], %[rhs], 5f\n"
    "addi %[score], %[score], 16\n"
    "5:\n"
    "bgeu %[lhs], %[rhs], 6f\n"
    "addi %[score], %[score], 32\n"
    "6:\n"
    : [score] "+&r"(score)
    : [lhs] "r"(lhs), [rhs] "r"(rhs)
    : "memory"
  );
  return score;
}

static uint32_t jal_score(void)
{
  uint32_t score = 0;
  uintptr_t link = 0;
  uintptr_t skipped = 0;
  asm volatile(
    "jal %[link], 1f\n"
    "2:\n"
    "addi %[score], %[score], 1\n"
    "1:\n"
    "la %[skipped], 2b\n"
    "addi %[score], %[score], 2\n"
    : [score] "+r"(score), [link] "=&r"(link), [skipped] "=&r"(skipped)
    :
    : "memory"
  );
  return score | (link == skipped ? 0u : 4u);
}

static uint32_t jalr_clears_bit_zero(void)
{
  uint32_t score = 0;
  asm volatile(
    "la t0, 1f\n"
    "ori t0, t0, 1\n"
    "jalr zero, 0(t0)\n"
    "addi %[score], %[score], 1\n"
    "1:\n"
    "addi %[score], %[score], 2\n"
    : [score] "+r"(score)
    :
    : "t0", "memory"
  );
  return score;
}

int main(void)
{
  check(branch_score(5, 5) == (2u | 4u | 16u));
  check(branch_score(-1, 1) == (1u | 8u | 16u));
  check(branch_score(1, -1) == (1u | 4u | 32u));
  check(branch_score((int32_t)0x80000000u, 0x7fffffff) == (1u | 8u | 16u));
  check(branch_score(0x7fffffff, (int32_t)0x80000000u) == (1u | 4u | 32u));

  check(jal_score() == 2u);
  check(jalr_clears_bit_zero() == 2u);

  return 0;
}
