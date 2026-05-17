#include <isa-jit.h>

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
     * NEMU has no separate hardware TLB.  The only local translation cache here
     * belongs to the RV32 JIT, so a legal SFENCE.VMA only needs to drop that
     * private Sv32 state after the architectural privilege checks.
     */
    if (cpu.prvi == RISCV32_PRIV_U ||
        (cpu.prvi == RISCV32_PRIV_S && (cpu.csr.mstatus & mstatus_tvm) != 0))
    {
        riscv32_raise_trap(s, RISCV32_CAUSE_ILLEGAL_INST, 0);
        return;
    }

    isa_jit_flush_data_tlb();
}

def_EHelper(wfi)
{
    /*
     * This functional emulator does not model a sleeping hart.  Treat WFI as a
     * non-blocking hint, matching RV64, and keep DiffTest from expecting a
     * reference-side wait state.
     */
    difftest_skip_ref();
}
