#ifndef __X86_ISA_JIT_H__
#define __X86_ISA_JIT_H__

#include <common.h>
#include <memory/paddr.h>

/*
 * x86 JIT public hooks.  The CPU loop owns scheduling and accounting; this
 * interface lets the x86 native path run bounded batches and lets PMEM writes
 * invalidate cached blocks compiled from stale instruction bytes.
 */
bool isa_jit_available(void);
bool isa_jit_exec(uint64_t remaining, uint32_t device_budget, uint32_t *executed);
extern bool isa_jit_invalidation_active;
void isa_jit_flush_all(void);
void isa_jit_flush_data_tlb(void);
bool isa_jit_may_invalidate_paddr(paddr_t addr, int len);
void isa_jit_invalidate_paddr(paddr_t addr, int len);
void isa_jit_dump_stats(void);

#endif
