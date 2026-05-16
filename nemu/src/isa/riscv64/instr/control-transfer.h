def_EHelper(jal)
{
    rtl_li(s, s0, id_src1->imm);
    rtl_sign_ext_pos(s, s0, s0, 20);
    rtl_add(s, s0, &s->pc, s0);

    if ((*s0 & 0x3u) != 0)
    {
        riscv64_raise_trap(s, RISCV64_CAUSE_INST_ADDR_MISALIGNED, *s0);
        return;
    }

    rtl_addi(s, ddest, &s->pc, 4);
    rtl_jr(s, s0);
}

def_EHelper(jalr)
{
    rtl_addi(s, s0, dsrc1, (sword_t)id_src2->imm);
    rtl_andi(s, s0, s0, ~(word_t)1u);

    if ((*s0 & 0x3u) != 0)
    {
        riscv64_raise_trap(s, RISCV64_CAUSE_INST_ADDR_MISALIGNED, *s0);
        return;
    }

    rtl_addi(s, ddest, &s->pc, 4);
    rtl_jr(s, s0);
}

def_EHelper(mret)
{
    if (cpu.prvi != RISCV64_PRIV_M)
    {
        riscv64_raise_trap(s, RISCV64_CAUSE_ILLEGAL_INST, 0);
        return;
    }

    word_t mstatus = cpu.csr.mstatus;
    const word_t mpp = (mstatus >> 11) & 0x3u;
    const word_t mpie = (mstatus >> 7) & 0x1u;

    if (mpp == 0x2u)
    {
        riscv64_raise_trap(s, RISCV64_CAUSE_ILLEGAL_INST, 0);
        return;
    }

    mstatus &= ~((word_t)0x3u << 11);
    mstatus = (mstatus & ~((word_t)1u << 3)) | (mpie << 3);
    mstatus |= ((word_t)1u << 7);

    if (mpp != RISCV64_PRIV_M)
    {
        mstatus &= ~((word_t)1u << 17);
    }

    cpu.csr.mstatus = riscv64_mstatus_normalise(mstatus);
    cpu.prvi = mpp;
    rtl_jr(s, &cpu.csr.mepc);
}
