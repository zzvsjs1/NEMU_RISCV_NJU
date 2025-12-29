#include <isa.h>
#include <memory/paddr.h>
#include <memory/vaddr.h>

int isa_mmu_check(vaddr_t vaddr, int len, int type) 
{
  uint32_t mode = (cpu.csr.satp >> 31) & 0x1;
  return mode ? MMU_TRANSLATE : MMU_DIRECT;
}

paddr_t isa_mmu_translate(vaddr_t vaddr, int len, int type) 
{
  return MEM_RET_FAIL;
}
