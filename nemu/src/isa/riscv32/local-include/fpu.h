#ifndef __RISCV_FPU_H__
#define __RISCV_FPU_H__

#include <cpu/decode.h>

#ifdef CONFIG_RISCV_FPU
/*
 * The direct interpreter does not need to inspect this result because its
 * Decode object already carries the redirected dnpc.  Generated code does:
 * it must distinguish a normal FP instruction from a trap even when mtvec
 * happens to equal the sequential PC.
 */
typedef enum
{
    RISCV_FPU_EXEC_TRAP = 0,
    RISCV_FPU_EXEC_OK = 1,
} riscv_fpu_exec_result_t;

/*
 * Execute one instruction from a configured RISC-V floating-point major
 * opcode. The caller has already matched the major opcode; this function owns
 * all finer encoding validation, architectural permission checks, and
 * writeback order.
 */
riscv_fpu_exec_result_t riscv_fpu_exec(Decode *s);

/*
 * Execute one already-fetched FP instruction on behalf of generated code.
 * The helper publishes the resulting PC, repairs architectural x0, and returns
 * one for normal completion or zero after an architectural trap.
 */
uint32_t riscv_fpu_jit_exec(uint32_t instr, vaddr_t pc);
#endif

#endif
