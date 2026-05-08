#ifndef __RISCV32_ISA_JIT_H__
#define __RISCV32_ISA_JIT_H__

#include <common.h>
#include <memory/paddr.h>

/*
 * RISC-V32 JIT public hooks. The CPU loop owns scheduling, while memory and
 * devices report physical writes here so native blocks compiled from stale
 * source bytes can be discarded before they are executed again.
 *
 * Keep this interface physical-address based. Virtual addresses may be remapped
 * under the same satp value, but overwritten PMEM bytes are the common fact seen
 * by interpreter stores, direct JIT stores, disk DMA, and snapshot restore.
 */
/* Return true when the current build and runtime flags allow JIT execution. */
bool isa_jit_available(void);
/*
 * Try to execute native blocks within both the CPU-loop budget and the device
 * update budget. `executed` receives the number of guest instructions completed.
 */
bool isa_jit_exec(uint64_t remaining, uint32_t device_budget, uint32_t *executed);
/* Drop every cached native block, used after broad address-space/state changes. */
void isa_jit_flush_all(void);
/* Invalidate cached blocks whose translated physical source bytes were written. */
void isa_jit_invalidate_paddr(paddr_t addr, int len);
/* Print optional runtime statistics when the binary and env flag enable them. */
void isa_jit_dump_stats(void);

#endif
