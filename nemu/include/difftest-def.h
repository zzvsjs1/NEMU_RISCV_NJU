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
#ifndef RISCV_GPR_TYPE
#define RISCV_GPR_TYPE MUXDEF(CONFIG_RV64, uint64_t, uint32_t)
#endif

#ifndef RISCV_GPR_NUM
#define RISCV_GPR_NUM MUXDEF(CONFIG_RVE, 16, 32)
#endif

#ifdef CONFIG_RV64_FPU
/*
 * The architectural F/D register file always contains f0-f31.  This public
 * count names the corresponding DiffTest wire-layout array and is checked
 * against NEMU's private CPU-state count by the RISC-V DiffTest adapter.
 *
 * The DiffTest fcsr value carries the two implemented architectural fields:
 * fflags occupies bits [4:0] and frm occupies bits [7:5].  Bits [31:8] are
 * reserved and read as zero in NEMU, so deriving an eight-low-bit mask here
 * documents exactly which state a reference model may exchange with the DUT.
 */
#define RISCV_DIFFTEST_FPR_NUM 32
#define RISCV_DIFFTEST_FCSR_IMPLEMENTED_WIDTH 8
#define RISCV_DIFFTEST_FCSR_MASK \
    ((UINT32_C(1) << RISCV_DIFFTEST_FCSR_IMPLEMENTED_WIDTH) - 1)
#endif

typedef struct
{
    RISCV_GPR_TYPE gpr[RISCV_GPR_NUM];
    RISCV_GPR_TYPE pc;

    /*
     * This member order is a stable wire-layout contract shared with NEMU's
     * private CPU state; it is not sorted by numerical CSR address.  Adding or
     * moving a member changes DIFFTEST_REG_SIZE and therefore requires both
     * sides of every reference-model bridge to change together.
     */
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

    /*
     * `prvi` carries the architectural U=0, S=1, or M=3 privilege encoding;
     * value 2 is reserved.  `INTR` is NEMU's device-pending latch, not the
     * architectural mip CSR or a complete interrupt-controller state.
     */
    RISCV_GPR_TYPE prvi;
    bool INTR;

#ifdef CONFIG_RV64_FPU
    /*
     * This tail exactly mirrors the conditional RV64 NEMU CPU state.  RV32 and
     * every non-RISC-V DiffTest ABI remain unchanged when the feature is absent.
     */
    uint64_t fpr[RISCV_DIFFTEST_FPR_NUM];
    uint32_t fcsr;
#endif
} riscv_difftest_state_t;

#define DIFFTEST_REG_SIZE (sizeof(riscv_difftest_state_t))
#elif defined(CONFIG_ISA_loongarch32r)
#define DIFFTEST_REG_SIZE (sizeof(uint32_t) * 33) // GPRs + pc
#else
#error Unsupport ISA
#endif

#endif
