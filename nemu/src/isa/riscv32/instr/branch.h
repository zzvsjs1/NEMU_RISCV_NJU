// Branch instructions compare two registers.
// BEQ and BNE take the branch if registers rs1 and rs2
// are equal or unequal respectively.
static inline bool riscv32_branch_taken(RELOP_TYPE relop, word_t lhs, word_t rhs)
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

static inline void riscv32_branch_or_trap(Decode *s, RELOP_TYPE relop)
{
    rtl_sign_ext_pos(s, s0, s0, 12);
    rtl_add(s, s0, s0, &s->pc);

    if (riscv32_branch_taken(relop, *dsrc1, *dsrc2) && ((*s0 & 0x3u) != 0))
    {
        riscv32_raise_trap(s, RISCV32_CAUSE_INST_ADDR_MISALIGNED, *s0);
        return;
    }

    rtl_jrelop(s, relop, dsrc1, dsrc2, *s0);
}

def_EHelper(beq)
{
    riscv32_branch_or_trap(s, RELOP_EQ);
}

def_EHelper(bne)
{
    riscv32_branch_or_trap(s, RELOP_NE);
}

// BLT and BLTU take the branch if rs1 is less than rs2, using
// signed and unsigned comparison respectively
def_EHelper(blt)
{
    riscv32_branch_or_trap(s, RELOP_LT);
}

def_EHelper(bltu)
{
    riscv32_branch_or_trap(s, RELOP_LTU);
}

// BGE and BGEU take the branch if rs1 is greater
// than or equal to rs2, using signed and unsigned comparison respectively.
def_EHelper(bge)
{
    riscv32_branch_or_trap(s, RELOP_GE);
}

def_EHelper(bgeu)
{
    riscv32_branch_or_trap(s, RELOP_GEU);
}
