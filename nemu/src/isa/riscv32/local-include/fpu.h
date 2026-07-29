#ifndef __RISCV_FPU_H__
#define __RISCV_FPU_H__

#include <cpu/decode.h>

#ifdef CONFIG_RISCV_FPU
/*
 * Execute one instruction from a configured RISC-V floating-point major
 * opcode. The caller has already matched the major opcode; this function owns
 * all finer encoding validation, architectural permission checks, and
 * writeback order.
 */
void riscv_fpu_exec(Decode *s);
#endif

#endif
