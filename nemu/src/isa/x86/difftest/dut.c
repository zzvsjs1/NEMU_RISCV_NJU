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
#include <cpu/difftest.h>
#include "../local-include/reg.h"

/*
 * Arithmetic flags are deliberately not compared here: many x86 instructions
 * leave a subset of them undefined, and PA2 notes already allow simplified
 * EFLAGS modelling.  Keep checking the architectural always-on bit plus IF/DF,
 * which are stable control state for PA3 interrupts and string instructions.
 */
#define X86_DIFFTEST_EFLAGS_MASK ((uint32_t)((1u << 1) | (1u << 9) | (1u << 10)))

bool isa_difftest_checkregs(CPU_state *ref_r, vaddr_t pc)
{
    bool same = true;

    for (int i = R_EAX; i <= R_EDI; i++)
    {
        same &= difftest_check_reg(reg_name(i, 4), pc, ref_r->gpr[i]._32, reg_l(i));
    }

    same &= difftest_check_reg("eip", pc, ref_r->pc, cpu.pc);
    same &= difftest_check_reg("eflags", pc,
                               ref_r->eflags & X86_DIFFTEST_EFLAGS_MASK,
                               cpu.eflags & X86_DIFFTEST_EFLAGS_MASK);
    same &= difftest_check_reg("cs", pc, ref_r->cs, cpu.cs);
    return same;
}

void isa_difftest_attach()
{
}
