def_EHelper(inv)
{
    /*
     * Invalid instructions leave architectural state changes to the hostcall
     * handler.  Keeping the helper tiny avoids accidentally advancing control
     * flow differently from other decode failures.
     */
    rtl_hostcall(s, HOSTCALL_INV, NULL, NULL, NULL, 0);
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
