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
#include "local-include/reg.h"

const char *regsl[] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
const char *regsw[] = {"ax", "cx", "dx", "bx", "sp", "bp", "si", "di"};
const char *regsb[] = {"al", "cl", "dl", "bl", "ah", "ch", "dh", "bh"};

void reg_test()
{
    word_t sample[8];
    word_t pc_sample = rand();
    cpu.pc = pc_sample;

    int i;
    for (i = R_EAX; i <= R_EDI; i++)
    {
        sample[i] = rand();
        reg_l(i) = sample[i];
        assert(reg_w(i) == (sample[i] & 0xffff));
    }

    assert(reg_b(R_AL) == (sample[R_EAX] & 0xff));
    assert(reg_b(R_AH) == ((sample[R_EAX] >> 8) & 0xff));
    assert(reg_b(R_BL) == (sample[R_EBX] & 0xff));
    assert(reg_b(R_BH) == ((sample[R_EBX] >> 8) & 0xff));
    assert(reg_b(R_CL) == (sample[R_ECX] & 0xff));
    assert(reg_b(R_CH) == ((sample[R_ECX] >> 8) & 0xff));
    assert(reg_b(R_DL) == (sample[R_EDX] & 0xff));
    assert(reg_b(R_DH) == ((sample[R_EDX] >> 8) & 0xff));

    assert(sample[R_EAX] == cpu.eax);
    assert(sample[R_ECX] == cpu.ecx);
    assert(sample[R_EDX] == cpu.edx);
    assert(sample[R_EBX] == cpu.ebx);
    assert(sample[R_ESP] == cpu.esp);
    assert(sample[R_EBP] == cpu.ebp);
    assert(sample[R_ESI] == cpu.esi);
    assert(sample[R_EDI] == cpu.edi);

    assert(pc_sample == cpu.pc);
}

void isa_reg_display()
{
    for (int i = R_EAX; i <= R_EDI; i++)
    {
        printf("%-3s " FMT_WORD "\n", regsl[i], reg_l(i));
    }
    printf("%-6s " FMT_WORD "\n", "eflags", cpu.eflags);
    printf("%-6s " FMT_WORD "\n", "eip", cpu.pc);
    printf("%-6s " FMT_WORD "\n", "cs", cpu.cs);
    printf("%-6s " FMT_WORD "\n", "ds", cpu.ds);
    printf("%-6s " FMT_WORD "\n", "es", cpu.es);
    printf("%-6s " FMT_WORD "\n", "ss", cpu.ss);
    printf("%-6s " FMT_WORD "\n", "cr0", cpu.cr0);
    printf("%-6s " FMT_WORD "\n", "cr2", cpu.cr2);
    printf("%-6s " FMT_WORD "\n", "cr3", cpu.cr3);
    printf("%-6s " FMT_WORD "\n", "cr4", cpu.cr4);
    printf("%-6s " FMT_WORD "\n", "pferr", cpu.pf_errcode);
    printf("%-6s " FMT_WORD " limit %#x\n", "idtr", cpu.idtr_base, cpu.idtr_limit);
    printf("%-6s " FMT_WORD " limit %#x\n", "gdtr", cpu.gdtr_base, cpu.gdtr_limit);
    printf("%-6s %#x base " FMT_WORD " limit %#x\n", "tr", cpu.tr, cpu.tss_base, cpu.tss_limit);
}

word_t isa_reg_str2val(const char *s, bool *success)
{
    for (int i = R_EAX; i <= R_EDI; i++)
    {
        if (strcmp(s, regsl[i]) == 0)
        {
            *success = true;
            return reg_l(i);
        }
        if (strcmp(s, regsw[i]) == 0)
        {
            *success = true;
            return reg_w(i);
        }
    }

    for (int i = R_AL; i <= R_BH; i++)
    {
        if (strcmp(s, regsb[i]) == 0)
        {
            *success = true;
            return reg_b(i);
        }
    }

    if (strcmp(s, "eflags") == 0)
    {
        *success = true;
        return cpu.eflags;
    }

    if (strcmp(s, "eip") == 0 || strcmp(s, "pc") == 0)
    {
        *success = true;
        return cpu.pc;
    }

    if (strcmp(s, "cs") == 0)
    {
        *success = true;
        return cpu.cs;
    }

    if (strcmp(s, "ds") == 0)
    {
        *success = true;
        return cpu.ds;
    }

    if (strcmp(s, "es") == 0)
    {
        *success = true;
        return cpu.es;
    }

    if (strcmp(s, "ss") == 0)
    {
        *success = true;
        return cpu.ss;
    }

    if (strcmp(s, "cr0") == 0)
    {
        *success = true;
        return cpu.cr0;
    }

    if (strcmp(s, "cr2") == 0)
    {
        *success = true;
        return cpu.cr2;
    }

    if (strcmp(s, "cr3") == 0)
    {
        *success = true;
        return cpu.cr3;
    }

    if (strcmp(s, "cr4") == 0)
    {
        *success = true;
        return cpu.cr4;
    }

    if (strcmp(s, "pf_errcode") == 0 || strcmp(s, "pferr") == 0)
    {
        *success = true;
        return cpu.pf_errcode;
    }

    if (strcmp(s, "idtr_base") == 0)
    {
        *success = true;
        return cpu.idtr_base;
    }

    if (strcmp(s, "idtr_limit") == 0)
    {
        *success = true;
        return cpu.idtr_limit;
    }

    if (strcmp(s, "gdtr_base") == 0)
    {
        *success = true;
        return cpu.gdtr_base;
    }

    if (strcmp(s, "gdtr_limit") == 0)
    {
        *success = true;
        return cpu.gdtr_limit;
    }

    if (strcmp(s, "tr") == 0)
    {
        *success = true;
        return cpu.tr;
    }

    if (strcmp(s, "tss_base") == 0)
    {
        *success = true;
        return cpu.tss_base;
    }

    if (strcmp(s, "tss_limit") == 0)
    {
        *success = true;
        return cpu.tss_limit;
    }

    *success = false;
    return 0;
}

void isa_set_reg_val(const char *name, const word_t val)
{
    for (int i = R_EAX; i <= R_EDI; i++)
    {
        if (strcmp(name, regsl[i]) == 0)
        {
            reg_l(i) = val;
            return;
        }
        if (strcmp(name, regsw[i]) == 0)
        {
            reg_w(i) = val;
            return;
        }
    }

    for (int i = R_AL; i <= R_BH; i++)
    {
        if (strcmp(name, regsb[i]) == 0)
        {
            reg_b(i) = val;
            return;
        }
    }

    if (strcmp(name, "eflags") == 0)
    {
        cpu.eflags = val;
        return;
    }

    if (strcmp(name, "eip") == 0 || strcmp(name, "pc") == 0)
    {
        cpu.pc = val;
        return;
    }

    if (strcmp(name, "cs") == 0)
    {
        cpu.cs = val;
        return;
    }

    if (strcmp(name, "ds") == 0)
    {
        cpu.ds = val;
        return;
    }

    if (strcmp(name, "es") == 0)
    {
        cpu.es = val;
        return;
    }

    if (strcmp(name, "ss") == 0)
    {
        cpu.ss = val;
        return;
    }

    if (strcmp(name, "cr0") == 0)
    {
        cpu.cr0 = val;
        return;
    }

    if (strcmp(name, "cr2") == 0)
    {
        cpu.cr2 = val;
        return;
    }

    if (strcmp(name, "cr3") == 0)
    {
        cpu.cr3 = val;
        return;
    }

    if (strcmp(name, "cr4") == 0)
    {
        cpu.cr4 = val;
        return;
    }

    if (strcmp(name, "pf_errcode") == 0 || strcmp(name, "pferr") == 0)
    {
        cpu.pf_errcode = val;
        return;
    }

    if (strcmp(name, "idtr_base") == 0)
    {
        cpu.idtr_base = val;
        return;
    }

    if (strcmp(name, "idtr_limit") == 0)
    {
        cpu.idtr_limit = val;
        return;
    }

    if (strcmp(name, "gdtr_base") == 0)
    {
        cpu.gdtr_base = val;
        return;
    }

    if (strcmp(name, "gdtr_limit") == 0)
    {
        cpu.gdtr_limit = val;
        return;
    }

    if (strcmp(name, "tr") == 0)
    {
        cpu.tr = val;
        return;
    }

    if (strcmp(name, "tss_base") == 0)
    {
        cpu.tss_base = val;
        return;
    }

    if (strcmp(name, "tss_limit") == 0)
    {
        cpu.tss_limit = val;
        return;
    }

    printf("Unknown register: %s\n", name);
}
