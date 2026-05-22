#include "trap.h"

static uint32_t dec_jnz_loop(uint32_t count, uint8_t *cf) {
  uint32_t out = count;
  uint8_t carry = 0;

  asm volatile(
      "stc\n\t"
      "1:\n\t"
      "decl %[out]\n\t"
      "jnz 1b\n\t"
      "setc %[cf]"
      : [out] "+r"(out),
        [cf] "=qm"(carry)
      :
      : "cc");

  *cf = carry;
  return out;
}

int main() {
  uint8_t cf = 0;

  check(dec_jnz_loop(10000, &cf) == 0);
  check(cf == 1);

  return 0;
}
