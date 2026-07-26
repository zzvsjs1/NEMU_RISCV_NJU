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

#ifndef __ISA_MIPS32_H__
#define __ISA_MIPS32_H__

#include <common.h>

#define MIPS32_TLB_NR 16u
#define MIPS32_INDEX_P 0x80000000u
#define MIPS32_ENTRYHI_VPN2_MASK 0xffffe000u
#define MIPS32_ENTRYLO_V 0x00000002u
#define MIPS32_ENTRYLO_D 0x00000004u
#define MIPS32_ENTRYLO_PFN_MASK 0x03ffffc0u

typedef struct
{
    word_t entryhi;
    word_t entrylo0;
    word_t entrylo1;
} mips32_TLB_entry;

/* A miss records whether the instruction was loading or storing. */
typedef enum
{
    MIPS32_TLB_FAULT_NONE,
    MIPS32_TLB_FAULT_REFILL_LOAD,
    MIPS32_TLB_FAULT_REFILL_STORE,
} mips32_TLB_fault;

typedef struct
{
    word_t gpr[32];
    word_t status;
    word_t lo;
    word_t hi;
    word_t badvaddr;
    word_t cause;
    vaddr_t pc;

    /* EPC is maintained separately from the general register state. */
    word_t epc;
    bool INTR;

    /* Software-managed MIPS32 TLB state. */
    word_t index;
    word_t entrylo0;
    word_t entrylo1;
    word_t entryhi;
    mips32_TLB_entry tlb[MIPS32_TLB_NR];
    uint32_t tlbwr_next;
    mips32_TLB_fault tlb_fault;
} mips32_CPU_state;

enum
{
    MIPS32_CP0_INDEX = 0,
    MIPS32_CP0_ENTRYLO0 = 2,
    MIPS32_CP0_ENTRYLO1 = 3,
    MIPS32_CP0_BADVADDR = 8,
    MIPS32_CP0_ENTRYHI = 10,
    MIPS32_CP0_STATUS = 12,
    MIPS32_CP0_CAUSE = 13,
    MIPS32_CP0_EPC = 14,
};

enum
{
    MIPS32_EXC_SYS = 8,
    MIPS32_EXC_OV = 12,
    MIPS32_EXC_TRAP = 13,
};

#define MIPS32_STATUS_IE (1u << 0)
#define MIPS32_STATUS_EXL (1u << 1)
#define MIPS32_STATUS_ERL (1u << 2)
#define MIPS32_EXC_VECTOR 0x80000180u

void mips32_tlb_reset(void);
void mips32_tlbp(void);
void mips32_tlbwi(void);
void mips32_tlbwr(void);
bool mips32_debug_vaddr_read(vaddr_t addr, int len, word_t *value);

// decode
typedef struct
{
    uint32_t inst;
} mips32_ISADecodeInfo;

int isa_mmu_check(vaddr_t vaddr, int len, int type);

#endif
