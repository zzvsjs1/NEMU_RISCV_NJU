#include <common.h>
#if defined(__ISA_MIPS32__)
#include <proc.h>
#endif

#ifdef NANOS_INIT_PRIV_TEST
#define X86_EFLAGS_CF (1u << 0)
#define X86_EFLAGS_ZF (1u << 6)
#endif

Context *schedule(Context *prev);

// Provided by syscall.c
int syscall_need_resched_and_clear(void);
Context *syscall_replacement_context_and_clear(void);

static Context *do_event(Event e, Context *c)
{
    switch (e.event)
    {
    case EVENT_YIELD:
    {
#if defined(__ISA_MIPS32__)
        /* An image entered directly through naive_uload() has no scheduler-owned PCB. */
        if (current == NULL)
        {
            return c;
        }
#endif

        return schedule(c);
    }

    case EVENT_SYSCALL:
    {
        void do_syscall(Context * c);
        do_syscall(c);

        /*
         * execve() is special: it replaces the current PCB's saved Context on
         * the same kernel stack. Returning the old syscall frame would resume
         * memory that now belongs to the new image's initial context.
         */
        Context *replacement = syscall_replacement_context_and_clear();

        if (replacement != NULL)
        {
            return replacement;
        }

        // Perform scheduling once if the syscall requested it.
        if (syscall_need_resched_and_clear())
        {
            return schedule(c);
        }

        break;
    }

    case EVENT_IRQ_TIMER:
    {
        // Timer IRQs are the pre-emptive path. They do not need a syscall return
        // value, only a scheduler decision based on the interrupted context.
        return schedule(c);
    }

    case EVENT_PAGEFAULT:
    {
#ifdef NANOS_INIT_PF_CAUSE_TEST
        const uintptr_t expected_ref = 0x50000000u;

        if (e.ref != expected_ref)
        {
            panic("pf-cause-test expected CR2 %p, got %p", (void *)expected_ref, (void *)e.ref);
        }

        if (e.cause != MMAP_WRITE)
        {
            panic("pf-cause-test expected write cause, got 0x%x", (unsigned)e.cause);
        }

        Log("pf-cause-test page fault at %p, cause = 0x%x", (void *)e.ref, (unsigned)e.cause);
        halt(0);
#elif defined(NANOS_INIT_PRIV_TEST)
        /*
         * priv-test deliberately writes to its read-only text page after all
         * other checks.  Reaching this event means x86 delivered #PF with CR2
         * instead of letting the write proceed.  The saved flags must still be
         * from before the faulting instruction, because x86 faults are
         * restartable.
         */
        if ((c->eflags & (X86_EFLAGS_CF | X86_EFLAGS_ZF)) != (X86_EFLAGS_CF | X86_EFLAGS_ZF))
        {
            panic("priv-test #PF committed EFLAGS before the fault: eflags = 0x%x", (unsigned)c->eflags);
        }

        Log("priv-test page fault at %p, cause = 0x%x", (void *)e.ref, (unsigned)e.cause);
        halt(0);
#else
        panic("Unhandled page fault at %p, cause = 0x%x", (void *)e.ref, (unsigned)e.cause);
#endif
        break;
    }

    default:
    {
        panic("Unhandled event ID = %d", e.event);
        break;
    }
    }

    return c;
}

void init_irq(void)
{
    Log("Initializing interrupt/exception handler...");
    cte_init(do_event);
}
