#include <am.h>
#include <klib-macros.h>

bool mpe_init(void (*entry)())
{
    // The NEMU AM target is single-core, so MPE startup is a direct call on the
    // boot CPU. A returning entry is a programming error because there are no
    // secondary CPUs or scheduler hand-off point to resume.
    entry();
    panic("MPE entry returns");
}

int cpu_count()
{
    // Preserve the AM MPE contract even on a uniprocessor target: portable code
    // can still query the CPU count and run the same loops as on SMP platforms.
    return 1;
}

int cpu_current()
{
    return 0;
}

int atomic_xchg(int *addr, int newval)
{
    /*
     * The NEMU AM target is uniprocessor.  A real atomic instruction would pull
     * in the RISC-V A extension or a hosted libatomic helper, neither of which
     * belongs in the current RV64IM bring-up.  A plain exchange preserves the AM
     * contract for single-core kernels such as thread-os.
     */
    int old = *addr;
    *addr = newval;
    return old;
}
