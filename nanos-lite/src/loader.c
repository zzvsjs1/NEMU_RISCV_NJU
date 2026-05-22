#include <proc.h>
#include <elf.h>
#include "debug.h"
#include "fs.h"
#include "pagewalk.h"

#ifdef __LP64__
#define Elf_Ehdr Elf64_Ehdr
#define Elf_Phdr Elf64_Phdr
#else
#define Elf_Ehdr Elf32_Ehdr
#define Elf_Phdr Elf32_Phdr
#endif

#if defined(__ISA_X86__)
#define EXPECT_TYPE EM_386
#elif defined(__ISA_MIPS32__)
#define EXPECT_TYPE EF_MIPS_ARCH_32
#elif defined(__ISA_RISCV32__) || defined(__ISA_RISCV32E__) || defined(__ISA_RISCV64__)
#define EXPECT_TYPE EM_RISCV
#else
#error unsupported ISA __ISA__
// # define EXPECT_TYPE EM_X86_64
#endif

// Provided by Nanos-lite memory system.
extern Area heap;

enum
{
    USTACK_PAGES = 8
};

static uintptr_t align_down(uintptr_t x, uintptr_t a)
{
    return x & ~(a - 1);
}

static uintptr_t align_up(uintptr_t x, uintptr_t a)
{
    return (x + a - 1) & ~(a - 1);
}

static int elf_load_prot(uint32_t flags)
{
    return MMAP_READ | ((flags & PF_W) ? MMAP_WRITE : 0);
}

static void load_direct(int fd, const Elf_Phdr *phdr)
{
    uintptr_t seg_va = (uintptr_t)phdr->p_vaddr;

    assert(phdr->p_filesz <= phdr->p_memsz);
    assert(fs_lseek(fd, phdr->p_offset, SEEK_SET) != (size_t)-1);
    assert(fs_read(fd, (void *)seg_va, phdr->p_filesz) == phdr->p_filesz);

    if (phdr->p_memsz > phdr->p_filesz)
    {
        memset((void *)(seg_va + phdr->p_filesz), 0, phdr->p_memsz - phdr->p_filesz);
    }
}

static uintptr_t load_mapped(PCB *pcb, int fd, const Elf_Phdr *phdr, uintptr_t max_end)
{
    uintptr_t seg_va = (uintptr_t)phdr->p_vaddr;
    uintptr_t file_va_end = seg_va + phdr->p_filesz;
    uintptr_t mem_va_end = seg_va + phdr->p_memsz;
    int prot = elf_load_prot(phdr->p_flags);

    assert(phdr->p_filesz <= phdr->p_memsz);

    // Track the maximum end address of all loadable segments.
    if (mem_va_end > max_end)
    {
        max_end = mem_va_end;
    }

    uintptr_t page_va_begin = align_down(seg_va, PGSIZE);
    uintptr_t page_va_end = align_up(mem_va_end, PGSIZE);

    for (uintptr_t page_va = page_va_begin; page_va < page_va_end; page_va += PGSIZE)
    {
        // Reuse an existing mapping if this page VA was already mapped by another segment.
        // ELF segments can share a page at their boundary; allocating a
        // fresh page here would lose bytes copied for the previous segment.
        void *page_pa = nanos_pagewalk_lookup_page(pcb->as.ptr, page_va);

        if (page_pa == NULL)
        {
            page_pa = new_page(1);
            assert(page_pa != NULL);
            memset(page_pa, 0, PGSIZE);
            map(&pcb->as, (void *)page_va, page_pa, prot);
        }
#if defined(__ISA_X86__)
        else if (prot & MMAP_WRITE)
        {
            /*
             * Adjacent ELF segments can share one boundary page.  If a
             * read-only segment mapped it first, a later writable segment must
             * upgrade the same physical page rather than allocate a new one.
             */
            map(&pcb->as, (void *)page_va, page_pa, MMAP_NONE);
            map(&pcb->as, (void *)page_va, page_pa, MMAP_READ | MMAP_WRITE);
        }
#endif

        // Load file bytes that overlap with this page.
        uintptr_t page_begin = page_va;
        uintptr_t page_end = page_va + PGSIZE;

        uintptr_t is = (page_begin > seg_va) ? page_begin : seg_va;
        uintptr_t ie = (page_end < file_va_end) ? page_end : file_va_end;

        if (is < ie)
        {
            size_t bytes = (size_t)(ie - is);
            size_t inpage_off = (size_t)(is - page_begin);
            size_t file_off = (size_t)phdr->p_offset + (size_t)(is - seg_va);

            assert(fs_lseek(fd, file_off, SEEK_SET) != (size_t)-1);
            assert(fs_read(fd, (void *)((uintptr_t)page_pa + inpage_off), bytes) == bytes);
        }
    }

    return max_end;
}

static uintptr_t loader(PCB *pcb, const char *filename)
{
    Log("Load exec filename = %s", filename);

    const int fd = fs_open(filename, 0, 0);

    assert(fd >= 0);

    // Read header.
    Elf_Ehdr elfH;
    assert(fs_read(fd, &elfH, sizeof(Elf_Ehdr)) == sizeof(Elf_Ehdr));

    // Check header.
    assert(memcmp(elfH.e_ident, ELFMAG, SELFMAG) == 0);
    assert(elfH.e_machine == EXPECT_TYPE);
    assert(elfH.e_type == ET_EXEC);
    assert(elfH.e_phoff != 0);
    assert(elfH.e_phentsize == sizeof(Elf_Phdr));
    assert(elfH.e_phnum != 0);

    uintptr_t max_end = 0;

    for (int i = 0; i < (int)elfH.e_phnum; i++)
    {
        Elf_Phdr phdr;
        size_t phdrOffset = elfH.e_phoff + i * elfH.e_phentsize;

        assert(fs_lseek(fd, phdrOffset, SEEK_SET) != (size_t)-1);
        assert(fs_read(fd, &phdr, elfH.e_phentsize) == elfH.e_phentsize);

        if (phdr.p_type != PT_LOAD)
        {
            continue;
        }

        if (pcb == NULL)
        {
            load_direct(fd, &phdr);
        }
        else
        {
            max_end = load_mapped(pcb, fd, &phdr, max_end);
        }
    }

    // Close fd.
    assert(fs_close(fd) == 0);

    if (pcb != NULL)
    {
        // Initialise max_brk to the end of loaded image, with a lower bound of user space start.
        uintptr_t us = (uintptr_t)pcb->as.area.start;
        pcb->max_brk = (max_end > us) ? max_end : us;
    }

    // Return entry point.
    return elfH.e_entry;
}

uintptr_t build_user_stack(uintptr_t ustack_va_end, uintptr_t ustack_pa_end, char *const argv[], char *const envp[])
{
    int argc = 0;
    int envc = 0;

    if (argv != NULL)
    {
        while (argv[argc] != NULL)
            argc++;
    }

    if (envp != NULL)
    {
        while (envp[envc] != NULL)
            envc++;
    }

    // Keep small fixed limits for simplicity in this program.
    char *argv_ptrs[64];
    char *envp_ptrs[64];
    assert(argc < 64 && envc < 64);

    uintptr_t sp_va = ustack_va_end;
    uintptr_t sp_pa = ustack_pa_end;

    // Copy strings into the stack memory.
    // Writes go to physical addresses because the kernel is building another
    // address space. The saved argv/envp pointers must be user virtual
    // addresses, because crt0 will dereference them after trap return in that
    // process address space.
    for (int i = 0; i < argc; i++)
    {
        const size_t len = strlen(argv[i]) + 1;
        sp_va -= len;
        sp_pa -= len;
        memcpy((void *)sp_pa, argv[i], len);
        argv_ptrs[i] = (char *)sp_va;
    }

    for (int i = 0; i < envc; i++)
    {
        const size_t len = strlen(envp[i]) + 1;
        sp_va -= len;
        sp_pa -= len;
        memcpy((void *)sp_pa, envp[i], len);
        envp_ptrs[i] = (char *)sp_va;
    }

    // Align for pushing uintptr_t values.
    sp_va = align_down(sp_va, sizeof(uintptr_t));
    sp_pa = align_down(sp_pa, sizeof(uintptr_t));

#define PUSH_U(v) \
    do \
    { \
        sp_va -= sizeof(uintptr_t); \
        sp_pa -= sizeof(uintptr_t); \
        *(uintptr_t *)sp_pa = (uintptr_t)(v); \
    } while (0)

    // envp NULL terminator
    PUSH_U(0);
    for (int i = envc - 1; i >= 0; i--)
        PUSH_U(envp_ptrs[i]);

    // argv NULL terminator
    PUSH_U(0);
    for (int i = argc - 1; i >= 0; i--)
        PUSH_U(argv_ptrs[i]);

    // argc at the bottom
    PUSH_U((uintptr_t)argc);

#undef PUSH_U

    // Return the user virtual address of argc, this is the initial user SP as well.
    return sp_va;
}

void context_uload(PCB *pcb, const char *filename, char *const argv[], char *const envp[])
{
    // Ensure envp is not NULL.
    static char *const empty_envp[] = {NULL};

    if (envp == NULL)
    {
        envp = empty_envp;
    }

    // 1) Create user address space and copy kernel mappings.
    protect(&pcb->as);

    // 2) Load program image by page mapping.
    uintptr_t entry = loader(pcb, filename);

    // 3) Allocate and map user stack, 32KB = 8 pages.
    uintptr_t ustack_va_end = (uintptr_t)pcb->as.area.end;
    uintptr_t ustack_va_base = ustack_va_end - (uintptr_t)USTACK_PAGES * PGSIZE;

    void *ustack_pa_base = new_page(USTACK_PAGES);
    assert(ustack_pa_base != NULL);
    memset(ustack_pa_base, 0, (size_t)USTACK_PAGES * PGSIZE);

    for (int i = 0; i < USTACK_PAGES; i++)
    {
        map(
            &pcb->as,
            (void *)(ustack_va_base + (uintptr_t)i * PGSIZE),
            (void *)((uintptr_t)ustack_pa_base + (uintptr_t)i * PGSIZE),
            MMAP_WRITE);
    }

    // 4) Build argc/argv/envp on the stack.
    uintptr_t ustack_pa_end = (uintptr_t)ustack_pa_base + (uintptr_t)USTACK_PAGES * PGSIZE;
    uintptr_t args_va = build_user_stack(ustack_va_end, ustack_pa_end, argv, envp);

    // 5) Create user context on kernel stack.
    // The Context itself must live on the PCB kernel stack, not on the user
    // stack. trap.S and the scheduler need to read it while still running on a
    // kernel mapping, before the process address space is restored.
    Area kstack = (Area){.start = pcb->stack, .end = pcb + 1};
    pcb->cp = ucontext(&pcb->as, kstack, (void *)entry);

    // 6) Set user initial SP and the ABI argument pointer.
    /*
     * Only the actively supported PA4 targets expose GPRSP in their AM Context
     * headers. Other historical NEMU ISA headers can still syntax-check this
     * file, but they do not have enough ABI information here to initialise a
     * user stack pointer.
     */
#if defined(__ISA_X86__) || defined(__ISA_RISCV32__) || defined(__ISA_RISCV32E__) || defined(__ISA_RISCV64__)
    pcb->cp->GPRSP = args_va;
#endif
    pcb->cp->GPRx = args_va;
}

void naive_uload(PCB *pcb, const char *filename)
{
    uintptr_t entry = loader(pcb, filename);
    Log("Jump to entry = %p", (void *)entry);
    ((void (*)())entry)();
}
