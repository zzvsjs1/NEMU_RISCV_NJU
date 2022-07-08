#include <isa.h>
#include <cpu/difftest.h>
#include "../local-include/reg.h"

const char *reggs[] = {
  "$0", "ra", "sp", "gp", "tp", "t0", "t1", "t2",
  "s0", "s1", "a0", "a1", "a2", "a3", "a4", "a5",
  "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7",
  "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6",
  "pc"
};

#define REG_FMT ("%-5s " FMT_WORD "%-5s" FMT_DECIMAL_WORD "%-5s" FMT_DECIMAL_WORD_SIGN "\n")

bool isa_difftest_checkregs(CPU_state *ref_r, vaddr_t pc) 
{
	const bool ret = memcmp(ref_r, &cpu, sizeof cpu) == 0;

	if (!ret)
	{
		word_t* p = (word_t*) ref_r;
		PRI_ERR_E("Correct:\n");
		for (size_t i = 0, end = sizeof cpu / sizeof(word_t); i < end; ++i)
		{
			printf(REG_FMT, reggs[i], p[i], " ", p[i], " ", (sword_t) p[i]);
		}

		printf("\n\n");
	}

	return ret;
}

void isa_difftest_attach() 
{
}
