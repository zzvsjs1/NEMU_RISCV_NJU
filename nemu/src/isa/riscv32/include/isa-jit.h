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
bool isa_jit_available(void);
bool isa_jit_exec(uint64_t remaining, uint32_t device_budget, uint32_t *executed);
void isa_jit_flush_all(void);
void isa_jit_invalidate_paddr(paddr_t addr, int len);
void isa_jit_dump_stats(void);

#endif
