#include <memory.h>

static void *pf = NULL;

void* new_page(size_t nr_page) 
{
  // Simple bump allocator, returns a contiguous physical memory region.
  void *p = pf;
  pf = (void *)((uintptr_t)pf + nr_page * PGSIZE);

  // Optional sanity check, avoid running out of heap.
  assert((uintptr_t)pf <= (uintptr_t)heap.end);

  return p;
}

#ifdef HAS_VME
static void* pg_alloc(int n) 
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
  return 0;
}

void init_mm() {
  pf = (void *)ROUNDUP(heap.start, PGSIZE);
  Log("free physical pages starting from %p", pf);

#ifdef HAS_VME
  vme_init(pg_alloc, free_page);
#endif
}
