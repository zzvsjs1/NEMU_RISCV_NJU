// Jump and link
// PAGE: 20
// The offset is sign-extended and added to the address of the
// jump instruction to form the jump target address
def_EHelper(jal)
{
    // Save next instuctions address to rd, and set new insturction address to pc.

    // Store next inst to ddest.
    rtl_addi(s, ddest, &s->pc, sizeof(rtlreg_t));

    // Compute the inst address jump to.
    rtl_li(s, s0, id_src1->imm);
    rtl_add(s, s0, s0, &s->pc);

    rtl_jr(s, s0);
}

// Jump and link register
// Page: 
// The indirect jump instruction JALR (jump and link register) uses the I-type encoding. The target
// address is obtained by adding the sign-extended 12-bit I-immediate to the register rs1, then setting
// the least-significant bit of the result to zero. The address of the instruction following the jump
// (pc+4) is written to register rd. Register x0 can be used as the destination if the result is not
// required.
def_EHelper(jalr)
{
    // Store the next instuction(pc + 4) to ddest(rd).
    rtl_addi(s, ddest, &s->pc, sizeof(rtlreg_t));

    // Adding sign-extended 12-bit I-immediate to the register rs1.
    
    // s0 = imm[0:11].
    rtl_li(s, s0, id_src2->imm);

    // sign ext imm[11:0] in s0.
    rtl_sign_ext_pos(s, s0, s0, 12 - 1);

    // rd = src_1(rs1) (NOTE: The original register!) + s0(imm[11:0])
    rtl_add(s, s0, s0, dsrc1);

    // Jump to *s0.
    rtl_jr(s, s0);
}
