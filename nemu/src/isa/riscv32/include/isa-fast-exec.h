#ifndef __RISCV32_FAST_EXEC_H__
#define __RISCV32_FAST_EXEC_H__

#include <isa.h>
#include <memory/host.h>
#include <memory/paddr.h>
#include <memory/vaddr.h>
#include <utils.h>
#include "../local-include/reg.h"

#define RV32_FAST_SATP_MODE_MASK 0x80000000u
#define RV32_FAST_SATP_PPN_MASK  0x003fffffu

#define RV32_FAST_PTE_V 0x001u
#define RV32_FAST_PTE_R 0x002u
#define RV32_FAST_PTE_W 0x004u
#define RV32_FAST_PTE_X 0x008u

#define RV32_FAST_TLB_SIZE 64u

typedef struct
{
  uint32_t satp;
  uint32_t vpn;
  uint32_t perm;
  paddr_t pg_paddr;
  paddr_t pt_page;
  bool valid;
} rv32_fast_tlb_entry_t;

static rv32_fast_tlb_entry_t rv32_fast_tlb[RV32_FAST_TLB_SIZE];

static inline uint32_t rv32_fast_bits(uint32_t value, int hi, int lo)
{
  return (value >> lo) & ((1u << (hi - lo + 1)) - 1u);
}

static inline sword_t rv32_fast_sext(uint32_t value, unsigned bits)
{
  const uint32_t sign = 1u << (bits - 1u);
  return (sword_t)((value ^ sign) - sign);
}

static inline sword_t rv32_fast_imm_i(uint32_t instr)
{
  return rv32_fast_sext(rv32_fast_bits(instr, 31, 20), 12);
}

static inline sword_t rv32_fast_imm_s(uint32_t instr)
{
  const uint32_t imm = rv32_fast_bits(instr, 11, 7)
    | (rv32_fast_bits(instr, 31, 25) << 5);
  return rv32_fast_sext(imm, 12);
}

static inline sword_t rv32_fast_imm_b(uint32_t instr)
{
  const uint32_t imm = (rv32_fast_bits(instr, 11, 8) << 1)
    | (rv32_fast_bits(instr, 30, 25) << 5)
    | (rv32_fast_bits(instr, 7, 7) << 11)
    | (rv32_fast_bits(instr, 31, 31) << 12);
  return rv32_fast_sext(imm, 13);
}

static inline word_t rv32_fast_imm_u(uint32_t instr)
{
  return instr & 0xfffff000u;
}

static inline sword_t rv32_fast_imm_j(uint32_t instr)
{
  const uint32_t imm = (rv32_fast_bits(instr, 19, 12) << 12)
    | (rv32_fast_bits(instr, 20, 20) << 11)
    | (rv32_fast_bits(instr, 30, 21) << 1)
    | (rv32_fast_bits(instr, 31, 31) << 20);
  return rv32_fast_sext(imm, 21);
}

static inline void rv32_fast_write_gpr(uint32_t rd, word_t value)
{
  if (rd != 0)
  {
    gpr(rd) = value;
  }
}

static inline bool rv32_fast_direct_mode()
{
  return likely((cpu.csr.satp & RV32_FAST_SATP_MODE_MASK) == 0);
}

static inline bool rv32_fast_cross_page(vaddr_t addr, int len)
{
  const word_t off = (word_t)(addr & PAGE_MASK);
  return off + (word_t)len > PAGE_SIZE;
}

static inline bool rv32_fast_pmem_range(paddr_t addr, int len)
{
  const paddr_t end = addr + (paddr_t)len - 1u;
  return len > 0 && end >= addr && likely(in_pmem(addr) && in_pmem(end));
}

static inline uint32_t rv32_fast_required_perm(int type)
{
  switch (type)
  {
    case MEM_TYPE_IFETCH: return RV32_FAST_PTE_X;
    case MEM_TYPE_READ: return RV32_FAST_PTE_R;
    case MEM_TYPE_WRITE: return RV32_FAST_PTE_W;
    default: return 0;
  }
}

static inline void rv32_fast_tlb_flush()
{
  memset(rv32_fast_tlb, 0, sizeof(rv32_fast_tlb));
}

static inline bool rv32_fast_translate(vaddr_t addr, int len, int type, paddr_t *paddr)
{
  const uint32_t satp = cpu.csr.satp;

  if (rv32_fast_direct_mode())
  {
    *paddr = (paddr_t)addr;
    return true;
  }

  if (rv32_fast_cross_page(addr, len))
  {
    return false;
  }

  const uint32_t need_perm = rv32_fast_required_perm(type);
  const uint32_t vpn = (uint32_t)(addr >> PAGE_SHIFT);
  const uint32_t idx = vpn & (RV32_FAST_TLB_SIZE - 1u);
  rv32_fast_tlb_entry_t *entry = &rv32_fast_tlb[idx];

  if (likely(entry->valid && entry->satp == satp && entry->vpn == vpn &&
      (entry->perm & need_perm) != 0))
  {
    *paddr = entry->pg_paddr | (paddr_t)(addr & PAGE_MASK);
    return true;
  }

  const paddr_t root = (paddr_t)((satp & RV32_FAST_SATP_PPN_MASK) << PAGE_SHIFT);
  const word_t vpn1 = (word_t)((addr >> 22) & 0x3ffu);
  const word_t vpn0 = (word_t)((addr >> 12) & 0x3ffu);
  const paddr_t pte1_addr = root + (paddr_t)(vpn1 * 4u);
  const uint32_t pte1 = (uint32_t)paddr_read(pte1_addr, 4);

  if ((pte1 & RV32_FAST_PTE_V) == 0)
  {
    return false;
  }

  const uint32_t pte1_rwx = pte1 & (RV32_FAST_PTE_R | RV32_FAST_PTE_W | RV32_FAST_PTE_X);
  if (pte1_rwx != 0)
  {
    return false;
  }

  const paddr_t l0_pt = (paddr_t)((pte1 >> 10) << PAGE_SHIFT);
  const paddr_t pte0_addr = l0_pt + (paddr_t)(vpn0 * 4u);
  const uint32_t pte0 = (uint32_t)paddr_read(pte0_addr, 4);

  if ((pte0 & RV32_FAST_PTE_V) == 0)
  {
    return false;
  }

  const uint32_t perm = pte0 & (RV32_FAST_PTE_R | RV32_FAST_PTE_W | RV32_FAST_PTE_X);
  if (perm == 0 || (perm & need_perm) == 0)
  {
    return false;
  }

  const paddr_t pg_paddr = (paddr_t)((pte0 >> 10) << PAGE_SHIFT);
  *entry = (rv32_fast_tlb_entry_t) {
    .satp = satp,
    .vpn = vpn,
    .perm = perm,
    .pg_paddr = pg_paddr,
    .pt_page = l0_pt,
    .valid = true,
  };

  *paddr = pg_paddr | (paddr_t)(addr & PAGE_MASK);
  return true;
}

static inline bool rv32_fast_store_may_touch_page_table(paddr_t paddr)
{
  if (rv32_fast_direct_mode())
  {
    return false;
  }

  const uint32_t satp = cpu.csr.satp;
  const paddr_t page = paddr & ~(paddr_t)PAGE_MASK;
  const paddr_t root = (paddr_t)((satp & RV32_FAST_SATP_PPN_MASK) << PAGE_SHIFT);

  if (page == root)
  {
    return true;
  }

  for (size_t i = 0; i < RV32_FAST_TLB_SIZE; i++)
  {
    const rv32_fast_tlb_entry_t *entry = &rv32_fast_tlb[i];
    if (entry->valid && entry->satp == satp && entry->pt_page == page)
    {
      return true;
    }
  }

  return false;
}

static inline bool rv32_fast_read_mem(vaddr_t addr, int len, int type, word_t *data)
{
  if (rv32_fast_direct_mode())
  {
    const paddr_t paddr = (paddr_t)addr;
    if (!likely(in_pmem(paddr)))
    {
      return false;
    }

    *data = host_read(guest_to_host(paddr), len);
    return true;
  }

  paddr_t paddr = 0;
  if (!rv32_fast_translate(addr, len, type, &paddr) ||
      !rv32_fast_pmem_range(paddr, len))
  {
    return false;
  }

  *data = host_read(guest_to_host(paddr), len);
  return true;
}

static inline bool rv32_fast_write_mem(vaddr_t addr, int len, word_t data)
{
  if (rv32_fast_direct_mode())
  {
    const paddr_t paddr = (paddr_t)addr;
    if (!likely(in_pmem(paddr)))
    {
      return false;
    }

    host_write(guest_to_host(paddr), len, data);
    return true;
  }

  paddr_t paddr = 0;
  if (!rv32_fast_translate(addr, len, MEM_TYPE_WRITE, &paddr) ||
      !rv32_fast_pmem_range(paddr, len))
  {
    return false;
  }

  const bool flush_tlb = rv32_fast_store_may_touch_page_table(paddr);
  host_write(guest_to_host(paddr), len, data);
  if (unlikely(flush_tlb))
  {
    rv32_fast_tlb_flush();
  }
  return true;
}

static inline word_t rv32_fast_read_or_fallback(vaddr_t addr, int len, int type)
{
  if (type == MEM_TYPE_IFETCH && rv32_fast_direct_mode())
  {
    const paddr_t paddr = (paddr_t)addr;
    if (likely(in_pmem(paddr)))
    {
      return *(uint32_t *)guest_to_host(paddr);
    }
  }

  word_t data = 0;
  if (rv32_fast_read_mem(addr, len, type, &data))
  {
    return data;
  }

  return type == MEM_TYPE_IFETCH ? vaddr_ifetch(addr, len) : vaddr_read(addr, len);
}

static inline void rv32_fast_write_or_fallback(vaddr_t addr, int len, word_t data)
{
  if (!rv32_fast_write_mem(addr, len, data))
  {
    vaddr_write(addr, len, data);
  }
}

static inline void rv32_fast_tlb_flush_if_satp(word_t csr_addr)
{
  if (unlikely(csr_addr == 0x180u))
  {
    rv32_fast_tlb_flush();
  }
}

static inline void rv32_fast_nemu_trap(vaddr_t pc)
{
  nemu_state.state = NEMU_END;
  nemu_state.halt_pc = pc;
  nemu_state.halt_ret = gpr(10);
  cpu.pc = pc + 4;
}

static inline void rv32_fast_mret()
{
  word_t mstatus = cpu.csr.mstatus;
  word_t mpp = (mstatus >> 11) & 0x3u;
  word_t mpie = (mstatus >> 7) & 0x1u;

  mstatus &= ~((word_t)0x3u << 11);
  mstatus = (mstatus & ~((word_t)1u << 3)) | (mpie << 3);
  mstatus |= ((word_t)1u << 7);

  if (mpp != 0x3u)
  {
    mstatus &= ~((word_t)1u << 17);
  }

  cpu.csr.mstatus = mstatus;
  cpu.prvi = mpp;
  cpu.pc = cpu.csr.mepc;
}

static inline bool rv32_fast_exec_load(uint32_t instr, vaddr_t pc)
{
  const uint32_t rd = rv32_fast_bits(instr, 11, 7);
  const uint32_t funct3 = rv32_fast_bits(instr, 14, 12);
  const uint32_t rs1 = rv32_fast_bits(instr, 19, 15);
  const vaddr_t addr = gpr(rs1) + rv32_fast_imm_i(instr);

  switch (funct3)
  {
    case 0x0: rv32_fast_write_gpr(rd, (sword_t)(int8_t)rv32_fast_read_or_fallback(addr, 1, MEM_TYPE_READ)); break;
    case 0x1: rv32_fast_write_gpr(rd, (sword_t)(int16_t)rv32_fast_read_or_fallback(addr, 2, MEM_TYPE_READ)); break;
    case 0x2: rv32_fast_write_gpr(rd, rv32_fast_read_or_fallback(addr, 4, MEM_TYPE_READ)); break;
    case 0x4: rv32_fast_write_gpr(rd, rv32_fast_read_or_fallback(addr, 1, MEM_TYPE_READ)); break;
    case 0x5: rv32_fast_write_gpr(rd, rv32_fast_read_or_fallback(addr, 2, MEM_TYPE_READ)); break;
    default: return false;
  }

  cpu.pc = pc + 4;
  return true;
}

static inline bool rv32_fast_exec_store(uint32_t instr, vaddr_t pc)
{
  const uint32_t funct3 = rv32_fast_bits(instr, 14, 12);
  const uint32_t rs1 = rv32_fast_bits(instr, 19, 15);
  const uint32_t rs2 = rv32_fast_bits(instr, 24, 20);
  const vaddr_t addr = gpr(rs1) + rv32_fast_imm_s(instr);

  switch (funct3)
  {
    case 0x0: rv32_fast_write_or_fallback(addr, 1, gpr(rs2)); break;
    case 0x1: rv32_fast_write_or_fallback(addr, 2, gpr(rs2)); break;
    case 0x2: rv32_fast_write_or_fallback(addr, 4, gpr(rs2)); break;
    default: return false;
  }

  cpu.pc = pc + 4;
  return true;
}

static inline bool rv32_fast_exec_op_imm(uint32_t instr, vaddr_t pc)
{
  const uint32_t rd = rv32_fast_bits(instr, 11, 7);
  const uint32_t funct3 = rv32_fast_bits(instr, 14, 12);
  const uint32_t rs1 = rv32_fast_bits(instr, 19, 15);
  const uint32_t funct7 = rv32_fast_bits(instr, 31, 25);
  const word_t src = gpr(rs1);
  const sword_t imm = rv32_fast_imm_i(instr);

  switch (funct3)
  {
    case 0x0: rv32_fast_write_gpr(rd, src + imm); break;
    case 0x2: rv32_fast_write_gpr(rd, (sword_t)src < imm); break;
    case 0x3: rv32_fast_write_gpr(rd, src < (word_t)imm); break;
    case 0x4: rv32_fast_write_gpr(rd, src ^ (word_t)imm); break;
    case 0x6: rv32_fast_write_gpr(rd, src | (word_t)imm); break;
    case 0x7: rv32_fast_write_gpr(rd, src & (word_t)imm); break;
    case 0x1:
      if (funct7 != 0x00) return false;
      rv32_fast_write_gpr(rd, src << rv32_fast_bits(instr, 24, 20));
      break;
    case 0x5:
      if (funct7 == 0x00)
      {
        rv32_fast_write_gpr(rd, src >> rv32_fast_bits(instr, 24, 20));
      }
      else if (funct7 == 0x20)
      {
        rv32_fast_write_gpr(rd, (word_t)((sword_t)src >> rv32_fast_bits(instr, 24, 20)));
      }
      else
      {
        return false;
      }
      break;
    default: return false;
  }

  cpu.pc = pc + 4;
  return true;
}

static inline word_t rv32_fast_mulhsu(word_t lhs, word_t rhs)
{
  return (uint64_t)((int64_t)(sword_t)lhs * (uint64_t)rhs) >> 32;
}

static inline bool rv32_fast_exec_op(uint32_t instr, vaddr_t pc)
{
  const uint32_t rd = rv32_fast_bits(instr, 11, 7);
  const uint32_t funct3 = rv32_fast_bits(instr, 14, 12);
  const uint32_t rs1 = rv32_fast_bits(instr, 19, 15);
  const uint32_t rs2 = rv32_fast_bits(instr, 24, 20);
  const uint32_t funct7 = rv32_fast_bits(instr, 31, 25);
  const uint32_t key = (funct7 << 3) | funct3;
  const word_t lhs = gpr(rs1);
  const word_t rhs = gpr(rs2);

  switch (key)
  {
    case 0x000: rv32_fast_write_gpr(rd, lhs + rhs); break;
    case 0x100: rv32_fast_write_gpr(rd, lhs - rhs); break;
    case 0x001: rv32_fast_write_gpr(rd, lhs << (rhs & 0x1fu)); break;
    case 0x002: rv32_fast_write_gpr(rd, (sword_t)lhs < (sword_t)rhs); break;
    case 0x003: rv32_fast_write_gpr(rd, lhs < rhs); break;
    case 0x004: rv32_fast_write_gpr(rd, lhs ^ rhs); break;
    case 0x005: rv32_fast_write_gpr(rd, lhs >> (rhs & 0x1fu)); break;
    case 0x105: rv32_fast_write_gpr(rd, (word_t)((sword_t)lhs >> (rhs & 0x1fu))); break;
    case 0x006: rv32_fast_write_gpr(rd, lhs | rhs); break;
    case 0x007: rv32_fast_write_gpr(rd, lhs & rhs); break;
    case 0x008: rv32_fast_write_gpr(rd, lhs * rhs); break;
    case 0x009: rv32_fast_write_gpr(rd, ((int64_t)(sword_t)lhs * (int64_t)(sword_t)rhs) >> 32); break;
    case 0x00a: rv32_fast_write_gpr(rd, rv32_fast_mulhsu(lhs, rhs)); break;
    case 0x00b: rv32_fast_write_gpr(rd, ((uint64_t)lhs * (uint64_t)rhs) >> 32); break;
    case 0x00c:
      if ((sword_t)rhs == 0) rv32_fast_write_gpr(rd, (word_t)-1);
      else if ((sword_t)lhs == (sword_t)(1ULL << 31) && (sword_t)rhs == -1) rv32_fast_write_gpr(rd, lhs);
      else rv32_fast_write_gpr(rd, (word_t)((sword_t)lhs / (sword_t)rhs));
      break;
    case 0x00d:
      rv32_fast_write_gpr(rd, rhs == 0 ? (word_t)-1 : lhs / rhs);
      break;
    case 0x00e:
      if ((sword_t)rhs == 0) rv32_fast_write_gpr(rd, lhs);
      else if ((sword_t)lhs == (sword_t)(1ULL << 31) && (sword_t)rhs == -1) rv32_fast_write_gpr(rd, 0);
      else rv32_fast_write_gpr(rd, (word_t)((sword_t)lhs % (sword_t)rhs));
      break;
    case 0x00f:
      rv32_fast_write_gpr(rd, rhs == 0 ? lhs : lhs % rhs);
      break;
    default: return false;
  }

  cpu.pc = pc + 4;
  return true;
}

static inline bool rv32_fast_exec_branch(uint32_t instr, vaddr_t pc)
{
  const uint32_t funct3 = rv32_fast_bits(instr, 14, 12);
  const uint32_t rs1 = rv32_fast_bits(instr, 19, 15);
  const uint32_t rs2 = rv32_fast_bits(instr, 24, 20);
  const word_t lhs = gpr(rs1);
  const word_t rhs = gpr(rs2);
  bool taken = false;

  switch (funct3)
  {
    case 0x0: taken = lhs == rhs; break;
    case 0x1: taken = lhs != rhs; break;
    case 0x4: taken = (sword_t)lhs < (sword_t)rhs; break;
    case 0x5: taken = (sword_t)lhs >= (sword_t)rhs; break;
    case 0x6: taken = lhs < rhs; break;
    case 0x7: taken = lhs >= rhs; break;
    default: return false;
  }

  cpu.pc = taken ? pc + rv32_fast_imm_b(instr) : pc + 4;
  return true;
}

static inline bool rv32_fast_exec_system(uint32_t instr, vaddr_t pc)
{
  const uint32_t rd = rv32_fast_bits(instr, 11, 7);
  const uint32_t funct3 = rv32_fast_bits(instr, 14, 12);
  const uint32_t rs1 = rv32_fast_bits(instr, 19, 15);
  const word_t rs1_value = gpr(rs1);
  const word_t csr_addr = rv32_fast_bits(instr, 31, 20);
  rtlreg_t *csr = NULL;
  word_t old = 0;

  if (funct3 == 0)
  {
    switch (instr)
    {
      case 0x00000073u:
        cpu.pc = isa_raise_intr(gpr(17), pc);
        return true;
      case 0x30200073u:
        rv32_fast_mret();
        return true;
      default:
        return false;
    }
  }

  if (funct3 != 0x1 && funct3 != 0x2 && funct3 != 0x3 &&
      funct3 != 0x5 && funct3 != 0x6 && funct3 != 0x7)
  {
    return false;
  }

  csr = getCSRAddress(csr_addr);
  old = *csr;

  switch (funct3)
  {
    case 0x1:
      rv32_fast_write_gpr(rd, old);
      *csr = rs1_value;
      rv32_fast_tlb_flush_if_satp(csr_addr);
      break;
    case 0x2:
      rv32_fast_write_gpr(rd, old);
      if (isCSRWriteable(csr_addr) && rs1 != 0)
      {
        *csr = old | rs1_value;
        rv32_fast_tlb_flush_if_satp(csr_addr);
      }
      break;
    case 0x3:
      rv32_fast_write_gpr(rd, old);
      if (isCSRWriteable(csr_addr) && rs1 != 0)
      {
        *csr = old & ~rs1_value;
        rv32_fast_tlb_flush_if_satp(csr_addr);
      }
      break;
    case 0x5:
      rv32_fast_write_gpr(rd, old);
      *csr = rs1;
      rv32_fast_tlb_flush_if_satp(csr_addr);
      break;
    case 0x6:
      rv32_fast_write_gpr(rd, old);
      if (isCSRWriteable(csr_addr) && rs1 != 0)
      {
        *csr = old | rs1;
        rv32_fast_tlb_flush_if_satp(csr_addr);
      }
      break;
    case 0x7:
      rv32_fast_write_gpr(rd, old);
      if (isCSRWriteable(csr_addr) && rs1 != 0)
      {
        *csr = old & ~(word_t)rs1;
        rv32_fast_tlb_flush_if_satp(csr_addr);
      }
      break;
    default:
      return false;
  }

  cpu.pc = pc + 4;
  return true;
}

static inline bool isa_fast_exec_once()
{
  const vaddr_t pc = cpu.pc;
  const uint32_t instr = rv32_fast_read_or_fallback(pc, 4, MEM_TYPE_IFETCH);
  const uint32_t opcode = instr & 0x7fu;
  const uint32_t rd = rv32_fast_bits(instr, 11, 7);
  const uint32_t rs1 = rv32_fast_bits(instr, 19, 15);

  switch (opcode)
  {
    case 0x03: return rv32_fast_exec_load(instr, pc);
    case 0x13: return rv32_fast_exec_op_imm(instr, pc);
    case 0x17:
      rv32_fast_write_gpr(rd, pc + rv32_fast_imm_u(instr));
      cpu.pc = pc + 4;
      return true;
    case 0x23: return rv32_fast_exec_store(instr, pc);
    case 0x33: return rv32_fast_exec_op(instr, pc);
    case 0x37:
      rv32_fast_write_gpr(rd, rv32_fast_imm_u(instr));
      cpu.pc = pc + 4;
      return true;
    case 0x63: return rv32_fast_exec_branch(instr, pc);
    case 0x67:
      if (rv32_fast_bits(instr, 14, 12) != 0) return false;
      {
        const vaddr_t target = (gpr(rs1) + rv32_fast_imm_i(instr)) & ~(vaddr_t)1u;
        rv32_fast_write_gpr(rd, pc + 4);
        cpu.pc = target;
      }
      return true;
    case 0x6f:
      rv32_fast_write_gpr(rd, pc + 4);
      cpu.pc = pc + rv32_fast_imm_j(instr);
      return true;
    case 0x73: return rv32_fast_exec_system(instr, pc);
    case 0x6b:
      rv32_fast_nemu_trap(pc);
      return true;
    default:
      return false;
  }
}

#endif
