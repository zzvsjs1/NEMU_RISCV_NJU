#include <isa.h>
#include <cpu/difftest.h>
#include "../local-include/reg.h"
#include <inttypes.h>
#include <stddef.h>

_Static_assert(sizeof(CPU_state) == DIFFTEST_REG_SIZE, "RISC-V DiffTest ABI must cover the full CPU_state");
_Static_assert(offsetof(CPU_state, gpr) == offsetof(riscv_difftest_state_t, gpr), "RISC-V DiffTest GPR offset drifted");
_Static_assert(offsetof(CPU_state, pc) == offsetof(riscv_difftest_state_t, pc), "RISC-V DiffTest PC offset drifted");
_Static_assert(offsetof(CPU_state, csr.satp) == offsetof(riscv_difftest_state_t, csr.satp), "RISC-V DiffTest satp offset drifted");
_Static_assert(offsetof(CPU_state, csr.mtval) == offsetof(riscv_difftest_state_t, csr.mtval), "RISC-V DiffTest mtval offset drifted");
_Static_assert(offsetof(CPU_state, prvi) == offsetof(riscv_difftest_state_t, prvi), "RISC-V DiffTest privilege offset drifted");
_Static_assert(offsetof(CPU_state, INTR) == offsetof(riscv_difftest_state_t, INTR), "RISC-V DiffTest interrupt-pending offset drifted");

#ifdef CONFIG_RISCV_FPU
_Static_assert(RISCV_FPR_NUM == RISCV_DIFFTEST_FPR_NUM, "RISC-V DiffTest FPR count drifted");
_Static_assert(RISCV_FCSR_MASK == RISCV_DIFFTEST_FCSR_MASK, "RISC-V DiffTest fcsr mask drifted");
_Static_assert(sizeof(((CPU_state *)0)->fpr[0]) == sizeof(((riscv_difftest_state_t *)0)->fpr[0]), "RISC-V DiffTest FPR width drifted");
_Static_assert(offsetof(CPU_state, fpr) == offsetof(riscv_difftest_state_t, fpr), "RISC-V DiffTest FPR offset drifted");
_Static_assert(offsetof(CPU_state, fcsr) == offsetof(riscv_difftest_state_t, fcsr), "RISC-V DiffTest FCSR offset drifted");
#endif

#ifdef CONFIG_RV64
#define RISCV_DIFF_GPR(state, idx) ((state)->gpr[idx]._64)
#define RISCV_DIFF_REG_NAME_LEN 8
#ifdef CONFIG_RISCV_FPU
#define RISCV_DIFF_FP_MSTATUS_MASK (RISCV_MSTATUS_FS_MASK | RISCV_MSTATUS_SD)
#else
#define RISCV_DIFF_FP_MSTATUS_MASK 0
#endif
/*
 * Preserve the established mstatus comparison contract, expressed as named
 * architectural fields rather than bit literals.  UXL/SXL join that contract
 * now that writes are canonicalised to the only lower-privilege XLEN this target
 * implements.  TVM, WPRI fields, and unimplemented extension state remain
 * intentionally outside this DiffTest mask.
 */
#define RISCV_DIFF_MSTATUS_MASK \
    (RISCV_MSTATUS_MIE | RISCV_MSTATUS_MPIE | RISCV_MSTATUS_MPP_MASK | RISCV_MSTATUS_MPRV | RISCV_MSTATUS_SUM | RISCV_MSTATUS_MXR | \
     (word_t)RISCV64_MSTATUS_UXL_SXL_MASK | RISCV_DIFF_FP_MSTATUS_MASK)
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

#ifdef CONFIG_RISCV_FPU
    for (size_t i = 0; i < RISCV_FPR_NUM; i++)
    {
        if (ref_r->fpr[i] != cpu.fpr[i])
        {
            return false;
        }
    }

    if ((ref_r->fcsr & RISCV_FCSR_MASK) != (cpu.fcsr & RISCV_FCSR_MASK))
    {
        return false;
    }
#endif

    return ref_r->pc == cpu.pc && ref_r->csr.satp == cpu.csr.satp && ref_r->csr.mcause == cpu.csr.mcause && ref_r->csr.mepc == cpu.csr.mepc &&
           riscv_difftest_same_mstatus(ref_r->csr.mstatus, cpu.csr.mstatus) && ref_r->csr.mtvec == cpu.csr.mtvec &&
           ref_r->csr.mscratch == cpu.csr.mscratch && ref_r->csr.mtval == cpu.csr.mtval && ref_r->prvi == cpu.prvi;
}

static void riscv_difftest_print_reg(size_t idx, word_t ref, word_t dut)
{
    if (ref == dut)
    {
        printf("%-10s " FMT_WORD "%-10s" FMT_DECIMAL_WORD "%-10s" FMT_DECIMAL_WORD_SIGN "     DUT: " FMT_WORD "\n",
               reg_name(idx, RISCV_DIFF_REG_NAME_LEN), ref, " ", ref, " ", (sword_t)ref, dut);
    }
    else
    {
        PRI_ERR("%-10s " FMT_WORD "%-10s" FMT_DECIMAL_WORD "%-10s" FMT_DECIMAL_WORD_SIGN "     DUT: " FMT_WORD "\n",
                reg_name(idx, RISCV_DIFF_REG_NAME_LEN), ref, " ", ref, " ", (sword_t)ref, dut);
    }
}

static void riscv_difftest_print_named(const char *name, word_t ref, word_t dut)
{
    if (ref == dut)
    {
        printf("%-10s " FMT_WORD "%-10s" FMT_DECIMAL_WORD "%-10s" FMT_DECIMAL_WORD_SIGN "     DUT: " FMT_WORD "\n", name, ref, " ", ref, " ",
               (sword_t)ref, dut);
    }
    else
    {
        PRI_ERR("%-10s " FMT_WORD "%-10s" FMT_DECIMAL_WORD "%-10s" FMT_DECIMAL_WORD_SIGN "     DUT: " FMT_WORD "\n", name, ref, " ", ref, " ",
                (sword_t)ref, dut);
    }
}

#ifdef CONFIG_RISCV_FPU
#ifdef CONFIG_RISCV_D
/*
 * RV32D has 64-bit FPRs but 32-bit integer words.  Keep its diagnostics on a
 * dedicated FLEN-wide path so a mismatch in bits 63:32 is never truncated by
 * the ordinary XLEN-wide register printer.
 */
static void riscv_difftest_print_fpr(const char *name, uint64_t ref, uint64_t dut)
{
    if (ref == dut)
    {
        printf("%-10s 0x%016" PRIx64 "%-10s%" PRIu64 "%-10s%" PRId64 "     DUT: 0x%016" PRIx64 "\n", name, ref, " ", ref, " ", (int64_t)ref, dut);
    }
    else
    {
        PRI_ERR("%-10s 0x%016" PRIx64 "%-10s%" PRIu64 "%-10s%" PRId64 "     DUT: 0x%016" PRIx64 "\n", name, ref, " ", ref, " ", (int64_t)ref, dut);
    }
}
#else
static void riscv_difftest_print_fpr(const char *name, uint32_t ref, uint32_t dut)
{
    riscv_difftest_print_named(name, (word_t)ref, (word_t)dut);
}
#endif
#endif

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

#ifdef CONFIG_RISCV_FPU
    for (size_t i = 0; i < RISCV_FPR_NUM; i++)
    {
        char name[RISCV_DIFF_REG_NAME_LEN];
        snprintf(name, sizeof(name), "f%zu", i);
        riscv_difftest_print_fpr(name, ref_r->fpr[i], cpu.fpr[i]);
    }

    riscv_difftest_print_named("fcsr", ref_r->fcsr & RISCV_FCSR_MASK, cpu.fcsr & RISCV_FCSR_MASK);
#endif

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
