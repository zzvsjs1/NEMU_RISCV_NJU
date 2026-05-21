#ifndef __CPU_IFETCH_H__

#include <memory/vaddr.h>

/*
 * Fetch len bytes from the guest instruction stream and advance the caller's
 * temporary PC.  The real cpu.pc is not changed here; the interpreter commits
 * s->dnpc only after decode and execution finish.
 */
static inline uint32_t instr_fetch(vaddr_t *pc, int len)
{
    uint32_t instr = vaddr_ifetch(*pc, len);
    (*pc) += len;
    return instr;
}

/*
 * Compatibility spelling used by the newer RISC-V direct interpreter.  It keeps
 * the call site short while preserving the older instr_fetch() helper for the
 * table-interpreter code.
 */
static inline uint32_t inst_fetch(vaddr_t *pc, int len)
{
    return instr_fetch(pc, len);
}

#endif
