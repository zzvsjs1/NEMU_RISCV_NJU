#ifndef __RISCV64_RTL_H__
#define __RISCV64_RTL_H__

#include <rtl/rtl.h>
#include <cpu/difftest.h>
#include "reg.h"

static inline void riscv64_raise_trap(Decode *s, word_t cause, word_t tval)
{
    /*
     * Instruction helpers use this after detecting a visible synchronous trap.
     * The helper redirects the dynamic next PC to mtvec and leaves normal
     * instruction writeback to the caller, so callers must invoke it before
     * committing any destination register or memory side effect.
     */
    rtlreg_t target = isa_raise_intr_tval(cause, s->pc, tval);
    rtl_li(s, s0, target);
    rtl_jr(s, s0);
    difftest_skip_ref();
}

#endif
