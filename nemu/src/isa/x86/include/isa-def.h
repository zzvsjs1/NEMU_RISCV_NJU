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

#ifndef __ISA_X86_H__
#define __ISA_X86_H__

#include <common.h>


typedef struct {
  union {
    union {
      uint32_t _32;
      uint16_t _16;
      uint8_t _8[2];
    } gpr[8];

    /* Keep this order identical to the ModR/M register encoding. */
    struct {
      uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    };
  };

  uint32_t eflags;
  vaddr_t pc;
  uint32_t cs;
  uint32_t ds;
  uint32_t es;
  uint32_t ss;
  uint32_t cr0;
  uint32_t cr2;
  uint32_t cr3;
  uint32_t cr4;
  uint32_t pf_errcode;
  uint32_t idtr_base;
  uint16_t idtr_limit;
  uint32_t gdtr_base;
  uint16_t gdtr_limit;
  uint16_t tr;
  uint32_t tss_base;
  uint32_t tss_limit;
  uint8_t sti_shadow;
  bool INTR;
} x86_CPU_state;

// decode
typedef struct {
  uint8_t inst[16];
  uint8_t *p_inst;
} x86_ISADecodeInfo;

enum { R_EAX, R_ECX, R_EDX, R_EBX, R_ESP, R_EBP, R_ESI, R_EDI };
enum { R_AX, R_CX, R_DX, R_BX, R_SP, R_BP, R_SI, R_DI };
enum { R_AL, R_CL, R_DL, R_BL, R_AH, R_CH, R_DH, R_BH };

#endif
