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

// BLT and BLTU take the branch if rs1 is less than rs2, using
// signed and unsigned comparison respectively
def_EHelper(blt)
{
    rtl_sign_ext_pos(s, s0, s0, 11);
    rtl_add(s, s0, s0, &s->pc);
    rtl_jrelop(s, RELOP_LT, dsrc1, dsrc2, *s0);
}

def_EHelper(bltu)
{    
    rtl_sign_ext_pos(s, s0, s0, 11);
    rtl_add(s, s0, s0, &s->pc);
    rtl_jrelop(s, RELOP_LTU, dsrc1, dsrc2, *s0);
}

// BGE and BGEU take the branch if rs1 is greater
// than or equal to rs2, using signed and unsigned comparison respectively.
def_EHelper(bge)
{
    rtl_sign_ext_pos(s, s0, s0, 11);
    rtl_add(s, s0, s0, &s->pc);
    rtl_jrelop(s, RELOP_GE, dsrc1, dsrc2, *s0);
}

def_EHelper(bgeu)
{
    rtl_sign_ext_pos(s, s0, s0, 11);
    rtl_add(s, s0, s0, &s->pc);
    rtl_jrelop(s, RELOP_GEU, dsrc1, dsrc2, *s0);
}

