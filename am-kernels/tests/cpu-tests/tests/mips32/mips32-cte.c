#include "trap.h"
#include <stdint.h>

#if !defined(__mips__)
#error "This test must be compiled for MIPS32"
#endif

static volatile int yield_count = 0;

enum
{
    MIPS32_O32_STACK_ALIGNMENT = 8,
    KCONTEXT_STACK_SIZE = 512,
};

static uint8_t kcontext_stack[KCONTEXT_STACK_SIZE] __attribute__((aligned(MIPS32_O32_STACK_ALIGNMENT)));

static void kcontext_entry(void *arg)
{
    (void)arg;
}

static Context *handle_event(Event event, Context *context)
{
    check(event.event == EVENT_YIELD);
    check(((context->cause >> 2) & 0x1fu) == 8u);
    check((context->status & 0x2u) != 0);

    const uint32_t syscall_inst = *(const uint32_t *)(context->epc - sizeof(uint32_t));
    check((syscall_inst & 0xfc00003fu) == 0x0000000cu);
    check(((syscall_inst >> 6) & 0xfffffu) == 1u);

    yield_count++;
    return context;
}

int main(void)
{
    /*
     * An exception frame is subtracted from an O32-aligned stack before the
     * assembly trap path calls C.  Its size must therefore preserve the
     * ABI's eight-byte stack alignment, including for newly made contexts.
     */
    check((sizeof(Context) % MIPS32_O32_STACK_ALIGNMENT) == 0u);

    const Area kstack = RANGE(kcontext_stack, kcontext_stack + sizeof(kcontext_stack));
    Context *const context = kcontext(kstack, kcontext_entry, NULL);
    check(((uintptr_t)context % MIPS32_O32_STACK_ALIGNMENT) == 0u);
    check(context->np == 0u);

    /*
     * A newly entered O32 function is still a callee even though ERET, rather
     * than JAL, starts it.  Its synthetic caller must therefore leave the four
     * standard argument-home words immediately above the initial SP.  Keeping
     * those words below Context also prevents an entry prologue from replacing
     * the only saved state before the new kernel context has run.
     */
    check(context->GPRSP == (uintptr_t)context - 4u * sizeof(uintptr_t));
    check((context->GPRSP % MIPS32_O32_STACK_ALIGNMENT) == 0u);

    check(cte_init(handle_event));

    iset(false);
    check(!ienabled());

    iset(true);
    check(ienabled());

    yield();
    check(yield_count == 1);
    check(ienabled());

    yield();
    check(yield_count == 2);

    iset(false);
    check(!ienabled());
    return 0;
}
