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

