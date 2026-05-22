#include "trap.h"

static uint32_t shift_mem[3] __attribute__((aligned(4096)));

int main() {
  uint32_t shl_cl = 0;
  uint32_t shr_cl = 0;
  uint32_t sar_cl = 0;
  uint8_t shl_of = 0;
  uint8_t sar_sf = 0;

  shift_mem[0] = 0x40000000u;
  shift_mem[1] = 0x80000001u;
  shift_mem[2] = 0x80000000u;

  asm volatile(
      "movl $0x40000000, %%eax\n\t"
      "movl $1, %%ecx\n\t"
      "shll %%cl, %%eax\n\t"
      "seto %[shl_of]\n\t"
      "movl %%eax, %[shl_cl]\n\t"
      "movl $0x80000000, %%eax\n\t"
      "movl $4, %%ecx\n\t"
      "shrl %%cl, %%eax\n\t"
      "movl %%eax, %[shr_cl]\n\t"
      "movl $0x80000000, %%eax\n\t"
      "movl $4, %%ecx\n\t"
      "sarl %%cl, %%eax\n\t"
      "sets %[sar_sf]\n\t"
      "movl %%eax, %[sar_cl]\n\t"
      "shll $1, %[mem_shl]\n\t"
      "movl $4, %%ecx\n\t"
      "shrl %%cl, %[mem_shr]\n\t"
      "sarl $4, %[mem_sar]"
      : [shl_cl] "=m"(shl_cl),
        [shr_cl] "=m"(shr_cl),
        [sar_cl] "=m"(sar_cl),
        [shl_of] "=qm"(shl_of),
        [sar_sf] "=qm"(sar_sf),
        [mem_shl] "+m"(shift_mem[0]),
        [mem_shr] "+m"(shift_mem[1]),
        [mem_sar] "+m"(shift_mem[2])
      :
      : "eax", "ecx", "memory", "cc");

  check(shl_cl == 0x80000000u);
  check(shl_of == 1);
  check(shr_cl == 0x08000000u);
  check(sar_cl == 0xf8000000u);
  check(sar_sf == 1);
  check(shift_mem[0] == 0x80000000u);
  check(shift_mem[1] == 0x08000000u);
  check(shift_mem[2] == 0xf8000000u);
  return 0;
}
