def_EHelper(lui) 
{
    rtl_li(s, ddest, id_src1->imm);
}

// Start here

def_EHelper(addi)
{
    rtl_li(s, s0, id_src2->imm);
    rtl_sign_ext_pos(s, s0, s0, 11);

    // Add s0 and the original src register value to ddest.
    rtl_add(s, ddest, dsrc1, s0);
}

def_EHelper(auipc)
{
    // Adds this offset to the address of the AUIPC instruction, 
    // then places the result in register rd.
    rtl_addi(s, ddest, &s->pc, id_src1->simm);
}

