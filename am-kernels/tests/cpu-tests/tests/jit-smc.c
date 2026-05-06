#include "trap.h"
#include <stdint.h>

typedef int (*generated_fn_t)(void);

/*
 * Tiny generated RISC-V code:
 *   addi a0, zero, imm
 *   ret
 *
 * The buffer deliberately lives in normal writable memory.  NEMU allows
 * instruction fetches from PMEM in this bare-metal test environment, so this
 * gives a compact self-modifying-code check for the JIT invalidation path.
 */
static uint32_t generated_code[2] __attribute__((aligned(16))) = {
  0x00100513u,  /* addi a0, zero, 1 */
  0x00008067u,  /* jalr zero, 0(ra), the standard RISC-V ret pseudo-op */
};

static uint32_t addi_a0_zero_imm(uint32_t imm)
{
  /*
   * ADDI's 12-bit immediate occupies bits [31:20].  rd is a0/x10, rs1 is
   * zero/x0, funct3 is 0, and opcode 0x13 selects OP-IMM.
   */
  return ((imm & 0xfffu) << 20) | (10u << 7) | 0x13u;
}

int main()
{
  generated_fn_t fn = (generated_fn_t)(uintptr_t)generated_code;

  const int first = fn();
  check(first == 1);

  /*
   * After the first call, a JIT implementation may have cached native code for
   * generated_code.  Patching the first guest instruction must invalidate that
   * cached block; otherwise the second call would keep returning the stale
   * value 1.
   */
  generated_code[0] = addi_a0_zero_imm(2);

  const int second = fn();
  check(second == 2);

  return 0;
}
