#ifndef __ARCH_H__
#define __ARCH_H__

struct Context
{
    /*
     * trap.S stores one word per architectural GPR at offsets 0..31.  GPR 0
     * never needs restoring, so the MIPS32 NEMU context layout reuses that word
     * for the address-space pointer without making the trap frame larger.
     */
    union
    {
        uintptr_t gpr[32];
        struct
        {
            void *pdir;
            uintptr_t _gpr[31];
        };
    };

    uintptr_t lo;
    uintptr_t hi;
    uintptr_t cause;
    uintptr_t status;
    uintptr_t epc;

    /*
     * np is AM's explicit "next privilege" marker: one means trap return
     * resumes user execution and zero means it resumes kernel execution.
     * trap.S records how the interrupted stack was selected, while ucontext()
     * seeds the first user return.  This non-architectural tail word also
     * rounds the 37-word architectural prefix up to an O32-aligned 152 bytes.
     */
    uintptr_t np;
};

#define GPR1 gpr[2] // v0
#define GPR2 gpr[4] // a0
#define GPR3 gpr[5] // a1
#define GPR4 gpr[6] // a2
#define GPRx gpr[2] // v0
#define GPRSP gpr[29]

#endif
