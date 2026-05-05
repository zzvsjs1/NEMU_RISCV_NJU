#include <isa-jit.h>

bool isa_jit_available(void)
{
  return false;
}

bool isa_jit_exec(uint64_t remaining, uint32_t device_budget, uint32_t *executed)
{
  (void)remaining;
  (void)device_budget;
  *executed = 0;
  return false;
}

void isa_jit_flush_all(void)
{
}

void isa_jit_invalidate_paddr(paddr_t addr, int len)
{
  (void)addr;
  (void)len;
}
