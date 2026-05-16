
#include <isa-jit.h>

static inline void csr_flush_jit_if_satp(word_t csr_addr)
{
    /*
     * satp selects the active Sv32 page-table root.  JIT blocks are already
     * tagged by satp and jit_block_matches() revalidates the physical source
     * address before entering cached native code.  Nanos-lite writes satp during
     * normal context switches, so flushing the whole native-code arena here
     * throws away hot blocks even though they remain safely separated by tag.
     */
    (void)csr_addr;
}

static inline bool riscv32_csr_privilege_ok(word_t csr_addr)
{
    const word_t required_priv = (csr_addr >> 8) & 0x3u;
    return cpu.prvi >= required_priv;
}

static inline bool riscv32_csr_write_allowed(word_t csr_addr, bool will_write)
{
    return !will_write || isCSRWriteable(csr_addr);
}

static inline rtlreg_t *riscv32_get_csr_or_trap(Decode *s, word_t csr_addr, bool will_write)
{
    if (!isCSRImplemented(csr_addr) ||
        !riscv32_csr_privilege_ok(csr_addr) ||
        !riscv32_csr_write_allowed(csr_addr, will_write))
    {
        riscv32_raise_trap(s, RISCV32_CAUSE_ILLEGAL_INST, 0);
        return NULL;
    }

    return getCSRAddress(csr_addr);
}

def_EHelper(ecall)
{
    const word_t cause = riscv32_ecall_cause_from_priv(cpu.prvi);

    // printf("\n\nCurrent PC %x\n", s->pc);
    // printf("RET PC %x\n\n", cpu.csr.mepc);

    riscv32_raise_trap(s, cause, 0);
}

def_EHelper(ebreak)
{
    riscv32_raise_trap(s, RISCV32_CAUSE_BREAKPOINT, 0);
}

// The CSRRW (Atomic Read/Write CSR) instruction atomically swaps values in the CSRs and integer
// registers. CSRRW reads the old value of the CSR, zero-extends the value to
// XLEN bits, then writes it to
// integer register rd. The initial value in rs1 is written to the CSR. If rd=x0, then the instruction shall not
// read the CSR and shall not cause any of the side effects that might occur on a CSR read.
def_EHelper(csrrw)
{
    const word_t csrAddress = id_src2->imm;

    // printf("\nWrite %x to CSR %x\n", *dsrc1, csrAddress);

    // Get csr register address.
    rtlreg_t *csrPtr = riscv32_get_csr_or_trap(s, csrAddress, true);
    if (csrPtr == NULL)
        return;

    const rtlreg_t newCSRValue = *dsrc1;

    // If rd is not x0, read the old CSR value and write it to rd.

    if (s->isa.instr.CSR.rd != 0)
    {
        const rtlreg_t oldCSRValue = *csrPtr;
        rtl_li(s, ddest, oldCSRValue);
    }

    // Write the original rs1 value to the CSR. This must still be correct
    // when rd == rs1, for example: csrrw sp, mscratch, sp.
    rtl_li(s, csrPtr, newCSRValue);
    csr_flush_jit_if_satp(csrAddress);
}

// The CSRRS (Atomic Read and Set Bits in CSR) instruction reads the value of the CSR, zero-extends the
// value to XLEN bits, and writes it to integer register rd. The initial value in integer register rs1 is treated
// as a bit mask that specifies bit positions to be set in the CSR. Any bit that is high in rs1 will cause the
// corresponding bit to be set in the CSR, if that CSR bit is writable.
def_EHelper(csrrs)
{
    const word_t csrAddress = id_src2->imm;

    const bool will_write = s->isa.instr.CSR.rs1 != 0;
    rtlreg_t *csrPtr = riscv32_get_csr_or_trap(s, csrAddress, will_write);
    if (csrPtr == NULL)
        return;

    // Write it to integer register rd
    rtl_li(s, ddest, *csrPtr);

    // if that CSR bit is writable
    // For both CSRRS and CSRRC, if rs1=x0, then the instruction will not write to the CSR at all

    if (will_write)
    {
        rtl_or(s, csrPtr, csrPtr, dsrc1);
        csr_flush_jit_if_satp(csrAddress);
    }
}

// The CSRRC (Atomic Read and Clear Bits in CSR) instruction reads the value of the CSR, zero-extends
// the value to XLEN bits, and writes it to integer register rd. The initial value in integer register rs1 is
// treated as a bit mask that specifies bit positions to be cleared in the CSR. Any bit that is high in rs1 will
// cause the corresponding bit to be cleared in the CSR, if that CSR bit is writable.
def_EHelper(csrrc)
{
    const word_t csrAddress = id_src2->imm;
    const bool will_write = s->isa.instr.CSR.rs1 != 0;
    rtlreg_t *csrPtr = riscv32_get_csr_or_trap(s, csrAddress, will_write);
    if (csrPtr == NULL)
        return;

    // Write it to integer register rd
    rtl_li(s, ddest, *csrPtr);

    if (will_write)
    {
        rtl_mv(s, s0, dsrc1);
        rtl_not(s, s0, s0);
        rtl_and(s, csrPtr, csrPtr, s0);
        csr_flush_jit_if_satp(csrAddress);
    }
}

def_EHelper(csrrwi)
{
    const word_t csrAddress = id_src2->imm;
    rtlreg_t *csrPtr = riscv32_get_csr_or_trap(s, csrAddress, true);
    if (csrPtr == NULL)
        return;
    const rtlreg_t uimm = s->isa.instr.CSR.rs1;

    if (s->isa.instr.CSR.rd != 0)
    {
        rtl_li(s, ddest, *csrPtr);
    }

    rtl_li(s, csrPtr, uimm);
    csr_flush_jit_if_satp(csrAddress);
}

def_EHelper(csrrsi)
{
    const word_t csrAddress = id_src2->imm;
    const bool will_write = s->isa.instr.CSR.rs1 != 0;
    rtlreg_t *csrPtr = riscv32_get_csr_or_trap(s, csrAddress, will_write);
    if (csrPtr == NULL)
        return;
    const rtlreg_t uimm = s->isa.instr.CSR.rs1;

    rtl_li(s, ddest, *csrPtr);

    if (will_write)
    {
        rtl_ori(s, csrPtr, csrPtr, uimm);
        csr_flush_jit_if_satp(csrAddress);
    }
}

def_EHelper(csrrci)
{
    const word_t csrAddress = id_src2->imm;
    const bool will_write = s->isa.instr.CSR.rs1 != 0;
    rtlreg_t *csrPtr = riscv32_get_csr_or_trap(s, csrAddress, will_write);
    if (csrPtr == NULL)
        return;
    const rtlreg_t uimm = s->isa.instr.CSR.rs1;

    rtl_li(s, ddest, *csrPtr);

    if (will_write)
    {
        rtl_li(s, s0, ~uimm);
        rtl_and(s, csrPtr, csrPtr, s0);
        csr_flush_jit_if_satp(csrAddress);
    }
}
