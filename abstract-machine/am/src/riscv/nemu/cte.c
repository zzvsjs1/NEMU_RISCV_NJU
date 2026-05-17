#include <am.h>
#include <riscv/riscv.h>
#include <klib.h>

static Context *(*user_handler)(Event, Context *) = NULL;

enum {
  NP_KERNEL = 0,
  NP_USER = 1,
};

#if __riscv_xlen == 64
#define IRQ_TIMER ((uintptr_t)0x8000000000000007ull)
#elif __riscv_xlen == 32
#define IRQ_TIMER ((uintptr_t)0x80000007u)
#else
#error "Unsupported RISC-V XLEN"
#endif

#define CAUSE_ECALL_U 8u
#define CAUSE_ECALL_S 9u
#define CAUSE_ECALL_M 11u
#define MSTATUS_MIE   ((uintptr_t)1u << 3)
#define MSTATUS_MPIE  ((uintptr_t)1u << 7)
#define MSTATUS_MPP_M ((uintptr_t)3u << 11)

Context *__am_irq_handle(Context *c) {
  void __am_get_cur_as(Context *c);
  __am_get_cur_as(c);

  if (user_handler) {
    Event ev = {0};
    switch (c->mcause) {
      case IRQ_TIMER:
        ev.event = EVENT_IRQ_TIMER;
        break;
      default: ev.event = EVENT_ERROR; break;
    }

    if (c->mcause == CAUSE_ECALL_U ||
        c->mcause == CAUSE_ECALL_S ||
        c->mcause == CAUSE_ECALL_M) {
      /*
       * AM uses GPR1 == -1 as its private yield request. Normal syscalls keep
       * their syscall number in the same ABI register for the OS handler.
       */
      ev.event = (c->GPR1 == (uintptr_t)-1) ? EVENT_YIELD : EVENT_SYSCALL;
      c->mepc += sizeof(uint32_t);
    }

    c = user_handler(ev, c);
    assert(c != NULL);
  }

  return c;
}

extern void __am_asm_trap(void);

bool cte_init(Context *(*handler)(Event, Context *)) {
  // initialize exception entry
  asm volatile("csrw mtvec, %0" : : "r"(__am_asm_trap));
  asm volatile("csrw mscratch, zero");

  // register event handler
  user_handler = handler;

  return true;
}

Context *kcontext(Area kstack, void (*entry)(void *), void *arg) {
  uintptr_t sp = (uintptr_t)kstack.end;
  sp &= ~((uintptr_t)0xf);

  Context *c = (Context *)(sp - sizeof(Context));
  memset(c, 0, sizeof(Context));

  c->mepc = (uintptr_t)entry;
  c->GPRSP = sp;
  c->GPR2 = (uintptr_t)arg;
  c->mstatus = MSTATUS_MPP_M | MSTATUS_MPIE;
  c->pdir = NULL;
  c->ksp = kstack.end;
  c->np = NP_KERNEL;
  c->gpr[1] = 0;

  return c;
}

void yield() {
#ifdef __riscv_e
  asm volatile("li a5, -1; ecall");
#else
  asm volatile("li a7, -1; ecall");
#endif
}

bool ienabled() {
  uintptr_t mstatus;
  asm volatile("csrr %0, mstatus" : "=r"(mstatus));
  return (mstatus & MSTATUS_MIE) != 0;
}

void iset(bool enable) {
  if (enable) {
    asm volatile("csrsi mstatus, 8");
  } else {
    asm volatile("csrci mstatus, 8");
  }
}
