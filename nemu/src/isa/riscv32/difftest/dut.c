#include <isa.h>
#include <cpu/difftest.h>
#include "../local-include/reg.h"
#include <stddef.h>

_Static_assert(sizeof(CPU_state) == DIFFTEST_REG_SIZE,
               "RISC-V DiffTest ABI must cover the full CPU_state");
_Static_assert(offsetof(CPU_state, gpr) == offsetof(riscv_difftest_state_t, gpr),
               "RISC-V DiffTest GPR offset drifted");
_Static_assert(offsetof(CPU_state, pc) == offsetof(riscv_difftest_state_t, pc),
               "RISC-V DiffTest PC offset drifted");
_Static_assert(offsetof(CPU_state, csr.satp) == offsetof(riscv_difftest_state_t, csr.satp),
               "RISC-V DiffTest satp offset drifted");
_Static_assert(offsetof(CPU_state, csr.mtval) == offsetof(riscv_difftest_state_t, csr.mtval),
               "RISC-V DiffTest mtval offset drifted");
_Static_assert(offsetof(CPU_state, prvi) == offsetof(riscv_difftest_state_t, prvi),
               "RISC-V DiffTest privilege offset drifted");
_Static_assert(offsetof(CPU_state, INTR) == offsetof(riscv_difftest_state_t, INTR),
               "RISC-V DiffTest interrupt-pending offset drifted");

#ifdef CONFIG_RV64
#define RISCV_DIFF_GPR(state, idx) ((state)->gpr[idx]._64)
#define RISCV_DIFF_REG_NAME_LEN 8
#define RISCV_DIFF_MSTATUS_MASK (((word_t)1u << 3) | \
                                 ((word_t)1u << 7) | \
                                 ((word_t)0x3u << 11) | \
                                 ((word_t)1u << 17) | \
                                 ((word_t)1u << 18) | \
                                 ((word_t)1u << 19))
#else
#define RISCV_DIFF_GPR(state, idx) ((state)->gpr[idx]._32)
#define RISCV_DIFF_REG_NAME_LEN 4
#endif

static bool riscv_difftest_same_mstatus(word_t ref, word_t dut)
{
#ifdef CONFIG_RV64
    return ((ref ^ dut) & RISCV_DIFF_MSTATUS_MASK) == 0;
#else
    return ref == dut;
#endif
}

static bool riscv_difftest_same_state(CPU_state *ref_r)
{
    for (size_t i = 0; i < RISCV_GPR_NUM; i++)
    {
        if (gpr(i) != RISCV_DIFF_GPR(ref_r, i))
        {
            return false;
        }
    }

    return ref_r->pc == cpu.pc &&
           ref_r->csr.satp == cpu.csr.satp &&
           ref_r->csr.mcause == cpu.csr.mcause &&
           ref_r->csr.mepc == cpu.csr.mepc &&
           riscv_difftest_same_mstatus(ref_r->csr.mstatus, cpu.csr.mstatus) &&
           ref_r->csr.mtvec == cpu.csr.mtvec &&
           ref_r->csr.mscratch == cpu.csr.mscratch &&
           ref_r->csr.mtval == cpu.csr.mtval &&
           ref_r->prvi == cpu.prvi;
}

static void riscv_difftest_print_reg(size_t idx, word_t ref, word_t dut)
{
    if (ref == dut)
    {
        printf("%-10s " FMT_WORD "%-10s" FMT_DECIMAL_WORD "%-10s" FMT_DECIMAL_WORD_SIGN
               "     DUT: " FMT_WORD "\n",
               reg_name(idx, RISCV_DIFF_REG_NAME_LEN),
               ref, " ", ref, " ",
               (sword_t)ref,
               dut);
    }
    else
    {
        PRI_ERR("%-10s " FMT_WORD "%-10s" FMT_DECIMAL_WORD "%-10s" FMT_DECIMAL_WORD_SIGN
                "     DUT: " FMT_WORD "\n",
                reg_name(idx, RISCV_DIFF_REG_NAME_LEN),
                ref, " ", ref, " ",
                (sword_t)ref,
                dut);
    }
}

static void riscv_difftest_print_named(const char *name, word_t ref, word_t dut)
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
    (void)pc;

    if (riscv_difftest_same_state(ref_r))
    {
        return true;
    }

    /*
     * Print the complete architectural state on mismatch.  This is noisy, but
     * early shared-interpreter work benefits from seeing every nearby register.
     */
    for (size_t i = 0; i < RISCV_GPR_NUM; i++)
    {
        riscv_difftest_print_reg(i, RISCV_DIFF_GPR(ref_r, i), gpr(i));
    }

    riscv_difftest_print_named("pc", ref_r->pc, cpu.pc);
    riscv_difftest_print_named("satp", ref_r->csr.satp, cpu.csr.satp);
    riscv_difftest_print_named("mstatus", ref_r->csr.mstatus, cpu.csr.mstatus);
    riscv_difftest_print_named("mtvec", ref_r->csr.mtvec, cpu.csr.mtvec);
    riscv_difftest_print_named("mscratch", ref_r->csr.mscratch, cpu.csr.mscratch);
    riscv_difftest_print_named("mepc", ref_r->csr.mepc, cpu.csr.mepc);
    riscv_difftest_print_named("mcause", ref_r->csr.mcause, cpu.csr.mcause);
    riscv_difftest_print_named("mtval", ref_r->csr.mtval, cpu.csr.mtval);
    riscv_difftest_print_named("prvi", ref_r->prvi, cpu.prvi);

    return false;
}

void isa_difftest_attach()
{
}
