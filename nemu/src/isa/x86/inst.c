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

#include "local-include/reg.h"
#include <cpu/cpu.h>
#include <cpu/decode.h>
#include <cpu/difftest.h>
#include <cpu/ifetch.h>
#include <isa-jit.h>

uint32_t pio_read(ioaddr_t addr, int len);
void pio_write(ioaddr_t addr, int len, uint32_t data);
word_t x86_vaddr_read_kernel(vaddr_t addr, int len);
void x86_vaddr_write_kernel(vaddr_t addr, int len, word_t data);

/*
 * Common byte widths used by this interpreter.  The decoder passes widths in
 * bytes, not bits: 1 = byte, 2 = word, 4 = doubleword.  These names keep the
 * manual's operand-size rules readable when comments refer to a specific form.
 */
enum {
  X86_WIDTH_BYTE = 1,
  X86_WIDTH_WORD = 2,
  X86_WIDTH_DWORD = 4,
  X86_BITS_PER_BYTE = 8,
  X86_WORD_BITS = 16,
  X86_DWORD_BITS = 32,
};

/*
 * Segment selectors are 16-bit values.  Intel defines bits 0..1 as RPL, bit 2
 * as TI (0 = GDT, 1 = LDT), and bits 3..15 as the descriptor-table index.
 * This NEMU x86 subset supports the GDT path used by AM/Nanos but still keeps
 * the privilege and null-selector checks explicit.
 */
enum {
  X86_SELECTOR_RPL_MASK = 0x3u,
  X86_SELECTOR_TI_MASK = 0x4u,
  X86_SELECTOR_INDEX_SHIFT = 3,
};

/*
 * Protected-mode descriptors are 8 bytes.  The high descriptor word contains
 * the type, S bit, DPL, present bit, and granularity bit at the positions below.
 * Naming these positions is safer than scattering raw shifts through segment,
 * LTR, and table-loading code.
 */
enum {
  X86_DESC_SIZE = 8u,
  X86_DESC_LAST_BYTE = X86_DESC_SIZE - 1u,
  X86_DESC_HIGH_OFFSET = 4u,
  X86_DESC_BASE_HIGH_MASK = 0xff000000u,
  X86_DESC_TYPE_SHIFT = 8,
  X86_DESC_TYPE_MASK = 0xfu,
  X86_DESC_S_SHIFT = 12,
  X86_DESC_DPL_SHIFT = 13,
  X86_DESC_DPL_MASK = 0x3u,
  X86_DESC_PRESENT_SHIFT = 15,
  X86_DESC_LIMIT_HIGH_SHIFT = 16,
  X86_DESC_GRANULARITY_SHIFT = 23,
  X86_DESC_CODE_TYPE = 0x8u,
  X86_DESC_RW_TYPE = 0x2u,
  X86_TSS_AVAILABLE_32 = 0x9u,
  X86_TSS_BUSY_TYPE_BIT = 0x2u,
};

/*
 * The Intel EFLAGS layout reserves bit 1 as always one.  Keep this bit set
 * whenever helpers write EFLAGS, otherwise simple PUSHF/POPF tests observe a
 * state no IA-32 processor should expose.
 */
enum {
  X86_EFLAGS_FIXED_ONE = 1u << 1,
  X86_EFLAGS_IOPL_SHIFT = 12,
};

enum {
  X86_SHIFT_COUNT_MASK = 0x1fu,       // 32-bit x86 masks byte/word/dword shift counts to 5 bits.
  X86_AUX_CARRY_BIT = 0x10u,         // AF observes carry/borrow between bit 3 and bit 4.
  X86_BYTE_MASK = 0xffu,
  X86_WORD_MASK = 0xffffu,
  X86_DWORD_MASK = 0xffffffffu,
  X86_OPCODE_REG_MASK = 0x7u,         // Low three opcode bits select a GPR in opcodes such as 40+rw and B8+rw.
  X86_COND_MASK = 0xfu,               // Jcc/SETcc condition codes live in the low opcode nibble.
  X86_PARITY_FOLD_NIBBLE_MASK = 0xfu, // PF only sees a folded four-bit value before table lookup.
  X86_PARITY_FOLD_TABLE = 0x6996u,    // Compact even-parity table for the folded byte.
  X86_ONE_BIT_MASK = 1u,              // Used after shifting a descriptor field down to bit 0.
  X86_PAGE_SHIFT = 12,
  X86_PAGE_OFFSET_MASK = 0xfffu,     // CR3 stores a 4 KiB page base; low bits are flags/reserved.
  X86_DWORD_BASE = 0x100000000ll,    // 2^32, used to combine EDX:EAX for signed division.
};

/*
 * Control-register numbers are encoded in the ModR/M reg field for MOV CRn.
 * Only the registers modelled by this NEMU x86 subset are accepted.
 */
enum {
  X86_CR0_INDEX = 0,
  X86_CR2_INDEX = 2,
  X86_CR3_INDEX = 3,
  X86_CR4_INDEX = 4,
};

/* System-instruction group extensions used by 0F 00/01 ModR/M encodings. */
enum {
  X86_GROUP_LGDT_EXT = 2,
  X86_GROUP_LIDT_EXT = 3,
  X86_GROUP_LTR_EXT = 3,
};

/*
 * Intel condition-code numbers used by Jcc/SETcc.  The low bit is usually the
 * negation bit, so O/NO, B/AE, Z/NZ, and the remaining pairs stay adjacent.
 */
enum {
  X86_CC_O  = 0x0,
  X86_CC_NO = 0x1,
  X86_CC_B  = 0x2,
  X86_CC_AE = 0x3,
  X86_CC_Z  = 0x4,
  X86_CC_NZ = 0x5,
  X86_CC_BE = 0x6,
  X86_CC_A  = 0x7,
  X86_CC_S  = 0x8,
  X86_CC_NS = 0x9,
  X86_CC_P  = 0xa,
  X86_CC_NP = 0xb,
  X86_CC_L  = 0xc,
  X86_CC_GE = 0xd,
  X86_CC_LE = 0xe,
  X86_CC_G  = 0xf,
};

/* ModR/M byte layout: mod selects register/memory form, reg can be a register or opcode extension, R/M selects the operand. */
typedef union {
  struct {
    uint8_t R_M		:3;
    uint8_t reg		:3;
    uint8_t mod		:2;
  };
  struct {
    uint8_t dont_care	:3;
    uint8_t opcode		:3;
  };
  uint8_t val;
} ModR_M;

/* SIB byte layout used when ModR/M chooses ESP as the addressing form in 32-bit mode. */
typedef union {
  struct {
    uint8_t base	:3;
    uint8_t index	:3;
    uint8_t ss		:2;
  };
  uint8_t val;
} SIB;

static uint32_t seg_linear(int idx, vaddr_t off, int len, const char *op);

/*
 * Fetch instruction bytes and advance snpc.  With tracing enabled, also copy
 * the bytes into Decode::isa.inst so itrace/iqueue can print the original
 * instruction stream after execution.
 */
static word_t x86_inst_fetch(Decode *s, int len) {
#if defined(CONFIG_ITRACE) || defined(CONFIG_IQUEUE)
  uint8_t *p = &s->isa.inst[s->snpc - s->pc];
  word_t ret = vaddr_ifetch(seg_linear(X86_SREG_CS, s->snpc, len, "ifetch"), len);
  s->snpc += len;
  word_t ret_save = ret;
  int i;
  assert(s->snpc - s->pc < sizeof(s->isa.inst));
  for (i = 0; i < len; i ++) {
    p[i] = ret & X86_BYTE_MASK;
    ret >>= 8;
  }
  return ret_save;
#else
  word_t ret = vaddr_ifetch(seg_linear(X86_SREG_CS, s->snpc, len, "ifetch"), len);
  s->snpc += len;
  return ret;
#endif
}

/* Read a general-purpose register alias selected by byte width. */
word_t reg_read(int idx, int width) {
  switch (width) {
    case X86_WIDTH_DWORD: return reg_l(idx);
    case X86_WIDTH_BYTE: return reg_b(idx);
    case X86_WIDTH_WORD: return reg_w(idx);
    default: assert(0);
  }
}

/* Write a general-purpose register alias selected by byte width. */
static void reg_write(int idx, int width, word_t data) {
  switch (width) {
    case X86_WIDTH_DWORD: reg_l(idx) = data; return;
    case X86_WIDTH_BYTE: reg_b(idx) = data; return;
    case X86_WIDTH_WORD: reg_w(idx) = data; return;
    default: assert(0);
  }
}

/* Require CPL0 for instructions whose architecture requires kernel privilege in this subset. */
static void require_kernel(Decode *s, const char *op) {
  Assert((cpu.cs & X86_SELECTOR_RPL_MASK) == 0,
      "x86 %s from CPL %u would raise #GP at pc = " FMT_WORD,
      op, cpu.cs & X86_SELECTOR_RPL_MASK, s->pc);
}

/*
 * Control-register bits used by the interpreter.  CR0.PE enables protected
 * mode, CR0.PG enables paging, and CR3 bits 3/4 carry PWT/PCD cache controls
 * while the rest of the low 12 bits must not become part of the page-directory
 * base.
 */
enum {
  CR0_PE = 1u << 0,
  CR0_WP = 1u << 16,
  CR0_PG = 1u << 31,
  CR3_PWT_PCD_MASK = 0x18u,
  CR4_PSE = 1u << 4,
  CR4_PAE = 1u << 5,
};

static uint32_t *sreg_visible_ptr(int idx) {
  switch (idx) {
    case X86_SREG_ES: return &cpu.es;
    case X86_SREG_CS: return &cpu.cs;
    case X86_SREG_SS: return &cpu.ss;
    case X86_SREG_DS: return &cpu.ds;
    default: assert(0);
  }
}

void x86_seg_set_flat(int idx, uint16_t selector) {
  *sreg_visible_ptr(idx) = selector;
  cpu.seg_cache[idx].base = 0;
  cpu.seg_cache[idx].limit = X86_DWORD_MASK;
  cpu.seg_cache[idx].attr = 0;
}

/* Unpack the 32-bit descriptor base from the low and high descriptor dwords. */
static uint32_t desc_base(uint32_t lo, uint32_t hi) {
  return ((lo >> X86_WORD_BITS) & X86_WORD_MASK) |
      ((hi & X86_BYTE_MASK) << X86_WORD_BITS) |
      (hi & X86_DESC_BASE_HIGH_MASK);
}

/*
 * Unpack the descriptor limit and expand it when the granularity bit is set.
 * A page-granular limit stores 4 KiB units, so the byte limit gets the low
 * 12 bits filled in.
 */
static uint32_t desc_limit(uint32_t lo, uint32_t hi) {
  uint32_t limit = (lo & X86_WORD_MASK) |
      (((hi >> X86_DESC_LIMIT_HIGH_SHIFT) & X86_DESC_TYPE_MASK) << X86_DESC_LIMIT_HIGH_SHIFT);
  if ((hi & (1u << X86_DESC_GRANULARITY_SHIFT)) != 0) {
    limit = (limit << X86_PAGE_SHIFT) | X86_PAGE_OFFSET_MASK;
  }
  return limit;
}

void x86_seg_load_from_descriptor(int idx, uint16_t selector, uint32_t lo, uint32_t hi) {
  uint32_t old_cpl = cpu.cs & X86_SELECTOR_RPL_MASK;
  *sreg_visible_ptr(idx) = selector;
  cpu.seg_cache[idx].base = desc_base(lo, hi);
  cpu.seg_cache[idx].limit = desc_limit(lo, hi);
  cpu.seg_cache[idx].attr = hi;

  if (idx == X86_SREG_CS &&
      (selector & X86_SELECTOR_RPL_MASK) != old_cpl) {
    isa_jit_flush_data_tlb();
  }
}

static uint32_t seg_linear(int idx, vaddr_t off, int len, const char *op) {
  uint32_t selector = *sreg_visible_ptr(idx);
  Assert(selector != 0 || idx == X86_SREG_CS,
      "x86 %s through a null segment register at offset " FMT_WORD, op, off);

  uint64_t end = (uint64_t)(uint32_t)off + (uint64_t)len - 1u;
  Assert(end <= cpu.seg_cache[idx].limit,
      "x86 %s segment limit violation: selector %#x offset " FMT_WORD
      " len %d limit %#x", op, selector, off, len, cpu.seg_cache[idx].limit);

  return cpu.seg_cache[idx].base + (uint32_t)off;
}

static word_t seg_read(int idx, vaddr_t off, int len) {
  return vaddr_read(seg_linear(idx, off, len, "read"), len);
}

static void seg_write(int idx, vaddr_t off, int len, word_t data) {
  vaddr_write(seg_linear(idx, off, len, "write"), len, data);
}

/* Return the visible selector value for the four segment registers implemented by this x86 subset. */
static word_t sreg_read(Decode *s, int idx) {
  switch (idx) {
    case X86_SREG_ES: return cpu.es;
    case X86_SREG_CS: return cpu.cs;
    case X86_SREG_SS: return cpu.ss;
    case X86_SREG_DS: return cpu.ds;
    default:
      INV(s->pc);
      return 0;
  }
}

/*
 * MOV r/m16,rS stores the visible selector.  Register destinations in IA-32 are
 * decoded as r32 here and get a zero-extended selector; memory destinations
 * remain a 16-bit store because the memory form is explicitly r/m16.
 */
static void mov_sreg_to_rm(Decode *s, int sreg, int rd, word_t addr, int seg) {
  word_t selector = sreg_read(s, sreg) & X86_WORD_MASK;

  /*
   * Intel specifies a 16-bit selector value, but IA-32 register destinations are
   * zero-extended on common 32-bit processors.  Memory operands remain 16-bit
   * because the instruction stores only the visible selector there.
   */
  if (rd != -1) reg_write(rd, X86_WIDTH_DWORD, selector);
  else seg_write(seg, addr, X86_WIDTH_WORD, selector);
}

/*
 * Load DS/ES/SS visible selector state and perform the protected-mode checks
 * AM/Nanos depend on.  FS/GS and LDT addressing remain outside this interpreter
 * subset, but loaded descriptors now update the hidden base/limit cache used by
 * segmented memory accesses.
 */
static void sreg_write(Decode *s, int idx, word_t data) {
  uint16_t selector = data & X86_WORD_MASK;
  int cpl = cpu.cs & X86_SELECTOR_RPL_MASK;
  int rpl = selector & X86_SELECTOR_RPL_MASK;

  if ((selector & ~X86_SELECTOR_RPL_MASK) == 0) {
    Assert(idx != X86_SREG_SS, "x86 loads a null SS selector at pc = " FMT_WORD, s->pc);
    if (idx == X86_SREG_ES || idx == X86_SREG_DS) {
      *sreg_visible_ptr(idx) = 0;
      cpu.seg_cache[idx].base = 0;
      cpu.seg_cache[idx].limit = 0;
      cpu.seg_cache[idx].attr = 0;
    }
    else INV(s->pc);
    return;
  }

  Assert((selector & X86_SELECTOR_TI_MASK) == 0,
      "x86 LDT selector %#x is not supported by this NEMU x86 segment subset at pc = " FMT_WORD,
      selector, s->pc);

  uint32_t desc_off = (selector >> X86_SELECTOR_INDEX_SHIFT) * X86_DESC_SIZE;
  Assert(desc_off + X86_DESC_LAST_BYTE <= cpu.gdtr_limit,
      "x86 segment selector %#x exceeds GDT limit %#x at pc = " FMT_WORD,
      selector, cpu.gdtr_limit, s->pc);

  vaddr_t desc_addr = cpu.gdtr_base + desc_off;
  uint32_t lo = x86_vaddr_read_kernel(desc_addr, X86_WIDTH_DWORD);
  uint32_t hi = x86_vaddr_read_kernel(desc_addr + X86_DESC_HIGH_OFFSET, X86_WIDTH_DWORD);
  uint32_t type = (hi >> X86_DESC_TYPE_SHIFT) & X86_DESC_TYPE_MASK;
  uint32_t system = (hi >> X86_DESC_S_SHIFT) & X86_ONE_BIT_MASK;
  uint32_t dpl = (hi >> X86_DESC_DPL_SHIFT) & X86_DESC_DPL_MASK;
  uint32_t present = (hi >> X86_DESC_PRESENT_SHIFT) & X86_ONE_BIT_MASK;
  bool code = (type & X86_DESC_CODE_TYPE) != 0;
  bool writable_or_readable = (type & X86_DESC_RW_TYPE) != 0;

  Assert(present != 0, "x86 loads a non-present segment selector %#x at pc = " FMT_WORD,
      selector, s->pc);
  Assert(system != 0, "x86 loads system descriptor selector %#x into a data segment at pc = " FMT_WORD,
      selector, s->pc);

  switch (idx) {
    case X86_SREG_ES:
    case X86_SREG_DS:
      Assert(!code || writable_or_readable,
          "x86 loads unreadable code selector %#x into a data segment at pc = " FMT_WORD,
          selector, s->pc);
      Assert(cpl <= (int)dpl && rpl <= (int)dpl,
          "x86 loads selector %#x with DPL %u from CPL %d/RPL %d at pc = " FMT_WORD,
          selector, dpl, cpl, rpl, s->pc);
      x86_seg_load_from_descriptor(idx, selector, lo, hi);
      return;
    case X86_SREG_SS:
      Assert(!code && writable_or_readable,
          "x86 loads non-writable selector %#x into SS at pc = " FMT_WORD,
          selector, s->pc);
      Assert(rpl == cpl && (int)dpl == cpl,
          "x86 loads SS selector %#x with DPL %u from CPL %d/RPL %d at pc = " FMT_WORD,
          selector, dpl, cpl, rpl, s->pc);
      x86_seg_load_from_descriptor(idx, selector, lo, hi);
      return;
    default:
      INV(s->pc);
      return;
  }
}

static void load_seg_cache_from_gdt(int idx, uint16_t selector) {
  if ((selector & ~X86_SELECTOR_RPL_MASK) == 0) {
    *sreg_visible_ptr(idx) = selector;
    cpu.seg_cache[idx].base = 0;
    cpu.seg_cache[idx].limit = 0;
    cpu.seg_cache[idx].attr = 0;
    return;
  }

  Assert((selector & X86_SELECTOR_TI_MASK) == 0,
      "x86 selector %#x uses the unsupported LDT table", selector);
  uint32_t desc_off = (selector >> X86_SELECTOR_INDEX_SHIFT) * X86_DESC_SIZE;
  Assert(desc_off + X86_DESC_LAST_BYTE <= cpu.gdtr_limit,
      "x86 selector %#x exceeds GDT limit %#x", selector, cpu.gdtr_limit);
  vaddr_t desc_addr = cpu.gdtr_base + desc_off;
  uint32_t lo = x86_vaddr_read_kernel(desc_addr, X86_WIDTH_DWORD);
  uint32_t hi = x86_vaddr_read_kernel(desc_addr + X86_DESC_HIGH_OFFSET, X86_WIDTH_DWORD);
  x86_seg_load_from_descriptor(idx, selector, lo, hi);
}

/* EFLAGS status/control bits used by integer instructions and interrupt control. */
enum {
  FLAG_CF = 1u << 0,
  FLAG_PF = 1u << 2,
  FLAG_AF = 1u << 4,
  FLAG_ZF = 1u << 6,
  FLAG_SF = 1u << 7,
  FLAG_IF = 1u << 9,
  FLAG_DF = 1u << 10,
  FLAG_OF = 1u << 11,
  FLAG_IOPL = X86_SELECTOR_RPL_MASK << X86_EFLAGS_IOPL_SHIFT,
};

/* Local ALU operation ids mirror the /digit encodings used by x86 group-1 opcodes. */
enum {
  ALU_ADD,
  ALU_OR,
  ALU_ADC,
  ALU_SBB,
  ALU_AND,
  ALU_SUB,
  ALU_XOR,
  ALU_CMP,
};

/* Return an all-ones mask for the active operand width. */
static inline uint32_t width_mask(int width) {
  return width == X86_WIDTH_DWORD ? X86_DWORD_MASK : ((1u << (width * X86_BITS_PER_BYTE)) - 1u);
}

/* Return the top bit for the active operand width; SF and signed overflow use it. */
static inline uint32_t sign_bit(int width) {
  return 1u << (width * X86_BITS_PER_BYTE - 1);
}

/* Truncate a temporary arithmetic value to byte/word/dword architectural size. */
static inline word_t mask_width(word_t val, int width) {
  return val & width_mask(width);
}

/* Test one EFLAGS bit without exposing the raw bit twiddling at each call site. */
static inline bool flag_get(uint32_t flag) {
  return (cpu.eflags & flag) != 0;
}

/* Set or clear one EFLAGS bit while preserving Intel's fixed bit-1 value. */
static inline void flag_set(uint32_t flag, bool val) {
  if (val) cpu.eflags |= flag;
  else cpu.eflags &= ~flag;
  cpu.eflags |= X86_EFLAGS_FIXED_ONE;
}

/* Extract the two-bit IOPL field from EFLAGS. */
static inline int x86_iopl(uint32_t eflags) {
  return (eflags & FLAG_IOPL) >> X86_EFLAGS_IOPL_SHIFT;
}

/*
 * Apply POPF/IRET privilege filtering before committing EFLAGS.  User code may
 * request many flag changes through the stack image, but IF and IOPL are
 * guarded by CPL and IOPL rules in protected mode.
 */
static word_t eflags_write_protected_width(word_t requested, int width) {
  word_t old = cpu.eflags | X86_EFLAGS_FIXED_ONE;
  int cpl = cpu.cs & X86_SELECTOR_RPL_MASK;

  if (width == X86_WIDTH_WORD) {
    requested = (old & ~X86_WORD_MASK) | (requested & X86_WORD_MASK);
  }
  requested |= X86_EFLAGS_FIXED_ONE;

  /*
   * Intel POPF/IRET do not let ring-3 code take control of interrupt delivery.
   * IOPL can only be changed at CPL0, and IF can only be changed when CPL <=
   * IOPL.  Other EFLAGS bits used by PA programs remain writable.
   */
  if (cpl != 0) {
    requested = (requested & ~FLAG_IOPL) | (old & FLAG_IOPL);
  }
  if (cpl > x86_iopl(old)) {
    requested = (requested & ~FLAG_IF) | (old & FLAG_IF);
  }

  return requested;
}

static word_t eflags_write_protected(word_t requested) {
  return eflags_write_protected_width(requested, X86_WIDTH_DWORD);
}

/* Shared privilege check for IN/OUT and for the IF-changing CLI/STI forms. */
static void require_io_privilege(Decode *s, const char *op) {
  int cpl = cpu.cs & X86_SELECTOR_RPL_MASK;
  Assert(cpl <= x86_iopl(cpu.eflags),
      "x86 %s from CPL %u with IOPL %u would raise #GP at pc = " FMT_WORD,
      op, cpl, x86_iopl(cpu.eflags), s->pc);
}

/* Shared CLI/STI gate; these instructions are allowed by IOPL, not only by CPL0. */
static void require_interrupt_flag_privilege(Decode *s, const char *op) {
  /*
   * CLI/STI are not strictly ring-0-only in protected mode.  They are allowed
   * whenever CPL <= IOPL; AM normally executes them at CPL0, while this check
   * keeps the emulator correct for ring-1/2 kernels without opening ring-3 IF
   * control.
   */
  require_io_privilege(s, op);
}

/* Privilege-checked IN wrapper; the actual device access is delegated to the port I/O layer. */
static word_t x86_pio_read(Decode *s, ioaddr_t addr, int len) {
  require_io_privilege(s, "in");
  return pio_read(addr, len);
}

/* Privilege-checked OUT wrapper; keeping it here centralises the IOPL check. */
static void x86_pio_write(Decode *s, ioaddr_t addr, int len, uint32_t data) {
  require_io_privilege(s, "out");
  pio_write(addr, len, data);
}

/*
 * Compute x86 PF from the least significant byte.  Intel defines PF as even
 * parity over that byte only.  The named table constant is a compact 16-entry
 * lookup after folding the byte down to four bits.
 */
static bool parity_even(uint8_t val) {
  val ^= val >> 4;
  val &= X86_PARITY_FOLD_NIBBLE_MASK;
  return ((X86_PARITY_FOLD_TABLE >> val) & 1) == 0;
}

/* Set the ZF/SF/PF status flags shared by arithmetic and logical instructions. */
static void set_zsp_flags(word_t result, int width) {
  result = mask_width(result, width);
  flag_set(FLAG_ZF, result == 0);
  flag_set(FLAG_SF, (result & sign_bit(width)) != 0);
  flag_set(FLAG_PF, parity_even(result & X86_BYTE_MASK));
}

/*
 * Set flags for addition-like operations.  CF reports unsigned carry, AF
 * reports carry from bit 3 to bit 4, and OF reports signed two's-complement
 * overflow.
 */
static void set_add_flags(word_t lhs, word_t rhs, word_t result, int width) {
  uint32_t mask = width_mask(width);
  uint32_t sign = sign_bit(width);
  uint32_t l = lhs & mask;
  uint32_t r = rhs & mask;
  uint32_t res = result & mask;
  uint64_t raw = (uint64_t)l + (uint64_t)r;

  set_zsp_flags(res, width);
  flag_set(FLAG_CF, raw > mask);
  flag_set(FLAG_AF, ((l ^ r ^ res) & X86_AUX_CARRY_BIT) != 0);
  flag_set(FLAG_OF, ((~(l ^ r) & (l ^ res) & sign) != 0));
}

/* Set flags for subtraction-like operations; CF means an unsigned borrow. */
static void set_sub_flags(word_t lhs, word_t rhs, word_t result, int width) {
  uint32_t mask = width_mask(width);
  uint32_t sign = sign_bit(width);
  uint32_t l = lhs & mask;
  uint32_t r = rhs & mask;
  uint32_t res = result & mask;

  set_zsp_flags(res, width);
  flag_set(FLAG_CF, l < r);
  flag_set(FLAG_AF, ((l ^ r ^ res) & X86_AUX_CARRY_BIT) != 0);
  flag_set(FLAG_OF, (((l ^ r) & (l ^ res) & sign) != 0));
}

/* Logical operations define ZF/SF/PF and clear CF/OF/AF in this subset. */
static void set_logic_flags(word_t result, int width) {
  set_zsp_flags(result, width);
  flag_set(FLAG_CF, false);
  flag_set(FLAG_OF, false);
  flag_set(FLAG_AF, false);
}

/*
 * Execute a local ALU operation and update architectural flags.  The result is
 * always width-truncated before returning, while CMP only keeps the flag side
 * effects at its call sites.
 */
static word_t alu_exec(int op, word_t lhs, word_t rhs, int width) {
  word_t carry = flag_get(FLAG_CF) ? 1 : 0;
  word_t result = 0;

  switch (op) {
    case ALU_ADD:
      result = lhs + rhs;
      set_add_flags(lhs, rhs, result, width);
      break;
    case ALU_OR:
      result = lhs | rhs;
      set_logic_flags(result, width);
      break;
    case ALU_ADC:
    {
      uint32_t mask = width_mask(width);
      uint32_t sign = sign_bit(width);
      uint32_t l = lhs & mask;
      uint32_t r = rhs & mask;
      uint64_t r_full = (uint64_t)r + carry;
      uint32_t res = (uint64_t)l + r_full;
      result = res;
      set_zsp_flags(res, width);
      flag_set(FLAG_CF, (uint64_t)l + r_full > mask);
      flag_set(FLAG_AF, ((l ^ r ^ res) & X86_AUX_CARRY_BIT) != 0);
      flag_set(FLAG_OF, ((~(l ^ r) & (l ^ res) & sign) != 0));
      break;
    }
    case ALU_SBB:
    {
      uint32_t mask = width_mask(width);
      uint32_t sign = sign_bit(width);
      uint32_t l = lhs & mask;
      uint32_t r = rhs & mask;
      uint64_t r_full = (uint64_t)r + carry;
      uint32_t res = (uint64_t)l - r_full;
      result = res;
      set_zsp_flags(res, width);
      flag_set(FLAG_CF, (uint64_t)l < r_full);
      flag_set(FLAG_AF, ((l ^ r ^ res) & X86_AUX_CARRY_BIT) != 0);
      flag_set(FLAG_OF, (((l ^ r) & (l ^ res) & sign) != 0));
      break;
    }
    case ALU_AND:
      result = lhs & rhs;
      set_logic_flags(result, width);
      break;
    case ALU_SUB:
      result = lhs - rhs;
      set_sub_flags(lhs, rhs, result, width);
      break;
    case ALU_XOR:
      result = lhs ^ rhs;
      set_logic_flags(result, width);
      break;
    case ALU_CMP:
      result = lhs - rhs;
      set_sub_flags(lhs, rhs, result, width);
      break;
    default:
      assert(0);
  }

  return mask_width(result, width);
}

/* Sign-extend a byte/word/dword value into the host signed word type. */
static sword_t sext_width(word_t val, int width) {
  switch (width) {
    case X86_WIDTH_BYTE: return (int8_t)val;
    case X86_WIDTH_WORD: return (int16_t)val;
    case X86_WIDTH_DWORD: return (int32_t)val;
    default: assert(0);
  }
}

/* Push a word or doubleword on the current 32-bit stack through SS:ESP. */
static void push_width(int width, word_t val) {
  cpu.esp -= width;
  seg_write(X86_SREG_SS, cpu.esp, width, val);
}

/* Pop a word or doubleword from SS:ESP and advance ESP by the operand size. */
static word_t pop_width(int width) {
  word_t val = seg_read(X86_SREG_SS, cpu.esp, width);
  cpu.esp += width;
  return val;
}

static word_t pop32() { return pop_width(X86_WIDTH_DWORD); }

/*
 * Decode the memory-address part of a ModR/M operand.  A SIB byte follows when
 * R/M selects ESP; mod then decides whether the displacement is absent, 8-bit,
 * or 32-bit.  A base register of -1 means absolute displacement-only addressing.
 */
static void load_addr32(Decode *s, ModR_M *m, word_t *rm_addr,
    int *rm_seg, bool *addr_uses_esp) {
  assert(m->mod != 3);

  sword_t disp = 0;
  int disp_size = X86_WIDTH_DWORD;
  int base_reg = -1, index_reg = -1, scale = 0;

  if (m->R_M == R_ESP) {
    SIB sib;
    sib.val = x86_inst_fetch(s, X86_WIDTH_BYTE);
    base_reg = sib.base;
    scale = sib.ss;

    if (sib.index != R_ESP) { index_reg = sib.index; }
  }
  else { base_reg = m->R_M; } /* no SIB */

  if (m->mod == 0) {
    if (base_reg == R_EBP) { base_reg = -1; }
    else { disp_size = 0; }
  }
  else if (m->mod == 1) { disp_size = X86_WIDTH_BYTE; }
  else if (m->mod == 2) { disp_size = X86_WIDTH_DWORD; }

  if (disp_size != 0) { /* has disp */
    disp = x86_inst_fetch(s, disp_size);
    if (disp_size == X86_WIDTH_BYTE) { disp = (int8_t)disp; }
  }

  word_t addr = disp;
  if (base_reg != -1)  addr += reg_l(base_reg);
  if (index_reg != -1) addr += reg_l(index_reg) << scale;
  *rm_addr = addr;
  *rm_seg = (base_reg == R_EBP || base_reg == R_ESP) ? X86_SREG_SS : X86_SREG_DS;
  *addr_uses_esp = base_reg == R_ESP;
}

static uint16_t addr16_base(int rm, int mod, int *rm_seg) {
  switch (rm) {
    case 0: *rm_seg = X86_SREG_DS; return reg_w(R_BX) + reg_w(R_SI);
    case 1: *rm_seg = X86_SREG_DS; return reg_w(R_BX) + reg_w(R_DI);
    case 2: *rm_seg = X86_SREG_SS; return reg_w(R_BP) + reg_w(R_SI);
    case 3: *rm_seg = X86_SREG_SS; return reg_w(R_BP) + reg_w(R_DI);
    case 4: *rm_seg = X86_SREG_DS; return reg_w(R_SI);
    case 5: *rm_seg = X86_SREG_DS; return reg_w(R_DI);
    case 6:
      *rm_seg = mod == 0 ? X86_SREG_DS : X86_SREG_SS;
      return mod == 0 ? 0 : reg_w(R_BP);
    case 7: *rm_seg = X86_SREG_DS; return reg_w(R_BX);
    default: assert(0);
  }
}

static void load_addr16(Decode *s, ModR_M *m, word_t *rm_addr, int *rm_seg) {
  assert(m->mod != 3);

  int disp_size = 0;
  sword_t disp = 0;
  uint16_t base = addr16_base(m->R_M, m->mod, rm_seg);

  if (m->mod == 0) {
    disp_size = (m->R_M == 6) ? X86_WIDTH_WORD : 0;
  }
  else if (m->mod == 1) {
    disp_size = X86_WIDTH_BYTE;
  }
  else if (m->mod == 2) {
    disp_size = X86_WIDTH_WORD;
  }

  if (disp_size != 0) {
    disp = x86_inst_fetch(s, disp_size);
    if (disp_size == X86_WIDTH_BYTE) {
      disp = (int8_t)disp;
    }
  }

  *rm_addr = (uint16_t)(base + disp);
}

static void load_addr(Decode *s, ModR_M *m, word_t *rm_addr,
    int *rm_seg, bool *addr_uses_esp, bool addr_size_16) {
  if (addr_size_16) {
    *addr_uses_esp = false;
    load_addr16(s, m, rm_addr, rm_seg);
  }
  else {
    load_addr32(s, m, rm_addr, rm_seg, addr_uses_esp);
  }
}

/*
 * Decode a ModR/M byte into either a register id or a memory address.  The
 * interpreter uses rm_reg == -1 as the memory sentinel; otherwise rm_reg is a
 * general-purpose register index.
 */
static void decode_rm(Decode *s, int *rm_reg, word_t *rm_addr, int *rm_seg,
    bool *addr_uses_esp, int *reg, int width, bool addr_size_16) {
  ModR_M m;
  m.val = x86_inst_fetch(s, X86_WIDTH_BYTE);
  if (reg != NULL) *reg = m.reg;
  if (m.mod == 3) {
    *rm_reg = m.R_M;
    *rm_seg = -1;
    *addr_uses_esp = false;
  }
  else { load_addr(s, &m, rm_addr, rm_seg, addr_uses_esp, addr_size_16); *rm_reg = -1; }
}

/*
 * Short operand access aliases used inside instruction bodies.  RMr/RMw assume
 * the local INSTPAT variable names `rd`, `addr`, and `w`; use rm_write() in
 * helper functions where those names are not in scope.
 */
#define Rr reg_read
#define Rw reg_write
#define Mr(off, seg, width) seg_read(seg, off, width)
#define Mw(off, seg, width, data) seg_write(seg, off, width, data)
#define RMr(reg, w)  (reg != -1 ? Rr(reg, w) : Mr(addr, seg, w))
#define RMw(data) do { if (rd != -1) Rw(rd, w, data); else Mw(addr, seg, w, data); } while (0)

/* Write through the register-or-memory abstraction produced by decode_rm(). */
static inline void rm_write(int rm_reg, word_t rm_addr, int rm_seg, int width, word_t data) {
  if (rm_reg != -1) Rw(rm_reg, width, data);
  else Mw(rm_addr, rm_seg, width, data);
}

/* Write r/m first, then commit flags, so a faulting memory destination leaves old EFLAGS visible. */
static inline void rm_write_defer_flags(int rm_reg, word_t rm_addr, int rm_seg, int width,
    word_t data, word_t old_eflags, word_t new_eflags) {
  /*
   * A memory write can raise #PF.  Intel fault semantics are restartable: the
   * saved EFLAGS must be the value from before the faulting instruction, not the
   * ALU result that would have been committed after a successful write.
  */
  cpu.eflags = old_eflags | X86_EFLAGS_FIXED_ONE;
  rm_write(rm_reg, rm_addr, rm_seg, width, data);
  cpu.eflags = new_eflags | X86_EFLAGS_FIXED_ONE;
}

/*
 * Operand-decoder helper macros.  They intentionally rely on the local names in
 * decode_operand(); keeping them small makes each TYPE_* case read like the
 * Intel operand notation.
 */
#define destr(r)  do { *rd_ = (r); } while (0)
#define src1r(r)  do { *src1 = Rr(r, w); } while (0)
#define imm()     do { *imm = x86_inst_fetch(s, w); } while (0)
#define simm(width) do { *imm = sext_width(x86_inst_fetch(s, width), width); } while (0)

/*
 * Operand decode forms.  A width argument of 0 in INSTPAT means "use the
 * current operand-size attribute" (32-bit by default, 16-bit after 0x66).
 */
enum {
  TYPE_r, TYPE_I, TYPE_SI, TYPE_J, TYPE_E,
  TYPE_I2r,  // XX <- Ib / eXX <- Iv
  TYPE_I2a,  // AL <- Ib / eAX <- Iv
  TYPE_G2E,  // Eb <- Gb / Ev <- Gv
  TYPE_E2G,  // Gb <- Eb / Gv <- Ev
  TYPE_I2E,  // Eb <- Ib / Ev <- Iv
  TYPE_Ib2E, TYPE_cl2E, TYPE_1_E, TYPE_SI2E,
  TYPE_GP3,  // F6/F7 group: /0 has an immediate, the remaining forms do not
  TYPE_Eb2G, TYPE_Ew2G,
  TYPE_O2a, TYPE_a2O,
  TYPE_P,  // imm8 port number
  TYPE_I_E2G,  // Gv <- EvIb / Gv <- EvIv // use for imul
  TYPE_SI_E2G,  // Gv <- EvIb / Gv <- EvIv // use for imul
  TYPE_Ib_G2E, // Ev <- GvIb // use for shld/shrd
  TYPE_cl_G2E, // Ev <- GvCL // use for shld/shrd
  TYPE_N, // none
};

/*
 * INSTPAT binds a binary opcode string to one decoded operand shape and an
 * execution body.  The dispatch layer expands this macro, so comments here
 * document the local contract instead of repeating it for every opcode row.
 */
#define INSTPAT_INST(s) opcode
#define INSTPAT_MATCH(s, name, type, width, ... /* execute body */ ) { \
  int rd = 0, rs = 0, gp_idx = 0, seg = -1; \
  bool addr_uses_esp = false; \
  word_t src1 = 0, addr = 0, imm = 0; \
  int w = width == 0 ? (is_operand_size_16 ? X86_WIDTH_WORD : X86_WIDTH_DWORD) : width; \
  decode_operand(s, opcode, &rd, &src1, &addr, &seg, &addr_uses_esp, &rs, &gp_idx, &imm, w, concat(TYPE_, type), is_addr_size_16); \
  if (seg_override >= 0 && seg >= 0) seg = seg_override; \
  s->dnpc = s->snpc; \
  __VA_ARGS__ ; \
}

/*
 * Decode the operands requested by one INSTPAT entry.  Output pointers are used
 * because each instruction form needs a different subset: rd for destination,
 * rs/src1 for source register/value, gp_idx for opcode-extension groups, addr
 * for memory operands, and imm for immediate/displacement operands.
 */
static void decode_operand(Decode *s, uint8_t opcode, int *rd_, word_t *src1,
    word_t *addr, int *seg, bool *addr_uses_esp, int *rs, int *gp_idx,
    word_t *imm, int w, int type, bool addr_size_16) {
  switch (type) {
    case TYPE_r:    destr(opcode & X86_OPCODE_REG_MASK); break;
    case TYPE_I:    imm(); break;
    case TYPE_SI:   simm(w); break;
    case TYPE_J:    simm(w); break;
    case TYPE_E:    decode_rm(s, rd_, addr, seg, addr_uses_esp, gp_idx, w, addr_size_16); break;
    case TYPE_I2r:  destr(opcode & X86_OPCODE_REG_MASK); imm(); break;
    case TYPE_I2a:  destr(R_EAX); imm(); break;
    case TYPE_G2E:  decode_rm(s, rd_, addr, seg, addr_uses_esp, rs, w, addr_size_16); src1r(*rs); break;
    case TYPE_E2G:  decode_rm(s, rs, addr, seg, addr_uses_esp, rd_, w, addr_size_16); break;
    case TYPE_I2E:  decode_rm(s, rd_, addr, seg, addr_uses_esp, gp_idx, w, addr_size_16); imm(); break;
    case TYPE_Ib2E: decode_rm(s, rd_, addr, seg, addr_uses_esp, gp_idx, w, addr_size_16); simm(X86_WIDTH_BYTE); break;
    case TYPE_cl2E: decode_rm(s, rd_, addr, seg, addr_uses_esp, gp_idx, w, addr_size_16); *src1 = Rr(R_CL, X86_WIDTH_BYTE); break;
    case TYPE_1_E:  decode_rm(s, rd_, addr, seg, addr_uses_esp, gp_idx, w, addr_size_16); *src1 = 1; break;
    case TYPE_SI2E: decode_rm(s, rd_, addr, seg, addr_uses_esp, gp_idx, w, addr_size_16); simm(X86_WIDTH_BYTE); break;
    case TYPE_GP3:
      decode_rm(s, rd_, addr, seg, addr_uses_esp, gp_idx, w, addr_size_16);
      if (*gp_idx == 0) imm();
      break;
    case TYPE_Eb2G: decode_rm(s, rs, addr, seg, addr_uses_esp, rd_, X86_WIDTH_BYTE, addr_size_16); break;
    case TYPE_Ew2G: decode_rm(s, rs, addr, seg, addr_uses_esp, rd_, X86_WIDTH_WORD, addr_size_16); break;
    case TYPE_O2a:  destr(R_EAX); *seg = X86_SREG_DS; *addr = x86_inst_fetch(s, addr_size_16 ? X86_WIDTH_WORD : X86_WIDTH_DWORD); break;
    case TYPE_a2O:  *rs = R_EAX; *seg = X86_SREG_DS; *addr = x86_inst_fetch(s, addr_size_16 ? X86_WIDTH_WORD : X86_WIDTH_DWORD); break;
    case TYPE_P:    *imm = x86_inst_fetch(s, X86_WIDTH_BYTE); break;
    case TYPE_I_E2G:
      decode_rm(s, rs, addr, seg, addr_uses_esp, rd_, w, addr_size_16);
      imm();
      break;
    case TYPE_SI_E2G:
      decode_rm(s, rs, addr, seg, addr_uses_esp, rd_, w, addr_size_16);
      simm(X86_WIDTH_BYTE);
      break;
    case TYPE_Ib_G2E:
      decode_rm(s, rd_, addr, seg, addr_uses_esp, rs, w, addr_size_16);
      src1r(*rs);
      *imm = x86_inst_fetch(s, X86_WIDTH_BYTE);
      break;
    case TYPE_cl_G2E:
      decode_rm(s, rd_, addr, seg, addr_uses_esp, rs, w, addr_size_16);
      src1r(*rs);
      *imm = Rr(R_CL, X86_WIDTH_BYTE);
      break;
    case TYPE_N:    break;
    default: panic("Unsupported type = %d", type);
  }
}

/*
 * Evaluate the low-nibble condition-code encoding used by Jcc and SETcc.
 * The case numbers are the Intel condition-code values: overflow, below,
 * zero, sign, parity, less, and less-or-equal, each with its negated twin.
 */
static bool cc_eval(int cc) {
  bool cf = flag_get(FLAG_CF);
  bool zf = flag_get(FLAG_ZF);
  bool sf = flag_get(FLAG_SF);
  bool of = flag_get(FLAG_OF);
  bool pf = flag_get(FLAG_PF);

  switch (cc & X86_COND_MASK) {
    case X86_CC_O:  return of;
    case X86_CC_NO: return !of;
    case X86_CC_B:  return cf;
    case X86_CC_AE: return !cf;
    case X86_CC_Z:  return zf;
    case X86_CC_NZ: return !zf;
    case X86_CC_BE: return cf || zf;
    case X86_CC_A:  return !cf && !zf;
    case X86_CC_S:  return sf;
    case X86_CC_NS: return !sf;
    case X86_CC_P:  return pf;
    case X86_CC_NP: return !pf;
    case X86_CC_L:  return sf != of;
    case X86_CC_GE: return sf == of;
    case X86_CC_LE: return zf || (sf != of);
    case X86_CC_G:  return !zf && (sf == of);
    default: assert(0);
  }
}

/* Apply a relative conditional branch; x86 relative offsets are based on snpc after the instruction bytes. */
static word_t branch_target(word_t base, word_t offset, int width) {
  word_t target = base + offset;
  return width == X86_WIDTH_WORD ? (target & X86_WORD_MASK) : target;
}

static void jcc(Decode *s, int cc, word_t offset, int width) {
  if (cc_eval(cc)) s->dnpc = branch_target(s->snpc, offset, width);
}

/* Execute group-1 ALU opcodes 80/81/83, where the ModR/M reg field selects ADD/OR/ADC/SBB/AND/SUB/XOR/CMP. */
static void gp1(Decode *s, int gp_idx, int rd, word_t addr, int seg, int w, word_t imm) {
  word_t lhs = RMr(rd, w);
  word_t old_eflags = cpu.eflags;
  word_t result = alu_exec(gp_idx, lhs, imm, w);
  if (gp_idx != ALU_CMP) {
    word_t new_eflags = cpu.eflags;
    rm_write_defer_flags(rd, addr, seg, w, result, old_eflags, new_eflags);
  }
}

/* ALU form with register source and r/m destination; memory writes defer flag commit until after the write succeeds. */
static void alu_rm_reg(int op, int rd, word_t addr, int seg, int w, word_t src) {
  word_t lhs = RMr(rd, w);
  word_t old_eflags = cpu.eflags;
  word_t result = alu_exec(op, lhs, src, w);
  if (op != ALU_CMP) {
    word_t new_eflags = cpu.eflags;
    rm_write_defer_flags(rd, addr, seg, w, result, old_eflags, new_eflags);
  }
}

/* ALU form with r/m source and register destination; no deferred memory write is needed. */
static void alu_reg_rm(int op, int rd, int rs, word_t addr, int seg, int w) {
  word_t lhs = Rr(rd, w);
  word_t rhs = RMr(rs, w);
  word_t result = alu_exec(op, lhs, rhs, w);
  if (op != ALU_CMP) Rw(rd, w, result);
}

/*
 * INC/DEC update the same status flags as ADD/SUB by one, except CF is
 * architecturally preserved.  The deferred write keeps memory faults
 * restartable with the old flag image.
 */
static void incdec_rm(int is_dec, int rd, word_t addr, int seg, int w) {
  word_t old_eflags = cpu.eflags;
  bool old_cf = flag_get(FLAG_CF);
  word_t lhs = RMr(rd, w);
  word_t result = is_dec ? alu_exec(ALU_SUB, lhs, 1, w) : alu_exec(ALU_ADD, lhs, 1, w);
  flag_set(FLAG_CF, old_cf);
  word_t new_eflags = cpu.eflags;
  rm_write_defer_flags(rd, addr, seg, w, result, old_eflags, new_eflags);
}

/*
 * Execute the C0/C1/D0-D3 shift/rotate groups.  x86 masks the count to 5 bits
 * in 32-bit mode, then the ModR/M reg field selects ROL/ROR/SHL/SHR/SAR.  Only
 * a count of one defines OF for most of these instructions.
 */
static void shift_rm(Decode *s, int gp_idx, int rd, word_t addr, int seg, int w, word_t count) {
  count &= X86_SHIFT_COUNT_MASK;
  if (count == 0) return;

  int bits = w * X86_BITS_PER_BYTE;
  word_t lhs = RMr(rd, w);
  word_t result = lhs;
  bool cf = false;
  bool of = false;

  if (gp_idx == 2 || gp_idx == 3) {
    word_t rotate_count = (bits == X86_DWORD_BITS) ? count : (count % (bits + 1));
    if (rotate_count == 0) return;

    uint64_t operand_mask = width_mask(w);
    uint64_t ring_mask = (1ull << (bits + 1)) - 1ull;
    uint64_t ring = ((uint64_t)(flag_get(FLAG_CF) ? 1u : 0u) << bits) |
        (mask_width(lhs, w) & operand_mask);

    if (gp_idx == 2) {
      ring = ((ring << rotate_count) | (ring >> ((bits + 1) - rotate_count))) & ring_mask;
    }
    else {
      ring = ((ring >> rotate_count) | (ring << ((bits + 1) - rotate_count))) & ring_mask;
    }

    result = ring & operand_mask;
    cf = ((ring >> bits) & 1u) != 0;
    rm_write(rd, addr, seg, w, result);
    flag_set(FLAG_CF, cf);
    if (rotate_count == 1) {
      if (gp_idx == 2) {
        flag_set(FLAG_OF, (((result & sign_bit(w)) != 0) != cf));
      }
      else {
        flag_set(FLAG_OF, ((result ^ (result << 1)) & sign_bit(w)) != 0);
      }
    }
    return;
  }

  if (gp_idx == 0 || gp_idx == 1) {
    word_t rotate_count = count % bits;
    if (rotate_count == 0) return;

    lhs = mask_width(lhs, w);
    if (gp_idx == 0) {
      result = mask_width((lhs << rotate_count) | (lhs >> (bits - rotate_count)), w);
      cf = (result & 1) != 0;
      if (rotate_count == 1) of = (((result & sign_bit(w)) != 0) != cf);
    }
    else {
      result = mask_width((lhs >> rotate_count) | (lhs << (bits - rotate_count)), w);
      cf = (result & sign_bit(w)) != 0;
      if (rotate_count == 1) of = ((result ^ (result << 1)) & sign_bit(w)) != 0;
    }

      rm_write(rd, addr, seg, w, result);
      flag_set(FLAG_CF, cf);
    if (rotate_count == 1) flag_set(FLAG_OF, of);
    return;
  }

  switch (gp_idx) {
    case 4:
    case 6:
      result = mask_width(lhs << count, w);
      if (count <= (word_t)bits) cf = ((lhs >> (bits - count)) & 1) != 0;
      if (count == 1) of = (((result ^ lhs) & sign_bit(w)) != 0);
      break;
    case 5:
      result = mask_width(lhs, w) >> count;
      cf = ((lhs >> (count - 1)) & 1) != 0;
      if (count == 1) of = (lhs & sign_bit(w)) != 0;
      break;
    case 7:
      result = mask_width((word_t)(sext_width(lhs, w) >> count), w);
      cf = ((lhs >> (count - 1)) & 1) != 0;
      of = false;
      break;
    default:
      INV(s->pc);
      return;
  }

    rm_write(rd, addr, seg, w, result);
  set_zsp_flags(result, w);
  flag_set(FLAG_CF, cf);
  if (count == 1) flag_set(FLAG_OF, of);
}

/*
 * Execute SHLD/SHRD.  These use two same-sized operands: the r/m destination is
 * shifted while bits flow in from the register source.  Counts above operand
 * width are treated as the local subset's chosen deterministic behaviour.
 */
static void double_shift_rm(int is_right, int rd, word_t addr, int seg, int w, word_t src, word_t count) {
  count &= X86_SHIFT_COUNT_MASK;
  if (count == 0) return;

  int bits = w * X86_BITS_PER_BYTE;
  if (count > (word_t)bits) {
    count %= bits;
    if (count == 0) return;
  }

  word_t dest = mask_width(RMr(rd, w), w);
  src = mask_width(src, w);

  word_t result;
  bool cf;
  if (is_right) {
    result = count == (word_t)bits ? src : ((dest >> count) | (src << (bits - count)));
    cf = ((dest >> (count - 1)) & 1) != 0;
  }
  else {
    result = count == (word_t)bits ? src : ((dest << count) | (src >> (bits - count)));
    cf = ((dest >> (bits - count)) & 1) != 0;
  }
  result = mask_width(result, w);

  rm_write(rd, addr, seg, w, result);
  set_zsp_flags(result, w);
  flag_set(FLAG_CF, cf);
  if (count == 1) {
    flag_set(FLAG_OF, ((dest ^ result) & sign_bit(w)) != 0);
  }
}

/* MOVS copies source-segment:(E)SI to ES:(E)DI, then applies DF by element width. */
static void movs(int w, int src_seg, bool addr_size_16) {
  word_t src = addr_size_16 ? reg_w(R_SI) : cpu.esi;
  word_t dst = addr_size_16 ? reg_w(R_DI) : cpu.edi;
  word_t data = Mr(src, src_seg, w);
  Mw(dst, X86_SREG_ES, w, data);
  if (flag_get(FLAG_DF)) {
    if (addr_size_16) {
      reg_w(R_SI) -= w;
      reg_w(R_DI) -= w;
    }
    else {
      cpu.esi -= w;
      cpu.edi -= w;
    }
  }
  else {
    if (addr_size_16) {
      reg_w(R_SI) += w;
      reg_w(R_DI) += w;
    }
    else {
      cpu.esi += w;
      cpu.edi += w;
    }
  }
}

/* PUSHA/PUSHAD save the original stack pointer slot between BX/EBX and BP/EBP. */
static void pusha_width(int width) {
  word_t old_esp = cpu.esp;
  push_width(width, Rr(R_EAX, width));
  push_width(width, Rr(R_ECX, width));
  push_width(width, Rr(R_EDX, width));
  push_width(width, Rr(R_EBX, width));
  push_width(width, width == X86_WIDTH_WORD ? (old_esp & X86_WORD_MASK) : old_esp);
  push_width(width, Rr(R_EBP, width));
  push_width(width, Rr(R_ESI, width));
  push_width(width, Rr(R_EDI, width));
}

/* POPA/POPAD restore the register order and skip the saved SP/ESP image. */
static void popa_width(int width) {
  Rw(R_EDI, width, pop_width(width));
  Rw(R_ESI, width, pop_width(width));
  Rw(R_EBP, width, pop_width(width));
  cpu.esp += width; // POPA discards the saved SP/ESP slot.
  Rw(R_EBX, width, pop_width(width));
  Rw(R_EDX, width, pop_width(width));
  Rw(R_ECX, width, pop_width(width));
  Rw(R_EAX, width, pop_width(width));
}

/*
 * Minimal protected-mode IRET for the AM/Nanos path.  Same-privilege returns
 * pop EIP/CS/EFLAGS; returns to an outer ring also pop ESP/SS.  Full descriptor
 * validation, NT, and VM86 returns remain outside this interpreter subset.
 */
static void iret32(Decode *s) {
  word_t target = pop32();
  word_t cs = pop32() & X86_WORD_MASK;
  word_t eflags = eflags_write_protected(pop32());
  int old_cpl = cpu.cs & X86_SELECTOR_RPL_MASK;
  int new_cpl = cs & X86_SELECTOR_RPL_MASK;

  Assert(new_cpl >= old_cpl, "x86 iret from CPL %d to inner CPL %d would raise #GP at pc = " FMT_WORD,
      old_cpl, new_cpl, s->pc);

  /*
   * Intel iret pops the saved user stack only when returning to a less
   * privileged ring.  Kernel-to-kernel returns use the current stack.
   */
  if (new_cpl > old_cpl) {
    word_t esp = pop32();
    word_t ss = pop32() & X86_WORD_MASK;
    cpu.esp = esp;
    load_seg_cache_from_gdt(X86_SREG_SS, ss);
  }

  s->dnpc = target;
  load_seg_cache_from_gdt(X86_SREG_CS, cs);
  cpu.eflags = eflags;
}

/* Read CR0/CR2/CR3/CR4 after checking that the instruction is privileged. */
static word_t cr_read(Decode *s, int cr_idx) {
  require_kernel(s, "mov from control register");

  switch (cr_idx) {
    case X86_CR0_INDEX: return cpu.cr0;
    case X86_CR2_INDEX: return cpu.cr2;
    case X86_CR3_INDEX: return cpu.cr3;
    case X86_CR4_INDEX: return cpu.cr4;
    default:
      INV(s->pc);
      return 0;
  }
}

static inline uint32_t cr3_translation_key(word_t data) {
  return data & ~X86_PAGE_OFFSET_MASK;
}

static inline uint32_t cr0_translation_bits(word_t data) {
  return data & (CR0_PG | CR0_WP);
}

static inline uint32_t cr4_translation_bits(word_t data) {
  return data & (CR4_PSE | CR4_PAE);
}

/* Write CR0/CR2/CR3/CR4 and enforce the small set of architectural constraints modelled here. */
static void cr_write(Decode *s, int cr_idx, word_t data) {
  require_kernel(s, "mov to control register");

  switch (cr_idx) {
    case X86_CR0_INDEX: {
      /*
       * Paging without protected mode is architecturally invalid.  AM enables
       * paging from the flat protected-mode reset state, so this rejects only
       * broken CR0 combinations while preserving the normal VME path.
       */
      Assert((data & CR0_PG) == 0 || (data & CR0_PE) != 0,
          "x86 mov to CR0 sets PG while PE is clear at pc = " FMT_WORD, s->pc);
      const uint32_t old_cr0_translation = cr0_translation_bits(cpu.cr0);
      cpu.cr0 = data;
      if (cr0_translation_bits(cpu.cr0) != old_cr0_translation) {
        isa_jit_flush_data_tlb();
      }
      break;
    }
    case X86_CR2_INDEX:
      /* CR2 is normally written by page-fault hardware; monitor tests may still restore it. */
      cpu.cr2 = data;
      break;
    case X86_CR3_INDEX: {
      const uint32_t old_cr3_key = cr3_translation_key(cpu.cr3);
      cpu.cr3 = cr3_translation_key(data) | (data & CR3_PWT_PCD_MASK);
      /*
       * NEMU already invalidates the JIT DTLB on guest page-table writes.
       * Re-loading the same page-directory base on a context return therefore
       * does not make cached translations stale.
       */
      if (cr3_translation_key(cpu.cr3) != old_cr3_key) {
        isa_jit_flush_data_tlb();
      }
      break;
    }
    case X86_CR4_INDEX: {
      const uint32_t old_cr4_translation = cr4_translation_bits(cpu.cr4);
      cpu.cr4 = data;
      if (cr4_translation_bits(cpu.cr4) != old_cr4_translation) {
        isa_jit_flush_data_tlb();
      }
      break;
    }
    default:
      INV(s->pc);
      break;
  }
}

/* MOV r32,CRn; memory destinations are invalid for this control-register encoding. */
static void mov_cr_to_rm(Decode *s, int cr_idx, int rd) {
  if (rd == -1) {
    INV(s->pc);
    return;
  }

  Rw(rd, X86_WIDTH_DWORD, cr_read(s, cr_idx));
}

/* MOV CRn,r32; the source must be a register operand in the ModR/M byte. */
static void mov_rm_to_cr(Decode *s, int cr_idx, int rd) {
  if (rd == -1) {
    INV(s->pc);
    return;
  }

  cr_write(s, cr_idx, Rr(rd, X86_WIDTH_DWORD));
}

/*
 * Load GDTR/IDTR from the 6-byte pseudo-descriptor used by 32-bit LGDT/LIDT:
 * a 16-bit limit followed by a 32-bit base.  The ModR/M reg field is /2 for
 * LGDT and /3 for LIDT.
 */
static void load_desc_table(Decode *s, int gp_idx, int rd, word_t addr, int seg) {
  require_kernel(s, gp_idx == X86_GROUP_LGDT_EXT ? "lgdt" : "lidt");

  if (rd != -1) {
    INV(s->pc);
    return;
  }

  uint16_t limit = Mr(addr, seg, X86_WIDTH_WORD);
  uint32_t base = Mr(addr + X86_WIDTH_WORD, seg, X86_WIDTH_DWORD);

  switch (gp_idx) {
    case X86_GROUP_LGDT_EXT:
      cpu.gdtr_limit = limit;
      cpu.gdtr_base = base;
      break;
    case X86_GROUP_LIDT_EXT:
      cpu.idtr_limit = limit;
      cpu.idtr_base = base;
      break;
    default:
      INV(s->pc);
      break;
  }
}

/*
 * Load the task register from a 16-bit selector.  LTR is encoded as group /3 of
 * 0F 00 and accepts only an available 32-bit TSS descriptor; after loading it
 * marks the descriptor busy in memory.
 */
static void ltr(Decode *s, int gp_idx, int rd, word_t addr, int seg, int w) {
  require_kernel(s, "ltr");

  if (gp_idx != X86_GROUP_LTR_EXT) {
    INV(s->pc);
    return;
  }

  uint16_t selector = RMr(rd, w) & X86_WORD_MASK;
  uint32_t index = selector >> X86_SELECTOR_INDEX_SHIFT;
  uint32_t desc_off = index * X86_DESC_SIZE;

  Assert((selector & ~X86_SELECTOR_RPL_MASK) != 0, "x86 ltr uses a null selector at pc = " FMT_WORD, s->pc);
  Assert((selector & X86_SELECTOR_TI_MASK) == 0, "x86 ltr selector %#x uses the unsupported LDT table", selector);
  Assert(desc_off + X86_DESC_LAST_BYTE <= cpu.gdtr_limit,
      "x86 ltr selector %#x exceeds GDT limit %#x", selector, cpu.gdtr_limit);

  vaddr_t desc_addr = cpu.gdtr_base + desc_off;
  uint32_t lo = x86_vaddr_read_kernel(desc_addr, X86_WIDTH_DWORD);
  uint32_t hi = x86_vaddr_read_kernel(desc_addr + X86_DESC_HIGH_OFFSET, X86_WIDTH_DWORD);
  uint32_t type = (hi >> X86_DESC_TYPE_SHIFT) & X86_DESC_TYPE_MASK;
  uint32_t system = (hi >> X86_DESC_S_SHIFT) & X86_ONE_BIT_MASK;
  uint32_t present = (hi >> X86_DESC_PRESENT_SHIFT) & X86_ONE_BIT_MASK;

  Assert(present != 0, "x86 ltr selector %#x points to a non-present descriptor", selector);
  Assert(system == 0 && type == X86_TSS_AVAILABLE_32,
      "x86 ltr selector %#x uses unsupported descriptor type %#x", selector, type);

  /*
   * LTR is defined for an available TSS and changes the descriptor to busy in
   * memory.  AM loads TR once during CTE setup; marking the descriptor here keeps
   * subsequent accidental LTRs from silently reusing the same available TSS.
   */
  x86_vaddr_write_kernel(desc_addr + X86_DESC_HIGH_OFFSET, X86_WIDTH_DWORD,
      hi | (X86_TSS_BUSY_TYPE_BIT << X86_DESC_TYPE_SHIFT));
  cpu.tr = selector;
  cpu.tss_base = desc_base(lo, hi);
  cpu.tss_limit = desc_limit(lo, hi);
}

/*
 * Execute F6/F7 group-3 operations.  The group index maps to TEST, NOT, NEG,
 * MUL, IMUL, DIV, and IDIV.  DIV/IDIV use x86's implicit accumulator pairs
 * (AX, DX:AX, or EDX:EAX) and raise #DE-like assertions for zero/overflow.
 */
static void gp3(Decode *s, int gp_idx, int rd, word_t addr, int seg, int w, word_t imm) {
  word_t lhs = RMr(rd, w);
  switch (gp_idx) {
    case 0: // /0 TEST r/m, imm: updates flags only.
      set_logic_flags(lhs & imm, w);
      break;
    case 2: // /2 NOT r/m: bitwise complement with no defined flag updates.
      RMw(~lhs);
      break;
    case 3: { // /3 NEG r/m: subtract operand from zero.
      word_t old_eflags = cpu.eflags;
      word_t result = alu_exec(ALU_SUB, 0, lhs, w);
      flag_set(FLAG_CF, mask_width(lhs, w) != 0);
      word_t new_eflags = cpu.eflags;
      rm_write_defer_flags(rd, addr, seg, w, result, old_eflags, new_eflags);
      break;
    }
    case 4: { // /4 MUL: unsigned multiply with implicit accumulator operands.
      if (w == X86_WIDTH_BYTE) {
        uint16_t product = (uint8_t)Rr(R_EAX, X86_WIDTH_BYTE) * (uint8_t)lhs;
        Rw(R_EAX, X86_WIDTH_WORD, product);
        flag_set(FLAG_CF, (product >> X86_BITS_PER_BYTE) != 0);
        flag_set(FLAG_OF, (product >> X86_BITS_PER_BYTE) != 0);
      }
      else if (w == X86_WIDTH_WORD) {
        uint32_t product = (uint16_t)Rr(R_EAX, X86_WIDTH_WORD) * (uint16_t)lhs;
        Rw(R_EAX, X86_WIDTH_WORD, product);
        Rw(R_EDX, X86_WIDTH_WORD, product >> X86_WORD_BITS);
        flag_set(FLAG_CF, (product >> X86_WORD_BITS) != 0);
        flag_set(FLAG_OF, (product >> X86_WORD_BITS) != 0);
      }
      else {
        uint64_t product = (uint64_t)reg_l(R_EAX) * (uint64_t)mask_width(lhs, X86_WIDTH_DWORD);
        cpu.eax = product;
        cpu.edx = product >> X86_DWORD_BITS;
        flag_set(FLAG_CF, cpu.edx != 0);
        flag_set(FLAG_OF, cpu.edx != 0);
      }
      break;
    }
    case 5: { // /5 IMUL: signed multiply with implicit accumulator operands.
      if (w == X86_WIDTH_BYTE) {
        int16_t product = (int16_t)((int8_t)Rr(R_EAX, X86_WIDTH_BYTE) * (int8_t)lhs);
        Rw(R_EAX, X86_WIDTH_WORD, (uint16_t)product);
        bool truncated = product != (int16_t)(int8_t)product;
        flag_set(FLAG_CF, truncated);
        flag_set(FLAG_OF, truncated);
      }
      else if (w == X86_WIDTH_WORD) {
        int32_t product = (int32_t)((int16_t)Rr(R_EAX, X86_WIDTH_WORD) * (int16_t)lhs);
        Rw(R_EAX, X86_WIDTH_WORD, (uint16_t)product);
        Rw(R_EDX, X86_WIDTH_WORD, (uint32_t)product >> X86_WORD_BITS);
        bool truncated = product != (int32_t)(int16_t)product;
        flag_set(FLAG_CF, truncated);
        flag_set(FLAG_OF, truncated);
      }
      else {
        int64_t product = (int64_t)(int32_t)cpu.eax * (int64_t)(int32_t)lhs;
        cpu.eax = product;
        cpu.edx = (uint64_t)product >> X86_DWORD_BITS;
        bool truncated = product != (int64_t)(int32_t)cpu.eax;
        flag_set(FLAG_CF, truncated);
        flag_set(FLAG_OF, truncated);
      }
      break;
    }
    case 6: { // /6 DIV: unsigned divide, quotient must fit AL/AX/EAX.
      if (w == X86_WIDTH_BYTE) {
        uint16_t dividend = Rr(R_EAX, X86_WIDTH_WORD);
        uint8_t divisor = lhs;
        Assert(divisor != 0, "x86 div by zero at pc = " FMT_WORD, s->pc);
        uint16_t quotient = dividend / divisor;
        uint8_t remainder = dividend % divisor;
        Assert(quotient <= X86_BYTE_MASK, "x86 div quotient overflow at pc = " FMT_WORD, s->pc);
        Rw(R_AL, X86_WIDTH_BYTE, quotient);
        Rw(R_AH, X86_WIDTH_BYTE, remainder);
      }
      else if (w == X86_WIDTH_WORD) {
        uint32_t dividend = ((uint32_t)Rr(R_EDX, X86_WIDTH_WORD) << X86_WORD_BITS) |
            Rr(R_EAX, X86_WIDTH_WORD);
        uint16_t divisor = lhs;
        Assert(divisor != 0, "x86 div by zero at pc = " FMT_WORD, s->pc);
        uint32_t quotient = dividend / divisor;
        uint16_t remainder = dividend % divisor;
        Assert(quotient <= X86_WORD_MASK, "x86 div quotient overflow at pc = " FMT_WORD, s->pc);
        Rw(R_EAX, X86_WIDTH_WORD, quotient);
        Rw(R_EDX, X86_WIDTH_WORD, remainder);
      }
      else {
        uint64_t dividend = ((uint64_t)cpu.edx << X86_DWORD_BITS) | cpu.eax;
        uint32_t divisor = lhs;
        Assert(divisor != 0, "x86 div by zero at pc = " FMT_WORD, s->pc);
        uint64_t quotient = dividend / divisor;
        Assert(quotient <= X86_DWORD_MASK, "x86 div quotient overflow at pc = " FMT_WORD, s->pc);
        cpu.eax = quotient;
        cpu.edx = dividend % divisor;
      }
      break;
    }
    case 7: { // /7 IDIV: signed divide, including the signed-minimum / -1 overflow guard.
      if (w == X86_WIDTH_BYTE) {
        int16_t dividend = (int16_t)Rr(R_EAX, X86_WIDTH_WORD);
        int8_t divisor = lhs;
        Assert(divisor != 0, "x86 idiv by zero at pc = " FMT_WORD, s->pc);
        Assert(!(dividend == INT16_MIN && divisor == -1),
            "x86 idiv quotient overflow at pc = " FMT_WORD, s->pc);
        int16_t quotient = dividend / divisor;
        int8_t remainder = dividend % divisor;
        Assert(quotient >= INT8_MIN && quotient <= INT8_MAX,
            "x86 idiv quotient overflow at pc = " FMT_WORD, s->pc);
        Rw(R_AL, X86_WIDTH_BYTE, quotient);
        Rw(R_AH, X86_WIDTH_BYTE, remainder);
      }
      else if (w == X86_WIDTH_WORD) {
        int32_t dividend = (int32_t)(((uint32_t)Rr(R_EDX, X86_WIDTH_WORD) << X86_WORD_BITS) |
            Rr(R_EAX, X86_WIDTH_WORD));
        int16_t divisor = lhs;
        Assert(divisor != 0, "x86 idiv by zero at pc = " FMT_WORD, s->pc);
        Assert(!(dividend == INT32_MIN && divisor == -1),
            "x86 idiv quotient overflow at pc = " FMT_WORD, s->pc);
        int32_t quotient = dividend / divisor;
        int16_t remainder = dividend % divisor;
        Assert(quotient >= INT16_MIN && quotient <= INT16_MAX,
            "x86 idiv quotient overflow at pc = " FMT_WORD, s->pc);
        Rw(R_EAX, X86_WIDTH_WORD, quotient);
        Rw(R_EDX, X86_WIDTH_WORD, remainder);
      }
      else {
        int64_t dividend = (int64_t)(int32_t)cpu.edx * X86_DWORD_BASE + cpu.eax;
        int32_t divisor = lhs;
        Assert(divisor != 0, "x86 idiv by zero at pc = " FMT_WORD, s->pc);
        Assert(!(dividend == INT64_MIN && divisor == -1),
            "x86 idiv quotient overflow at pc = " FMT_WORD, s->pc);
        int64_t quotient = dividend / divisor;
        Assert(quotient >= INT32_MIN && quotient <= INT32_MAX,
            "x86 idiv quotient overflow at pc = " FMT_WORD, s->pc);
        cpu.eax = quotient;
        cpu.edx = dividend % divisor;
      }
      break;
    }
    default:
      INV(s->pc);
      break;
  }
}

/*
 * Execute FF group-5 operations.  Only INC, DEC, near CALL, near JMP, and PUSH
 * are implemented because those are the forms used by this 32-bit AM/Nanos
 * environment; each case reads r/m only when it needs the value.
 */
static word_t control_target(word_t target, int width) {
  return width == X86_WIDTH_WORD ? (target & X86_WORD_MASK) : target;
}

static void gp5(Decode *s, int gp_idx, int rd, word_t addr, int seg, int w) {
  switch (gp_idx) {
    case 0:
      incdec_rm(0, rd, addr, seg, w);
      break;
    case 1:
      incdec_rm(1, rd, addr, seg, w);
      break;
    case 2:
    {
      word_t target = RMr(rd, w);
      push_width(w, s->snpc);
      s->dnpc = control_target(target, w);
      break;
    }
    case 4:
    {
      word_t target = RMr(rd, w);
      s->dnpc = control_target(target, w);
      break;
    }
    case 6:
    {
      word_t target = RMr(rd, w);
      push_width(w, target);
      break;
    }
    default:
      INV(s->pc);
      break;
  }
}

/* Execute FE group-4 byte INC/DEC operations. */
static void gp4(Decode *s, int gp_idx, int rd, word_t addr, int seg, int w) {
  switch (gp_idx) {
    case 0:
      incdec_rm(0, rd, addr, seg, w);
      break;
    case 1:
      incdec_rm(1, rd, addr, seg, w);
      break;
    default:
      INV(s->pc);
      break;
  }
}

/* Return the highest set-bit index for BSR, or -1 so the caller can set ZF on a zero input. */
static int bsr_index(word_t value, int width) {
  value = mask_width(value, width);
  for (int bit = width * X86_BITS_PER_BYTE - 1; bit >= 0; bit--) {
    if ((value & (1u << bit)) != 0) {
      return bit;
    }
  }
  return -1;
}

/* Return the lowest set-bit index for BSF, or -1 so the caller can set ZF on a zero input. */
static int bsf_index(word_t value, int width) {
  value = mask_width(value, width);
  for (int bit = 0; bit < width * X86_BITS_PER_BYTE; bit++) {
    if ((value & (1u << bit)) != 0) {
      return bit;
    }
  }
  return -1;
}

/* Handle the compact 90+rd XCHG form, which always exchanges the selected register with EAX. */
static void xchg_reg_eax(int reg, int width) {
  word_t old_eax = Rr(R_EAX, width);
  word_t old_reg = Rr(reg, width);
  Rw(R_EAX, width, old_reg);
  Rw(reg, width, old_eax);
}

/* Handle the ModR/M XCHG form by swapping r/m and register operands. */
static void xchg_rm_reg(int rm_reg, word_t rm_addr, int rm_seg, int reg, int width) {
  word_t old_rm = (rm_reg != -1) ? Rr(rm_reg, width) : Mr(rm_addr, rm_seg, width);
  word_t old_reg = Rr(reg, width);

  /*
   * For a memory destination, commit the memory write before changing the
   * register.  If the write raises #PF, the architectural register state still
   * describes the instruction before it ran, which keeps the fault restartable.
   */
  rm_write(rm_reg, rm_addr, rm_seg, width, old_reg);
  Rw(reg, width, old_rm);
}

/* Execute opcode 98: CBW when 0x66 forced 16-bit operand size, otherwise CWDE. */
static void cbw_cwde(bool operand_size_16) {
  if (operand_size_16) {
    /*
     * 0x66 0x98 is CBW in 32-bit mode: AL is sign-extended into AX only, so the
     * high half of EAX is intentionally preserved.
     */
    Rw(R_EAX, X86_WIDTH_WORD, (uint16_t)(int16_t)(int8_t)Rr(R_EAX, X86_WIDTH_BYTE));
  }
  else {
    cpu.eax = (uint32_t)(int32_t)(int16_t)Rr(R_EAX, X86_WIDTH_WORD);
  }
}

/* Execute opcode 99: CWD when 0x66 forced 16-bit operand size, otherwise CDQ. */
static void cwd_cdq(bool operand_size_16) {
  if (operand_size_16) {
    /*
     * 0x66 0x99 is CWD in 32-bit mode: AX controls the sign and only DX is
     * written.  CDQ remains the default 32-bit form below.
     */
    Rw(R_EDX, X86_WIDTH_WORD, ((int16_t)Rr(R_EAX, X86_WIDTH_WORD) < 0) ? X86_WORD_MASK : 0u);
  }
  else {
    cpu.edx = ((int32_t)cpu.eax < 0) ? X86_DWORD_MASK : 0;
  }
}

/*
 * Decode and execute the 0F two-byte opcode map.  The pattern strings are the
 * second opcode byte in binary; the width column follows the local convention
 * where 0 means operand-size dependent, 1/2/4 are fixed byte widths.
 */
void _2byte_esc(Decode *s, bool is_operand_size_16, bool is_addr_size_16, int seg_override) {
  uint8_t opcode = x86_inst_fetch(s, X86_WIDTH_BYTE);
  INSTPAT_START();
  INSTPAT("0000 0000", ltr,       E,    2, ltr(s, gp_idx, rd, addr, seg, w));
  INSTPAT("0000 0001", lgdt_lidt, E,    0, load_desc_table(s, gp_idx, rd, addr, seg));
  INSTPAT("0010 0000", mov_cr,    E,    4, mov_cr_to_rm(s, gp_idx, rd));
  INSTPAT("0010 0010", mov_cr,    E,    4, mov_rm_to_cr(s, gp_idx, rd));
  INSTPAT("1000 ????", jcc,       J,    0, jcc(s, opcode & X86_COND_MASK, imm, w));
  INSTPAT("1001 ????", setcc,     E,    1, RMw(cc_eval(opcode & X86_COND_MASK) ? 1 : 0));
  INSTPAT("1010 0100", shld,      Ib_G2E, 0, double_shift_rm(0, rd, addr, seg, w, src1, imm));
  INSTPAT("1010 0101", shld,      cl_G2E, 0, double_shift_rm(0, rd, addr, seg, w, src1, imm));
  INSTPAT("1010 1100", shrd,      Ib_G2E, 0, double_shift_rm(1, rd, addr, seg, w, src1, imm));
  INSTPAT("1010 1101", shrd,      cl_G2E, 0, double_shift_rm(1, rd, addr, seg, w, src1, imm));
  INSTPAT("1010 1111", imul,      E2G,  0, {
    int64_t product = (int64_t)sext_width(Rr(rd, w), w) * (int64_t)sext_width(RMr(rs, w), w);
    Rw(rd, w, product);
    bool truncated = product != (int64_t)sext_width(Rr(rd, w), w);
    flag_set(FLAG_CF, truncated);
    flag_set(FLAG_OF, truncated);
  });
  INSTPAT("1011 1101", bsr,       E2G,  0, {
    int index = bsr_index(RMr(rs, w), w);
    flag_set(FLAG_ZF, index < 0);
    if (index >= 0) {
      Rw(rd, w, index);
    }
  });
  INSTPAT("1011 1100", bsf,       E2G,  0, {
    int index = bsf_index(RMr(rs, w), w);
    flag_set(FLAG_ZF, index < 0);
    if (index >= 0) {
      Rw(rd, w, index);
    }
  });
  INSTPAT("1011 0110", movzx,     Eb2G, 0, Rw(rd, w, RMr(rs, X86_WIDTH_BYTE)));
  INSTPAT("1011 0111", movzx,     Ew2G, 0, Rw(rd, w, RMr(rs, X86_WIDTH_WORD)));
  INSTPAT("1011 1110", movsx,     Eb2G, 0, Rw(rd, w, sext_width(RMr(rs, X86_WIDTH_BYTE), X86_WIDTH_BYTE)));
  INSTPAT("1011 1111", movsx,     Ew2G, 0, Rw(rd, w, sext_width(RMr(rs, X86_WIDTH_WORD), X86_WIDTH_WORD)));
  INSTPAT("???? ????", inv,    N,    0, INV(s->pc));
  INSTPAT_END();
}

/*
 * Execute one x86 instruction from the primary opcode map.  Prefix 0x66 loops
 * back to fetch the real opcode with the operand-size attribute set to 16-bit;
 * all other rows use INSTPAT's width column and operand form to perform decode
 * and execution in one compact table.
 */
int isa_exec_once(Decode *s) {
  bool is_operand_size_16 = false;
  bool is_addr_size_16 = false;
  int seg_override = -1;
  uint8_t opcode = 0;

again:
  opcode = x86_inst_fetch(s, X86_WIDTH_BYTE);

  INSTPAT_START();

  INSTPAT("0000 1111", 2byte_esc, N,    0, _2byte_esc(s, is_operand_size_16, is_addr_size_16, seg_override));

  INSTPAT("0110 0110", data_size, N,    0, is_operand_size_16 = true; goto again;);
  INSTPAT("0110 0111", addr_size, N,    0, is_addr_size_16 = true; goto again;);
  INSTPAT("0010 0110", es_prefix, N,    0, seg_override = X86_SREG_ES; goto again;);
  INSTPAT("0010 1110", cs_prefix, N,    0, seg_override = X86_SREG_CS; goto again;);
  INSTPAT("0011 0110", ss_prefix, N,    0, seg_override = X86_SREG_SS; goto again;);
  INSTPAT("0011 1110", ds_prefix, N,    0, seg_override = X86_SREG_DS; goto again;);

  INSTPAT("0000 0000", add,       G2E,  1, alu_rm_reg(ALU_ADD, rd, addr, seg, w, src1));
  INSTPAT("0000 0001", add,       G2E,  0, alu_rm_reg(ALU_ADD, rd, addr, seg, w, src1));
  INSTPAT("0000 0010", add,       E2G,  1, alu_reg_rm(ALU_ADD, rd, rs, addr, seg, w));
  INSTPAT("0000 0011", add,       E2G,  0, alu_reg_rm(ALU_ADD, rd, rs, addr, seg, w));
  INSTPAT("0000 0100", add,       I2a,  1, Rw(R_EAX, w, alu_exec(ALU_ADD, Rr(R_EAX, w), imm, w)));
  INSTPAT("0000 0101", add,       I2a,  0, Rw(R_EAX, w, alu_exec(ALU_ADD, Rr(R_EAX, w), imm, w)));

  INSTPAT("0000 1000", or,        G2E,  1, alu_rm_reg(ALU_OR, rd, addr, seg, w, src1));
  INSTPAT("0000 1001", or,        G2E,  0, alu_rm_reg(ALU_OR, rd, addr, seg, w, src1));
  INSTPAT("0000 1010", or,        E2G,  1, alu_reg_rm(ALU_OR, rd, rs, addr, seg, w));
  INSTPAT("0000 1011", or,        E2G,  0, alu_reg_rm(ALU_OR, rd, rs, addr, seg, w));
  INSTPAT("0000 1100", or,        I2a,  1, Rw(R_EAX, w, alu_exec(ALU_OR, Rr(R_EAX, w), imm, w)));
  INSTPAT("0000 1101", or,        I2a,  0, Rw(R_EAX, w, alu_exec(ALU_OR, Rr(R_EAX, w), imm, w)));

  INSTPAT("0001 0000", adc,       G2E,  1, alu_rm_reg(ALU_ADC, rd, addr, seg, w, src1));
  INSTPAT("0001 0001", adc,       G2E,  0, alu_rm_reg(ALU_ADC, rd, addr, seg, w, src1));
  INSTPAT("0001 0010", adc,       E2G,  1, alu_reg_rm(ALU_ADC, rd, rs, addr, seg, w));
  INSTPAT("0001 0011", adc,       E2G,  0, alu_reg_rm(ALU_ADC, rd, rs, addr, seg, w));
  INSTPAT("0001 0100", adc,       I2a,  1, Rw(R_EAX, w, alu_exec(ALU_ADC, Rr(R_EAX, w), imm, w)));
  INSTPAT("0001 0101", adc,       I2a,  0, Rw(R_EAX, w, alu_exec(ALU_ADC, Rr(R_EAX, w), imm, w)));

  INSTPAT("0001 1000", sbb,       G2E,  1, alu_rm_reg(ALU_SBB, rd, addr, seg, w, src1));
  INSTPAT("0001 1001", sbb,       G2E,  0, alu_rm_reg(ALU_SBB, rd, addr, seg, w, src1));
  INSTPAT("0001 1010", sbb,       E2G,  1, alu_reg_rm(ALU_SBB, rd, rs, addr, seg, w));
  INSTPAT("0001 1011", sbb,       E2G,  0, alu_reg_rm(ALU_SBB, rd, rs, addr, seg, w));
  INSTPAT("0001 1100", sbb,       I2a,  1, Rw(R_EAX, w, alu_exec(ALU_SBB, Rr(R_EAX, w), imm, w)));
  INSTPAT("0001 1101", sbb,       I2a,  0, Rw(R_EAX, w, alu_exec(ALU_SBB, Rr(R_EAX, w), imm, w)));

  INSTPAT("0010 0000", and,       G2E,  1, alu_rm_reg(ALU_AND, rd, addr, seg, w, src1));
  INSTPAT("0010 0001", and,       G2E,  0, alu_rm_reg(ALU_AND, rd, addr, seg, w, src1));
  INSTPAT("0010 0010", and,       E2G,  1, alu_reg_rm(ALU_AND, rd, rs, addr, seg, w));
  INSTPAT("0010 0011", and,       E2G,  0, alu_reg_rm(ALU_AND, rd, rs, addr, seg, w));
  INSTPAT("0010 0100", and,       I2a,  1, Rw(R_EAX, w, alu_exec(ALU_AND, Rr(R_EAX, w), imm, w)));
  INSTPAT("0010 0101", and,       I2a,  0, Rw(R_EAX, w, alu_exec(ALU_AND, Rr(R_EAX, w), imm, w)));

  INSTPAT("0010 1000", sub,       G2E,  1, alu_rm_reg(ALU_SUB, rd, addr, seg, w, src1));
  INSTPAT("0010 1001", sub,       G2E,  0, alu_rm_reg(ALU_SUB, rd, addr, seg, w, src1));
  INSTPAT("0010 1010", sub,       E2G,  1, alu_reg_rm(ALU_SUB, rd, rs, addr, seg, w));
  INSTPAT("0010 1011", sub,       E2G,  0, alu_reg_rm(ALU_SUB, rd, rs, addr, seg, w));
  INSTPAT("0010 1100", sub,       I2a,  1, Rw(R_EAX, w, alu_exec(ALU_SUB, Rr(R_EAX, w), imm, w)));
  INSTPAT("0010 1101", sub,       I2a,  0, Rw(R_EAX, w, alu_exec(ALU_SUB, Rr(R_EAX, w), imm, w)));

  INSTPAT("0011 0000", xor,       G2E,  1, alu_rm_reg(ALU_XOR, rd, addr, seg, w, src1));
  INSTPAT("0011 0001", xor,       G2E,  0, alu_rm_reg(ALU_XOR, rd, addr, seg, w, src1));
  INSTPAT("0011 0010", xor,       E2G,  1, alu_reg_rm(ALU_XOR, rd, rs, addr, seg, w));
  INSTPAT("0011 0011", xor,       E2G,  0, alu_reg_rm(ALU_XOR, rd, rs, addr, seg, w));
  INSTPAT("0011 0100", xor,       I2a,  1, Rw(R_EAX, w, alu_exec(ALU_XOR, Rr(R_EAX, w), imm, w)));
  INSTPAT("0011 0101", xor,       I2a,  0, Rw(R_EAX, w, alu_exec(ALU_XOR, Rr(R_EAX, w), imm, w)));

  INSTPAT("0011 1000", cmp,       G2E,  1, alu_rm_reg(ALU_CMP, rd, addr, seg, w, src1));
  INSTPAT("0011 1001", cmp,       G2E,  0, alu_rm_reg(ALU_CMP, rd, addr, seg, w, src1));
  INSTPAT("0011 1010", cmp,       E2G,  1, alu_reg_rm(ALU_CMP, rd, rs, addr, seg, w));
  INSTPAT("0011 1011", cmp,       E2G,  0, alu_reg_rm(ALU_CMP, rd, rs, addr, seg, w));
  INSTPAT("0011 1100", cmp,       I2a,  1, alu_exec(ALU_CMP, Rr(R_EAX, w), imm, w));
  INSTPAT("0011 1101", cmp,       I2a,  0, alu_exec(ALU_CMP, Rr(R_EAX, w), imm, w));

  INSTPAT("0100 0???", inc,       r,    0, {
    bool old_cf = flag_get(FLAG_CF);
    Rw(rd, w, alu_exec(ALU_ADD, Rr(rd, w), 1, w));
    flag_set(FLAG_CF, old_cf);
  });
  INSTPAT("0100 1???", dec,       r,    0, {
    bool old_cf = flag_get(FLAG_CF);
    Rw(rd, w, alu_exec(ALU_SUB, Rr(rd, w), 1, w));
    flag_set(FLAG_CF, old_cf);
  });

  INSTPAT("0101 0???", push,      r,    0, push_width(w, Rr(rd, w)));
  INSTPAT("0101 1???", pop,       r,    0, Rw(rd, w, pop_width(w)));

  INSTPAT("0110 0000", pusha,     N,    0, pusha_width(is_operand_size_16 ? X86_WIDTH_WORD : X86_WIDTH_DWORD));
  INSTPAT("0110 0001", popa,      N,    0, popa_width(is_operand_size_16 ? X86_WIDTH_WORD : X86_WIDTH_DWORD));
  INSTPAT("0110 1000", push,      I,    0, push_width(w, imm));
  INSTPAT("0110 1001", imul,      I_E2G,0, {
    int64_t product = (int64_t)sext_width(RMr(rs, w), w) * (int64_t)sext_width(imm, w);
    Rw(rd, w, product);
    bool truncated = product != (int64_t)sext_width(Rr(rd, w), w);
    flag_set(FLAG_CF, truncated);
    flag_set(FLAG_OF, truncated);
  });
  INSTPAT("0110 1010", push,      SI,   1, push_width(is_operand_size_16 ? X86_WIDTH_WORD : X86_WIDTH_DWORD, imm));
  INSTPAT("0110 1011", imul,      SI_E2G,0, {
    int64_t product = (int64_t)sext_width(RMr(rs, w), w) * (int64_t)sext_width(imm, w);
    Rw(rd, w, product);
    bool truncated = product != (int64_t)sext_width(Rr(rd, w), w);
    flag_set(FLAG_CF, truncated);
    flag_set(FLAG_OF, truncated);
  });

  INSTPAT("0111 ????", jcc,       J,    1, jcc(s, opcode & X86_COND_MASK, imm, X86_WIDTH_DWORD));

  INSTPAT("1000 0000", gp1,       I2E,  1, gp1(s, gp_idx, rd, addr, seg, w, imm));
  INSTPAT("1000 0001", gp1,       I2E,  0, gp1(s, gp_idx, rd, addr, seg, w, imm));
  INSTPAT("1000 0011", gp1,       Ib2E, 0, gp1(s, gp_idx, rd, addr, seg, w, imm));
  INSTPAT("1000 0100", test,      G2E,  1, set_logic_flags(RMr(rd, w) & src1, w));
  INSTPAT("1000 0101", test,      G2E,  0, set_logic_flags(RMr(rd, w) & src1, w));
  INSTPAT("1000 0110", xchg,      G2E,  1, xchg_rm_reg(rd, addr, seg, rs, w));
  INSTPAT("1000 0111", xchg,      G2E,  0, xchg_rm_reg(rd, addr, seg, rs, w));
  INSTPAT("1000 1100", mov,       E,    2, mov_sreg_to_rm(s, gp_idx, rd, addr, seg));
  INSTPAT("1000 1000", mov,       G2E,  1, RMw(src1));
  INSTPAT("1000 1001", mov,       G2E,  0, RMw(src1));
  INSTPAT("1000 1010", mov,       E2G,  1, Rw(rd, w, RMr(rs, w)));
  INSTPAT("1000 1011", mov,       E2G,  0, Rw(rd, w, RMr(rs, w)));
  INSTPAT("1000 1101", lea,       E2G,  0, if (rs == -1) Rw(rd, w, addr); else INV(s->pc));
  INSTPAT("1000 1110", mov,       E,    2, sreg_write(s, gp_idx, RMr(rd, w)));
  INSTPAT("1000 1111", pop,       E,    0, if (gp_idx == 0) { word_t data = pop_width(w); if (rd != -1) Rw(rd, w, data); else Mw(addr + (addr_uses_esp ? w : 0), seg, w, data); } else INV(s->pc));

  INSTPAT("1001 0???", xchg,      r,    0, xchg_reg_eax(rd, w));
  INSTPAT("1001 1000", cbw_cwde,  N,    0, cbw_cwde(is_operand_size_16));
  INSTPAT("1001 1001", cwd_cdq,   N,    0, cwd_cdq(is_operand_size_16));
  INSTPAT("1001 1100", pushf,     N,    0, push_width(is_operand_size_16 ? X86_WIDTH_WORD : X86_WIDTH_DWORD, cpu.eflags | X86_EFLAGS_FIXED_ONE));
  INSTPAT("1001 1101", popf,      N,    0, cpu.eflags = eflags_write_protected_width(pop_width(is_operand_size_16 ? X86_WIDTH_WORD : X86_WIDTH_DWORD), is_operand_size_16 ? X86_WIDTH_WORD : X86_WIDTH_DWORD));

  INSTPAT("1010 0000", mov,       O2a,  1, Rw(R_EAX, X86_WIDTH_BYTE, Mr(addr, seg, X86_WIDTH_BYTE)));
  INSTPAT("1010 0001", mov,       O2a,  0, Rw(R_EAX, w, Mr(addr, seg, w)));
  INSTPAT("1010 0010", mov,       a2O,  1, Mw(addr, seg, X86_WIDTH_BYTE, Rr(R_EAX, X86_WIDTH_BYTE)));
  INSTPAT("1010 0011", mov,       a2O,  0, Mw(addr, seg, w, Rr(R_EAX, w)));
  INSTPAT("1010 0100", movs,      N,    1, movs(X86_WIDTH_BYTE, seg_override >= 0 ? seg_override : X86_SREG_DS, is_addr_size_16));
  INSTPAT("1010 0101", movs,      N,    0, movs(w, seg_override >= 0 ? seg_override : X86_SREG_DS, is_addr_size_16));
  INSTPAT("1010 1000", test,      I2a,  1, set_logic_flags(Rr(R_EAX, w) & imm, w));
  INSTPAT("1010 1001", test,      I2a,  0, set_logic_flags(Rr(R_EAX, w) & imm, w));

  INSTPAT("1011 0???", mov,       I2r,  1, Rw(rd, 1, imm));
  INSTPAT("1011 1???", mov,       I2r,  0, Rw(rd, w, imm));

  INSTPAT("1100 0000", shift,     I2E,  1, shift_rm(s, gp_idx, rd, addr, seg, w, imm));
  INSTPAT("1100 0001", shift,     Ib2E, 0, shift_rm(s, gp_idx, rd, addr, seg, w, imm));
  INSTPAT("1100 0010", ret,       I,    2, { int rw = is_operand_size_16 ? X86_WIDTH_WORD : X86_WIDTH_DWORD; word_t target = pop_width(rw); cpu.esp += imm; s->dnpc = control_target(target, rw); });
  INSTPAT("1100 0011", ret,       N,    0, { int rw = is_operand_size_16 ? X86_WIDTH_WORD : X86_WIDTH_DWORD; s->dnpc = control_target(pop_width(rw), rw); });
  INSTPAT("1100 0110", mov,       I2E,  1, RMw(imm));
  INSTPAT("1100 0111", mov,       I2E,  0, RMw(imm));
  INSTPAT("1100 1001", leave,     N,    0, { cpu.esp = cpu.ebp; cpu.ebp = pop32(); });
  INSTPAT("1100 1100", nemu_trap, N,    0, { difftest_skip_ref(); NEMUTRAP(s->pc, cpu.eax); });
  INSTPAT("1100 1101", int,       I,    1, s->dnpc = isa_raise_intr_sw(imm, s->snpc));
  INSTPAT("1101 0000", shift,     1_E,  1, shift_rm(s, gp_idx, rd, addr, seg, w, src1));
  INSTPAT("1101 0001", shift,     1_E,  0, shift_rm(s, gp_idx, rd, addr, seg, w, src1));
  INSTPAT("1101 0010", shift,     cl2E, 1, shift_rm(s, gp_idx, rd, addr, seg, w, src1));
  INSTPAT("1101 0011", shift,     cl2E, 0, shift_rm(s, gp_idx, rd, addr, seg, w, src1));
  INSTPAT("1101 0110", nemu_trap, N,    0, { difftest_skip_ref(); NEMUTRAP(s->pc, cpu.eax); });
  INSTPAT("1110 0100", in,        P,    1, Rw(R_EAX, X86_WIDTH_BYTE, x86_pio_read(s, imm, X86_WIDTH_BYTE)));
  INSTPAT("1110 0101", in,        P,    0, Rw(R_EAX, w, x86_pio_read(s, imm, w)));
  INSTPAT("1110 0110", out,       P,    1, x86_pio_write(s, imm, X86_WIDTH_BYTE, Rr(R_EAX, X86_WIDTH_BYTE)));
  INSTPAT("1110 0111", out,       P,    0, x86_pio_write(s, imm, w, Rr(R_EAX, w)));
  INSTPAT("1110 1000", call,      J,    0, { push_width(w, s->snpc); s->dnpc = branch_target(s->snpc, imm, w); });
  INSTPAT("1110 1001", jmp,       J,    0, s->dnpc = branch_target(s->snpc, imm, w));
  INSTPAT("1110 1011", jmp,       J,    1, s->dnpc = s->snpc + imm);
  INSTPAT("1110 1100", in,        N,    1, Rw(R_EAX, X86_WIDTH_BYTE, x86_pio_read(s, Rr(R_EDX, X86_WIDTH_WORD), X86_WIDTH_BYTE)));
  INSTPAT("1110 1101", in,        N,    0, Rw(R_EAX, w, x86_pio_read(s, Rr(R_EDX, X86_WIDTH_WORD), w)));
  INSTPAT("1110 1110", out,       N,    1, x86_pio_write(s, Rr(R_EDX, X86_WIDTH_WORD), X86_WIDTH_BYTE, Rr(R_EAX, X86_WIDTH_BYTE)));
  INSTPAT("1110 1111", out,       N,    0, x86_pio_write(s, Rr(R_EDX, X86_WIDTH_WORD), w, Rr(R_EAX, w)));
  INSTPAT("1111 0101", cmc,       N,    0, flag_set(FLAG_CF, !flag_get(FLAG_CF)));
  INSTPAT("1111 1000", clc,       N,    0, flag_set(FLAG_CF, false));
  INSTPAT("1111 1001", stc,       N,    0, flag_set(FLAG_CF, true));
  INSTPAT("1111 1010", cli,       N,    0, { require_interrupt_flag_privilege(s, "cli"); flag_set(FLAG_IF, false); cpu.sti_shadow = 0; });
  INSTPAT("1111 1011", sti,       N,    0, {
    bool was_enabled = flag_get(FLAG_IF);
    require_interrupt_flag_privilege(s, "sti");
    flag_set(FLAG_IF, true);
    if (!was_enabled) {
      cpu.sti_shadow = 1;
    }
  });
  INSTPAT("1111 0110", gp3,       GP3,  1, gp3(s, gp_idx, rd, addr, seg, w, imm));
  INSTPAT("1111 0111", gp3,       GP3,  0, gp3(s, gp_idx, rd, addr, seg, w, imm));
  INSTPAT("1111 1100", cld,       N,    0, flag_set(FLAG_DF, false));
  INSTPAT("1111 1101", std,       N,    0, flag_set(FLAG_DF, true));
  INSTPAT("1100 1111", iret,      N,    0, iret32(s));
  INSTPAT("1111 1110", gp4,       E,    1, gp4(s, gp_idx, rd, addr, seg, w));
  INSTPAT("1111 1111", gp5,       E,    0, gp5(s, gp_idx, rd, addr, seg, w));
  INSTPAT("???? ????", inv,       N,    0, INV(s->pc));
  INSTPAT_END();

  return 0;
}
