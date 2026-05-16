def_EHelper(inv)
{
    /*
     * A reserved or unsupported encoding is an illegal-instruction exception.
     * mtval may legally be zero on this implementation; the handler can still
     * inspect mepc if it needs the original instruction bits.
     */
    riscv32_raise_trap(s, RISCV32_CAUSE_ILLEGAL_INST, 0);
}

def_EHelper(nemu_trap)
{
    /*
     * The built-in trap instruction reports the conventional RISC-V return
     * value register a0 to the host.  The simulator exit path decides whether
     * that value means HIT GOOD or HIT BAD.
     */
    rtl_hostcall(s, HOSTCALL_EXIT, NULL, &gpr(10), NULL, 0); // gpr(10) is $a0
}
