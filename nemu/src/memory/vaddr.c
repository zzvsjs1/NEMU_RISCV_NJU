#include <isa.h>
#include <memory/paddr.h>

word_t vaddr_ifetch(vaddr_t addr, int len) 
{
  return paddr_read(addr, len);
}

word_t vaddr_read(vaddr_t addr, int len) 
{
  int mmu_state = isa_mmu_check(addr, len, MEM_TYPE_READ);
  if (mmu_state == MMU_DIRECT) 
  {
    return paddr_read(addr, len);
  }

  if (mmu_state == MMU_TRANSLATE) 
  {
    paddr_t pa = isa_mmu_translate(addr, len, MEM_TYPE_READ);
    return paddr_read(pa, len);
  }

  assert(0);
  return 0;
}

void vaddr_write(vaddr_t addr, int len, word_t data) 
{
  int mmu_state = isa_mmu_check(addr, len, MEM_TYPE_WRITE);
  if (mmu_state == MMU_DIRECT) 
  {
    paddr_write(addr, len, data);
    return;
  }

  if (mmu_state == MMU_TRANSLATE) 
  {
    paddr_t pa = isa_mmu_translate(addr, len, MEM_TYPE_WRITE);
    paddr_write(pa, len, data);
    return;
  }

  assert(0);
}

