#ifndef __RISCV_REG_H__
#define __RISCV_REG_H__

#include <isa.h>

/*
 * Keep register indexing checks in one place.  Release builds simply return the
 * index, while CONFIG_RT_CHECK builds catch any helper that tries to access a
 * GPR outside the configured RV32E/RV32/RV64 register file.
 */
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

void setCSRValue(const word_t address, word_t value);

rtlreg_t *getCSRAddress(const word_t address);

bool isCSRImplemented(const word_t address);

bool isCSRWriteable(const word_t csrAddr);

/*
 * Return the ABI name used by tracing and the monitor.  Width is ignored for
 * RISC-V because each architectural register has one name regardless of access
 * size.
 */
static inline const char *reg_name(int idx, int width)
{
    (void)width;
    extern const char *regs[];
    return regs[check_reg_idx(idx)];
}

#endif
