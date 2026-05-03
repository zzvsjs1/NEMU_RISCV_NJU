#include <isa.h>
#include "local-include/reg.h"
#include <string.h>   // strcmp
#include <stdio.h>    // printf

#define REG_FMT ("%-5s " FMT_WORD "%-5s" FMT_DECIMAL_WORD "%-5s" FMT_DECIMAL_WORD_SIGN "\n")

const char *regs[] = {
  "$0", "ra", "sp", "gp", "tp", "t0", "t1", "t2",
  "s0", "s1", "a0", "a1", "a2", "a3", "a4", "a5",
  "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7",
  "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6"
};

const char *csrs[] = {
    "mstatus", "mtvec", "mepc", "mcause"
};

/* Use an explicit CSR list for display to avoid struct-layout scanning and OOB bugs. */
typedef struct {
  word_t addr;         /* CSR address */
  const char *name;    /* display name */
} csr_disp_t;

static const csr_disp_t csr_list[] = {
  {0x180, "satp"},
  {0x300, "mstatus"},
  {0x305, "mtvec"},
  {0x340, "mscratch"},
  {0x341, "mepc"},
  {0x342, "mcause"},
};

static size_t csr_list_len(void) 
{
  return sizeof(csr_list) / sizeof(csr_list[0]);
}

word_t getCSRValue(const word_t address) 
{
  return (word_t)(*getCSRAddress(address));
}

rtlreg_t* getCSRAddress(const word_t address) 
{
  switch (address) {
    case 0x180: return &cpu.csr.satp;
    case 0x300: return &cpu.csr.mstatus;
    case 0x305: return &cpu.csr.mtvec;
    case 0x340: return &cpu.csr.mscratch;
    case 0x341: return &cpu.csr.mepc;
    case 0x342: return &cpu.csr.mcause;
    default:
      Assert(false, "Invalid csr address: " FMT_WORD "\n", address);
      return NULL; /* keep compiler happy */
  }
}

bool isCSRWriteable(const word_t csrAddr) {
  /*
   * CSR bits [11:10] encode access type.
   * 0b11 means read-only.
   * Note, this is only a read-only check, not a full privilege check.
   */
  const uint32_t access_type = (csrAddr >> 10) & 0b11;
  return access_type != 0b11;
}

void isa_reg_display() 
{
  printf("\n");

  /* Print GPRs. */
  for (size_t i = 0; i < ARRLEN(regs); i++) 
  {
    const word_t v = gpr(i);
    printf(REG_FMT, reg_name(i, 5), v, " ", v, " ", (sword_t)v);
  }

  /* Print PC. */
  {
    const word_t pc = ((riscv32_CPU_state*)&cpu)->pc;
    printf(REG_FMT, "pc", pc, " ", pc, " ", (sword_t)pc);
  }

  printf("\n\n");

  /* Print selected CSRs by address list, avoids out-of-bounds and struct-layout assumptions. */
  for (size_t i = 0; i < csr_list_len(); i++) 
  {
    const word_t v = getCSRValue(csr_list[i].addr);
    printf(REG_FMT, csr_list[i].name, v, " ", v, " ", (sword_t)v);
  }

  printf("\n\n");
}

word_t isa_reg_str2val(const char *s, bool *success) 
{
  if (success == NULL) 
  {
    return (word_t)-1;
  }

  if (s == NULL) 
  {
    *success = false;
    return (word_t)-1;
  }

  if (strcmp(s, "pc") == 0) 
  {
    *success = true; /* bug fix */
    return ((riscv32_CPU_state*)&cpu)->pc;
  }

  if (strcmp(s, "0") == 0 || strcmp(s, "$0") == 0)
  {
    *success = true;
    return gpr(0);
  }

  for (size_t i = 0; i < ARRLEN(regs); i++) 
  {
    if (strcmp(regs[i], s) == 0) 
    {
      *success = true;
      return gpr(i);
    }
  }

  *success = false;
  return (word_t)-1;
}

void isa_set_reg_val(const char* name, const word_t val) 
{
  if (name == NULL) 
  {
    PRI_ERR_E("Failed to set value, register name is NULL.");
    return;
  }

  if (strcmp("pc", name) == 0) 
  {
    ((riscv32_CPU_state*)&cpu)->pc = val;
    return;
  }

  for (size_t i = 0; i < ARRLEN(regs); i++) 
  {
    if (strcmp(regs[i], name) == 0) 
    {
      /* RISC-V x0 is hardwired to zero, ignore writes to $0. */
      if (i == 0) return;

      gpr(i) = val;
      return;
    }
  }

  PRI_ERR("Failed to set value, unknown register %s.", name);
}
