#ifndef __RISCV32_RTL_H__
#define __RISCV32_RTL_H__

#include <rtl/rtl.h>
#include <cpu/difftest.h>
#include "reg.h"

static inline void riscv32_raise_trap(Decode *s, word_t cause, word_t tval)
{
    /*
     * The instruction helpers own architectural trap delivery.  They set dnpc to
     * mtvec through the common trap helper and then return without performing the
     * trapping instruction's normal architectural writeback.
     */
    rtlreg_t target = isa_raise_intr_tval(cause, s->pc, tval);
    rtl_li(s, s0, target);
    rtl_jr(s, s0);
    /*
     * The reference has already been moved to the trap entry.  Copy DUT state on
     * the following DiffTest hook instead of letting Spike execute one handler
     * instruction ahead of NEMU.
     */
    difftest_skip_ref();
}

#endif
