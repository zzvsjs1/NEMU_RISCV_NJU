#ifndef __RISCV64_ISA_JIT_H__
#define __RISCV64_ISA_JIT_H__

#include <common.h>
#include <memory/paddr.h>

/*
 * RISC-V64 JIT public hooks. The CPU loop owns scheduling and accounting, while
 * memory/device paths report physical writes here so native blocks compiled from
 * stale source bytes can be discarded before they are entered again.
 */

/* Return true when the current build and runtime flags allow RV64 JIT execution. */
bool isa_jit_available(void);

/* Execute native RV64 blocks within the CPU and device polling budgets. */
bool isa_jit_exec(uint64_t remaining, uint32_t device_budget, uint32_t *executed);

/*
 * True after RV64 JIT state exists and physical writes may need invalidation.
 * Writers use this as a cheap guard before calling the heavier hook.
 */
extern bool isa_jit_invalidation_active;

/* Drop all cached native blocks and private JIT state. */
void isa_jit_flush_all(void);

/* Notify the JIT that a physical PMEM byte range has been written. */
void isa_jit_invalidate_paddr(paddr_t addr, int len);

/* Print optional runtime statistics when enabled by config and environment. */
void isa_jit_dump_stats(void);

#endif
