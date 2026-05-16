#include <isa.h>
#include <cpu/difftest.h>
#include "../local-include/reg.h"

#define RV64_DIFF_MSTATUS_MASK (((word_t)1u << 3) | \
                                ((word_t)1u << 7) | \
                                ((word_t)0x3u << 11) | \
                                ((word_t)1u << 17) | \
                                ((word_t)1u << 18))

static bool riscv64_difftest_same_mstatus(word_t ref, word_t dut)
{
    return ((ref ^ dut) & RV64_DIFF_MSTATUS_MASK) == 0;
}

static bool riscv64_difftest_same_state(CPU_state *ref_r)
{
    for (size_t i = 0; i < ARRLEN(cpu.gpr); i++)
    {
        if (cpu.gpr[i]._64 != ref_r->gpr[i]._64)
        {
            return false;
        }
    }

    return ref_r->pc == cpu.pc &&
           ref_r->csr.satp == cpu.csr.satp &&
           ref_r->csr.mcause == cpu.csr.mcause &&
           ref_r->csr.mepc == cpu.csr.mepc &&
           riscv64_difftest_same_mstatus(ref_r->csr.mstatus, cpu.csr.mstatus) &&
           ref_r->csr.mtvec == cpu.csr.mtvec &&
           ref_r->csr.mscratch == cpu.csr.mscratch &&
           ref_r->csr.mtval == cpu.csr.mtval;
}

static void riscv64_difftest_print_reg(size_t idx, word_t ref, word_t dut)
{
    if (ref == dut)
    {
        printf("%-10s " FMT_WORD "%-10s" FMT_DECIMAL_WORD "%-10s" FMT_DECIMAL_WORD_SIGN
               "     DUT: " FMT_WORD "\n",
               reg_name(idx, 8),
               ref, " ", ref, " ",
               (sword_t)ref,
               dut);
    }
    else
    {
        PRI_ERR("%-10s " FMT_WORD "%-10s" FMT_DECIMAL_WORD "%-10s" FMT_DECIMAL_WORD_SIGN
                "     DUT: " FMT_WORD "\n",
                reg_name(idx, 8),
                ref, " ", ref, " ",
                (sword_t)ref,
                dut);
    }
}

static void riscv64_difftest_print_named(const char *name, word_t ref, word_t dut)
{
    if (ref == dut)
    {
        printf("%-10s " FMT_WORD "%-10s" FMT_DECIMAL_WORD "%-10s" FMT_DECIMAL_WORD_SIGN
               "     DUT: " FMT_WORD "\n",
               name, ref, " ", ref, " ", (sword_t)ref, dut);
    }
    else
    {
        PRI_ERR("%-10s " FMT_WORD "%-10s" FMT_DECIMAL_WORD "%-10s" FMT_DECIMAL_WORD_SIGN
                "     DUT: " FMT_WORD "\n",
                name, ref, " ", ref, " ", (sword_t)ref, dut);
    }
}

bool isa_difftest_checkregs(CPU_state *ref_r, vaddr_t pc)
{
    if (riscv64_difftest_same_state(ref_r))
    {
        return true;
    }

    /*
     * Print the complete architectural state on mismatch.  This is noisy, but
     * the RV64 DiffTest path is meant for early correctness work where seeing
     * every register is more useful than hiding the surrounding context.
     */
    for (size_t i = 0; i < ARRLEN(cpu.gpr); i++)
    {
        riscv64_difftest_print_reg(i, ref_r->gpr[i]._64, cpu.gpr[i]._64);
    }

    riscv64_difftest_print_named("pc", ref_r->pc, cpu.pc);
    riscv64_difftest_print_named("satp", ref_r->csr.satp, cpu.csr.satp);
    riscv64_difftest_print_named("mstatus", ref_r->csr.mstatus, cpu.csr.mstatus);
    riscv64_difftest_print_named("mtvec", ref_r->csr.mtvec, cpu.csr.mtvec);
    riscv64_difftest_print_named("mscratch", ref_r->csr.mscratch, cpu.csr.mscratch);
    riscv64_difftest_print_named("mepc", ref_r->csr.mepc, cpu.csr.mepc);
    riscv64_difftest_print_named("mcause", ref_r->csr.mcause, cpu.csr.mcause);
    riscv64_difftest_print_named("mtval", ref_r->csr.mtval, cpu.csr.mtval);

    return false;
}

void isa_difftest_attach()
{
}
