// Branch instructions compare two registers. 
// BEQ and BNE take the branch if registers rs1 and rs2
// are equal or unequal respectively. 
def_EHelper(beq)
{
    rtl_sign_ext_pos(s, s0, s0, 11);
    rtl_add(s, s0, s0, &s->pc);
    rtl_jrelop(s, RELOP_EQ, dsrc1, dsrc2, *s0);
}

def_EHelper(bne)
{
    rtl_sign_ext_pos(s, s0, s0, 11);
    rtl_add(s, s0, s0, &s->pc);
    rtl_jrelop(s, RELOP_NE, dsrc1, dsrc2, *s0);
}


