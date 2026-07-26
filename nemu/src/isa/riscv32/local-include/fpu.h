#ifndef __RISCV_FPU_H__
#define __RISCV_FPU_H__

#include <cpu/decode.h>

#ifdef CONFIG_RV64_FPU
/*
 * Execute one instruction from an RV64 floating-point major opcode.  The
 * caller has already matched the major opcode; this function owns all finer
 * encoding validation, architectural permission checks, and writeback order.
 */
void riscv64_fpu_exec(Decode *s);
#endif

#endif
