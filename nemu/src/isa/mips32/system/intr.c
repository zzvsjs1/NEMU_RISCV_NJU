/***************************************************************************************
 * Copyright (c) 2014-2024 Zihao Yu, Nanjing University
 *
 * NEMU is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 *
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 *
 * See the Mulan PSL v2 for more details.
 ***************************************************************************************/

#include <isa.h>
#include <utils.h>

jmp_buf mips32_exception_env;
bool mips32_exception_env_valid = false;
vaddr_t mips32_exception_target = 0;

static vaddr_t mips32_raise_exception(word_t exception, vaddr_t epc,
                                      bool refill)
{
    const bool old_exl = (cpu.status & MIPS32_STATUS_EXL) != 0;

    /*
     * MIPS records a synchronous exception at the faulting instruction.  EPC
     * is left untouched for a nested exception while EXL is already set, which
     * preserves the address needed by the outer handler.
     */
    if (!old_exl)
    {
        cpu.epc = epc;
        /* NEMU omits delay slots, so an exception can never set Cause.BD. */
        cpu.cause &= ~(1u << 31);
    }

    cpu.cause = (cpu.cause & ~(0x1fu << 2)) |
                ((exception & 0x1fu) << 2);
    cpu.status |= MIPS32_STATUS_EXL;

    /*
     * A first-level TLB refill uses the dedicated 0x80000000 entry.  A nested
     * miss must instead use the general vector so a broken handler cannot
     * recursively re-enter a refill path while preserving the outer EPC.
     */
    const vaddr_t vector = refill && !old_exl
                               ? 0x80000000u
                               : MIPS32_EXC_VECTOR;

    /* ETrace observes only fully committed architectural exception state. */
    etrace_exception(exception, cpu.epc, vector, cpu.cause, cpu.status);
    return vector;
}

vaddr_t isa_raise_intr(word_t NO, vaddr_t epc)
{
    return mips32_raise_exception(NO, epc, false);
}

void mips32_raise_tlb_exception(void)
{
    Assert(mips32_exception_env_valid,
           "MIPS32 TLB exception outside instruction: pc=" FMT_WORD
           " badvaddr=" FMT_WORD,
           cpu.pc, cpu.badvaddr);

    word_t exception;

    switch (cpu.tlb_fault)
    {
    case MIPS32_TLB_FAULT_REFILL_LOAD:
        exception = 2;
        break;
    case MIPS32_TLB_FAULT_REFILL_STORE:
        exception = 3;
        break;
    default:
        panic("MIPS32 missing TLB refill classification");
    }

    /* Consume the miss before the refill handler retries the instruction. */
    cpu.tlb_fault = MIPS32_TLB_FAULT_NONE;
    mips32_exception_target =
        mips32_raise_exception(exception, cpu.pc, true);
    longjmp(mips32_exception_env, 1);
}

word_t isa_query_intr()
{
    /*
     * The platform connects one periodic timer directly to the MIPS interrupt
     * input, so its interrupt exception code is zero. The pending edge remains
     * latched while software masks interrupts; accepting it consumes exactly
     * that edge.
     *
     * The device model does not implement the per-source Status.IM/Cause.IP
     * masks described by the architecture. The global IE/EXL/ERL gates remain
     * active.
     */
    const bool interrupts_enabled =
        (cpu.status & MIPS32_STATUS_IE) != 0u &&
        (cpu.status & (MIPS32_STATUS_EXL | MIPS32_STATUS_ERL)) == 0u;

    if (cpu.INTR && interrupts_enabled)
    {
        cpu.INTR = false;
        return 0u;
    }

    return INTR_EMPTY;
}
