#include "trap.h"
#include <stdint.h>

static uint32_t imul_loop(uint32_t seed) {
  uint32_t acc = seed;
  uint32_t mul = 0x45d9f3bu;

  for (int i = 0; i < 4096; i++) {
    asm volatile("imull %1, %0" : "+r"(acc) : "r"(mul) : "cc");
    mul = mul * 33u + (uint32_t)i;
    acc ^= mul >> ((i & 7) + 1);
  }

  return acc;
}

static uint32_t imul_loop_ref(uint32_t seed) {
  uint32_t acc = seed;
  uint32_t mul = 0x45d9f3bu;

  for (int i = 0; i < 4096; i++) {
    acc = (uint32_t)((int64_t)(int32_t)acc * (int64_t)(int32_t)mul);
    mul = mul * 33u + (uint32_t)i;
    acc ^= mul >> ((i & 7) + 1);
  }

  return acc;
}

int main() {
  check(imul_loop(0x12345678u) == imul_loop_ref(0x12345678u));
  check(imul_loop(0x87654321u) == imul_loop_ref(0x87654321u));
  return 0;
}
