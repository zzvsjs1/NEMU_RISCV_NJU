#include <isa.h>
#include "local-include/reg.h"

#define REG_FMT ("%-5s " FMT_WORD "%-5s" FMT_DECIMAL_WORD "%-5s" FMT_DECIMAL_WORD_SIGN "\n")

const char *regs[] = {
  "$0", "ra", "sp", "gp", "tp", "t0", "t1", "t2",
  "s0", "s1", "a0", "a1", "a2", "a3", "a4", "a5",
  "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7",
  "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6"
};

void isa_reg_display() 
{
	printf("\n");
	for (size_t i = 0; i < ARRLEN(regs); ++i)
	{
		const word_t temp = gpr(i);
		printf(REG_FMT, reg_name(i, 5), temp, " ", temp, " ", (sword_t) temp);
	}

	const word_t pc = ((riscv32_CPU_state*)&cpu)->pc;
	printf(REG_FMT, "pc", pc, " ", pc, " ", (sword_t) pc);
	printf("\n");
}

word_t isa_reg_str2val(const char *s, bool *success) 
{
	if (!s)
	{
		*success = false;
		return -1;
	}

	if (strcmp(s, "pc") == 0)
	{
		return ((riscv32_CPU_state*)&cpu)->pc;
	}

	for (size_t i = 0; i < ARRLEN(regs); ++i)
	{
		if (strcmp(regs[i], s) == 0)
		{
			*success = true;
			return gpr(i);
		}
	}

	*success = false;
	return -1;
}

void isa_set_reg_val(const char* name, const word_t val)
{
	if (strcmp("pc", name) == 0)
	{
		((riscv32_CPU_state*)&cpu)->pc = val;
		return;
	}

	for (size_t i = 0; i < ARRLEN(regs); ++i)
	{
		if (strcmp(regs[i],name) == 0)
		{
			gpr(i) = val;
			return;
		}
	}

	PRI_ERR("Faile to set value. No this register %s.", name);
}
