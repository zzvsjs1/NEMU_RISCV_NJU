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

#ifndef __DIFFTEST_DEF_H__
#define __DIFFTEST_DEF_H__

#include <stdbool.h>
#include <stdint.h>
#include <macro.h>
#include <generated/autoconf.h>

#define __EXPORT __attribute__((visibility("default")))
enum
{
    DIFFTEST_TO_DUT,
    DIFFTEST_TO_REF
};

#if defined(CONFIG_ISA_x86)
#define DIFFTEST_REG_SIZE (sizeof(uint32_t) * 9) // GPRs + pc
#elif defined(CONFIG_ISA_mips32)
#define DIFFTEST_REG_SIZE (sizeof(uint32_t) * 38) // GRPs + status + lo + hi + badvaddr + cause + pc
#elif defined(CONFIG_ISA_riscv)
#define RISCV_GPR_TYPE MUXDEF(CONFIG_RV64, uint64_t, uint32_t)
#define RISCV_GPR_NUM  MUXDEF(CONFIG_RVE , 16, 32)
typedef struct
{
    RISCV_GPR_TYPE gpr[RISCV_GPR_NUM];
    RISCV_GPR_TYPE pc;
    struct
    {
        RISCV_GPR_TYPE satp;
        RISCV_GPR_TYPE mstatus;
        RISCV_GPR_TYPE mtvec;
        RISCV_GPR_TYPE mscratch;
        RISCV_GPR_TYPE mepc;
        RISCV_GPR_TYPE mcause;
        RISCV_GPR_TYPE mtval;
    } csr;
    RISCV_GPR_TYPE prvi;
    bool INTR;
} riscv_difftest_state_t;
#define DIFFTEST_REG_SIZE (sizeof(riscv_difftest_state_t))
#elif defined(CONFIG_ISA_loongarch32r)
#define DIFFTEST_REG_SIZE (sizeof(uint32_t) * 33) // GPRs + pc
#else
#error Unsupport ISA
#endif

#endif
