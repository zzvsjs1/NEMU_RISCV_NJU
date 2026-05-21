#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 64

#include <stdint.h>

extern Context *__am_irq_handle(Context *c);

static volatile int last_event = EVENT_NULL;

static Context *record_event(Event ev, Context *ctx)
{
    last_event = ev.event;
    return ctx;
}

static void dispatch_mcause(uint64_t mcause)
{
    Context ctx = {0};

    ctx.mcause = mcause;
    ctx.mstatus = 0x1800u; // MPP=M, so __am_get_cur_as() keeps pdir as a kernel context.
    last_event = EVENT_NULL;
    check(__am_irq_handle(&ctx) == &ctx);
}

static void test_rv64_timer_interrupt_uses_full_mcause(void)
{
    cte_init(record_event);

    /*
     * RV64 timer interrupts carry the interrupt bit in mcause[63].  Comparing
     * against only cause 7 would silently turn a real timer interrupt into
     * EVENT_ERROR, so keep the full architectural encoding in this regression.
     */
    dispatch_mcause(0x8000000000000007ull);
    check(last_event == EVENT_IRQ_TIMER);

    dispatch_mcause(7u);
    check(last_event == EVENT_ERROR);
}

#endif

int main(void)
{
#if defined(__riscv) && __riscv_xlen == 64
    test_rv64_timer_interrupt_uses_full_mcause();
#endif

    return 0;
}
