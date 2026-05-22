#include "trap.h"

static uint32_t values[6] __attribute__((aligned(4096)));
static uint16_t word_values[2] __attribute__((aligned(4096)));
static uint8_t byte_values[2] __attribute__((aligned(4096)));

static void run_mem_alu(uint8_t *cf, uint8_t *zf, uint8_t *sf,
    uint8_t *byte_sf, uint8_t *word_cf, uint8_t *byte_zf,
    uint8_t *word_sf) {
  uint8_t carry = 0;
  uint8_t zero = 0;
  uint8_t sign = 0;
  uint8_t byte_sign = 0;
  uint8_t word_carry = 0;
  uint8_t byte_zero = 0;
  uint8_t word_sign = 0;

  values[0] = 5u;
  values[1] = 3u;
  values[2] = 0x7fffffffu;
  values[3] = 0xaaaaaaaa;
  values[4] = 0x12345678u;
  values[5] = 0xf0f0f0f0u;
  word_values[0] = 0x00ffu;
  word_values[1] = 0x8000u;
  byte_values[0] = 0x7fu;
  byte_values[1] = 0xf0u;

  asm volatile(
      "movl $7, %%eax\n\t"
      "addl %%eax, %[add_mem]\n\t"
      "subl $5, %[sub_mem]\n\t"
      "setb %[cf]\n\t"
      "addl $1, %[of_mem]\n\t"
      "xorl $0xffffffff, %[xor_mem]\n\t"
      "movl %[cmp_mem], %%eax\n\t"
      "cmpl %%eax, %[cmp_mem]\n\t"
      "setz %[zf]\n\t"
      "movl $0x0f0f0000, %%eax\n\t"
      "testl %%eax, %[test_mem]\n\t"
      "sets %[sf]\n\t"
      "addb $1, %[byte_add]\n\t"
      "sets %[byte_sf]\n\t"
      "subw $0x0100, %[word_sub]\n\t"
      "setb %[word_cf]\n\t"
      "movb $0x0f, %%al\n\t"
      "testb %%al, %[byte_test]\n\t"
      "setz %[byte_zf]\n\t"
      "movw $0x8000, %%ax\n\t"
      "testw %%ax, %[word_test]\n\t"
      "sets %[word_sf]"
      : [add_mem] "+m"(values[0]),
        [sub_mem] "+m"(values[1]),
        [of_mem] "+m"(values[2]),
        [xor_mem] "+m"(values[3]),
        [byte_add] "+m"(byte_values[0]),
        [word_sub] "+m"(word_values[0]),
        [cf] "=qm"(carry),
        [zf] "=qm"(zero),
        [sf] "=qm"(sign),
        [byte_sf] "=qm"(byte_sign),
        [word_cf] "=qm"(word_carry),
        [byte_zf] "=qm"(byte_zero),
        [word_sf] "=qm"(word_sign)
      : [cmp_mem] "m"(values[4]),
        [test_mem] "m"(values[5]),
        [byte_test] "m"(byte_values[1]),
        [word_test] "m"(word_values[1])
      : "eax", "memory", "cc");

  *cf = carry;
  *zf = zero;
  *sf = sign;
  *byte_sf = byte_sign;
  *word_cf = word_carry;
  *byte_zf = byte_zero;
  *word_sf = word_sign;
}

int main() {
  uint8_t cf = 0;
  uint8_t zf = 0;
  uint8_t sf = 0;
  uint8_t byte_sf = 0;
  uint8_t word_cf = 0;
  uint8_t byte_zf = 0;
  uint8_t word_sf = 0;

  run_mem_alu(&cf, &zf, &sf, &byte_sf, &word_cf, &byte_zf, &word_sf);

  check(values[0] == 12u);
  check(values[1] == 0xfffffffeu);
  check(cf == 1);
  check(values[2] == 0x80000000u);
  check(values[3] == 0x55555555u);
  check(zf == 1);
  check(sf == 0);
  check(byte_values[0] == 0x80u);
  check(byte_sf == 1);
  check(word_values[0] == 0xffffu);
  check(word_cf == 1);
  check(byte_zf == 1);
  check(word_sf == 1);
  return 0;
}
