#include <am.h>
#include <nemu.h>
#include <klib.h>

static AddrSpace kas = {};
static void* (*pgalloc_usr)(int) = NULL;
static void (*pgfree_usr)(void*) = NULL;
static int vme_enable = 0;

static Area segments[] = {      // Kernel memory mappings
    NEMU_PADDR_SPACE
};

#define USER_SPACE RANGE(0x40000000, 0x80000000)

#define PAGE_SHIFT        12
#define PAGE_SIZE         (1ul << PAGE_SHIFT)
#define PAGE_MASK         (PAGE_SIZE - 1)

static inline void set_satp(void *pdir) 
{
    uintptr_t mode = 1ul << (__riscv_xlen - 1);
    asm volatile("csrw satp, %0" : : "r"(mode | ((uintptr_t)pdir >> 12)));
}

static inline uintptr_t get_satp() 
{
    uintptr_t satp;
    asm volatile("csrr %0, satp" : "=r"(satp));
    uintptr_t ppn = satp & 0x003FFFFFul;   // Sv32 PPN is low 22 bits
    return ppn << 12;
}

bool vme_init(void* (*pgalloc_f)(int), void (*pgfree_f)(void*)) 
{
    pgalloc_usr = pgalloc_f;
    pgfree_usr = pgfree_f;

    kas.ptr = pgalloc_f(PGSIZE);

    int i;
    for (i = 0; i < LENGTH(segments); i ++) {
        void *va = segments[i].start;
        for (; va < segments[i].end; va += PGSIZE) {
            map(&kas, va, va, 0);
        }
    }

    set_satp(kas.ptr);
    vme_enable = 1;

    return true;
}

void protect(AddrSpace *as) 
{
    PTE *updir = (PTE*)(pgalloc_usr(PGSIZE));
    as->ptr = updir;
    as->area = USER_SPACE;
    as->pgsize = PGSIZE;
    // map kernel space
    memcpy(updir, kas.ptr, PGSIZE);
}

void unprotect(AddrSpace *as) 
{

}

void __am_get_cur_as(Context *c) 
{
    uintptr_t mpp = (c->mstatus >> 11) & 0x3;
    c->pdir = (vme_enable && mpp == 0 ? (void *)get_satp() : NULL);
}

void __am_switch(Context *c) 
{
    if (vme_enable && c->pdir != NULL) 
    {
        set_satp(c->pdir);
    }
}

void map(AddrSpace *as, void *va, void *pa, int prot) 
{
    // ---- Basic sanity checks ----
    assert(as != NULL);
    assert(as->ptr != NULL);

    uintptr_t v = (uintptr_t)va;
    uintptr_t p = (uintptr_t)pa;

    // Sv32 uses 4KiB pages, so both virtual and physical addresses should be page-aligned for mapping.
    assert((v & PAGE_MASK) == 0);

    // ---- Split VPN fields for Sv32 ----
    // va[31:22] = VPN[1], va[21:12] = VPN[0], va[11:0] = page offset
    uint32_t vpn1 = (uint32_t)((v >> 22) & 0x3FFu);
    uint32_t vpn0 = (uint32_t)((v >> 12) & 0x3FFu);

    // Root page table (level-1) base.
    // In PA, physical address is identity-mapped for the kernel, so we can treat it as a C pointer.
    PTE *l1 = (PTE *)as->ptr;

    // ---- Ensure the level-0 page table exists ----
    PTE pte1 = l1[vpn1];

    if ((pte1 & PTE_V) == 0) 
    {
        // Allocate one page for the next-level page table (level-0 page table).
        void *new_pt_page = pgalloc_usr(PGSIZE);
        assert(new_pt_page != NULL);

        // pg_alloc() in OS already zeroes pages, but keeping this makes the function robust.
        memset(new_pt_page, 0, PGSIZE);

        // Build a non-leaf PTE:
        // PTE.PPN is stored in bits [31:10], flags in bits [9:0].
        uint32_t ppn = (uint32_t)(((uintptr_t)new_pt_page) >> 12);

        // For a pointer PTE, R/W/X must be 0 (otherwise it would be a leaf).
        // Set V=1, keep other bits 0 for forward compatibility.
        l1[vpn1] = (PTE)((ppn << 10) | PTE_V);

        pte1 = l1[vpn1];
    } 
    else 
    {
        // If R/W/X are not all zero at level-1, it means a superpage leaf PTE.
        // We does not build superpages, so assert to catch bugs early.
        assert((pte1 & (PTE_R | PTE_W | PTE_X)) == 0);
    }

    // ---- Locate the level-0 page table ----
    // Extract PPN from PTE and convert it to a physical page base address.
    uintptr_t l0_base = (uintptr_t)((((uintptr_t)pte1) >> 10) << 12);
    PTE *l0 = (PTE *)l0_base;

    // Physical address must also be page-aligned for 4KiB mapping.
    assert((p & PAGE_MASK) == 0);

    uint32_t leaf_ppn = (uint32_t)(p >> 12);

    // Build flags for leaf PTE.
    // In PA, we do not implement fine-grained protection,
    // so we can set R/W/X all to 1 to avoid permission faults.
    // Also set A and D to 1, avoiding hardware A/D update complexity.
    uint32_t flags = PTE_V | PTE_R | PTE_W | PTE_X | PTE_A | PTE_D;

    // Mark user pages with U=1, kernel pages keep U=0.
    // User space is as->area, protect() sets it to USER_SPACE.
    if (va >= as->area.start && va < as->area.end) 
    {
        flags |= PTE_U;
    }

    l0[vpn0] = (PTE)((leaf_ppn << 10) | flags);
}

Context *ucontext(AddrSpace *as, Area kstack, void *entry) 
{
    // Allocate the Context at the top of the kernel stack.
    uintptr_t sp = (uintptr_t)kstack.end;
    // 16-byte alignment is a good ABI habit
    sp &= ~((uintptr_t)0xF);

    Context *c = (Context *)(sp - sizeof(Context));

    // Clean zero.
    memset(c, 0, sizeof(Context));

    // Start executing from 'entry' after mret.
    c->mepc = (uintptr_t)entry;

    // For a user process, return to U-mode after mret.
    // RISC-V mstatus.MPP is at bits [12:11].
    // Set MPP=00 (U-mode) by clearing those bits.
    // Also set MPIE (bit 7) so interrupt enable state is sane after mret.
    c->mstatus = 0;
    c->mstatus |= (1 << 7);             // MPIE = 1
    c->mstatus &= ~((uintptr_t)0x1800); // MPP = 00 (U)

    c->pdir = as->ptr;   // Use this address space's page table root
    c->ksp = c;

    return c;
}
