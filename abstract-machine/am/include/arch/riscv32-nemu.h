#ifndef ARCH_H__
#define ARCH_H__

struct Context 
{
  /*
   * trap.S saves and restores this structure by fixed offsets, not by C field
   * names. Keep the order in sync with OFFSET_* there: changing it silently
   * turns a scheduler decision into corrupted registers or the wrong satp.
   */
  uintptr_t gpr[32];
  uintptr_t mcause;
  uintptr_t mstatus;
  uintptr_t mepc;
  void *pdir;
  /*
   * pdir is the process page-table root used by __am_switch(). ksp is the
   * kernel stack top used by trap entry, and np records the next privilege
   * mode so mscratch can be prepared correctly before mret.
   */
  void *ksp;
  uintptr_t np;
  uintptr_t _padding[2];
};

#define GPR1 gpr[17] // a7
#define GPR2 gpr[10] // a0, x10
#define GPR3 gpr[11] // a1
#define GPR4 gpr[12] // a2
#define GPRx gpr[10] // a0
#define GPRSP gpr[2] // x2

#endif
