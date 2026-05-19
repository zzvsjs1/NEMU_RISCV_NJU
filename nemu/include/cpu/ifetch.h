#ifndef __CPU_IFETCH_H__

#include <memory/vaddr.h>

static inline uint32_t instr_fetch(vaddr_t *pc, int len)
{
    uint32_t instr = vaddr_ifetch(*pc, len);
    (*pc) += len;
    return instr;
}

static inline uint32_t inst_fetch(vaddr_t *pc, int len)
{
    return instr_fetch(pc, len);
}

#endif
