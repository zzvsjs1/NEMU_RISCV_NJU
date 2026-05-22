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

static uint32_t inc_cmp_jne_loop(uint32_t limit, uint8_t *zf) {
  uint32_t out = 0;
  uint8_t zero = 0;

  asm volatile(
      "1:\n\t"
      "incl %[out]\n\t"
      "cmpl %[limit], %[out]\n\t"
      "jne 1b\n\t"
      "setz %[zf]"
      : [out] "+r"(out),
        [zf] "=qm"(zero)
      : [limit] "r"(limit)
      : "cc");

  *zf = zero;
  return out;
}

__attribute__((naked, noinline, used)) static void inc_cmp_same_reg_entry(void) {
  asm volatile(
      "1:\n\t"
      "incl %%eax\n\t"
      "cmpl %%eax, %%eax\n\t"
      "jne 1b\n\t"
      "ret"
      :
      :
      : "cc");
}

static uint32_t inc_cmp_same_reg_jne_once(uint32_t start, uint8_t *zf) {
  uint32_t out = start;
  uint8_t zero = 0;

  asm volatile(
      "movl %[start], %%eax\n\t"
      "call inc_cmp_same_reg_entry\n\t"
      "setz %[zf]"
      : "=a"(out),
        [zf] "=qm"(zero)
      : [start] "rm"(start)
      : "memory", "cc");

  *zf = zero;
  return out;
}

int main() {
  uint8_t cf = 0;

  check(dec_jnz_loop(10000, &cf) == 0);
  check(cf == 1);

  check(dec_jnz_loop(100000, &cf) == 0);
  check(cf == 1);

  uint32_t signed_count = 0xffff0000u;
  asm volatile(
      "stc\n\t"
      "1:\n\t"
      "incl %[signed_count]\n\t"
      "jl 1b\n\t"
      "setc %[cf]"
      : [signed_count] "+r"(signed_count),
        [cf] "=qm"(cf)
      :
      : "cc");

  check(signed_count == 0);
  check(cf == 1);

  uint8_t zf = 0;
  check(inc_cmp_jne_loop(100000, &zf) == 100000);
  check(zf == 1);

  check(inc_cmp_same_reg_jne_once(41, &zf) == 42);
  check(zf == 1);

  return 0;
}
