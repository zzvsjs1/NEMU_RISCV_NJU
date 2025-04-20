#include <isa.h>
#include <cpu/difftest.h>
#include "../local-include/reg.h"

#define REG_FMT ("%-10s " FMT_WORD "%-5s" FMT_DECIMAL_WORD "%-5s" FMT_DECIMAL_WORD_SIGN "     Original: " FMT_WORD "\n")

bool isSameState(CPU_state *ref_r, vaddr_t pc)
{
    for (size_t i = 0; i < 32; i++) 
	{
        if (cpu.gpr[i]._32 != ref_r->gpr[i]._32) 
		{
            return false;
        }
    }

    if (ref_r->pc != cpu.pc)
    {
        return false;
    }

    if (ref_r->csr.mcause != cpu.csr.mcause) 
    {
        return false;
    }

    if (ref_r->csr.mepc != cpu.csr.mepc) 
	{
        return false;
    }

    if (ref_r->csr.mstatus != cpu.csr.mstatus) 
	{
        return false;
    }

    if (ref_r->csr.mtvec != cpu.csr.mtvec) 
	{
        return false;
    }

    return true;
}


bool isa_difftest_checkregs(CPU_state *ref_r, vaddr_t pc) 
{
    if (isSameState(ref_r, pc)) 
    {
        return true;
    }

    /* GPR */
    for (size_t i = 0; i < 32; i++) 
	{
        if (cpu.gpr[i]._32 != ref_r->gpr[i]._32) 
		{
            PRI_ERR(
                "%-10s " FMT_WORD "%-10s" FMT_DECIMAL_WORD "%-10s" FMT_DECIMAL_WORD_SIGN
                "     Original: " FMT_WORD "\n",
                reg_name(i, 4),
                ref_r->gpr[i]._32, " ", ref_r->gpr[i]._32, " ",
                (sword_t)ref_r->gpr[i]._32,
                cpu.gpr[i]._32
            );
        }
        else 
		{
            printf(
                REG_FMT,
                reg_name(i, 4),
                ref_r->gpr[i]._32, " ", ref_r->gpr[i]._32, " ",
                (sword_t)ref_r->gpr[i]._32,
                cpu.gpr[i]._32
            );
        }
    }

    
    if (ref_r->pc != cpu.pc)
    {
        PRI_ERR(
            "%-10s " FMT_WORD "%-10s" FMT_DECIMAL_WORD "%-10s" FMT_DECIMAL_WORD_SIGN
            "     Original: " FMT_WORD "\n",
            "PC",
            ref_r->pc, " ", ref_r->pc, " ",
            (sword_t)ref_r->pc,
            cpu.pc
        );
    }


    /* CSR */
    if (ref_r->csr.mcause != cpu.csr.mcause) 
    {
        printf(
            REG_FMT,
            "mcause", ref_r->csr.mcause, " ", ref_r->csr.mcause, " ",
            (sword_t)ref_r->csr.mcause,
            cpu.csr.mcause
        );
    }

    if (ref_r->csr.mepc != cpu.csr.mepc) 
	{
        printf(
            REG_FMT,
            "mepc", ref_r->csr.mepc, " ", ref_r->csr.mepc, " ",
            (sword_t)ref_r->csr.mepc,
            cpu.csr.mepc
        );
    }

    if (ref_r->csr.mstatus != cpu.csr.mstatus) 
	{
        printf(
            REG_FMT,
            "mstatus", ref_r->csr.mstatus, " ", ref_r->csr.mstatus, " ",
            (sword_t)ref_r->csr.mstatus,
            cpu.csr.mstatus
        );
    }

    if (ref_r->csr.mtvec != cpu.csr.mtvec) 
	{
        printf(
            REG_FMT,
            "mtvec", ref_r->csr.mtvec, " ", ref_r->csr.mtvec, " ",
            (sword_t)ref_r->csr.mtvec,
            cpu.csr.mtvec
        );
    }

    printf("\n\n");
    return false;
}

void isa_difftest_attach() 
{

}
