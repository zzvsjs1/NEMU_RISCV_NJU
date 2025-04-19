#include <isa.h>
#include "local-include/reg.h"

#define REG_FMT ("%-5s " FMT_WORD "%-5s" FMT_DECIMAL_WORD "%-5s" FMT_DECIMAL_WORD_SIGN "\n")

const char *regs[] = {
  "$0", "ra", "sp", "gp", "tp", "t0", "t1", "t2",
  "s0", "s1", "a0", "a1", "a2", "a3", "a4", "a5",
  "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7",
  "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6"
};

const char *csrs[] = {
	"mstatus", "mtvec", "mepc", "mcause"
};

word_t getCSRValue(const word_t address)
{
	return *getCSRAddress(address);
}

rtlreg_t* getCSRAddress(const word_t address)
{
	switch (address)
	{
	case 0x300: {
		return &cpu.csr.mstatus;
	}
	
	case 0x305: {
		return &cpu.csr.mtvec;
	}

	case 0x341: {
		return &cpu.csr.mepc;
	}

	case 0x342: {
		return &cpu.csr.mcause;
	}
	
	default:
		Assert(false, "Invalid csr address: " FMT_WORD "\n", address);
		break;
	}

	Assert(false, "Should not come here!\n");
	return NULL;
}

bool isCSRWriteable(const word_t csrAddr)
{
	// Extract bits [11:10]
	const int32_t access_type = (csrAddr >> 10) & 0b11;

	// If bits are 0b11, it's read-only.
	return access_type != 0b11;
}

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
	printf("\n\n");

	// Print CSR.
	uint32_t *csrp = (rtlreg_t*)&cpu.csr;
	for (size_t i = 0; i < sizeof(cpu.csr) / sizeof(rtlreg_t); ++i)
	{
		const word_t temp = csrp[i];
		printf(REG_FMT, csrs[i], temp, " ", temp, " ", (sword_t) temp);
	}

	printf("\n\n");
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
