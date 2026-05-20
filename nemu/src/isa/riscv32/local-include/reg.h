#ifndef __RISCV_REG_H__
#define __RISCV_REG_H__

#include <isa.h>

static inline int check_reg_idx(int idx)
{
    IFDEF(CONFIG_RT_CHECK, assert(idx >= 0 && idx < RISCV_GPR_NUM));
    return idx;
}

#ifdef CONFIG_RV64
#define gpr(idx) (cpu.gpr[check_reg_idx(idx)]._64)
#else
#define gpr(idx) (cpu.gpr[check_reg_idx(idx)]._32)
#endif

word_t getCSRValue(const word_t address);

rtlreg_t *getCSRAddress(const word_t address);

bool isCSRImplemented(const word_t address);

bool isCSRWriteable(const word_t csrAddr);

static inline const char *reg_name(int idx, int width)
{
    (void)width;
    extern const char *regs[];
    return regs[check_reg_idx(idx)];
}

#endif
