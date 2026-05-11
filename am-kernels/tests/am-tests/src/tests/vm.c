#include <amtest.h>

static Context *uctx;
static AddrSpace prot;
static uintptr_t st = 0;
static int first_trap = 1;

void *simple_pgalloc(int size)
{
    // The VM test only needs monotonically allocated pages. Aligning by walking st
    // is slow but explicit, which makes the page-size assumption easy to see.

    if (st == 0)
    {
        st = (uintptr_t)heap.start;
    }
    while (st % size != 0)
        st++;
    void *ret = (void *)st;
    st += size;
    return ret;
}

void simple_pgfree(void *ptr)
{
}

Context *vm_handler(Event ev, Context *ctx)
{
    // The first trap is used as a controlled transition into the freshly built
    // user context. Later traps return the context that was interrupted, allowing
    // the tiny user image to keep issuing syscalls.
    switch (ev.event)
    {
    case EVENT_YIELD:
        break;
    case EVENT_IRQ_TIMER:
    case EVENT_IRQ_IODEV:
        printf("==== interrupt (%s)  ====\n", ev.msg);
        break;
    case EVENT_PAGEFAULT:
        printf("PF: %x %s%s%s\n",
               ev.ref,
               (ev.cause & MMAP_NONE) ? "[not present]" : "",
               (ev.cause & MMAP_READ) ? "[read fail]" : "",
               (ev.cause & MMAP_WRITE) ? "[write fail]" : "");
        break;
    case EVENT_SYSCALL:
        iset(1);
        for (int volatile i = 0; i < 1000000; i++)
            ;
        printf("%d ", ctx->GPRx);
        break;
    default:
        assert(0);
    }

    if (first_trap)
    {
        first_trap = 0;
        return uctx;
    }
    else
    {
        return ctx;
    }
}

uint8_t code[] = {
#ifdef __ARCH_NATIVE
    0x31, 0xc0,                               // xor %eax, %eax
    0x83, 0xc0, 0x01,                         // add $1, %eax
    0xff, 0x14, 0x25, 0x00, 0x00, 0x10, 0x00, // call *0x100000
    0xeb, 0xf4,                               // jmp 2
#else
    // Non-native AM tests still use x86 bytes here; vm_test() rejects other ISAs
    // before execution. Keeping the blob local documents the historical x86-only
    // nature of this test.
    0x31, 0xc0, // xor %eax, %eax
    0x8d, 0xb6, // lea 0(%esi), %esi
    0x00, 0x00, 0x00, 0x00,
    0x83, 0xc0, 0x01, // add $1, %eax
    0xcd, 0x80,       // int $0x80
    0xeb, 0xf9,       // jmp 8
#endif

};

void vm_test()
{
    // This test validates the AM VME API shape rather than every architecture.
    // Unsupported ISAs return early so their test suites can still include the
    // source file without executing incompatible instruction bytes.

    if (!strncmp(__ISA__, "x86", 3) == 0 &&
        !strcmp(__ISA__, "native") == 0)
    {
        printf("Not supported architecture.\n");
        return;
    }
    protect(&prot);
    printf("Protected address space: [%p, %p)\n", prot.area.start, prot.area.end);

    uint8_t *ptr = (void *)((uintptr_t)(prot.area.start) +
                            ((uintptr_t)(prot.area.end) - (uintptr_t)(prot.area.start)) / 2);

    void *pg = simple_pgalloc(prot.pgsize);
    memcpy(pg, code, sizeof(code));

    // The mapping deliberately separates the user virtual address from the host C
    // pointer used to seed the page. It catches bugs where VME code accidentally
    // treats user VA and physical/kernel pointer as interchangeable.
    map(&prot, ptr, pg, MMAP_WRITE | MMAP_READ);
    printf("Code copied to %p (physical %p) execute\n", ptr, pg);

    static uint8_t stack[4096];
    uctx = ucontext(&prot, RANGE(stack, stack + sizeof(stack)), ptr);

    iset(1);
    yield();
}
