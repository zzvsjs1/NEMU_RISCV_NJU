#include "trap.h"
#include <stdint.h>

static const uint16_t words[16] __attribute__((aligned(4096))) = {
  0x0000u, 0x0001u, 0x007fu, 0x0080u,
  0x00ffu, 0x0100u, 0x7fffu, 0x8000u,
  0xffffu, 0x1234u, 0xabcdu, 0x55aau,
  0xaa55u, 0x0f0fu, 0xf0f0u, 0x8001u,
};

static const uint8_t bytes[16] __attribute__((aligned(4096))) = {
  0x00u, 0x01u, 0x7fu, 0x80u,
  0xffu, 0x12u, 0x34u, 0x56u,
  0x78u, 0x9au, 0xbcu, 0xdeu,
  0xf0u, 0x0fu, 0xa5u, 0x5au,
};

static uint32_t movzx_sum(void) {
  uint32_t sum = 0;

  for (int round = 0; round < 512; round++) {
    for (int i = 0; i < 16; i++) {
      uint32_t word_value;
      uint32_t byte_value;
      asm volatile("movzwl %1, %0" : "=r"(word_value) : "m"(words[i]));
      asm volatile("movzbl %1, %0" : "=r"(byte_value) : "m"(bytes[i]));
      sum += word_value ^ (byte_value + (uint32_t)round);
    }
  }

  return sum;
}

static uint32_t movzx_ref(void) {
  uint32_t sum = 0;

  for (int round = 0; round < 512; round++) {
    for (int i = 0; i < 16; i++) {
      sum += (uint32_t)words[i] ^ ((uint32_t)bytes[i] + (uint32_t)round);
    }
  }

  return sum;
}

static uint32_t movzx_high_byte_sum(void) {
  uint32_t ah = 0;
  uint32_t ch = 0;
  uint32_t dh = 0;
  uint32_t bh = 0;

  asm volatile(
      "movl $0x11223344, %%eax\n\t"
      "movzbl %%ah, %%esi\n\t"
      "movl %%esi, %[ah]\n\t"
      "movl $0x55667788, %%ecx\n\t"
      "movzbl %%ch, %%esi\n\t"
      "movl %%esi, %[ch]\n\t"
      "movl $0x99aabbcc, %%edx\n\t"
      "movzbl %%dh, %%esi\n\t"
      "movl %%esi, %[dh]\n\t"
      "movl $0xddeeff00, %%ebx\n\t"
      "movzbl %%bh, %%esi\n\t"
      "movl %%esi, %[bh]\n\t"
      : [ah] "=m"(ah),
        [ch] "=m"(ch),
        [dh] "=m"(dh),
        [bh] "=m"(bh)
      :
      : "eax", "ebx", "ecx", "edx", "esi", "memory");

  return ah + (ch << 8) + (dh << 16) + (bh << 24);
}

int main(void) {
  check(movzx_sum() == movzx_ref());
  check(movzx_high_byte_sum() == 0xffbb7733u);
  return 0;
}
