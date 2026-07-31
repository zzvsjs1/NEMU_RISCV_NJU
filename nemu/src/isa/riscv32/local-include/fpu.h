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
 * Describe the complete integer-register footprint of one non-memory FP
 * instruction executed by the shared helper. A precise descriptor permits a
 * JIT to publish only `read_mask` before the call, retain unrelated dirty
 * mappings on success, and discard `success_write_mask` after helper
 * writeback. Unknown instructions remain a full synchronisation barrier.
 */
typedef struct
{
    uint32_t read_mask;
    uint32_t success_write_mask;
    bool precise;
    bool trap_preserves_gprs;
} riscv_fpu_gpr_effect_t;

/* Return a strict helper footprint, or an imprecise zero descriptor. */
riscv_fpu_gpr_effect_t riscv_fpu_gpr_effect(uint32_t instr);

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
