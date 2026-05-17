#include <am.h>
#include <nemu.h>
#include <klib.h>

static AddrSpace kas = {};
static void *(*pgalloc_usr)(int) = NULL;
static void (*pgfree_usr)(void *) = NULL;
static int vme_enable = 0;

static Area segments[] = { // Kernel memory mappings
    NEMU_PADDR_SPACE};

// User mappings are kept in a high half-open interval that does not overlap the
// identity-mapped NEMU physical memory window. This lets kernel mappings be
// copied into every address space while user pages remain easy to recognise.
#define USER_SPACE RANGE(0x40000000, 0x80000000)

#define PAGE_SHIFT 12
#define PAGE_SIZE (1ul << PAGE_SHIFT)
#define PAGE_MASK (PAGE_SIZE - 1)

#define PTE_FLAG_MASK 0x3FFul
#define PTE_PPN_SHIFT 10

#if __riscv_xlen == 32
#define SATP_MODE 1ul
#define SATP_MODE_SHIFT 31
#define SATP_PPN_MASK 0x003FFFFFul
#define PT_LEVELS 2
#define VPN_BITS 10
#elif __riscv_xlen == 64
#define SATP_MODE 8ul
#define SATP_MODE_SHIFT 60
#define SATP_PPN_MASK 0x00000FFFFFFFFFFFul
#define PT_LEVELS 3
#define VPN_BITS 9
#else
#error "Unsupported RISC-V XLEN"
#endif

#define PTE_PER_PAGE (PGSIZE / sizeof(PTE))
#define VPN_MASK ((1ul << VPN_BITS) - 1)

static inline void set_satp(void *pdir)
{
    // The AM kernel keeps page-table pages in identity-mapped physical memory.
    // Therefore the C pointer value is also the physical address that satp must
    // receive after dropping the 4 KiB page offset.
    //
    // RV32 uses Sv32, whose mode value is 1 in bit 31. RV64 uses Sv39, whose
    // mode value is 8 in bits [63:60]. ASID is left as zero for both cases
    // because this small NEMU port does not maintain per-address-space ASIDs.
    uintptr_t root_ppn = ((uintptr_t)pdir >> PAGE_SHIFT) & SATP_PPN_MASK;
    uintptr_t mode = SATP_MODE << SATP_MODE_SHIFT;
    asm volatile("csrw satp, %0" : : "r"(mode | root_ppn));
}

static inline uintptr_t get_satp()
{
    uintptr_t satp;
    asm volatile("csrr %0, satp" : "=r"(satp));
    // Keep the CTE contract unchanged: Context.pdir stores the root page-table
    // pointer, not the raw satp value. Masking with the active XLEN's PPN field
    // discards MODE and ASID before rebuilding the identity-mapped pointer.
    uintptr_t ppn = satp & SATP_PPN_MASK;
    return ppn << PAGE_SHIFT;
}

bool vme_init(void *(*pgalloc_f)(int), void (*pgfree_f)(void *))
{
    pgalloc_usr = pgalloc_f;
    pgfree_usr = pgfree_f;

    kas.ptr = pgalloc_f(PGSIZE);

    // The kernel address space is an identity map over NEMU's physical memory.
    // AM and the simple kernels rely on physical addresses also being valid C
    // pointers while they build page tables and touch device buffers.
    size_t i;
    for (i = 0; i < LENGTH(segments); i++)
    {
        void *va = segments[i].start;
        for (; va < segments[i].end; va += PGSIZE)
        {
            map(&kas, va, va, 0);
        }
    }

    set_satp(kas.ptr);
    vme_enable = 1;

    return true;
}

void protect(AddrSpace *as)
{
    PTE *updir = (PTE *)(pgalloc_usr(PGSIZE));
    as->ptr = updir;
    as->area = USER_SPACE;
    as->pgsize = PGSIZE;
    // map kernel space
    // Copying the kernel root table gives every user address space the same
    // machine-mode mappings. User leaf entries are added later with PTE_U.
    memcpy(updir, kas.ptr, PGSIZE);
}

void unprotect(AddrSpace *as)
{
    (void)as;
}

void __am_get_cur_as(Context *c)
{
    // mstatus.MPP describes the privilege level that was interrupted. A trap
    // from U-mode needs its current satp saved in the Context; a trap from
    // M-mode is kernel-only and uses NULL to mean "no user address space".
    uintptr_t mpp = (c->mstatus >> 11) & 0x3;
    c->pdir = (vme_enable && mpp == 0 ? (void *)get_satp() : NULL);
}

void __am_switch(Context *c)
{
    // The assembly trap return path calls this before restoring registers. satp
    // is changed only for contexts that carry a user page directory. A NULL
    // pdir therefore means "keep the currently active translation" rather than
    // "force the kernel page table".
    // This is important for kernel threads: they may be scheduled while a user
    // page table is active, and they still need that table's copied kernel
    // mappings rather than a destructive switch back to kas.

    if (vme_enable && c->pdir != NULL)
    {
        set_satp(c->pdir);
    }
}

void map(AddrSpace *as, void *va, void *pa, int prot)
{
    (void)prot;

    // ---- Basic sanity checks ----
    assert(as != NULL);
    assert(as->ptr != NULL);

    uintptr_t v = (uintptr_t)va;
    uintptr_t p = (uintptr_t)pa;

    // This implementation intentionally maps one 4 KiB page per call. It does
    // not accept arbitrary byte ranges; callers must round addresses before
    // asking AM to install the mapping. Sv32 and Sv39 both use 4 KiB base
    // pages, so virtual and physical addresses must be page-aligned.
    assert((v & PAGE_MASK) == 0);
    assert((p & PAGE_MASK) == 0);

#if __riscv_xlen == 64
    // Sv39 virtual addresses are canonical: bits [63:39] must all match bit
    // 38. The current AM/NEMU layout uses low addresses for user memory,
    // physical memory, and devices, but this assertion catches accidental Sv48
    // or host-pointer values before they become malformed PTEs.
    uintptr_t upper = v >> 39;
    assert(upper == 0 || upper == ((1ul << 25) - 1));
#endif

    // Root page-table base. With identity-mapped physical memory, converting a
    // PPN back to an address is just a shift and cast to a C pointer.
    PTE *table = (PTE *)as->ptr;

    // Walk from the root level down to the leaf level, allocating missing
    // intermediate tables. RV32/Sv32 has levels 1 and 0 with 10-bit VPN fields
    // and 1024 PTEs per page. RV64/Sv39 has levels 2, 1, and 0 with 9-bit VPN
    // fields and 512 PTEs per page, because each PTE is 8 bytes.
    for (int level = PT_LEVELS - 1; level > 0; level--)
    {
        uintptr_t vpn = (v >> (PAGE_SHIFT + level * VPN_BITS)) & VPN_MASK;
        assert(vpn < PTE_PER_PAGE);

        PTE pte = table[vpn];

        if ((pte & PTE_V) == 0)
        {
            void *new_pt_page = pgalloc_usr(PGSIZE);
            assert(new_pt_page != NULL);

            // The allocator used by nanos-lite currently returns zeroed pages,
            // but clearing here keeps map() correct if a different allocator is
            // wired in later.
            memset(new_pt_page, 0, PGSIZE);

            uintptr_t ppn = (uintptr_t)new_pt_page >> PAGE_SHIFT;

            // A non-leaf PTE is valid but has R/W/X clear. That is how the
            // hardware distinguishes a pointer to the next page table from a
            // superpage leaf. The PPN field starts at bit 10 for both Sv32 and
            // Sv39.
            table[vpn] = (PTE)((ppn << PTE_PPN_SHIFT) | PTE_V);
            pte = table[vpn];
        }
        else
        {
            // This AM code always installs 4 KiB leaves at level 0. Encountering
            // R/W/X at an upper level would mean a superpage created elsewhere,
            // which would make the single-page mapping request ambiguous.
            assert((pte & (PTE_R | PTE_W | PTE_X)) == 0);
        }

        uintptr_t next_table_base = (((uintptr_t)pte >> PTE_PPN_SHIFT) << PAGE_SHIFT);
        table = (PTE *)next_table_base;
    }

    // Build flags for leaf PTE.
    // In PA, we do not implement fine-grained protection,
    // so we can set R/W/X all to 1 to avoid permission faults.
    // Also set A and D to 1, avoiding hardware A/D update complexity.
    uintptr_t flags = PTE_V | PTE_R | PTE_W | PTE_X | PTE_A | PTE_D;

    // Mark user pages with U=1, kernel pages keep U=0.
    // User space is as->area, protect() sets it to USER_SPACE.

    if (va >= as->area.start && va < as->area.end)
    {
        // User pages must carry PTE_U so mret into U-mode can fetch and touch
        // them. Copied kernel mappings intentionally omit PTE_U and remain
        // machine-mode only.
        flags |= PTE_U;
    }

    uintptr_t leaf_vpn = (v >> PAGE_SHIFT) & VPN_MASK;
    uintptr_t leaf_ppn = p >> PAGE_SHIFT;
    table[leaf_vpn] = (PTE)((leaf_ppn << PTE_PPN_SHIFT) | (flags & PTE_FLAG_MASK));
}

Context *ucontext(AddrSpace *as, Area kstack, void *entry)
{
    // ucontext creates the first trap frame for a user task. It is not executing
    // in user mode yet; mret from the trap return path will consume this frame
    // and enter the requested entry address with satp set to as->ptr.
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

    c->pdir = as->ptr; // Use this address space's page table root
    c->ksp = kstack.end;
    // np is consumed only by trap.S. It says the next mret enters user mode,
    // therefore mscratch must be armed with this context's kernel stack top.
    c->np = 1;

    return c;
}
