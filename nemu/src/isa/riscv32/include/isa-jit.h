#ifndef __RISCV32_ISA_JIT_H__
#define __RISCV32_ISA_JIT_H__

#include <common.h>
#include <memory/paddr.h>

bool isa_jit_available(void);
bool isa_jit_exec(uint64_t remaining, uint32_t device_budget, uint32_t *executed);
void isa_jit_flush_all(void);
void isa_jit_invalidate_paddr(paddr_t addr, int len);
void isa_jit_dump_stats(void);

#endif
