#include "trap.h"

static uint32_t load_sum(const uint32_t *data, int n) {
  uint32_t sum = 0;

  for (int i = 0; i < n; i++) {
    uint32_t value;
    asm volatile("movl %1, %0" : "=r"(value) : "m"(data[i]));
    sum += value;
  }

  return sum;
}

int main() {
  static const uint32_t data[] = {
    0x00000001u, 0x12345678u, 0x80000000u, 0x7fffffffu,
    0xa5a5a5a5u, 0x01020304u, 0xffffffffu, 0x13579bdfu,
  };

  uint32_t expected = 0;
  for (int i = 0; i < 8; i++) {
    expected += data[i];
  }

  check(load_sum(data, 8) == expected);
  return 0;
}
