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
#include <stdio.h>
#include <string.h>

#define REG_FMT ("%-10s " FMT_WORD "%-5s" FMT_DECIMAL_WORD "%-5s" FMT_DECIMAL_WORD_SIGN "\n")

const char *regs[] = {"$0", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
                      "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "s8", "ra"};

/* Accept both monitor names (a0) and conventional assembly names ($a0). */
static const char *strip_dollar(const char *name)
{
    return name != NULL && name[0] == '$' ? name + 1 : name;
}

/*
 * Resolve an ABI name or a decimal architectural index.  Numeric aliases are
 * useful when comparing a trace against the instruction manual, which labels
 * operands as GPR[0] through GPR[31].
 */
static int reg_name_to_index(const char *raw_name)
{
    const char *name = strip_dollar(raw_name);

    if (name == NULL || name[0] == '\0')
    {
        return -1;
    }

    if (strcmp(name, "0") == 0)
    {
        return 0;
    }

    for (int i = 1; i < 32; i++)
    {
        if (strcmp(name, regs[i]) == 0)
        {
            return i;
        }
    }

    int index = 0;

    for (const char *p = name; *p != '\0'; p++)
    {
        if (*p < '0' || *p > '9')
        {
            return -1;
        }

        index = index * 10 + (*p - '0');

        if (index >= 32)
        {
            return -1;
        }
    }

    return index;
}

static word_t *special_reg_address(const char *raw_name)
{
    const char *name = strip_dollar(raw_name);

    if (name == NULL)
    {
        return NULL;
    }

    if (strcmp(name, "pc") == 0)
        return &cpu.pc;
    if (strcmp(name, "hi") == 0)
        return &cpu.hi;
    if (strcmp(name, "lo") == 0)
        return &cpu.lo;
    if (strcmp(name, "index") == 0)
        return &cpu.index;
    if (strcmp(name, "entrylo0") == 0)
        return &cpu.entrylo0;
    if (strcmp(name, "entrylo1") == 0)
        return &cpu.entrylo1;
    if (strcmp(name, "status") == 0)
        return &cpu.status;
    if (strcmp(name, "cause") == 0)
        return &cpu.cause;
    if (strcmp(name, "badvaddr") == 0)
        return &cpu.badvaddr;
    if (strcmp(name, "entryhi") == 0)
        return &cpu.entryhi;
    if (strcmp(name, "epc") == 0)
        return &cpu.epc;

    return NULL;
}

void isa_reg_display()
{
    printf("\n");

    for (int i = 0; i < 32; i++)
    {
        const word_t value = gpr(i);
        printf(REG_FMT, reg_name(i), value, " ", value, " ", (sword_t)value);
    }

    const struct
    {
        const char *name;
        word_t value;
    } special[] = {
        {"pc", cpu.pc},
        {"hi", cpu.hi},
        {"lo", cpu.lo},
        {"index", cpu.index},
        {"entrylo0", cpu.entrylo0},
        {"entrylo1", cpu.entrylo1},
        {"status", cpu.status},
        {"cause", cpu.cause},
        {"badvaddr", cpu.badvaddr},
        {"entryhi", cpu.entryhi},
        {"epc", cpu.epc},
    };

    printf("\n");

    for (size_t i = 0; i < ARRLEN(special); i++)
    {
        const word_t value = special[i].value;
        printf(REG_FMT, special[i].name, value, " ", value, " ", (sword_t)value);
    }

    printf("\n");
}

word_t isa_reg_str2val(const char *s, bool *success)
{
    if (success == NULL)
    {
        return (word_t)-1;
    }

    const int index = reg_name_to_index(s);

    if (index >= 0)
    {
        *success = true;
        return gpr(index);
    }

    word_t *special = special_reg_address(s);

    if (special != NULL)
    {
        *success = true;
        return *special;
    }

    *success = false;
    PRI_ERR("Unknown MIPS32 register %s.\n", s == NULL ? "(null)" : s);
    return (word_t)-1;
}

void isa_set_reg_val(const char *name, const word_t value)
{
    const int index = reg_name_to_index(name);

    if (index >= 0)
    {
        /* GPR[0] is hard-wired to zero; debugger writes must not change it. */
        if (index != 0)
        {
            gpr(index) = value;
        }

        return;
    }

    word_t *special = special_reg_address(name);

    if (special != NULL)
    {
        *special = value;
        return;
    }

    PRI_ERR("Failed to set unknown MIPS32 register %s.\n", name == NULL ? "(null)" : name);
}
