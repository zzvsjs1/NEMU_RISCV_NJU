#include "trap.h"

#if !defined(__mips__)
#error "This test must be compiled for MIPS32"
#endif

enum
{
    MIPS32_STATUS_EXL = 1u << 1,
    DISABLED_WINDOW_US = 50000,
    ENABLED_TIMEOUT_US = 1000000,
    REQUIRED_TIMER_EVENTS = 2,
};

static volatile unsigned timer_events = 0;

static uint64_t uptime_us(void)
{
    return io_read(AM_TIMER_UPTIME).us;
}

static Context *handle_event(Event event, Context *context)
{
    /*
     * A hardware interrupt uses ExcCode zero on MIPS32.  Trap entry must also
     * have raised Status.EXL before CTE examines the saved frame, otherwise a
     * second timer edge could overwrite this context while it is being used.
     */
    check(event.event == EVENT_IRQ_TIMER);
    check(((context->cause >> 2) & 0x1fu) == 0u);
    check((context->status & MIPS32_STATUS_EXL) != 0u);

    timer_events++;
    return context;
}

int main(void)
{
    check(ioe_init());
    check(cte_init(handle_event));

    /*
     * A pending device edge must remain masked while IE is clear.  Waiting for
     * several host timer periods makes this a real delivery check rather than
     * merely testing the CTE event decoder with a fabricated Context.
     */
    iset(false);
    const uint64_t disabled_start = uptime_us();

    while (uptime_us() - disabled_start < DISABLED_WINDOW_US)
    {
        asm volatile("nop");
    }

    check(timer_events == 0u);

    iset(true);
    check(ienabled());

    const uint64_t enabled_start = uptime_us();

    while (timer_events < REQUIRED_TIMER_EVENTS && uptime_us() - enabled_start < ENABLED_TIMEOUT_US)
    {
        asm volatile("nop");
    }

    iset(false);
    check(timer_events >= REQUIRED_TIMER_EVENTS);
    return 0;
}
