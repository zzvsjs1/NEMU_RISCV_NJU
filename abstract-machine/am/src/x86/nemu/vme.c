#include <am.h>
#include <nemu.h>
#include <klib.h>

static AddrSpace kas = {};
static void *(*pgalloc_usr)(int) = NULL;
static void (*pgfree_usr)(void *) = NULL;
static int vme_enable = 0;

static Area segments[] = { // Kernel memory mappings
    NEMU_PADDR_SPACE};

#define USER_SPACE RANGE(0x40000000, 0xc0000000)

#define X86_PTE_P 0x001u
#define X86_PTE_W 0x002u
#define X86_PTE_U 0x004u
#define X86_DPL_USER 0x3u
#define X86_PTE_ADDR_MASK 0xfffff000u
#define X86_PDE_INDEX(va) (((uintptr_t)(va) >> 22) & 0x3ffu)
#define X86_PTE_INDEX(va) (((uintptr_t)(va) >> 12) & 0x3ffu)
#define X86_PAGE_OFFSET(va) ((uintptr_t)(va) & (PGSIZE - 1u))
#define X86_PTE_PER_PAGE (PGSIZE / sizeof(PTE))
#define SEG_KCODE 1
#define SEG_UCODE 3
#define SEG_UDATA 4

static void *pgallocz(int bytes)
{
    void *page = pgalloc_usr(bytes);
    assert(page != NULL);
    memset(page, 0, bytes);
    return page;
}

static inline uintptr_t pte_base(PTE pte)
{
    return (uintptr_t)pte & X86_PTE_ADDR_MASK;
}

bool vme_init(void *(*pgalloc_f)(int), void (*pgfree_f)(void *))
{
    pgalloc_usr = pgalloc_f;
    pgfree_usr = pgfree_f;

    kas.ptr = pgallocz(PGSIZE);
    kas.area = RANGE(0, 0);
    kas.pgsize = PGSIZE;

    int i;
    for (i = 0; i < LENGTH(segments); i++)
    {
        void *va = segments[i].start;
        for (; va < segments[i].end; va += PGSIZE)
        {
            map(&kas, va, va, MMAP_READ | MMAP_WRITE);
        }
    }

    set_cr3(kas.ptr);
    set_cr0(get_cr0() | CR0_PG);
    vme_enable = 1;

    return true;
}

void protect(AddrSpace *as)
{
    PTE *updir = (PTE *)pgallocz(PGSIZE);
    as->ptr = updir;
    as->area = USER_SPACE;
    as->pgsize = PGSIZE;
    // map kernel space
    memcpy(updir, kas.ptr, PGSIZE);
}

void unprotect(AddrSpace *as)
{
    (void)as;
}

void __am_get_cur_as(Context *c)
{
    /*
     * The address-space marker belongs to a user context.  Kernel threads may run
     * while a user page table is active, but their saved Context must keep NULL so
     * __am_switch() preserves the currently active mappings on return.
     */
    c->cr3 = (vme_enable && (c->cs & X86_DPL_USER) == X86_DPL_USER ? (void *)get_cr3() : NULL);
}

void __am_switch(Context *c)
{
    if (vme_enable && c->cr3 != NULL)
    {
        set_cr3(c->cr3);
    }
}

void map(AddrSpace *as, void *va, void *pa, int prot)
{
    assert(as != NULL);
    assert(as->ptr != NULL);

    uintptr_t v = (uintptr_t)va;
    uintptr_t p = (uintptr_t)pa;
    bool user_page = v >= (uintptr_t)as->area.start && v < (uintptr_t)as->area.end;

    assert(X86_PAGE_OFFSET(v) == 0);
    assert(X86_PAGE_OFFSET(p) == 0);
    assert(as == &kas || user_page);

    PTE *pdir = (PTE *)as->ptr;
    uint32_t pde_idx = X86_PDE_INDEX(v);
    uint32_t pte_idx = X86_PTE_INDEX(v);

    if (prot == MMAP_NONE)
    {
        assert((pdir[pde_idx] & X86_PTE_P) != 0);
        PTE *pt = (PTE *)pte_base(pdir[pde_idx]);
        assert((pt[pte_idx] & X86_PTE_P) != 0);
        pt[pte_idx] = 0;
        return;
    }

    if ((pdir[pde_idx] & X86_PTE_P) == 0)
    {
        PTE *new_pt = (PTE *)pgallocz(PGSIZE);
        uintptr_t flags = X86_PTE_P | X86_PTE_W;

        /*
         * User page directories need user-visible intermediate entries.  Without
         * X86_PTE_U in the PDE, a real x86 privilege check would reject even a leaf
         * PTE that has X86_PTE_U set.
         */
        if (user_page)
        {
            flags |= X86_PTE_U;
        }

        pdir[pde_idx] = ((uintptr_t)new_pt & X86_PTE_ADDR_MASK) | flags;
    }

    PTE *pt = (PTE *)pte_base(pdir[pde_idx]);
    uintptr_t flags = X86_PTE_P;

    if (prot & MMAP_WRITE)
    {
        flags |= X86_PTE_W;
    }

    if (user_page)
    {
        flags |= X86_PTE_U;
    }

    assert(pte_idx < X86_PTE_PER_PAGE);
    assert((pt[pte_idx] & X86_PTE_P) == 0);
    pt[pte_idx] = (p & X86_PTE_ADDR_MASK) | flags;
}

Context *ucontext(AddrSpace *as, Area kstack, void *entry)
{
    assert(as != NULL);
    assert(as->ptr != NULL);

    uintptr_t sp = (uintptr_t)kstack.end;
    sp &= ~((uintptr_t)0xf);

    Context *ctx = (Context *)(sp - sizeof(Context));
    *ctx = (Context){0};

    ctx->cr3 = as->ptr;
    ctx->ds = USEL(SEG_UDATA);
    ctx->cs = USEL(SEG_UCODE);
    ctx->ss3 = USEL(SEG_UDATA);
    ctx->eflags = FL_IF;
    ctx->eip = (uintptr_t)entry;
    ctx->esp0 = (uintptr_t)kstack.end;
    ctx->GPRSP = (uintptr_t)as->area.end;

    return ctx;
}
