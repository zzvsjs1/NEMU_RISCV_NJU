static inline bool riscv64_is_naturally_aligned(word_t addr, int len)
{
    return (addr & (word_t)(len - 1)) == 0;
}

static inline bool riscv64_check_load_alignment(Decode *s, word_t addr, int len)
{
    if (len > 1 && !riscv64_is_naturally_aligned(addr, len))
    {
        riscv64_raise_trap(s, RISCV64_CAUSE_LOAD_ADDR_MISALIGNED, addr);
        return false;
    }

    return true;
}

static inline bool riscv64_check_store_alignment(Decode *s, word_t addr, int len)
{
    if (len > 1 && !riscv64_is_naturally_aligned(addr, len))
    {
        riscv64_raise_trap(s, RISCV64_CAUSE_STORE_ADDR_MISALIGNED, addr);
        return false;
    }

    return true;
}

static inline word_t riscv64_load_addr(Decode *s)
{
    return *dsrc1 + (sword_t)id_src2->imm;
}

def_EHelper(lb)
{
    rtl_lm(s, ddest, dsrc1, (sword_t)id_src2->imm, 1);
    rtl_sext(s, ddest, ddest, 1);
}

def_EHelper(lh)
{
    if (!riscv64_check_load_alignment(s, riscv64_load_addr(s), 2))
        return;

    rtl_lm(s, ddest, dsrc1, (sword_t)id_src2->imm, 2);
    rtl_sext(s, ddest, ddest, 2);
}

def_EHelper(lw)
{
    if (!riscv64_check_load_alignment(s, riscv64_load_addr(s), 4))
        return;

    rtl_lm(s, ddest, dsrc1, (sword_t)id_src2->imm, 4);
    rtl_sext(s, ddest, ddest, 4);
}

def_EHelper(ld)
{
    if (!riscv64_check_load_alignment(s, riscv64_load_addr(s), 8))
        return;

    rtl_lm(s, ddest, dsrc1, (sword_t)id_src2->imm, 8);
}

def_EHelper(lbu)
{
    rtl_lm(s, ddest, dsrc1, (sword_t)id_src2->imm, 1);
    rtl_zext(s, ddest, ddest, 1);
}

def_EHelper(lhu)
{
    if (!riscv64_check_load_alignment(s, riscv64_load_addr(s), 2))
        return;

    rtl_lm(s, ddest, dsrc1, (sword_t)id_src2->imm, 2);
    rtl_zext(s, ddest, ddest, 2);
}

def_EHelper(lwu)
{
    if (!riscv64_check_load_alignment(s, riscv64_load_addr(s), 4))
        return;

    rtl_lm(s, ddest, dsrc1, (sword_t)id_src2->imm, 4);
    rtl_zext(s, ddest, ddest, 4);
}

def_EHelper(sb)
{
    rtl_sm(s, ddest, dsrc1, (sword_t)id_src2->imm, 1);
}

def_EHelper(sh)
{
    if (!riscv64_check_store_alignment(s, riscv64_load_addr(s), 2))
        return;

    rtl_sm(s, ddest, dsrc1, (sword_t)id_src2->imm, 2);
}

def_EHelper(sw)
{
    if (!riscv64_check_store_alignment(s, riscv64_load_addr(s), 4))
        return;

    rtl_sm(s, ddest, dsrc1, (sword_t)id_src2->imm, 4);
}

def_EHelper(sd)
{
    if (!riscv64_check_store_alignment(s, riscv64_load_addr(s), 8))
        return;

    rtl_sm(s, ddest, dsrc1, (sword_t)id_src2->imm, 8);
}
