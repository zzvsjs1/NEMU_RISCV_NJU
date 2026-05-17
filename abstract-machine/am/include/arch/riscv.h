#ifndef ARCH_H__
#define ARCH_H__

#ifdef __riscv_e
#define NR_REGS 16
#else
#define NR_REGS 32
#endif

struct Context {
  /*
   * This full layout is the NEMU trap-frame contract: riscv/nemu/trap.S saves
   * and restores it by fixed offsets. Keep this order in sync with OFFSET_*
   * there for RV32, RV32E, and RV64 builds. RV32E saves fewer GPRs than the
   * other RISC-V variants, so NR_REGS is part of the ABI boundary.
   *
   * NPC currently saves only the prefix fields through mepc. Fields after mepc
   * are NEMU-specific state and must not be assumed to exist in an NPC frame.
   */
  uintptr_t gpr[NR_REGS];
  uintptr_t mcause;
  uintptr_t mstatus;
  uintptr_t mepc;
  void *pdir;
  void *ksp;
  uintptr_t np;
  uintptr_t _padding[2];
};

#ifdef __riscv_e
#define GPR1 gpr[15] // a5
#else
#define GPR1 gpr[17] // a7
#endif

#define GPR2  gpr[10] // a0
#define GPR3  gpr[11] // a1
#define GPR4  gpr[12] // a2
#define GPRx  gpr[10] // a0
#define GPRSP gpr[2]  // sp

#endif
