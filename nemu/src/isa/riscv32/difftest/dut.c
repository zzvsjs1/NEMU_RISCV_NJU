#include <isa.h>
#include <cpu/difftest.h>
#include "../local-include/reg.h"

static const char *reggs[] = {
  "$0", "ra", "sp", "gp", "tp", "t0", "t1", "t2",
  "s0", "s1", "a0", "a1", "a2", "a3", "a4", "a5",
  "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7",
  "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6",
  "pc"
};

#define REG_FMT ("%-5s " FMT_WORD "%-5s" FMT_DECIMAL_WORD "%-5s" FMT_DECIMAL_WORD_SIGN "     Original: " FMT_WORD "\n")

bool isa_difftest_checkregs(CPU_state *ref_r, vaddr_t pc) 
{
	bool ret = true;

	word_t* p = (word_t*) ref_r;
	for (size_t i = 0, end = sizeof cpu / sizeof(word_t); i < end; ++i)
	{
		if (((word_t*)&cpu)[i] != p[i])
		{
			PRI_ERR(
			"%-5s " FMT_WORD "%-5s" FMT_DECIMAL_WORD "%-5s" FMT_DECIMAL_WORD_SIGN 
			"     Original: " FMT_WORD "\n", 
			reggs[i], p[i], " ", p[i], " ", (sword_t) p[i], ((word_t*)&cpu)[i]
		);
			ret = false;
		}
		else
		{
			printf(REG_FMT, reggs[i], p[i], " ", p[i], " ", (sword_t) p[i], ((word_t*)&cpu)[i]);
		}
	}

	printf("\n\n");
	return ret;
}

void isa_difftest_attach() 
{
}
