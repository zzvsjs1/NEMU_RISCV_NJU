#include <am.h>
#include <nemu.h>
#include <klib.h>

#define USER_SPACE RANGE(0x40000000, 0x80000000)
#define MIPS32_NORMAL_TRAP_PREFIX_SIZE 24u

static void *(*pgalloc_usr)(int) = NULL;
static void (*pgfree_usr)(void *) = NULL;
static bool vme_enable = false;
static PTE *cur_pdir = NULL;
static bool cur_is_user = false;

/*
 * trap.S consumes this direct-mapped hand-off word before its first frame
 * store.  A non-zero value names the kernel-stack top belonging to a Context
 * whose np marker requests a user return.  Volatile is required because
 * assembly clears and, around a TLB refill, restores the object behind C's
 * view.
 */
volatile uintptr_t __am_user_trap_stack_top = 0u;

static void validate_context_storage(const Context *c)
{
    const uintptr_t direct_start = 0x80000000u;
    const uintptr_t direct_end = 0xc0000000u;
    const uintptr_t address = (uintptr_t)c;

    /*
     * trap.S restores through this pointer after __am_switch() may have
     * replaced every TLB entry.  The complete Context and its 24-byte trap
     * prefix must therefore be aligned storage in kseg0 or kseg1, never a
     * translated user pointer or NEMU's unmapped kseg2/kseg3 range.
     */
    assert(c != NULL);
    assert((address & 0x7u) == 0u);
    assert(address >= direct_start + MIPS32_NORMAL_TRAP_PREFIX_SIZE);
    assert(address <= direct_end - sizeof(*c));
}

/*
 * AM's panic() accepts a literal string rather than printf-style arguments.
 * Emit the rejected values first so a malformed map request remains
 * diagnosable, then use panic() to stop execution with the source location.
 */
static void map_alignment_panic(uintptr_t vaddr, uintptr_t paddr)
{
    printf("MIPS32 map unaligned va=%p pa=%p\n",
           (void *)vaddr, (void *)paddr);
    panic("MIPS32 map received an unaligned address");
}

static void map_virtual_address_panic(const char *reason, uintptr_t vaddr)
{
    printf("MIPS32 map %s va=%p\n", reason, (void *)vaddr);
    panic("MIPS32 map rejected a virtual address");
}

/*
 * No architectural invalidate-all instruction exists for the pre-Release-6
 * MIPS32 TLB model used here.  Overwrite every slot with a distinct invalid
 * VPN2 instead.  Distinct EntryHi values also make accidental probe matches
 * impossible while both EntryLo valid bits remain clear.
 */
void __am_tlb_clear(void)
{
    const uintptr_t zero = 0u;

    for (uintptr_t index = 0; index < MIPS32_TLB_NR; index++)
    {
        const uintptr_t entryhi = 0x80000000u | (index << 13);

        asm volatile(
            "mtc0 %0, $0\n\t"
            "mtc0 %1, $10\n\t"
            "mtc0 %2, $2\n\t"
            "mtc0 %2, $3\n\t"
            "tlbwi\n\t"
            :
            : "r"(index), "r"(entryhi), "r"(zero)
            : "memory");
    }
}

/*
 * Convert one software page-table leaf into the hardware EntryLo layout.
 * The mapped address remains page aligned in bits 31:12; shifting it by six
 * places its PFN in EntryLo bits 25:6 while retaining V and D unchanged.
 */
static uintptr_t refill_entrylo(uintptr_t vaddr)
{
    const PTE pde = cur_pdir[PDX(vaddr)];

    if ((pde & PTE_V) == 0u)
        return 0u;

    PTE *const ptable = (PTE *)(pde & MIPS32_PTE_ADDR_MASK);
    const PTE pte = ptable[PTX(vaddr)];

    if ((pte & PTE_V) == 0u)
        return 0u;

    return ((pte & MIPS32_PTE_ADDR_MASK) >> 6) |
           (pte & (PTE_V | PTE_D));
}

void __am_tlb_refill(void)
{
    uintptr_t entryhi;
    uintptr_t badvaddr;
    uintptr_t cause;

    asm volatile("mfc0 %0, $10" : "=r"(entryhi));
    asm volatile("mfc0 %0, $8" : "=r"(badvaddr));
    asm volatile("mfc0 %0, $13" : "=r"(cause));

    assert(vme_enable && cur_is_user && cur_pdir != NULL);

    /* One TLB entry covers an even/odd pair of adjacent 4 KiB pages. */
    const uintptr_t pair_vaddr = entryhi & 0xffffe000u;
    const uintptr_t entrylo0 = refill_entrylo(pair_vaddr);
    const uintptr_t entrylo1 = refill_entrylo(pair_vaddr + PGSIZE);
    const bool odd_page = (badvaddr & PGSIZE) != 0u;
    const uintptr_t selected_entrylo = odd_page ? entrylo1 : entrylo0;

    if (selected_entrylo == 0u)
    {
        const unsigned exception = (unsigned)((cause >> 2) & 0x1fu);

        /* panic() is literal-only, so print every diagnostic value first. */
        printf("MIPS32 unmapped refill va=%p vpn2=%p half=%s access=%s\n",
               (void *)badvaddr, (void *)pair_vaddr,
               odd_page ? "odd" : "even",
               exception == 3u ? "write" : "read/ifetch");
        panic("MIPS32 software TLB refill found no selected mapping");
    }

    /* Install the even/odd pair under its VPN2 tag. */
    asm volatile(
        "mtc0 %0, $2\n\t"
        "mtc0 %1, $3\n\t"
        "mtc0 %2, $10\n\t"
        "tlbwr\n\t"
        :
        : "r"(entrylo0), "r"(entrylo1), "r"(pair_vaddr)
        : "memory");
}

bool vme_init(void *(*pgalloc_f)(int), void (*pgfree_f)(void *))
{
    assert(pgalloc_f != NULL);
    assert(pgfree_f != NULL);

    pgalloc_usr = pgalloc_f;
    pgfree_usr = pgfree_f;
    cur_pdir = NULL;
    cur_is_user = false;
    __am_user_trap_stack_top = 0u;
    vme_enable = true;
    __am_tlb_clear();
    return true;
}

void protect(AddrSpace *as)
{
    assert(as != NULL);
    assert(vme_enable && pgalloc_usr != NULL);

    PTE *const pdir = (PTE *)pgalloc_usr(PGSIZE);
    assert(pdir != NULL);

    /* Allocators are not required to return zero-filled page-table pages. */
    memset(pdir, 0, PGSIZE);

    as->ptr = pdir;
    as->pgsize = PGSIZE;
    as->area = USER_SPACE;
}

void unprotect(AddrSpace *as)
{
    /* Address spaces live for a process's lifetime, so leaf tables are not reclaimed here. */
    (void)as;
    (void)pgfree_usr;
}

void __am_get_cur_as(Context *c)
{
    c->pdir = vme_enable && cur_is_user ? cur_pdir : NULL;
}

void __am_switch(Context *c)
{
    /*
     * Disarm the previous user before changing translation state.  The new
     * value is published only after the selected address space is ready.
     */
    __am_user_trap_stack_top = 0u;

    /* Validate every byte needed by assembly before dereferencing c. */
    validate_context_storage(c);
    assert(c->np <= 1u);

    if (!vme_enable)
        return;

    if (c->pdir != NULL)
    {
        cur_pdir = c->pdir;
        cur_is_user = true;
        __am_tlb_clear();
    }
    else
    {
        /* NULL is AM's explicit marker for a kernel-only Context. */
        cur_is_user = false;
    }

    /*
     * Contexts always reside on direct-mapped kernel stacks.  np, rather than
     * EPC or pdir, is the authoritative record of whether trap return enters
     * user execution.  This distinction matters for a nested kernel exception
     * while a user page directory remains active: that nested Context can carry
     * the pdir but has np zero and must continue pushing below its kernel SP.
     * For np one, the byte immediately after Context is the aligned kernel-stack
     * top on which the next normal user trap may rebuild this fixed frame.
     */
    if (c->np != 0u)
    {
        assert(c->pdir != NULL);
        __am_user_trap_stack_top = (uintptr_t)c + sizeof(*c);
    }
}

void map(AddrSpace *as, void *va, void *pa, int prot)
{
    assert(as != NULL && as->ptr != NULL);

    const uintptr_t vaddr = (uintptr_t)va;
    const uintptr_t paddr = (uintptr_t)pa;

    if (((vaddr | paddr) & (PGSIZE - 1u)) != 0u)
        map_alignment_panic(vaddr, paddr);

    if (vaddr < (uintptr_t)as->area.start ||
        vaddr >= (uintptr_t)as->area.end)
    {
        map_virtual_address_panic("outside address space", vaddr);
    }

    assert(prot != MMAP_NONE);

    PTE *const pdir = as->ptr;
    PTE *ptable;
    const uintptr_t directory_index = PDX(vaddr);
    const uintptr_t table_index = PTX(vaddr);
    const PTE pde = pdir[directory_index];

    if ((pde & PTE_V) == 0u)
    {
        ptable = (PTE *)pgalloc_usr(PGSIZE);
        assert(ptable != NULL);

        /* See protect(): a leaf-table page must not depend on allocator data. */
        memset(ptable, 0, PGSIZE);
        pdir[directory_index] =
            ((uintptr_t)ptable & MIPS32_PTE_ADDR_MASK) | PTE_V;
    }
    else
    {
        ptable = (PTE *)(pde & MIPS32_PTE_ADDR_MASK);
    }

    if ((ptable[table_index] & PTE_V) != 0u)
        map_virtual_address_panic("duplicate", vaddr);

    /*
     * This mapping implementation does not distinguish fine-grained read/write
     * permissions.  A mapped leaf is valid and dirty so both loads and stores
     * can proceed.
     */
    ptable[table_index] =
        (paddr & MIPS32_PTE_ADDR_MASK) | PTE_V | PTE_D;

    /* A cached translation must never survive a page-table modification. */
    __am_tlb_clear();
}

Context *ucontext(AddrSpace *as, Area kstack, void *entry)
{
    assert(as != NULL && as->ptr != NULL);

    const uintptr_t start = (uintptr_t)kstack.start;
    const uintptr_t top = (uintptr_t)kstack.end & ~(uintptr_t)0x0fu;

    assert(top >= start);
    assert(top - start >=
           sizeof(Context) + MIPS32_NORMAL_TRAP_PREFIX_SIZE);

    Context *const c = (Context *)(top - sizeof(Context));

    memset(c, 0, sizeof(*c));
    c->pdir = as->ptr;
    c->epc = (uintptr_t)entry;
    c->np = 1u;

    /*
     * IE is set and EXL is clear, ready for ERET into the initial user frame.
     * np explicitly arms that first return's per-Context kernel trap top.
     */
    c->status = 1u;
    return c;
}
