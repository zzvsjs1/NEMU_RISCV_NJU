#ifndef __RISCV32_REG_H__
#define __RISCV32_REG_H__

#include <common.h>

static inline int check_reg_idx(int idx) 
{
	IFDEF(CONFIG_RT_CHECK, assert(idx >= 0 && idx < 32));
	return idx;
}

#define gpr(idx) (cpu.gpr[check_reg_idx(idx)]._32)

word_t getCSRValue(const word_t address);

rtlreg_t* getCSRAddress(const word_t address);

bool isCSRWriteable(const word_t address);

static inline const char* reg_name(int idx, int width) 
{
	extern const char* regs[];
	return regs[check_reg_idx(idx)];
}

#endif
