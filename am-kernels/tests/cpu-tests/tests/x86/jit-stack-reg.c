#include "trap.h"

static uint32_t pop_esp_stack[4] __attribute__((aligned(16)));

static uint32_t push_pop_roundtrip(uint32_t *esp_delta) {
  uint32_t before = 0;
  uint32_t after = 0;
  uint32_t value = 0;

  asm volatile(
      "movl %%esp, %[before]\n\t"
      "movl $0x13579bdf, %%eax\n\t"
      "pushl %%eax\n\t"
      "xorl %%eax, %%eax\n\t"
      "popl %%eax\n\t"
      "movl %%esp, %[after]"
      : [before] "=&r"(before),
        [after] "=&r"(after),
        "=a"(value)
      :
      : "memory", "cc");

  *esp_delta = before - after;
  return value;
}

static uint32_t push_esp_delta(void) {
  uint32_t delta = 0;

  asm volatile(
      "movl %%esp, %%eax\n\t"
      "pushl %%esp\n\t"
      "popl %%ecx\n\t"
      "subl %%ecx, %%eax"
      : "=a"(delta)
      :
      : "ecx", "memory", "cc");

  return delta;
}

static uint32_t pop_esp_value(void) {
  uintptr_t slot = (uintptr_t)&pop_esp_stack[1];
  uint32_t value = 0;

  pop_esp_stack[0] = 0xaaaaaaaau;
  pop_esp_stack[1] = 0x2468ace0u;
  pop_esp_stack[2] = 0xbbbbbbbbu;
  pop_esp_stack[3] = 0xccccccccu;

  asm volatile(
      "movl %%esp, %%edx\n\t"
      "movl %[slot], %%esp\n\t"
      "popl %%esp\n\t"
      "movl %%esp, %[value]\n\t"
      "movl %%edx, %%esp"
      : [value] "=&r"(value)
      : [slot] "r"(slot)
      : "edx", "memory", "cc");

  return value;
}

int main() {
  uint32_t esp_delta = 1;

  check(push_pop_roundtrip(&esp_delta) == 0x13579bdfu);
  check(esp_delta == 0);
  check(push_esp_delta() == 0);
  check(pop_esp_value() == 0x2468ace0u);
  check(pop_esp_stack[1] == 0x2468ace0u);
  return 0;
}
