
def_EHelper(ecall) 
{
    bool success = false;
    const word_t mcauseVal = isa_reg_str2val("a7", &success);
    const rtlreg_t nextPC = isa_raise_intr(mcauseVal, s->pc);

    // printf("\n\nCurrent PC %x\n", s->pc);
    // printf("JUMP PC %x\n", nextPC);
    // printf("RET PC %x\n\n", cpu.csr.mepc);

    Assert(success, "Cannot find a a7 register, it should be a logic error!\n");

    rtl_li(s, s0, nextPC);
    rtl_jr(s, s0);

#ifdef CONFIG_DIFFTEST
    // skip Spike’s next instruction
    difftest_skip_ref();
#endif
}

def_EHelper(ebreak) 
{
    Log("EBREAK trigger!\n");
    Assert(false, "EBREAK instruction, this should not happend!\n");
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
    rtlreg_t* csrPtr = getCSRAddress(csrAddress);

    // If not x0, we read the csr value and write to dest.
    // THIS IS A HACK!
    if (s->isa.instr.CSR.rd != 0)
    {
        const word_t oldCSRValue = *csrPtr;

        // zero-extends the value to XLEN bits, then writes it to integer register rd
        rtl_li(s, ddest, oldCSRValue & 0x1FFF);
    }

    // Write rs1 to csr
    rtl_mv(s, csrPtr, dsrc1);
}

// The CSRRS (Atomic Read and Set Bits in CSR) instruction reads the value of the CSR, zero-extends the
// value to XLEN bits, and writes it to integer register rd. The initial value in integer register rs1 is treated
// as a bit mask that specifies bit positions to be set in the CSR. Any bit that is high in rs1 will cause the
// corresponding bit to be set in the CSR, if that CSR bit is writable.
def_EHelper(csrrs) 
{
    const word_t csrAddress = id_src2->imm;

    rtlreg_t* csrPtr = getCSRAddress(csrAddress);

    // Write it to integer register rd
    rtl_li(s, ddest, *csrPtr);
    
    // if that CSR bit is writable
    // For both CSRRS and CSRRC, if rs1=x0, then the instruction will not write to the CSR at all
    if (isCSRWriteable(csrAddress) && s->isa.instr.CSR.rs1 != 0) 
    {
        rtl_or(s, csrPtr, csrPtr, dsrc1);
    }
}

// The CSRRC (Atomic Read and Clear Bits in CSR) instruction reads the value of the CSR, zero-extends
// the value to XLEN bits, and writes it to integer register rd. The initial value in integer register rs1 is
// treated as a bit mask that specifies bit positions to be cleared in the CSR. Any bit that is high in rs1 will
// cause the corresponding bit to be cleared in the CSR, if that CSR bit is writable.
def_EHelper(csrrc) 
{
    const word_t csrAddress = id_src2->imm;
    rtlreg_t* csrPtr = getCSRAddress(csrAddress);

    // Write it to integer register rd
    rtl_li(s, ddest, *csrPtr);
    
    if (isCSRWriteable(csrAddress) && s->isa.instr.CSR.rs1 != 0) 
    {
        rtl_mv(s, s0, dsrc1);
        rtl_not(s, s0, s0);
        rtl_and(s, csrPtr, csrPtr, s0);
    }
}
