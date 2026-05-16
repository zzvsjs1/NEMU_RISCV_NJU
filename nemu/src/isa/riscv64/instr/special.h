def_EHelper(inv)
{
    riscv64_raise_trap(s, RISCV64_CAUSE_ILLEGAL_INST, 0);
}

def_EHelper(nemu_trap)
{
    rtl_hostcall(s, HOSTCALL_EXIT, NULL, &gpr(10), NULL, 0); // gpr(10) is $a0
}

def_EHelper(fence)
{
}

def_EHelper(fence_i)
{
}

def_EHelper(wfi)
{
    /*
     * NEMU currently runs as a functional interpreter.  WFI only hints that the
     * hart may wait for an interrupt; treating it as a no-op keeps simple
     * kernels moving without modelling host-side sleeping.  Spike may model the
     * wait state differently, so keep DiffTest aligned by copying this post-WFI
     * state into the reference.
     */
    difftest_skip_ref();
}
