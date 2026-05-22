#include "trap.h"

static uint32_t xchg_eax_edx(uint32_t *out_edx) {
  uint32_t eax = 0x11223344u;
  uint32_t edx = 0xaabbccddu;

  /*
   * Opcode 90+rd is a compact XCHG form whose rd operand is exchanged with
   * EAX.  AM and compiler generated atomic code can rely on this being a real
   * exchange, not only the 0x90 NOP special case.
   */
  asm volatile(".byte 0x92" : "+a"(eax), "+d"(edx) : : "memory");
  *out_edx = edx;
  return eax;
}

static uint32_t xchg_reg_mem(uint32_t *mem) {
  uint32_t reg = 0x0badf00du;
  asm volatile("xchgl %0, %1" : "+m"(*mem), "+r"(reg) : : "memory");
  return reg;
}

static uint32_t bsf32_reg(uint32_t value) {
  uint32_t result;
  asm volatile("bsfl %1, %0" : "=r"(result) : "r"(value) : "cc");
  return result;
}

static uint16_t bsf16_reg(uint16_t value) {
  uint16_t result;
  asm volatile("bsfw %1, %0" : "=r"(result) : "r"(value) : "cc");
  return result;
}

static uint8_t bsf32_zero_sets_zf(uint32_t value) {
  uint32_t result = 0xdeadbeefu;
  uint8_t zf;
  asm volatile("bsfl %2, %0; setz %1"
               : "+r"(result), "=qm"(zf)
               : "r"(value)
               : "cc");
  return zf;
}

static uint32_t cwde_from_ax(void) {
  uint32_t eax = 0x12348001u;
  asm volatile(".byte 0x98" : "+a"(eax));
  return eax;
}

static uint32_t cbw_from_al(void) {
  uint32_t eax = 0x12340080u;
  asm volatile(".byte 0x66, 0x98" : "+a"(eax));
  return eax;
}

static uint32_t cwd_from_ax(uint32_t *out_eax) {
  uint32_t eax = 0x12348001u;
  uint32_t edx = 0x56789abcu;
  asm volatile(".byte 0x66, 0x99" : "+a"(eax), "+d"(edx));
  *out_eax = eax;
  return edx;
}

int main() {
  uint32_t edx_after;
  uint32_t eax_after = xchg_eax_edx(&edx_after);
  check(eax_after == 0xaabbccddu);
  check(edx_after == 0x11223344u);

  uint32_t mem = 0x01020304u;
  uint32_t reg_after = xchg_reg_mem(&mem);
  check(reg_after == 0x01020304u);
  check(mem == 0x0badf00du);

  check(bsf32_reg(0x80000000u) == 31);
  check(bsf32_reg(0x00008000u) == 15);
  check(bsf32_reg(0x00000010u) == 4);
  check(bsf16_reg(0x0010u) == 4);
  check(bsf32_zero_sets_zf(0) == 1);
  check(bsf32_zero_sets_zf(0x100u) == 0);

  check(cwde_from_ax() == 0xffff8001u);
  check(cbw_from_al() == 0x1234ff80u);

  uint32_t eax_after_cwd;
  uint32_t edx_after_cwd = cwd_from_ax(&eax_after_cwd);
  check(eax_after_cwd == 0x12348001u);
  check(edx_after_cwd == 0x5678ffffu);

  return 0;
}
