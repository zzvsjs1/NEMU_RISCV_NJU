def_EHelper(lui) 
{
    rtl_li(s, ddest, id_src1->imm);
}

// Page 18
// ADDI adds the sign-extended 12-bit immediate to register rs1. Arithmetic overflow is ignored and
// the result is simply the low XLEN bits of the result. ADDI rd, rs1, 0 is used to implement the MV
// rd, rs1 assembler pseudoinstruction.
def_EHelper(addi)
{
    // Load imm[11:0] to s0. 
    rtl_li(s, s0, id_src2->imm);

    // Extend imm[11:0] in s0.
    rtl_sign_ext_pos(s, s0, s0, 11);

    // Add s0 and the original src register value to ddest.
    rtl_add(s, ddest, dsrc1, s0);
}

// AUIPC (add upper immediate to pc) is used to build pc-relative addresses and uses the U-type
// format. AUIPC forms a 32-bit offset from the 20-bit U-immediate, filling in the lowest 12 bits with
// zeros, adds this offset to the address of the AUIPC instruction, then places the result in register
// rd.
def_EHelper(auipc)
{
    // Adds this offset to the address of the AUIPC instruction, 
    // then places the result in register rd.
    rtl_addi(s, ddest, &s->pc, id_src1->simm);
}

// SLTI (set less than immediate) places the value 1 in register rd if register rs1 is less than 
// the signextended immediate when both are treated as signed numbers, else 0 is written to rd. 
//
// SLTIU is similar but compares the values as unsigned numbers 
// (i.e., the immediate is first sign-extended to
// XLEN bits then treated as an unsigned number). Note, SLTIU rd, rs1, 1 sets rd to 1 if rs1 equals
// zero, otherwise sets rd to 0 (assembler pseudoinstruction SEQZ rd, rs).
def_EHelper(slti)
{
    rtl_li(s, s0, id_src2->imm);
    rtl_sign_ext_pos(s, s0, s0, 11);

    if (compareRegister(RELOP_LE, dsrc1, dsrc2))
    {
        rtl_li(s, ddest, 1);
        return;
    }

    rtl_li(s, ddest, 0);
}

def_EHelper(sltiu)
{
    rtl_li(s, s0, id_src2->imm);
    rtl_sign_ext_pos(s, s0, s0, 11);

    if (compareRegister(RELOP_LEU, dsrc1, s0))
    {
        rtl_li(s, ddest, 1);
        return;
    }

    rtl_li(s, ddest, 0);
}

def_EHelper(add)
{
    rtl_add(s, ddest, dsrc1, dsrc2);
}

def_EHelper(sub)
{
    rtl_sub(s, ddest, dsrc1, dsrc2);
}