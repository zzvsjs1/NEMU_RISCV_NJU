#include <memory.h>
#include "proc.h"

extern PCB *current;

static void *pf = NULL;

#ifndef ROUNDUP
#define ROUNDUP(x, a) (((x) + (a) - 1) & ~((a) - 1))
#endif

void *new_page(size_t nr_page)
{
    // Simple bump allocator, returns a contiguous physical memory region.
    void *p = pf;
    pf = (void *)((uintptr_t)pf + nr_page * PGSIZE);

    // Optional sanity check, avoid running out of heap.
    assert((uintptr_t)pf <= (uintptr_t)heap.end);

    return p;
}

#ifdef HAS_VME
static void *pg_alloc(int n)
{
    // vme_init() calls pg_alloc(PGSIZE), so n is bytes and is page-aligned.
    assert(n % PGSIZE == 0);
    size_t nr_page = (size_t)n / PGSIZE;

    void *p = new_page(nr_page);

    // Zero pages for safety, page tables usually require zero-init.
    memset(p, 0, n);

    return p;
}
#endif

void free_page(void *p)
{
    panic("not implement yet");
}

/* The brk() system call handler. */
int mm_brk(uintptr_t brk)
{
    // Some libc implementations may call brk(0) as a no-op in our program.

    if (brk == 0)
    {
        return 0;
    }

    // Only grow mappings, we do not reclaim pages when brk shrinks.

    if (brk <= current->max_brk)
    {
        return 0;
    }

    // map() requires page-aligned VA/PA in our implementation.
    uintptr_t va_begin = ROUNDUP(current->max_brk, PGSIZE);
    uintptr_t va_end = ROUNDUP(brk, PGSIZE);

    // Basic sanity: brk should stay inside this process user space.
    uintptr_t us = (uintptr_t)current->as.area.start;
    uintptr_t ue = (uintptr_t)current->as.area.end;

    if (brk < us || brk > ue)
    {
        // Return failure if user requests an invalid brk.
        return -1;
    }

    for (uintptr_t va = va_begin; va < va_end; va += PGSIZE)
    {
        void *pa = new_page(1);
        assert(pa != NULL);

        // New heap pages must be zero-filled.
        memset(pa, 0, PGSIZE);

        // prot is ignored in this AM map() implementation. Passing 0 keeps the
        // caller-side policy simple: any mapped user heap page receives the usual
        // readable/writable/executable Sv32 leaf flags inside map().
        map(&current->as, (void *)va, pa, 0);
    }

    current->max_brk = brk;
    return 0;
}

void init_mm()
{
    pf = (void *)ROUNDUP(heap.start, PGSIZE);
    Log("free physical pages starting from %p", pf);

#ifdef HAS_VME
    vme_init(pg_alloc, free_page);
#endif
}
