static inline word_t rv64_sext32(uint32_t value)
{
    return (word_t)(int64_t)(int32_t)value;
}

static inline word_t rv64_sign_extend_u_imm(word_t imm)
{
    return (word_t)(int64_t)(int32_t)imm;
}

def_EHelper(lui)
{
    rtl_li(s, ddest, rv64_sign_extend_u_imm(id_src1->imm));
}

def_EHelper(auipc)
{
    rtl_li(s, ddest, s->pc + rv64_sign_extend_u_imm(id_src1->imm));
}

def_EHelper(addi)
{
    rtl_addi(s, ddest, dsrc1, (sword_t)id_src2->imm);
}

def_EHelper(slti)
{
    rtl_setrelopi(s, RELOP_LT, ddest, dsrc1, (sword_t)id_src2->imm);
}

def_EHelper(sltiu)
{
    rtl_setrelopi(s, RELOP_LTU, ddest, dsrc1, (sword_t)id_src2->imm);
}

def_EHelper(xori)
{
    rtl_xori(s, ddest, dsrc1, (sword_t)id_src2->imm);
}

def_EHelper(ori)
{
    rtl_ori(s, ddest, dsrc1, (sword_t)id_src2->imm);
}

def_EHelper(andi)
{
    rtl_andi(s, ddest, dsrc1, (sword_t)id_src2->imm);
}

def_EHelper(slli)
{
    rtl_slli(s, ddest, dsrc1, id_src2->imm & 0x3fu);
}

def_EHelper(srli)
{
    rtl_srli(s, ddest, dsrc1, id_src2->imm & 0x3fu);
}

def_EHelper(srai)
{
    rtl_srai(s, ddest, dsrc1, id_src2->imm & 0x3fu);
}

def_EHelper(addiw)
{
    rtl_li(s, ddest, rv64_sext32((uint32_t)(*dsrc1 + (sword_t)id_src2->imm)));
}

def_EHelper(slliw)
{
    rtl_li(s, ddest, rv64_sext32((uint32_t)*dsrc1 << (id_src2->imm & 0x1fu)));
}

def_EHelper(srliw)
{
    rtl_li(s, ddest, rv64_sext32((uint32_t)*dsrc1 >> (id_src2->imm & 0x1fu)));
}

def_EHelper(sraiw)
{
    rtl_li(s, ddest, rv64_sext32((uint32_t)((int32_t)*dsrc1 >> (id_src2->imm & 0x1fu))));
}

def_EHelper(add)
{
    rtl_add(s, ddest, dsrc1, dsrc2);
}

def_EHelper(sub)
{
    rtl_sub(s, ddest, dsrc1, dsrc2);
}

def_EHelper(sll)
{
    rtl_andi(s, s0, dsrc2, 0x3f);
    rtl_sll(s, ddest, dsrc1, s0);
}

def_EHelper(slt)
{
    rtl_setrelop(s, RELOP_LT, ddest, dsrc1, dsrc2);
}

def_EHelper(sltu)
{
    rtl_setrelop(s, RELOP_LTU, ddest, dsrc1, dsrc2);
}

def_EHelper(xor)
{
    rtl_xor(s, ddest, dsrc1, dsrc2);
}

def_EHelper(srl)
{
    rtl_andi(s, s0, dsrc2, 0x3f);
    rtl_srl(s, ddest, dsrc1, s0);
}

def_EHelper(sra)
{
    rtl_andi(s, s0, dsrc2, 0x3f);
    rtl_sra(s, ddest, dsrc1, s0);
}

def_EHelper(or)
{
    rtl_or(s, ddest, dsrc1, dsrc2);
}

def_EHelper(and)
{
    rtl_and(s, ddest, dsrc1, dsrc2);
}

def_EHelper(addw)
{
    rtl_li(s, ddest, rv64_sext32((uint32_t)(*dsrc1 + *dsrc2)));
}

def_EHelper(subw)
{
    rtl_li(s, ddest, rv64_sext32((uint32_t)(*dsrc1 - *dsrc2)));
}

def_EHelper(sllw)
{
    rtl_li(s, ddest, rv64_sext32((uint32_t)*dsrc1 << (*dsrc2 & 0x1fu)));
}

def_EHelper(srlw)
{
    rtl_li(s, ddest, rv64_sext32((uint32_t)*dsrc1 >> (*dsrc2 & 0x1fu)));
}

def_EHelper(sraw)
{
    rtl_li(s, ddest, rv64_sext32((uint32_t)((int32_t)*dsrc1 >> (*dsrc2 & 0x1fu))));
}

def_EHelper(mul)
{
    rtl_mulu_lo(s, ddest, dsrc1, dsrc2);
}

def_EHelper(mulh)
{
    rtl_muls_hi(s, ddest, dsrc1, dsrc2);
}

def_EHelper(mulhsu)
{
    rtl_mulsu_hi(s, ddest, dsrc1, dsrc2);
}

def_EHelper(mulhu)
{
    rtl_mulu_hi(s, ddest, dsrc1, dsrc2);
}

def_EHelper(div)
{
    const sword_t dividend = (sword_t)*dsrc1;
    const sword_t divisor = (sword_t)*dsrc2;

    if (divisor == 0)
    {
        rtl_li(s, ddest, ~(word_t)0);
    }
    else if (dividend == INT64_MIN && divisor == -1)
    {
        rtl_mv(s, ddest, dsrc1);
    }
    else
    {
        rtl_divs_q(s, ddest, dsrc1, dsrc2);
    }
}

def_EHelper(divu)
{
    if (*dsrc2 == 0)
    {
        rtl_li(s, ddest, ~(word_t)0);
    }
    else
    {
        rtl_divu_q(s, ddest, dsrc1, dsrc2);
    }
}

def_EHelper(rem)
{
    const sword_t dividend = (sword_t)*dsrc1;
    const sword_t divisor = (sword_t)*dsrc2;

    if (divisor == 0)
    {
        rtl_mv(s, ddest, dsrc1);
    }
    else if (dividend == INT64_MIN && divisor == -1)
    {
        rtl_li(s, ddest, 0);
    }
    else
    {
        rtl_divs_r(s, ddest, dsrc1, dsrc2);
    }
}

def_EHelper(remu)
{
    if (*dsrc2 == 0)
    {
        rtl_mv(s, ddest, dsrc1);
    }
    else
    {
        rtl_divu_r(s, ddest, dsrc1, dsrc2);
    }
}

def_EHelper(mulw)
{
    rtl_li(s, ddest, rv64_sext32((uint32_t)((uint32_t)*dsrc1 * (uint32_t)*dsrc2)));
}

def_EHelper(divw)
{
    const int32_t dividend = (int32_t)*dsrc1;
    const int32_t divisor = (int32_t)*dsrc2;

    if (divisor == 0)
    {
        rtl_li(s, ddest, ~(word_t)0);
    }
    else if (dividend == INT32_MIN && divisor == -1)
    {
        rtl_li(s, ddest, rv64_sext32((uint32_t)dividend));
    }
    else
    {
        rtl_li(s, ddest, rv64_sext32((uint32_t)(dividend / divisor)));
    }
}

def_EHelper(divuw)
{
    const uint32_t dividend = (uint32_t)*dsrc1;
    const uint32_t divisor = (uint32_t)*dsrc2;

    if (divisor == 0)
    {
        rtl_li(s, ddest, ~(word_t)0);
    }
    else
    {
        rtl_li(s, ddest, rv64_sext32(dividend / divisor));
    }
}

def_EHelper(remw)
{
    const int32_t dividend = (int32_t)*dsrc1;
    const int32_t divisor = (int32_t)*dsrc2;

    if (divisor == 0)
    {
        rtl_li(s, ddest, rv64_sext32((uint32_t)dividend));
    }
    else if (dividend == INT32_MIN && divisor == -1)
    {
        rtl_li(s, ddest, 0);
    }
    else
    {
        rtl_li(s, ddest, rv64_sext32((uint32_t)(dividend % divisor)));
    }
}

def_EHelper(remuw)
{
    const uint32_t dividend = (uint32_t)*dsrc1;
    const uint32_t divisor = (uint32_t)*dsrc2;

    if (divisor == 0)
    {
        rtl_li(s, ddest, rv64_sext32(dividend));
    }
    else
    {
        rtl_li(s, ddest, rv64_sext32(dividend % divisor));
    }
}
