static inline bool riscv64_branch_taken(RELOP_TYPE relop, word_t lhs, word_t rhs)
{
    switch (relop)
    {
    case RELOP_EQ:
        return lhs == rhs;
    case RELOP_NE:
        return lhs != rhs;
    case RELOP_LT:
        return (sword_t)lhs < (sword_t)rhs;
    case RELOP_GE:
        return (sword_t)lhs >= (sword_t)rhs;
    case RELOP_LTU:
        return lhs < rhs;
    case RELOP_GEU:
        return lhs >= rhs;
    default:
        return false;
    }
}

static inline void riscv64_branch_or_trap(Decode *s, RELOP_TYPE relop)
{
    rtl_sign_ext_pos(s, s0, s0, 12);
    rtl_add(s, s0, s0, &s->pc);

    if (riscv64_branch_taken(relop, *dsrc1, *dsrc2) && ((*s0 & 0x3u) != 0))
    {
        riscv64_raise_trap(s, RISCV64_CAUSE_INST_ADDR_MISALIGNED, *s0);
        return;
    }

    rtl_jrelop(s, relop, dsrc1, dsrc2, *s0);
}

def_EHelper(beq)
{
    riscv64_branch_or_trap(s, RELOP_EQ);
}

def_EHelper(bne)
{
    riscv64_branch_or_trap(s, RELOP_NE);
}

def_EHelper(blt)
{
    riscv64_branch_or_trap(s, RELOP_LT);
}

def_EHelper(bge)
{
    riscv64_branch_or_trap(s, RELOP_GE);
}

def_EHelper(bltu)
{
    riscv64_branch_or_trap(s, RELOP_LTU);
}

def_EHelper(bgeu)
{
    riscv64_branch_or_trap(s, RELOP_GEU);
}
