#include <am.h>
#include <riscv/riscv.h>
#include <klib.h>

static Context *(*user_handler)(Event, Context *) = NULL;

enum
{
    NP_KERNEL = 0,
    NP_USER = 1,
};

// NEMU raises the machine timer interrupt with the standard RISC-V interrupt
// bit set in mcause. The portable AM layer should see EVENT_IRQ_TIMER instead
// of depending on that raw encoded value.
#define IRQ_TIMER ((uintptr_t)0x80000007u)
#define CAUSE_ECALL_U 8u
#define CAUSE_ECALL_S 9u
#define CAUSE_ECALL_M 11u
#define MSTATUS_MIE (1u << 3)
#define MSTATUS_MPIE (1u << 7)
#define MSTATUS_MPP_M (3u << 11)

Context *__am_irq_handle(Context *c)
{
    void __am_get_cur_as(Context * c);
    // Trap entry runs in machine mode, but the interrupted context may belong to
    // user space. Refresh c->pdir before dispatch so the OS handler can tell
    // whether the saved context needs an address-space switch on return.
    __am_get_cur_as(c);

    // printf("\n\nc->mcause= 0x%x\n\n", c->mcause);

    if (user_handler)
    {
        Event ev = {0};
        switch (c->mcause)
        {
        case IRQ_TIMER:
        {
            ev.event = EVENT_IRQ_TIMER;
            break;
        }

        default:
        {
            ev.event = EVENT_ERROR;
            break;
        }
        }

        if (c->mcause == CAUSE_ECALL_U ||
            c->mcause == CAUSE_ECALL_S ||
            c->mcause == CAUSE_ECALL_M)
        {
            /*
             * RISC-V keeps the ecall reason in mcause and the ABI payload in
             * normal registers.  AM uses a7 == -1 as its private yield request;
             * Nanos-lite syscalls keep using saved a7 as the syscall number.
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

bool cte_init(Context *(*handler)(Event, Context *))
{
    // initialize exception entry
    // mtvec must point at the assembly shim because the shim saves all general
    // registers before C code can safely examine or modify the interrupted state.
    asm volatile("csrw mtvec, %0" : : "r"(__am_asm_trap));
    // A zero mscratch marks the current execution as kernel mode. User contexts
    // install their kernel stack top just before mret, so the first trap from a
    // freshly booted kernel must not be mistaken for a user trap.
    asm volatile("csrw mscratch, zero");

    // register event handler
    user_handler = handler;

    return true;
}

Context *kcontext(Area kstack, void (*entry)(void *), void *arg)
{
    // Align to 16 bytes.
    // The saved Context lives at the top of the provided kernel stack. Keeping
    // the stack 16-byte aligned matches the RISC-V ABI and avoids surprises when
    // compiled C code spills wider objects.
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

    // Kernel contexts return through mret too, so MPP must be machine mode.
    // MPIE is set so interrupts can be enabled again after the return path
    // restores this synthetic context.
    c->mstatus = MSTATUS_MPP_M | MSTATUS_MPIE;

    c->mcause = 0;
    c->pdir = NULL;
    c->ksp = kstack.end;
    // NP_KERNEL makes trap.S keep mscratch at zero when this context resumes.
    // That prevents a later kernel timer interrupt from swapping to a user stack.
    c->np = NP_KERNEL;

    // Clash now!
    c->gpr[1] = 0;

    return c;
}

void yield()
{
    // The a7 value is an AM-private convention recognised by __am_irq_handle().
    // Hardware only sees an architectural ecall cause in mcause.
    asm volatile("li a7, -1; ecall");
}

bool ienabled()
{
    // Interrupt enable is tracked in mstatus.MIE while running in machine mode.
    // This helper intentionally reports the current hardware bit, not the MPIE
    // value that will be restored by a future mret.
    uintptr_t mstatus;
    asm volatile("csrr %0, mstatus" : "=r"(mstatus));
    return (mstatus & MSTATUS_MIE) != 0;
}

void iset(bool enable)
{
    if (enable)
    {
        asm volatile("csrsi mstatus, 8");
    }
    else
    {
        asm volatile("csrci mstatus, 8");
    }
}
