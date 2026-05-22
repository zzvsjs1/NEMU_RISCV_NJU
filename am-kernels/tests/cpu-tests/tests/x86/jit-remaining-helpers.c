#include "trap.h"

static uint8_t mem8[8] __attribute__((aligned(4096)));
static uint16_t mem16[8] __attribute__((aligned(4096)));
static uint32_t mem32[8] __attribute__((aligned(4096)));

int main() {
  uint32_t eax_after = 0;
  uint32_t ebx_after = 0;
  uint32_t ecx_after = 0;
  uint32_t edx_after = 0;
  uint8_t zf = 0;
  uint8_t sf = 0;
  uint8_t cf = 0;

  mem8[0] = 0x11u;
  mem8[1] = 0x22u;
  mem8[2] = 0x03u;
  mem8[3] = 0x7fu;
  mem8[5] = 0x10u;
  mem16[0] = 0x1234u;
  mem16[1] = 0x00f0u;
  mem16[2] = 0x0003u;
  mem16[3] = 0x8000u;
  mem16[5] = 0x0010u;
  mem32[0] = 0x12345678u;
  mem32[1] = 0x01020304u;
  mem32[2] = 0x00000003u;
  mem32[3] = 0x80000000u;
  mem32[5] = 0x00000010u;

  asm volatile(
      "movb $0x5a, %[imm8]\n\t"
      "movw $0x2468, %[imm16]\n\t"
      "movl $0x13572468, %[imm32]\n\t"
      "movl $0xaaaa0000, %%eax\n\t"
      "movl $0xbbbb0000, %%ebx\n\t"
      "movl $0xcccc0000, %%ecx\n\t"
      "movl $0xdddd0000, %%edx\n\t"
      "movb %[load8], %%al\n\t"
      "movw %[load16], %%cx\n\t"
      "movl %[load32], %%edx\n\t"
      "movb %%al, %%bl\n\t"
      "movw %%cx, %%dx\n\t"
      "movb %%bl, %[store8]\n\t"
      "movw %%dx, %[store16]\n\t"
      "movl %%edx, %[store32]"
      : [imm8] "+m"(mem8[0]),
        [imm16] "+m"(mem16[0]),
        [imm32] "+m"(mem32[0]),
        [store8] "=m"(mem8[4]),
        [store16] "=m"(mem16[4]),
        [store32] "=m"(mem32[4])
      : [load8] "m"(mem8[0]),
        [load16] "m"(mem16[0]),
        [load32] "m"(mem32[0])
      : "eax", "ebx", "ecx", "edx", "memory", "cc");

  asm volatile(
      "movl $0xaaaa005a, %%eax\n\t"
      "movl $0xcccc2468, %%ecx\n\t"
      "movl $0x13572468, %%edx\n\t"
      "addb %[alu8], %%al\n\t"
      "subw %[alu16], %%cx\n\t"
      "xorl %[alu32], %%edx\n\t"
      "cmpb %[cmp8], %%al\n\t"
      "setz %[zf]\n\t"
      "movl %%eax, %[eax_after]\n\t"
      "movl %%ecx, %[ecx_after]\n\t"
      "movl %%edx, %[edx_after]"
      : [zf] "=qm"(zf),
        [eax_after] "=m"(eax_after),
        [ecx_after] "=m"(ecx_after),
        [edx_after] "=m"(edx_after)
      : [alu8] "m"(mem8[2]),
        [alu16] "m"(mem16[2]),
        [alu32] "m"(mem32[2]),
        [cmp8] "m"(mem8[0])
      : "eax", "ecx", "edx", "memory", "cc");

  asm volatile(
      "movl $0xbbbb005a, %%ebx\n\t"
      "movl $0xaaaa005d, %%eax\n\t"
      "negb %[neg8]\n\t"
      "sets %[sf]\n\t"
      "negw %[neg16]\n\t"
      "negl %[neg32]\n\t"
      "incb %[inc8]\n\t"
      "decw %[dec16]\n\t"
      "incl %[inc32]\n\t"
      "testb $0x80, %%al\n\t"
      "setc %[cf]\n\t"
      "testw $0x8000, %%ax\n\t"
      "testl $0x80000000, %%eax\n\t"
      "addb %%al, %[alu_rm8]\n\t"
      "subw %%ax, %[alu_rm16]\n\t"
      "xorl %%eax, %[alu_rm32]\n\t"
      "movl %%ebx, %[ebx_after]"
      : [neg8] "+m"(mem8[3]),
        [neg16] "+m"(mem16[3]),
        [neg32] "+m"(mem32[3]),
        [inc8] "+m"(mem8[1]),
        [dec16] "+m"(mem16[1]),
        [inc32] "+m"(mem32[1]),
        [alu_rm8] "+m"(mem8[5]),
        [alu_rm16] "+m"(mem16[5]),
        [alu_rm32] "+m"(mem32[5]),
        [sf] "=qm"(sf),
        [cf] "=qm"(cf),
        [ebx_after] "=m"(ebx_after)
      :
      : "eax", "ebx", "memory", "cc");

  check(mem8[0] == 0x5au);
  check(mem16[0] == 0x2468u);
  check(mem32[0] == 0x13572468u);
  check(mem8[4] == 0x5au);
  check(mem16[4] == 0x2468u);
  check(mem32[4] == 0x13572468u);
  check((eax_after & 0xffu) == 0x5du);
  check((ebx_after & 0xffu) == 0x5au);
  check((ecx_after & 0xffffu) == 0x2465u);
  check(edx_after == (0x13572468u ^ 0x00000003u));
  check(zf == 0);
  check(mem8[3] == 0x81u);
  check(sf == 1);
  check(mem16[3] == 0x8000u);
  check(mem32[3] == 0x80000000u);
  check(mem8[1] == 0x23u);
  check(mem16[1] == 0x00efu);
  check(mem32[1] == 0x01020305u);
  check(mem8[5] == 0x6du);
  check(mem16[5] == 0xffb3u);
  check(mem32[5] == 0xaaaa004du);
  check(cf == 0);
  return 0;
}
