// Load and store instructions transfer a value between the registers and memory. Loads are encoded
// in the I-type format and stores are S-type. The effective address is obtained by adding register rs1
// to the sign-extended 12-bit offset. Loads copy a value from memory to register rd. Stores copy the
// value in register rs2 to memory.

// The LW instruction loads a 32-bit value from memory into rd. LH loads a 16-bit value from memory,
// then sign-extends to 32-bits before storing in rd. LHU loads a 16-bit value from memory but then
// zero extends to 32-bits before storing in rd. LB and LBU are defined analogously for 8-bit values.
// The SW, SH, and SB instructions store 32-bit, 16-bit, and 8-bit values from the low bits of register
// rs2 to memory.
def_EHelper(lb) 
{
    rtl_li(s, s0, id_src2->imm);
    rtl_sign_ext_pos(s, s0, s0, 11);

    rtl_lm(s, ddest, dsrc1, *s0, 1);
    rtl_sext(s, ddest, ddest, 1);
}

def_EHelper(lh)
{
    rtl_li(s, s0, id_src2->imm);
    rtl_sign_ext_pos(s, s0, s0, 11);

    rtl_lm(s, ddest, dsrc1, *s0, 2);
    rtl_sext(s, ddest, ddest, 2);
}

def_EHelper(lw) 
{
    rtl_li(s, s0, id_src2->imm);
    rtl_sign_ext_pos(s, s0, s0, 11);
    rtl_lm(s, ddest, dsrc1, *s0, 4);
}

def_EHelper(lbu)
{
    rtl_li(s, s0, id_src2->imm);
    rtl_sign_ext_pos(s, s0, s0, 11);

    rtl_lm(s, ddest, dsrc1, *s0, 1);
    rtl_zext(s, ddest, ddest, 1);
}

def_EHelper(lhu)
{
    rtl_li(s, s0, id_src2->imm);
    rtl_sign_ext_pos(s, s0, s0, 11);

    rtl_lm(s, ddest, dsrc1, *s0, 2);
    rtl_zext(s, ddest, ddest, 2);
}

// The SW, SH, and SB instructions store 32-bit, 16-bit, 
// and 8-bit values from the low bits of register
// rs2 to memory. 
def_EHelper(sb) 
{
    rtl_li(s, s0, id_src2->imm);
    rtl_sign_ext_pos(s, s0, s0, 11);

    rtl_sm(s, ddest, dsrc1, *s0, 1);
}

def_EHelper(sh) 
{
    rtl_li(s, s0, id_src2->imm);
    rtl_sign_ext_pos(s, s0, s0, 11);

    rtl_sm(s, ddest, dsrc1, *s0, 2);
}

def_EHelper(sw) 
{
    rtl_li(s, s0, id_src2->imm);
    rtl_sign_ext_pos(s, s0, s0, 11);

    rtl_sm(s, ddest, dsrc1, *s0, 4);
}
