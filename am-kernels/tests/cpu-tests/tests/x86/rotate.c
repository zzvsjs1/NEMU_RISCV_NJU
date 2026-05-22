#include "trap.h"

static uint32_t rol32_imm(uint32_t value) {
  asm volatile("roll $7, %0" : "+r"(value) :: "cc");
  return value;
}

static uint32_t ror32_imm(uint32_t value) {
  asm volatile("rorl $4, %0" : "+r"(value) :: "cc");
  return value;
}

static uint16_t rol16_cl(uint16_t value, uint8_t count) {
  asm volatile("rolw %%cl, %0" : "+r"(value) : "c"(count) : "cc");
  return value;
}

static uint16_t ror16_cl(uint16_t value, uint8_t count) {
  asm volatile("rorw %%cl, %0" : "+r"(value) : "c"(count) : "cc");
  return value;
}

static uint8_t rol_keeps_zf(uint32_t value) {
  uint8_t zf;
  asm volatile("xorl %%eax, %%eax; roll $7, %1; setz %0"
               : "=qm"(zf), "+r"(value)
               :
               : "eax", "cc");
  return zf;
}

int main() {
  check(rol32_imm(0x12345678) == 0x1a2b3c09);
  check(ror32_imm(0x12345678) == 0x81234567);
  check(rol16_cl(0x1234, 4) == 0x2341);
  check(ror16_cl(0x1234, 4) == 0x4123);
  check(rol_keeps_zf(0x12345678) == 1);

  return 0;
}
