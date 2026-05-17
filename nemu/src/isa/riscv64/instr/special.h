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

def_EHelper(sfence_vma)
{
    const word_t mstatus_tvm = (word_t)1u << 20;

    /*
     * NEMU has no hardware TLB to flush, so the architectural fence is a no-op
     * after privilege checks.  U-mode may not execute SFENCE.VMA, and S-mode is
     * blocked when mstatus.TVM is set, matching the privileged architecture.
     */
    if (cpu.prvi == RISCV64_PRIV_U ||
        (cpu.prvi == RISCV64_PRIV_S && (cpu.csr.mstatus & mstatus_tvm) != 0))
    {
        riscv64_raise_trap(s, RISCV64_CAUSE_ILLEGAL_INST, 0);
    }
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
