#include <isa.h>
#include <cpu/difftest.h>
#include "../local-include/reg.h"
#include <stddef.h>

_Static_assert(sizeof(CPU_state) == DIFFTEST_REG_SIZE,
               "RV32 DiffTest ABI must cover the full CPU_state");
_Static_assert(offsetof(CPU_state, gpr) == offsetof(riscv_difftest_state_t, gpr),
               "RV32 DiffTest GPR offset drifted");
_Static_assert(offsetof(CPU_state, pc) == offsetof(riscv_difftest_state_t, pc),
               "RV32 DiffTest PC offset drifted");
_Static_assert(offsetof(CPU_state, csr.satp) == offsetof(riscv_difftest_state_t, csr.satp),
               "RV32 DiffTest satp offset drifted");
_Static_assert(offsetof(CPU_state, csr.mtval) == offsetof(riscv_difftest_state_t, csr.mtval),
               "RV32 DiffTest mtval offset drifted");
_Static_assert(offsetof(CPU_state, prvi) == offsetof(riscv_difftest_state_t, prvi),
               "RV32 DiffTest privilege offset drifted");
_Static_assert(offsetof(CPU_state, INTR) == offsetof(riscv_difftest_state_t, INTR),
               "RV32 DiffTest interrupt-pending offset drifted");

#define REG_FMT ("%-10s " FMT_WORD "%-5s" FMT_DECIMAL_WORD "%-5s" FMT_DECIMAL_WORD_SIGN "     Original: " FMT_WORD "\n")

bool isSameState(CPU_state *ref_r, vaddr_t pc)
{
    for (size_t i = 0; i < MUXDEF(CONFIG_RVE, 16, 32); i++)
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

    if (ref_r->csr.satp != cpu.csr.satp)
    {
        return false;
    }

    if (ref_r->csr.mscratch != cpu.csr.mscratch)
    {
        return false;
    }

    if (ref_r->csr.mtval != cpu.csr.mtval)
    {
        return false;
    }

    if (ref_r->prvi != cpu.prvi)
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
    for (size_t i = 0; i < MUXDEF(CONFIG_RVE, 16, 32); i++)
    {
        if (cpu.gpr[i]._32 != ref_r->gpr[i]._32)
        {
            PRI_ERR(
                "%-10s " FMT_WORD "%-10s" FMT_DECIMAL_WORD "%-10s" FMT_DECIMAL_WORD_SIGN
                "     Original: " FMT_WORD "\n",
                reg_name(i, 4),
                ref_r->gpr[i]._32, " ", ref_r->gpr[i]._32, " ",
                (sword_t)ref_r->gpr[i]._32,
                cpu.gpr[i]._32);
        }
        else
        {
            printf(
                REG_FMT,
                reg_name(i, 4),
                ref_r->gpr[i]._32, " ", ref_r->gpr[i]._32, " ",
                (sword_t)ref_r->gpr[i]._32,
                cpu.gpr[i]._32);
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
            cpu.pc);
    }

    /* CSR */

    if (ref_r->csr.mcause != cpu.csr.mcause)
    {
        PRI_ERR(
            "%-10s " FMT_WORD "%-5s" FMT_DECIMAL_WORD "%-5s" FMT_DECIMAL_WORD_SIGN "     Original: " FMT_WORD "\n",
            "mcause", ref_r->csr.mcause, " ", ref_r->csr.mcause, " ",
            (sword_t)ref_r->csr.mcause,
            cpu.csr.mcause);
    }

    if (ref_r->csr.mepc != cpu.csr.mepc)
    {
        PRI_ERR(
            "%-10s " FMT_WORD "%-5s" FMT_DECIMAL_WORD "%-5s" FMT_DECIMAL_WORD_SIGN "     Original: " FMT_WORD "\n",
            "mepc", ref_r->csr.mepc, " ", ref_r->csr.mepc, " ",
            (sword_t)ref_r->csr.mepc,
            cpu.csr.mepc);
    }

    if (ref_r->csr.mstatus != cpu.csr.mstatus)
    {
        PRI_ERR(
            "%-10s " FMT_WORD "%-5s" FMT_DECIMAL_WORD "%-5s" FMT_DECIMAL_WORD_SIGN "     Original: " FMT_WORD "\n",
            "mstatus", ref_r->csr.mstatus, " ", ref_r->csr.mstatus, " ",
            (sword_t)ref_r->csr.mstatus,
            cpu.csr.mstatus);
    }

    if (ref_r->csr.mtvec != cpu.csr.mtvec)
    {
        PRI_ERR(
            "%-10s " FMT_WORD "%-5s" FMT_DECIMAL_WORD "%-5s" FMT_DECIMAL_WORD_SIGN "     Original: " FMT_WORD "\n",
            "mtvec", ref_r->csr.mtvec, " ", ref_r->csr.mtvec, " ",
            (sword_t)ref_r->csr.mtvec,
            cpu.csr.mtvec);
    }

    if (ref_r->csr.satp != cpu.csr.satp)
    {
        PRI_ERR(
            "%-10s " FMT_WORD "%-5s" FMT_DECIMAL_WORD "%-5s" FMT_DECIMAL_WORD_SIGN "     Original: " FMT_WORD "\n",
            "satp", ref_r->csr.satp, " ", ref_r->csr.satp, " ",
            (sword_t)ref_r->csr.satp,
            cpu.csr.satp);
    }

    if (ref_r->csr.mscratch != cpu.csr.mscratch)
    {
        PRI_ERR(
            "%-10s " FMT_WORD "%-5s" FMT_DECIMAL_WORD "%-5s" FMT_DECIMAL_WORD_SIGN "     Original: " FMT_WORD "\n",
            "mscratch", ref_r->csr.mscratch, " ", ref_r->csr.mscratch, " ",
            (sword_t)ref_r->csr.mscratch,
            cpu.csr.mscratch);
    }

    if (ref_r->csr.mtval != cpu.csr.mtval)
    {
        PRI_ERR(
            "%-10s " FMT_WORD "%-5s" FMT_DECIMAL_WORD "%-5s" FMT_DECIMAL_WORD_SIGN "     Original: " FMT_WORD "\n",
            "mtval", ref_r->csr.mtval, " ", ref_r->csr.mtval, " ",
            (sword_t)ref_r->csr.mtval,
            cpu.csr.mtval);
    }

    if (ref_r->prvi != cpu.prvi)
    {
        PRI_ERR(
            "%-10s " FMT_WORD "%-5s" FMT_DECIMAL_WORD "%-5s" FMT_DECIMAL_WORD_SIGN "     Original: " FMT_WORD "\n",
            "prvi", ref_r->prvi, " ", ref_r->prvi, " ",
            (sword_t)ref_r->prvi,
            cpu.prvi);
    }

    printf("\n\n");
    return false;
}

void isa_difftest_attach()
{
}
