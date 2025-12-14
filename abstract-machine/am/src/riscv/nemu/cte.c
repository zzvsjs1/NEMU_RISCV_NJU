#include <am.h>
#include <riscv/riscv.h>
#include <klib.h>

static Context* (*user_handler)(Event, Context*) = NULL;

Context* __am_irq_handle(Context *c)
{
  // printf("\n\nc->mcause= 0x%x\n\n", c->mcause);
  if (user_handler)
  {
    Event ev = {0};
    switch (c->mcause) 
    {
      case -1: {
        ev.event = EVENT_YIELD; 
        // Add pc, this must be done by software.
        c->mepc += sizeof(uint32_t);
        break;
      }
      default: {
        ev.event = EVENT_ERROR; 
        break;
      }
    }

    // Is system call?
    if (c->mcause >= 0 && c->mcause <= 19)
    {
      ev.event = EVENT_SYSCALL; 
      c->mepc += sizeof(uint32_t);
    }

    c = user_handler(ev, c);
    assert(c != NULL);
  }

  return c;
}

extern void __am_asm_trap(void);

bool cte_init(Context*(*handler)(Event, Context*))
{
  // initialize exception entry
  asm volatile("csrw mtvec, %0" : : "r"(__am_asm_trap));

  // register event handler
  user_handler = handler;

  return true;
}

Context *kcontext(Area kstack, void (*entry)(void *), void *arg) 
{
  // Align to 16 bytes.
  uintptr_t sp = (uintptr_t)kstack.end;
  sp &= ~((uintptr_t)0xF);

  // Place context
  Context *c = (Context *)(sp - sizeof(Context));

  // Fill zero
  memset(c, 0, sizeof(Context));

  c->mepc = (uintptr_t)entry;
  // Stack pointer is x2, which is gpr[2].
  c->GPRSP = sp;            

  // Place the argument.
  c->GPR2 = (uintptr_t)arg;

  c->mstatus = 0x1800 | 0x80;

  c->mcause = 0;
  c->pdir = NULL;

  // Clash now!
  c->gpr[1] = 0;
  
  return c;
}

void yield()
{
  asm volatile("li a7, -1; ecall");
}

bool ienabled()
{
  return false;
}

void iset(bool enable)
{
  // Do nothing.
}
