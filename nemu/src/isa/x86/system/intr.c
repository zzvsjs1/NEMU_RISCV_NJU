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
#include <memory/vaddr.h>

#define IRQ_TIMER 32

#define FLAG_IF (1u << 9)
#define FLAG_TF (1u << 8)
#define FLAG_NT (1u << 14)
#define FLAG_RF (1u << 16)
#define FLAG_VM (1u << 17)

#define X86_DESC_TYPE_CODE 0x8u
#define X86_DESC_TYPE_CONFORMING 0x4u
#define X86_DESC_TYPE_WRITABLE 0x2u

jmp_buf x86_exception_env;
bool x86_exception_env_valid = false;
vaddr_t x86_exception_target = 0;

word_t x86_vaddr_read_kernel(vaddr_t addr, int len);
void x86_vaddr_write_kernel(vaddr_t addr, int len, word_t data);

static void push32(word_t val) {
  cpu.esp -= 4;
  Assert(cpu.esp + 3 <= cpu.seg_cache[X86_SREG_SS].limit,
      "x86 interrupt stack push exceeds SS limit: esp=%#x limit=%#x",
      cpu.esp, cpu.seg_cache[X86_SREG_SS].limit);
  x86_vaddr_write_kernel(cpu.seg_cache[X86_SREG_SS].base + cpu.esp, 4, val);
}

static void read_gdt_desc(uint16_t selector, uint32_t *lo, uint32_t *hi) {
  uint32_t desc_off = (selector >> 3) * 8u;

  Assert((selector & ~0x3u) != 0, "x86 uses a null selector %#x in protected interrupt delivery", selector);
  Assert((selector & 0x4u) == 0, "x86 LDT selector %#x is not supported in this NEMU x86 subset", selector);
  Assert(desc_off + 7 <= cpu.gdtr_limit,
      "x86 selector %#x exceeds GDT limit %#x during interrupt delivery",
      selector, cpu.gdtr_limit);

  vaddr_t desc_addr = cpu.gdtr_base + desc_off;
  *lo = x86_vaddr_read_kernel(desc_addr, 4);
  *hi = x86_vaddr_read_kernel(desc_addr + 4, 4);
}

static uint32_t desc_type(uint32_t hi) {
  return (hi >> 8) & 0xfu;
}

static uint32_t desc_dpl(uint32_t hi) {
  return (hi >> 13) & 0x3u;
}

static bool desc_system(uint32_t hi) {
  return ((hi >> 12) & 0x1u) != 0;
}

static bool desc_present(uint32_t hi) {
  return ((hi >> 15) & 0x1u) != 0;
}

static uint32_t interrupt_target_cpl(uint16_t selector, int old_cpl, uint32_t *lo_out, uint32_t *hi_out) {
  uint32_t lo, hi;
  read_gdt_desc(selector, &lo, &hi);

  uint32_t type = desc_type(hi);
  uint32_t dpl = desc_dpl(hi);
  bool conforming = (type & X86_DESC_TYPE_CONFORMING) != 0;

  Assert(desc_present(hi), "x86 interrupt target selector %#x is non-present", selector);
  Assert(desc_system(hi) && (type & X86_DESC_TYPE_CODE) != 0,
      "x86 interrupt target selector %#x is not an executable code segment", selector);
  Assert(conforming || (int)dpl <= old_cpl,
      "x86 interrupt target selector %#x has DPL %u below current privilege rules from CPL %d",
      selector, dpl, old_cpl);

  /*
   * Non-conforming interrupt targets enter the descriptor DPL.  Conforming code
   * keeps the current CPL; AM uses non-conforming flat kernel/user segments, but
   * modelling both cases avoids deriving privilege from the selector RPL.
   */
  *lo_out = lo;
  *hi_out = hi;
  return conforming ? (uint32_t)old_cpl : dpl;
}

static void validate_interrupt_stack(uint16_t selector, int target_cpl, uint32_t *lo_out, uint32_t *hi_out) {
  uint32_t lo, hi;
  read_gdt_desc(selector, &lo, &hi);

  uint32_t type = desc_type(hi);
  uint32_t dpl = desc_dpl(hi);
  uint32_t rpl = selector & 0x3u;
  bool code = (type & X86_DESC_TYPE_CODE) != 0;
  bool writable = (type & X86_DESC_TYPE_WRITABLE) != 0;

  Assert(desc_present(hi), "x86 interrupt stack selector %#x is non-present", selector);
  Assert(desc_system(hi) && !code && writable,
      "x86 interrupt stack selector %#x is not a writable data segment", selector);
  Assert((int)dpl == target_cpl && (int)rpl == target_cpl,
      "x86 interrupt stack selector %#x has DPL %u/RPL %u for target CPL %d",
      selector, dpl, rpl, target_cpl);

  *lo_out = lo;
  *hi_out = hi;
}

static word_t raise_intr(word_t NO, vaddr_t ret_addr, bool software,
                         bool has_error_code, word_t error_code) {
  Assert(NO < 256, "x86 interrupt number out of range: " FMT_WORD, NO);

  vaddr_t gate = cpu.idtr_base + NO * 8;
  Assert((NO * 8 + 7) <= cpu.idtr_limit,
      "x86 interrupt " FMT_WORD " exceeds IDT limit %#x", NO, cpu.idtr_limit);

  uint32_t off_low = x86_vaddr_read_kernel(gate, 2);
  uint32_t selector = x86_vaddr_read_kernel(gate + 2, 2) & 0xffffu;
  uint32_t attr = x86_vaddr_read_kernel(gate + 4, 2);
  uint32_t off_high = x86_vaddr_read_kernel(gate + 6, 2);
  uint32_t type = (attr >> 8) & 0xf;
  uint32_t gate_dpl = (attr >> 13) & 0x3;
  uint32_t target = off_low | (off_high << 16);

  Assert((attr & 0x8000) != 0, "x86 interrupt " FMT_WORD " uses a non-present gate", NO);
  Assert(type == 0xe || type == 0xf,
      "x86 interrupt " FMT_WORD " uses unsupported gate type %#x", NO, type);
  Assert(!software || (cpu.cs & 0x3) <= gate_dpl,
      "x86 software interrupt " FMT_WORD " from CPL %u exceeds gate DPL %u",
      NO, cpu.cs & 0x3, gate_dpl);

  word_t old_eflags = cpu.eflags | 0x2;
  word_t old_cs = cpu.cs;
  word_t old_ss = cpu.ss;
  word_t old_esp = cpu.esp;
  int old_cpl = old_cs & 0x3;
  uint32_t cs_lo, cs_hi;
  int new_cpl = interrupt_target_cpl(selector, old_cpl, &cs_lo, &cs_hi);

  if (new_cpl < old_cpl) {
    Assert(cpu.tr != 0, "x86 interrupt from user mode without a loaded TSS");

    /*
     * A 32-bit TSS stores ESPn/SSn pairs at 4+8*n and 8+8*n.  AM normally uses
     * only the ring-3 to ring-0 path, but using the target CPL keeps ring 1/2
     * targets from silently reusing esp0/ss0.
     */
    uint32_t esp_off = 4u + (uint32_t)new_cpl * 8u;
    uint32_t ss_off = esp_off + 4u;
    Assert(cpu.tss_limit >= ss_off + 3u,
        "x86 TSS is too small for CPL %d stack slot: limit %#x", new_cpl, cpu.tss_limit);

    word_t esp0 = x86_vaddr_read_kernel(cpu.tss_base + esp_off, 4);
    word_t ss0 = x86_vaddr_read_kernel(cpu.tss_base + ss_off, 4) & 0xffffu;
    uint32_t ss_lo, ss_hi;
    validate_interrupt_stack(ss0, new_cpl, &ss_lo, &ss_hi);

    cpu.esp = esp0;
    x86_seg_load_from_descriptor(X86_SREG_SS, ss0, ss_lo, ss_hi);
    push32(old_ss);
    push32(old_esp);
  }

  push32(old_eflags);
  push32(old_cs);
  push32(ret_addr);
  if (has_error_code) {
    push32(error_code);
  }

  x86_seg_load_from_descriptor(X86_SREG_CS, selector, cs_lo, cs_hi);
  cpu.eflags &= ~(FLAG_TF | FLAG_NT | FLAG_RF | FLAG_VM);
  if (type == 0xe) {
    cpu.eflags &= ~FLAG_IF;
  }
  cpu.eflags |= 0x2;

  return target;
}

word_t isa_raise_intr(word_t NO, vaddr_t ret_addr) {
  return raise_intr(NO, ret_addr, false, false, 0);
}

word_t isa_raise_intr_sw(word_t NO, vaddr_t ret_addr) {
  return raise_intr(NO, ret_addr, true, false, 0);
}

word_t isa_raise_intr_err(word_t NO, vaddr_t ret_addr, word_t errcode) {
  return raise_intr(NO, ret_addr, false, true, errcode);
}

void x86_raise_page_fault(void) {
  Assert(x86_exception_env_valid,
      "x86 page fault outside instruction execution: pc=" FMT_WORD
      " cr2=%#x err=%#x", cpu.pc, cpu.cr2, cpu.pf_errcode);

  x86_exception_target = isa_raise_intr_err(14, cpu.pc, cpu.pf_errcode);
  x86_mmu_clear_cpl_override();
  longjmp(x86_exception_env, 1);
}

word_t isa_query_intr() {
  if (cpu.sti_shadow != 0) {
    cpu.sti_shadow --;
    return INTR_EMPTY;
  }

  if (cpu.INTR && (cpu.eflags & FLAG_IF) != 0) {
    cpu.INTR = false;
    return IRQ_TIMER;
  }

  return INTR_EMPTY;
}
