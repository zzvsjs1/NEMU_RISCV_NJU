#ifndef ARCH_H__
#define ARCH_H__

struct Context
{
    /*
     * trap.S saves and restores this structure by fixed offsets. Keep this
     * order in sync with OFFSET_* there for both RV32 and RV64 builds.
     */
    uintptr_t gpr[32];
    uintptr_t mcause;
    uintptr_t mstatus;
    uintptr_t mepc;
    void *pdir;
    void *ksp;
    uintptr_t np;
    uintptr_t _padding[2];
};

#define GPR1 gpr[17] // a7
#define GPR2 gpr[10] // a0
#define GPR3 gpr[11] // a1
#define GPR4 gpr[12] // a2
#define GPRx gpr[10] // a0
#define GPRSP gpr[2] // sp
#endif
