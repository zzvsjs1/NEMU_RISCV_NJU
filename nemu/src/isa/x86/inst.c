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
#include <cpu/ifetch.h>
#include <cpu/decode.h>

uint32_t pio_read(ioaddr_t addr, int len);
void pio_write(ioaddr_t addr, int len, uint32_t data);

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

typedef union {
  struct {
    uint8_t base	:3;
    uint8_t index	:3;
    uint8_t ss		:2;
  };
  uint8_t val;
} SIB;

static word_t x86_inst_fetch(Decode *s, int len) {
#if defined(CONFIG_ITRACE) || defined(CONFIG_IQUEUE)
  uint8_t *p = &s->isa.inst[s->snpc - s->pc];
  word_t ret = inst_fetch(&s->snpc, len);
  word_t ret_save = ret;
  int i;
  assert(s->snpc - s->pc < sizeof(s->isa.inst));
  for (i = 0; i < len; i ++) {
    p[i] = ret & 0xff;
    ret >>= 8;
  }
  return ret_save;
#else
  return inst_fetch(&s->snpc, len);
#endif
}

word_t reg_read(int idx, int width) {
  switch (width) {
    case 4: return reg_l(idx);
    case 1: return reg_b(idx);
    case 2: return reg_w(idx);
    default: assert(0);
  }
}

static void reg_write(int idx, int width, word_t data) {
  switch (width) {
    case 4: reg_l(idx) = data; return;
    case 1: reg_b(idx) = data; return;
    case 2: reg_w(idx) = data; return;
    default: assert(0);
  }
}

enum {
  FLAG_CF = 1u << 0,
  FLAG_PF = 1u << 2,
  FLAG_AF = 1u << 4,
  FLAG_ZF = 1u << 6,
  FLAG_SF = 1u << 7,
  FLAG_IF = 1u << 9,
  FLAG_DF = 1u << 10,
  FLAG_OF = 1u << 11,
};

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

static inline uint32_t width_mask(int width) {
  return width == 4 ? 0xffffffffu : ((1u << (width * 8)) - 1u);
}

static inline uint32_t sign_bit(int width) {
  return 1u << (width * 8 - 1);
}

static inline word_t mask_width(word_t val, int width) {
  return val & width_mask(width);
}

static inline bool flag_get(uint32_t flag) {
  return (cpu.eflags & flag) != 0;
}

static inline void flag_set(uint32_t flag, bool val) {
  if (val) cpu.eflags |= flag;
  else cpu.eflags &= ~flag;
  cpu.eflags |= 0x2;
}

static bool parity_even(uint8_t val) {
  val ^= val >> 4;
  val &= 0xf;
  return ((0x6996 >> val) & 1) == 0;
}

static void set_zsp_flags(word_t result, int width) {
  result = mask_width(result, width);
  flag_set(FLAG_ZF, result == 0);
  flag_set(FLAG_SF, (result & sign_bit(width)) != 0);
  flag_set(FLAG_PF, parity_even(result & 0xff));
}

static void set_add_flags(word_t lhs, word_t rhs, word_t result, int width) {
  uint32_t mask = width_mask(width);
  uint32_t sign = sign_bit(width);
  uint32_t l = lhs & mask;
  uint32_t r = rhs & mask;
  uint32_t res = result & mask;
  uint64_t raw = (uint64_t)l + (uint64_t)r;

  set_zsp_flags(res, width);
  flag_set(FLAG_CF, raw > mask);
  flag_set(FLAG_AF, ((l ^ r ^ res) & 0x10) != 0);
  flag_set(FLAG_OF, ((~(l ^ r) & (l ^ res) & sign) != 0));
}

static void set_sub_flags(word_t lhs, word_t rhs, word_t result, int width) {
  uint32_t mask = width_mask(width);
  uint32_t sign = sign_bit(width);
  uint32_t l = lhs & mask;
  uint32_t r = rhs & mask;
  uint32_t res = result & mask;

  set_zsp_flags(res, width);
  flag_set(FLAG_CF, l < r);
  flag_set(FLAG_AF, ((l ^ r ^ res) & 0x10) != 0);
  flag_set(FLAG_OF, (((l ^ r) & (l ^ res) & sign) != 0));
}

static void set_logic_flags(word_t result, int width) {
  set_zsp_flags(result, width);
  flag_set(FLAG_CF, false);
  flag_set(FLAG_OF, false);
  flag_set(FLAG_AF, false);
}

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
      uint64_t r_full = (uint64_t)(rhs & mask) + carry;
      uint32_t r = r_full & mask;
      uint32_t res = (uint64_t)l + r_full;
      result = res;
      set_zsp_flags(res, width);
      flag_set(FLAG_CF, (uint64_t)l + r_full > mask);
      flag_set(FLAG_AF, ((l ^ r ^ res) & 0x10) != 0);
      flag_set(FLAG_OF, ((~(l ^ r) & (l ^ res) & sign) != 0));
      break;
    }
    case ALU_SBB:
    {
      uint32_t mask = width_mask(width);
      uint32_t sign = sign_bit(width);
      uint32_t l = lhs & mask;
      uint64_t r_full = (uint64_t)(rhs & mask) + carry;
      uint32_t r = r_full & mask;
      uint32_t res = (uint64_t)l - r_full;
      result = res;
      set_zsp_flags(res, width);
      flag_set(FLAG_CF, (uint64_t)l < r_full);
      flag_set(FLAG_AF, ((l ^ r ^ res) & 0x10) != 0);
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

static sword_t sext_width(word_t val, int width) {
  switch (width) {
    case 1: return (int8_t)val;
    case 2: return (int16_t)val;
    case 4: return (int32_t)val;
    default: assert(0);
  }
}

static void push32(word_t val) {
  cpu.esp -= 4;
  vaddr_write(cpu.esp, 4, val);
}

static word_t pop32() {
  word_t val = vaddr_read(cpu.esp, 4);
  cpu.esp += 4;
  return val;
}

static void load_addr(Decode *s, ModR_M *m, word_t *rm_addr) {
  assert(m->mod != 3);

  sword_t disp = 0;
  int disp_size = 4;
  int base_reg = -1, index_reg = -1, scale = 0;

  if (m->R_M == R_ESP) {
    SIB sib;
    sib.val = x86_inst_fetch(s, 1);
    base_reg = sib.base;
    scale = sib.ss;

    if (sib.index != R_ESP) { index_reg = sib.index; }
  }
  else { base_reg = m->R_M; } /* no SIB */

  if (m->mod == 0) {
    if (base_reg == R_EBP) { base_reg = -1; }
    else { disp_size = 0; }
  }
  else if (m->mod == 1) { disp_size = 1; }

  if (disp_size != 0) { /* has disp */
    disp = x86_inst_fetch(s, disp_size);
    if (disp_size == 1) { disp = (int8_t)disp; }
  }

  word_t addr = disp;
  if (base_reg != -1)  addr += reg_l(base_reg);
  if (index_reg != -1) addr += reg_l(index_reg) << scale;
  *rm_addr = addr;
}

static void decode_rm(Decode *s, int *rm_reg, word_t *rm_addr, int *reg, int width) {
  ModR_M m;
  m.val = x86_inst_fetch(s, 1);
  if (reg != NULL) *reg = m.reg;
  if (m.mod == 3) *rm_reg = m.R_M;
  else { load_addr(s, &m, rm_addr); *rm_reg = -1; }
}

#define Rr reg_read
#define Rw reg_write
#define Mr vaddr_read
#define Mw vaddr_write
#define RMr(reg, w)  (reg != -1 ? Rr(reg, w) : Mr(addr, w))
#define RMw(data) do { if (rd != -1) Rw(rd, w, data); else Mw(addr, w, data); } while (0)

static inline void rm_write(int rm_reg, word_t rm_addr, int width, word_t data) {
  if (rm_reg != -1) Rw(rm_reg, width, data);
  else Mw(rm_addr, width, data);
}

#define destr(r)  do { *rd_ = (r); } while (0)
#define src1r(r)  do { *src1 = Rr(r, w); } while (0)
#define imm()     do { *imm = x86_inst_fetch(s, w); } while (0)
#define simm(width) do { *imm = sext_width(x86_inst_fetch(s, width), width); } while (0)

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

#define INSTPAT_INST(s) opcode
#define INSTPAT_MATCH(s, name, type, width, ... /* execute body */ ) { \
  int rd = 0, rs = 0, gp_idx = 0; \
  word_t src1 = 0, addr = 0, imm = 0; \
  int w = width == 0 ? (is_operand_size_16 ? 2 : 4) : width; \
  decode_operand(s, opcode, &rd, &src1, &addr, &rs, &gp_idx, &imm, w, concat(TYPE_, type)); \
  s->dnpc = s->snpc; \
  __VA_ARGS__ ; \
}

static void decode_operand(Decode *s, uint8_t opcode, int *rd_, word_t *src1,
    word_t *addr, int *rs, int *gp_idx, word_t *imm, int w, int type) {
  switch (type) {
    case TYPE_r:    destr(opcode & 0x7); break;
    case TYPE_I:    imm(); break;
    case TYPE_SI:   simm(w); break;
    case TYPE_J:    simm(w); break;
    case TYPE_E:    decode_rm(s, rd_, addr, gp_idx, w); break;
    case TYPE_I2r:  destr(opcode & 0x7); imm(); break;
    case TYPE_I2a:  destr(R_EAX); imm(); break;
    case TYPE_G2E:  decode_rm(s, rd_, addr, rs, w); src1r(*rs); break;
    case TYPE_E2G:  decode_rm(s, rs, addr, rd_, w); break;
    case TYPE_I2E:  decode_rm(s, rd_, addr, gp_idx, w); imm(); break;
    case TYPE_Ib2E: decode_rm(s, rd_, addr, gp_idx, w); simm(1); break;
    case TYPE_cl2E: decode_rm(s, rd_, addr, gp_idx, w); *src1 = Rr(R_CL, 1); break;
    case TYPE_1_E:  decode_rm(s, rd_, addr, gp_idx, w); *src1 = 1; break;
    case TYPE_SI2E: decode_rm(s, rd_, addr, gp_idx, w); simm(1); break;
    case TYPE_GP3:
      decode_rm(s, rd_, addr, gp_idx, w);
      if (*gp_idx == 0) imm();
      break;
    case TYPE_Eb2G: decode_rm(s, rs, addr, rd_, 1); break;
    case TYPE_Ew2G: decode_rm(s, rs, addr, rd_, 2); break;
    case TYPE_O2a:  destr(R_EAX); *addr = x86_inst_fetch(s, 4); break;
    case TYPE_a2O:  *rs = R_EAX;  *addr = x86_inst_fetch(s, 4); break;
    case TYPE_P:    *imm = x86_inst_fetch(s, 1); break;
    case TYPE_I_E2G:
      decode_rm(s, rs, addr, rd_, w);
      imm();
      break;
    case TYPE_SI_E2G:
      decode_rm(s, rs, addr, rd_, w);
      simm(1);
      break;
    case TYPE_N:    break;
    default: panic("Unsupported type = %d", type);
  }
}

static bool cc_eval(int cc) {
  bool cf = flag_get(FLAG_CF);
  bool zf = flag_get(FLAG_ZF);
  bool sf = flag_get(FLAG_SF);
  bool of = flag_get(FLAG_OF);
  bool pf = flag_get(FLAG_PF);

  switch (cc & 0xf) {
    case 0x0: return of;
    case 0x1: return !of;
    case 0x2: return cf;
    case 0x3: return !cf;
    case 0x4: return zf;
    case 0x5: return !zf;
    case 0x6: return cf || zf;
    case 0x7: return !cf && !zf;
    case 0x8: return sf;
    case 0x9: return !sf;
    case 0xa: return pf;
    case 0xb: return !pf;
    case 0xc: return sf != of;
    case 0xd: return sf == of;
    case 0xe: return zf || (sf != of);
    case 0xf: return !zf && (sf == of);
    default: assert(0);
  }
}

static void jcc(Decode *s, int cc, word_t offset) {
  if (cc_eval(cc)) s->dnpc = s->snpc + offset;
}

static void gp1(Decode *s, int gp_idx, int rd, word_t addr, int w, word_t imm) {
  word_t lhs = RMr(rd, w);
  word_t result = alu_exec(gp_idx, lhs, imm, w);
  if (gp_idx != ALU_CMP) RMw(result);
}

static void alu_rm_reg(int op, int rd, word_t addr, int w, word_t src) {
  word_t lhs = RMr(rd, w);
  word_t result = alu_exec(op, lhs, src, w);
  if (op != ALU_CMP) RMw(result);
}

static void alu_reg_rm(int op, int rd, int rs, word_t addr, int w) {
  word_t lhs = Rr(rd, w);
  word_t rhs = RMr(rs, w);
  word_t result = alu_exec(op, lhs, rhs, w);
  if (op != ALU_CMP) Rw(rd, w, result);
}

static void incdec_rm(int is_dec, int rd, word_t addr, int w) {
  bool old_cf = flag_get(FLAG_CF);
  word_t lhs = RMr(rd, w);
  word_t result = is_dec ? alu_exec(ALU_SUB, lhs, 1, w) : alu_exec(ALU_ADD, lhs, 1, w);
  flag_set(FLAG_CF, old_cf);
  RMw(result);
}

static void shift_rm(Decode *s, int gp_idx, int rd, word_t addr, int w, word_t count) {
  count &= 0x1f;
  if (count == 0) return;

  int bits = w * 8;
  word_t lhs = RMr(rd, w);
  word_t result = lhs;
  bool cf = false;
  bool of = false;

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

  rm_write(rd, addr, w, result);
  set_zsp_flags(result, w);
  flag_set(FLAG_CF, cf);
  if (count == 1) flag_set(FLAG_OF, of);
}

static void gp3(Decode *s, int gp_idx, int rd, word_t addr, int w, word_t imm) {
  word_t lhs = RMr(rd, w);
  switch (gp_idx) {
    case 0:
      set_logic_flags(lhs & imm, w);
      break;
    case 2:
      RMw(~lhs);
      break;
    case 3: {
      word_t result = alu_exec(ALU_SUB, 0, lhs, w);
      flag_set(FLAG_CF, mask_width(lhs, w) != 0);
      RMw(result);
      break;
    }
    case 4: {
      uint64_t product = (uint64_t)reg_l(R_EAX) * (uint64_t)mask_width(lhs, 4);
      cpu.eax = product;
      cpu.edx = product >> 32;
      flag_set(FLAG_CF, cpu.edx != 0);
      flag_set(FLAG_OF, cpu.edx != 0);
      break;
    }
    case 5: {
      int64_t product = (int64_t)(int32_t)cpu.eax * (int64_t)(int32_t)lhs;
      cpu.eax = product;
      cpu.edx = (uint64_t)product >> 32;
      bool truncated = product != (int64_t)(int32_t)cpu.eax;
      flag_set(FLAG_CF, truncated);
      flag_set(FLAG_OF, truncated);
      break;
    }
    case 6: {
      uint64_t dividend = ((uint64_t)cpu.edx << 32) | cpu.eax;
      uint32_t divisor = lhs;
      Assert(divisor != 0, "x86 div by zero at pc = " FMT_WORD, s->pc);
      cpu.eax = dividend / divisor;
      cpu.edx = dividend % divisor;
      break;
    }
    case 7: {
      int64_t dividend = (int64_t)(int32_t)cpu.edx * 0x100000000ll + cpu.eax;
      int32_t divisor = lhs;
      Assert(divisor != 0, "x86 idiv by zero at pc = " FMT_WORD, s->pc);
      cpu.eax = dividend / divisor;
      cpu.edx = dividend % divisor;
      break;
    }
    default:
      INV(s->pc);
      break;
  }
}

static void gp5(Decode *s, int gp_idx, int rd, word_t addr, int w) {
  word_t target = RMr(rd, w);
  switch (gp_idx) {
    case 0:
      incdec_rm(0, rd, addr, w);
      break;
    case 1:
      incdec_rm(1, rd, addr, w);
      break;
    case 2:
      push32(s->snpc);
      s->dnpc = target;
      break;
    case 4:
      s->dnpc = target;
      break;
    case 6:
      push32(target);
      break;
    default:
      INV(s->pc);
      break;
  }
}

void _2byte_esc(Decode *s, bool is_operand_size_16) {
  uint8_t opcode = x86_inst_fetch(s, 1);
  INSTPAT_START();
  INSTPAT("1000 ????", jcc,       J,    0, jcc(s, opcode & 0xf, imm));
  INSTPAT("1001 ????", setcc,     E,    1, RMw(cc_eval(opcode & 0xf) ? 1 : 0));
  INSTPAT("1010 1111", imul,      E2G,  0, {
    int64_t product = (int64_t)sext_width(Rr(rd, w), w) * (int64_t)sext_width(RMr(rs, w), w);
    Rw(rd, w, product);
    bool truncated = product != (int64_t)sext_width(Rr(rd, w), w);
    flag_set(FLAG_CF, truncated);
    flag_set(FLAG_OF, truncated);
  });
  INSTPAT("1011 0110", movzx,     Eb2G, 0, Rw(rd, w, RMr(rs, 1)));
  INSTPAT("1011 0111", movzx,     Ew2G, 0, Rw(rd, w, RMr(rs, 2)));
  INSTPAT("1011 1110", movsx,     Eb2G, 0, Rw(rd, w, sext_width(RMr(rs, 1), 1)));
  INSTPAT("1011 1111", movsx,     Ew2G, 0, Rw(rd, w, sext_width(RMr(rs, 2), 2)));
  INSTPAT("???? ????", inv,    N,    0, INV(s->pc));
  INSTPAT_END();
}

int isa_exec_once(Decode *s) {
  bool is_operand_size_16 = false;
  uint8_t opcode = 0;

again:
  opcode = x86_inst_fetch(s, 1);

  INSTPAT_START();

  INSTPAT("0000 1111", 2byte_esc, N,    0, _2byte_esc(s, is_operand_size_16));

  INSTPAT("0110 0110", data_size, N,    0, is_operand_size_16 = true; goto again;);

  INSTPAT("0000 0000", add,       G2E,  1, alu_rm_reg(ALU_ADD, rd, addr, w, src1));
  INSTPAT("0000 0001", add,       G2E,  0, alu_rm_reg(ALU_ADD, rd, addr, w, src1));
  INSTPAT("0000 0010", add,       E2G,  1, alu_reg_rm(ALU_ADD, rd, rs, addr, w));
  INSTPAT("0000 0011", add,       E2G,  0, alu_reg_rm(ALU_ADD, rd, rs, addr, w));
  INSTPAT("0000 0100", add,       I2a,  1, Rw(R_EAX, w, alu_exec(ALU_ADD, Rr(R_EAX, w), imm, w)));
  INSTPAT("0000 0101", add,       I2a,  0, Rw(R_EAX, w, alu_exec(ALU_ADD, Rr(R_EAX, w), imm, w)));

  INSTPAT("0000 1000", or,        G2E,  1, alu_rm_reg(ALU_OR, rd, addr, w, src1));
  INSTPAT("0000 1001", or,        G2E,  0, alu_rm_reg(ALU_OR, rd, addr, w, src1));
  INSTPAT("0000 1010", or,        E2G,  1, alu_reg_rm(ALU_OR, rd, rs, addr, w));
  INSTPAT("0000 1011", or,        E2G,  0, alu_reg_rm(ALU_OR, rd, rs, addr, w));
  INSTPAT("0000 1100", or,        I2a,  1, Rw(R_EAX, w, alu_exec(ALU_OR, Rr(R_EAX, w), imm, w)));
  INSTPAT("0000 1101", or,        I2a,  0, Rw(R_EAX, w, alu_exec(ALU_OR, Rr(R_EAX, w), imm, w)));

  INSTPAT("0001 0000", adc,       G2E,  1, alu_rm_reg(ALU_ADC, rd, addr, w, src1));
  INSTPAT("0001 0001", adc,       G2E,  0, alu_rm_reg(ALU_ADC, rd, addr, w, src1));
  INSTPAT("0001 0010", adc,       E2G,  1, alu_reg_rm(ALU_ADC, rd, rs, addr, w));
  INSTPAT("0001 0011", adc,       E2G,  0, alu_reg_rm(ALU_ADC, rd, rs, addr, w));
  INSTPAT("0001 0100", adc,       I2a,  1, Rw(R_EAX, w, alu_exec(ALU_ADC, Rr(R_EAX, w), imm, w)));
  INSTPAT("0001 0101", adc,       I2a,  0, Rw(R_EAX, w, alu_exec(ALU_ADC, Rr(R_EAX, w), imm, w)));

  INSTPAT("0001 1000", sbb,       G2E,  1, alu_rm_reg(ALU_SBB, rd, addr, w, src1));
  INSTPAT("0001 1001", sbb,       G2E,  0, alu_rm_reg(ALU_SBB, rd, addr, w, src1));
  INSTPAT("0001 1010", sbb,       E2G,  1, alu_reg_rm(ALU_SBB, rd, rs, addr, w));
  INSTPAT("0001 1011", sbb,       E2G,  0, alu_reg_rm(ALU_SBB, rd, rs, addr, w));
  INSTPAT("0001 1100", sbb,       I2a,  1, Rw(R_EAX, w, alu_exec(ALU_SBB, Rr(R_EAX, w), imm, w)));
  INSTPAT("0001 1101", sbb,       I2a,  0, Rw(R_EAX, w, alu_exec(ALU_SBB, Rr(R_EAX, w), imm, w)));

  INSTPAT("0010 0000", and,       G2E,  1, alu_rm_reg(ALU_AND, rd, addr, w, src1));
  INSTPAT("0010 0001", and,       G2E,  0, alu_rm_reg(ALU_AND, rd, addr, w, src1));
  INSTPAT("0010 0010", and,       E2G,  1, alu_reg_rm(ALU_AND, rd, rs, addr, w));
  INSTPAT("0010 0011", and,       E2G,  0, alu_reg_rm(ALU_AND, rd, rs, addr, w));
  INSTPAT("0010 0100", and,       I2a,  1, Rw(R_EAX, w, alu_exec(ALU_AND, Rr(R_EAX, w), imm, w)));
  INSTPAT("0010 0101", and,       I2a,  0, Rw(R_EAX, w, alu_exec(ALU_AND, Rr(R_EAX, w), imm, w)));

  INSTPAT("0010 1000", sub,       G2E,  1, alu_rm_reg(ALU_SUB, rd, addr, w, src1));
  INSTPAT("0010 1001", sub,       G2E,  0, alu_rm_reg(ALU_SUB, rd, addr, w, src1));
  INSTPAT("0010 1010", sub,       E2G,  1, alu_reg_rm(ALU_SUB, rd, rs, addr, w));
  INSTPAT("0010 1011", sub,       E2G,  0, alu_reg_rm(ALU_SUB, rd, rs, addr, w));
  INSTPAT("0010 1100", sub,       I2a,  1, Rw(R_EAX, w, alu_exec(ALU_SUB, Rr(R_EAX, w), imm, w)));
  INSTPAT("0010 1101", sub,       I2a,  0, Rw(R_EAX, w, alu_exec(ALU_SUB, Rr(R_EAX, w), imm, w)));

  INSTPAT("0011 0000", xor,       G2E,  1, alu_rm_reg(ALU_XOR, rd, addr, w, src1));
  INSTPAT("0011 0001", xor,       G2E,  0, alu_rm_reg(ALU_XOR, rd, addr, w, src1));
  INSTPAT("0011 0010", xor,       E2G,  1, alu_reg_rm(ALU_XOR, rd, rs, addr, w));
  INSTPAT("0011 0011", xor,       E2G,  0, alu_reg_rm(ALU_XOR, rd, rs, addr, w));
  INSTPAT("0011 0100", xor,       I2a,  1, Rw(R_EAX, w, alu_exec(ALU_XOR, Rr(R_EAX, w), imm, w)));
  INSTPAT("0011 0101", xor,       I2a,  0, Rw(R_EAX, w, alu_exec(ALU_XOR, Rr(R_EAX, w), imm, w)));

  INSTPAT("0011 1000", cmp,       G2E,  1, alu_rm_reg(ALU_CMP, rd, addr, w, src1));
  INSTPAT("0011 1001", cmp,       G2E,  0, alu_rm_reg(ALU_CMP, rd, addr, w, src1));
  INSTPAT("0011 1010", cmp,       E2G,  1, alu_reg_rm(ALU_CMP, rd, rs, addr, w));
  INSTPAT("0011 1011", cmp,       E2G,  0, alu_reg_rm(ALU_CMP, rd, rs, addr, w));
  INSTPAT("0011 1100", cmp,       I2a,  1, alu_exec(ALU_CMP, Rr(R_EAX, w), imm, w));
  INSTPAT("0011 1101", cmp,       I2a,  0, alu_exec(ALU_CMP, Rr(R_EAX, w), imm, w));

  INSTPAT("0100 0???", inc,       r,    4, {
    bool old_cf = flag_get(FLAG_CF);
    Rw(rd, w, alu_exec(ALU_ADD, Rr(rd, w), 1, w));
    flag_set(FLAG_CF, old_cf);
  });
  INSTPAT("0100 1???", dec,       r,    4, {
    bool old_cf = flag_get(FLAG_CF);
    Rw(rd, w, alu_exec(ALU_SUB, Rr(rd, w), 1, w));
    flag_set(FLAG_CF, old_cf);
  });

  INSTPAT("0101 0???", push,      r,    4, push32(Rr(rd, 4)));
  INSTPAT("0101 1???", pop,       r,    4, Rw(rd, 4, pop32()));

  INSTPAT("0110 1000", push,      I,    4, push32(imm));
  INSTPAT("0110 1001", imul,      I_E2G,0, {
    int64_t product = (int64_t)sext_width(RMr(rs, w), w) * (int64_t)sext_width(imm, w);
    Rw(rd, w, product);
    bool truncated = product != (int64_t)sext_width(Rr(rd, w), w);
    flag_set(FLAG_CF, truncated);
    flag_set(FLAG_OF, truncated);
  });
  INSTPAT("0110 1010", push,      SI,   1, push32(imm));
  INSTPAT("0110 1011", imul,      SI_E2G,0, {
    int64_t product = (int64_t)sext_width(RMr(rs, w), w) * (int64_t)sext_width(imm, w);
    Rw(rd, w, product);
    bool truncated = product != (int64_t)sext_width(Rr(rd, w), w);
    flag_set(FLAG_CF, truncated);
    flag_set(FLAG_OF, truncated);
  });

  INSTPAT("0111 ????", jcc,       J,    1, jcc(s, opcode & 0xf, imm));

  INSTPAT("1000 0000", gp1,       I2E,  1, gp1(s, gp_idx, rd, addr, w, imm));
  INSTPAT("1000 0001", gp1,       I2E,  0, gp1(s, gp_idx, rd, addr, w, imm));
  INSTPAT("1000 0011", gp1,       Ib2E, 0, gp1(s, gp_idx, rd, addr, w, imm));
  INSTPAT("1000 0100", test,      G2E,  1, set_logic_flags(RMr(rd, w) & src1, w));
  INSTPAT("1000 0101", test,      G2E,  0, set_logic_flags(RMr(rd, w) & src1, w));
  INSTPAT("1000 1000", mov,       G2E,  1, RMw(src1));
  INSTPAT("1000 1001", mov,       G2E,  0, RMw(src1));
  INSTPAT("1000 1010", mov,       E2G,  1, Rw(rd, w, RMr(rs, w)));
  INSTPAT("1000 1011", mov,       E2G,  0, Rw(rd, w, RMr(rs, w)));
  INSTPAT("1000 1101", lea,       E2G,  0, if (rs == -1) Rw(rd, w, addr); else INV(s->pc));
  INSTPAT("1000 1111", pop,       E,    0, if (gp_idx == 0) RMw(pop32()); else INV(s->pc));

  INSTPAT("1001 0000", nop,       N,    0, );
  INSTPAT("1001 1001", cdq,       N,    0, cpu.edx = ((int32_t)cpu.eax < 0) ? 0xffffffffu : 0);

  INSTPAT("1010 0000", mov,       O2a,  1, Rw(R_EAX, 1, Mr(addr, 1)));
  INSTPAT("1010 0001", mov,       O2a,  0, Rw(R_EAX, w, Mr(addr, w)));
  INSTPAT("1010 0010", mov,       a2O,  1, Mw(addr, 1, Rr(R_EAX, 1)));
  INSTPAT("1010 0011", mov,       a2O,  0, Mw(addr, w, Rr(R_EAX, w)));
  INSTPAT("1010 1000", test,      I2a,  1, set_logic_flags(Rr(R_EAX, w) & imm, w));
  INSTPAT("1010 1001", test,      I2a,  0, set_logic_flags(Rr(R_EAX, w) & imm, w));

  INSTPAT("1011 0???", mov,       I2r,  1, Rw(rd, 1, imm));
  INSTPAT("1011 1???", mov,       I2r,  0, Rw(rd, w, imm));

  INSTPAT("1100 0000", shift,     I2E,  1, shift_rm(s, gp_idx, rd, addr, w, imm));
  INSTPAT("1100 0001", shift,     Ib2E, 0, shift_rm(s, gp_idx, rd, addr, w, imm));
  INSTPAT("1100 0010", ret,       I,    2, { word_t target = pop32(); cpu.esp += imm; s->dnpc = target; });
  INSTPAT("1100 0011", ret,       N,    0, s->dnpc = pop32());
  INSTPAT("1100 0110", mov,       I2E,  1, RMw(imm));
  INSTPAT("1100 0111", mov,       I2E,  0, RMw(imm));
  INSTPAT("1100 1001", leave,     N,    0, { cpu.esp = cpu.ebp; cpu.ebp = pop32(); });
  INSTPAT("1100 1100", nemu_trap, N,    0, NEMUTRAP(s->pc, cpu.eax));
  INSTPAT("1101 0000", shift,     1_E,  1, shift_rm(s, gp_idx, rd, addr, w, src1));
  INSTPAT("1101 0001", shift,     1_E,  0, shift_rm(s, gp_idx, rd, addr, w, src1));
  INSTPAT("1101 0010", shift,     cl2E, 1, shift_rm(s, gp_idx, rd, addr, w, src1));
  INSTPAT("1101 0011", shift,     cl2E, 0, shift_rm(s, gp_idx, rd, addr, w, src1));
  INSTPAT("1101 0110", nemu_trap, N,    0, NEMUTRAP(s->pc, cpu.eax));
  INSTPAT("1110 0100", in,        P,    1, Rw(R_EAX, 1, pio_read(imm, 1)));
  INSTPAT("1110 0101", in,        P,    0, Rw(R_EAX, w, pio_read(imm, w)));
  INSTPAT("1110 0110", out,       P,    1, pio_write(imm, 1, Rr(R_EAX, 1)));
  INSTPAT("1110 0111", out,       P,    0, pio_write(imm, w, Rr(R_EAX, w)));
  INSTPAT("1110 1000", call,      J,    4, { push32(s->snpc); s->dnpc = s->snpc + imm; });
  INSTPAT("1110 1001", jmp,       J,    4, s->dnpc = s->snpc + imm);
  INSTPAT("1110 1011", jmp,       J,    1, s->dnpc = s->snpc + imm);
  INSTPAT("1110 1100", in,        N,    1, Rw(R_EAX, 1, pio_read(Rr(R_EDX, 2), 1)));
  INSTPAT("1110 1101", in,        N,    0, Rw(R_EAX, w, pio_read(Rr(R_EDX, 2), w)));
  INSTPAT("1110 1110", out,       N,    1, pio_write(Rr(R_EDX, 2), 1, Rr(R_EAX, 1)));
  INSTPAT("1110 1111", out,       N,    0, pio_write(Rr(R_EDX, 2), w, Rr(R_EAX, w)));
  INSTPAT("1111 0110", gp3,       GP3,  1, gp3(s, gp_idx, rd, addr, w, imm));
  INSTPAT("1111 0111", gp3,       GP3,  0, gp3(s, gp_idx, rd, addr, w, imm));
  INSTPAT("1111 1111", gp5,       E,    0, gp5(s, gp_idx, rd, addr, w));
  INSTPAT("???? ????", inv,       N,    0, INV(s->pc));
  INSTPAT_END();

  return 0;
}
