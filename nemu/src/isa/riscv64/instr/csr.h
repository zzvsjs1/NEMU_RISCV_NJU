static inline bool riscv64_csr_privilege_ok(word_t csr_addr)
{
    const word_t required_priv = (csr_addr >> 8) & 0x3u;
    return cpu.prvi >= required_priv;
}

static inline bool riscv64_csr_write_allowed(word_t csr_addr, bool will_write)
{
    return !will_write || isCSRWriteable(csr_addr);
}

static inline rtlreg_t *riscv64_get_csr_or_trap(Decode *s, word_t csr_addr, bool will_write)
{
    if (!isCSRImplemented(csr_addr) ||
        !riscv64_csr_privilege_ok(csr_addr) ||
        !riscv64_csr_write_allowed(csr_addr, will_write))
    {
        riscv64_raise_trap(s, RISCV64_CAUSE_ILLEGAL_INST, 0);
        return NULL;
    }

    return getCSRAddress(csr_addr);
}

static inline word_t riscv64_csr_normalise_write(word_t csr_addr, word_t value)
{
    return csr_addr == 0x300 ? riscv64_mstatus_normalise(value) : value;
}

static inline void riscv64_write_csr(Decode *s, word_t csr_addr, rtlreg_t *csr, word_t value)
{
    rtl_li(s, csr, riscv64_csr_normalise_write(csr_addr, value));
}

def_EHelper(ecall)
{
    riscv64_raise_trap(s, riscv64_ecall_cause_from_priv(cpu.prvi), 0);
}

def_EHelper(ebreak)
{
    riscv64_raise_trap(s, RISCV64_CAUSE_BREAKPOINT, 0);
}

def_EHelper(csrrw)
{
    const word_t csr_addr = id_src2->imm;
    rtlreg_t *csr = riscv64_get_csr_or_trap(s, csr_addr, true);
    if (csr == NULL)
        return;

    const rtlreg_t new_value = *dsrc1;

    if (s->isa.instr.CSR.rd != 0)
    {
        rtl_li(s, ddest, *csr);
    }

    riscv64_write_csr(s, csr_addr, csr, new_value);
}

def_EHelper(csrrs)
{
    const word_t csr_addr = id_src2->imm;
    const bool will_write = s->isa.instr.CSR.rs1 != 0;
    rtlreg_t *csr = riscv64_get_csr_or_trap(s, csr_addr, will_write);
    if (csr == NULL)
        return;

    rtl_li(s, ddest, *csr);

    if (will_write)
    {
        riscv64_write_csr(s, csr_addr, csr, *csr | *dsrc1);
    }
}

def_EHelper(csrrc)
{
    const word_t csr_addr = id_src2->imm;
    const bool will_write = s->isa.instr.CSR.rs1 != 0;
    rtlreg_t *csr = riscv64_get_csr_or_trap(s, csr_addr, will_write);
    if (csr == NULL)
        return;

    rtl_li(s, ddest, *csr);

    if (will_write)
    {
        riscv64_write_csr(s, csr_addr, csr, *csr & ~*dsrc1);
    }
}

def_EHelper(csrrwi)
{
    const word_t csr_addr = id_src2->imm;
    rtlreg_t *csr = riscv64_get_csr_or_trap(s, csr_addr, true);
    if (csr == NULL)
        return;

    if (s->isa.instr.CSR.rd != 0)
    {
        rtl_li(s, ddest, *csr);
    }

    riscv64_write_csr(s, csr_addr, csr, s->isa.instr.CSR.rs1);
}

def_EHelper(csrrsi)
{
    const word_t csr_addr = id_src2->imm;
    const bool will_write = s->isa.instr.CSR.rs1 != 0;
    rtlreg_t *csr = riscv64_get_csr_or_trap(s, csr_addr, will_write);
    if (csr == NULL)
        return;

    rtl_li(s, ddest, *csr);

    if (will_write)
    {
        riscv64_write_csr(s, csr_addr, csr, *csr | s->isa.instr.CSR.rs1);
    }
}

def_EHelper(csrrci)
{
    const word_t csr_addr = id_src2->imm;
    const bool will_write = s->isa.instr.CSR.rs1 != 0;
    rtlreg_t *csr = riscv64_get_csr_or_trap(s, csr_addr, will_write);
    if (csr == NULL)
        return;

    rtl_li(s, ddest, *csr);

    if (will_write)
    {
        riscv64_write_csr(s, csr_addr, csr, *csr & ~(word_t)s->isa.instr.CSR.rs1);
    }
}
