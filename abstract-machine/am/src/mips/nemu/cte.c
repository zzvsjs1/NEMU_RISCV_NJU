#include <am.h>
#include <mips/mips32.h>
#include <klib.h>

static Context *(*user_handler)(Event, Context *) = NULL;
extern volatile uintptr_t __am_user_trap_stack_top;

#define MIPS32_EXC_SYS 8u
#define MIPS32_EXC_INT 0u
#define MIPS32_STATUS_IE ((uintptr_t)1u << 0)

_Static_assert(offsetof(Context, gpr) == 0,
               "MIPS32 Context GPRs must begin at trap-frame offset zero");
_Static_assert(offsetof(Context, lo) == 32 * sizeof(uintptr_t),
               "MIPS32 Context LO offset must match trap.S");
_Static_assert(offsetof(Context, hi) == 33 * sizeof(uintptr_t),
               "MIPS32 Context HI offset must match trap.S");
_Static_assert(offsetof(Context, cause) == 34 * sizeof(uintptr_t),
               "MIPS32 Context Cause offset must match trap.S");
_Static_assert(offsetof(Context, status) == 35 * sizeof(uintptr_t),
               "MIPS32 Context Status offset must match trap.S");
_Static_assert(offsetof(Context, epc) == 36 * sizeof(uintptr_t),
               "MIPS32 Context EPC offset must match trap.S");
_Static_assert(offsetof(Context, np) == 37 * sizeof(uintptr_t),
               "MIPS32 Context np marker must follow the architectural state");
_Static_assert(sizeof(Context) == 38 * sizeof(uintptr_t),
               "MIPS32 Context size must match trap.S");
_Static_assert(sizeof(Context) % 8 == 0,
               "MIPS32 Context must preserve O32 stack alignment");

static uint32_t syscall_code_at(uintptr_t epc)
{
    const uint32_t inst = *(const uint32_t *)epc;
    return (inst >> 6) & 0xfffffu;
}

Context *__am_irq_handle(Context *c)
{
    void __am_get_cur_as(Context * c);
    __am_get_cur_as(c);

    if (user_handler)
    {
        Event ev = {0};
        const uint32_t ex_code = (c->cause >> 2) & 0x1fu;

        switch (ex_code)
        {
        case MIPS32_EXC_INT:
            /* This platform exposes only the periodic timer as a hardware interrupt. */
            ev.event = EVENT_IRQ_TIMER;
            break;
        case MIPS32_EXC_SYS:
            /* `syscall 1` is AM's private yield; code zero is a normal syscall. */
            ev.event = syscall_code_at(c->epc) == 1u ? EVENT_YIELD : EVENT_SYSCALL;
            c->epc += sizeof(uint32_t);
            break;
        default:
            ev.event = EVENT_ERROR;
            ev.cause = ex_code;
            break;
        }

        c = user_handler(ev, c);
        assert(c != NULL);
    }

    return c;
}

extern void __am_asm_trap(void);
extern void __am_tlb_refill_entry(void);

#define TLB_REFILL_ENTRY 0x80000000u
#define GENERAL_EXCEPTION_ENTRY 0x80000180u

bool cte_init(Context *(*handler)(Event, Context *))
{
    const uint32_t j_opcode = 0x08000000u;
    const uint32_t general_jump =
        j_opcode | (((uintptr_t)__am_asm_trap >> 2) & 0x03ffffffu);
    const uint32_t refill_jump =
        j_opcode | (((uintptr_t)__am_tlb_refill_entry >> 2) & 0x03ffffffu);

    /* The refill entry must never use the general trap's possibly-user SP. */
    *(uint32_t *)TLB_REFILL_ENTRY = refill_jump;
    *(uint32_t *)(TLB_REFILL_ENTRY + sizeof(uint32_t)) = 0u;

    *(uint32_t *)GENERAL_EXCEPTION_ENTRY = general_jump;
    *(uint32_t *)(GENERAL_EXCEPTION_ENTRY + sizeof(uint32_t)) = 0u;

    /* No user Context has been selected when the exception vectors start. */
    __am_user_trap_stack_top = 0u;

    // register event handler
    user_handler = handler;

    return true;
}

Context *kcontext(Area kstack, void (*entry)(void *), void *arg)
{
    const uintptr_t stack_start = (uintptr_t)kstack.start;
    uintptr_t stack_top = (uintptr_t)kstack.end & ~(uintptr_t)0x7u;
    const uintptr_t caller_home_size = 4u * sizeof(uintptr_t);

    assert(stack_top >= stack_start);
    assert(stack_top - stack_start >= sizeof(Context) + caller_home_size);

    Context *c = (Context *)(stack_top - sizeof(Context));

    memset(c, 0, sizeof(*c));
    c->epc = (uintptr_t)entry;

    /*
     * ERET enters `entry` without a real caller, so construct the four-word
     * argument-home area that an O32 caller would normally reserve.  Placing
     * it below the saved Context also prevents an entry prologue from
     * replacing that Context before the new kernel task has run.
     */
    c->GPRSP = (uintptr_t)c - caller_home_size;
    c->GPR2 = (uintptr_t)arg;

    /*
     * trap.S temporarily sets EXL before ERET.  ERET then clears EXL and
     * resumes with this saved IE bit, so a newly created kernel context must
     * opt in to the normal interrupt-enabled AM execution state explicitly.
     * memset() deliberately leaves np zero because this Context returns to
     * kernel execution and must retain its active kernel stack on later traps.
     */
    c->status = MIPS32_STATUS_IE;
    return c;
}

void yield()
{
    asm volatile("syscall 1");
}

bool ienabled()
{
    uintptr_t status = 0;
    asm volatile("mfc0 %0, $12" : "=r"(status));
    return (status & MIPS32_STATUS_IE) != 0;
}

void iset(bool enable)
{
    uintptr_t status = 0;
    asm volatile("mfc0 %0, $12" : "=r"(status));

    if (enable)
        status |= MIPS32_STATUS_IE;
    else
        status &= ~MIPS32_STATUS_IE;

    asm volatile("mtc0 %0, $12" : : "r"(status) : "memory");
}
