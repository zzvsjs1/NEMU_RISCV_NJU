#include <proc.h>
#include <elf.h>
#include "debug.h"
#include "fs.h"
#include "loader_checks.h"
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
#define EXPECT_TYPE EM_MIPS
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
    USTACK_PAGES = 8,
    USTACK_VECTOR_SLOTS = 64,
};

static uintptr_t align_down(uintptr_t x, uintptr_t a)
{
    return x & ~(a - 1);
}

static uintptr_t align_up(uintptr_t x, uintptr_t a)
{
    assert(a != 0 && (a & (a - 1)) == 0);
    assert(x <= UINTPTR_MAX - (a - 1));
    return (x + a - 1) & ~(a - 1);
}

/*
 * Keep the stack reservation in one place.  The ELF loader and the stack
 * mapper must agree on this boundary or a PT_LOAD page could be mistaken for
 * an already allocated stack or kernel mapping.
 */
static uintptr_t user_stack_base(const AddrSpace *as)
{
    const uintptr_t user_start = (uintptr_t)as->area.start;
    const uintptr_t user_end = (uintptr_t)as->area.end;
    const uintptr_t stack_bytes = (uintptr_t)USTACK_PAGES * PGSIZE;

    assert(user_end >= user_start);
    assert(stack_bytes <= user_end - user_start);
    assert((user_start & (PGSIZE - 1)) == 0);
    assert((user_end & (PGSIZE - 1)) == 0);

    return user_end - stack_bytes;
}

static int stack_vector_count(char *const vector[])
{
    int count = 0;

    if (vector == NULL)
    {
        return 0;
    }

    while (vector[count] != NULL)
    {
        /* The final array slot is reserved for the vector terminator. */
        assert(count < USTACK_VECTOR_SLOTS - 1);
        count++;
    }

    return count;
}

static size_t stack_string_bytes(char *const vector[], int count)
{
    size_t total = 0;

    for (int i = 0; i < count; i++)
    {
        size_t length = strlen(vector[i]);

        assert(length < (size_t)-1);
        length++;
        assert(length <= (size_t)-1 - total);
        total += length;
    }

    return total;
}

static size_t checked_add_size(size_t left, size_t right)
{
    assert(right <= (size_t)-1 - left);
    return left + right;
}

/*
 * The preflight check catches oversized argument vectors, while this helper
 * makes each later subtraction safe if the stack layout changes in future.
 */
static void reserve_stack_bytes(uintptr_t *sp_va, uintptr_t va_base,
                                uintptr_t *sp_pa, uintptr_t pa_base,
                                size_t bytes)
{
    const uintptr_t amount = (uintptr_t)bytes;

    assert((size_t)amount == bytes);
    assert(*sp_va >= va_base && *sp_pa >= pa_base);
    assert(amount <= *sp_va - va_base);
    assert(amount <= *sp_pa - pa_base);

    *sp_va -= amount;
    *sp_pa -= amount;
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

static uintptr_t load_mapped(PCB *pcb, int fd, const Elf_Phdr *phdr,
                             uintptr_t max_end, uintptr_t stack_va_base,
                             uintptr_t entry, bool *entry_mapped)
{
    const uintptr_t user_va_base = (uintptr_t)pcb->as.area.start;
    const uintptr_t seg_va = (uintptr_t)phdr->p_vaddr;
    uintptr_t file_va_end;
    uintptr_t mem_va_end;
    const int prot = elf_load_prot(phdr->p_flags);

    assert(phdr->p_filesz <= phdr->p_memsz);

    /* A zero-sized load segment has no image bytes or address-space effect. */
    if (phdr->p_memsz == 0)
    {
        return max_end;
    }

    /*
     * protect() copies kernel mappings into pcb->as.  Validate this range
     * before page-table lookup so an ELF header can never make the loader
     * reuse and overwrite one of those inherited kernel pages.
     */
    assert(nanos_loader_load_range_fits(user_va_base, (uintptr_t)pcb->as.area.end,
                                        (size_t)USTACK_PAGES * PGSIZE, seg_va,
                                        (uintptr_t)phdr->p_memsz, &mem_va_end));
    assert(mem_va_end <= stack_va_base);
    assert(nanos_loader_checked_add_uintptr(seg_va, (uintptr_t)phdr->p_filesz,
                                            &file_va_end));

    assert(entry_mapped != NULL);
    if ((phdr->p_flags & PF_X) != 0 && entry >= seg_va && entry < mem_va_end)
    {
        *entry_mapped = true;
    }

    // Track the maximum end address of all loadable segments.
    if (mem_va_end > max_end)
    {
        max_end = mem_va_end;
    }

    uintptr_t page_va_begin = align_down(seg_va, PGSIZE);
    uintptr_t page_va_end = align_up(mem_va_end, PGSIZE);

    assert(page_va_begin >= user_va_base);
    assert(page_va_end <= stack_va_base);

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
    uintptr_t stack_va_base = 0;
    bool entry_mapped = false;

    if (pcb != NULL)
    {
        const uintptr_t entry = (uintptr_t)elfH.e_entry;
        const uintptr_t user_va_base = (uintptr_t)pcb->as.area.start;

        stack_va_base = user_stack_base(&pcb->as);
        assert(entry >= user_va_base && entry < stack_va_base);
    }

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
            max_end = load_mapped(pcb, fd, &phdr, max_end, stack_va_base,
                                  (uintptr_t)elfH.e_entry, &entry_mapped);
        }
    }

    // Close fd.
    assert(fs_close(fd) == 0);

    if (pcb != NULL)
    {
        assert(entry_mapped);
        assert(max_end <= stack_va_base);

        // Initialise max_brk to the end of loaded image, with a lower bound of user space start.
        uintptr_t us = (uintptr_t)pcb->as.area.start;
        pcb->max_brk = (max_end > us) ? max_end : us;
    }

    // Return entry point.
    return elfH.e_entry;
}

static uintptr_t build_user_stack(uintptr_t ustack_va_base, uintptr_t ustack_va_end,
                                  uintptr_t ustack_pa_base, uintptr_t ustack_pa_end,
                                  char *const argv[], char *const envp[])
{
    char *argv_ptrs[USTACK_VECTOR_SLOTS];
    char *envp_ptrs[USTACK_VECTOR_SLOTS];
    const int argc = stack_vector_count(argv);
    const int envc = stack_vector_count(envp);
    const size_t argv_bytes = stack_string_bytes(argv, argc);
    const size_t envp_bytes = stack_string_bytes(envp, envc);
    const size_t string_bytes = checked_add_size(argv_bytes, envp_bytes);
    const size_t pointer_words = checked_add_size(
        checked_add_size((size_t)argc, (size_t)envc), 3u);
    uintptr_t expected_sp_va;
    uintptr_t expected_sp_pa;

    /*
     * Account for both NULL terminators and argc in pointer_words.  This must
     * happen before copying strings: the physical stack sits after earlier
     * bump allocations, so writing below its base would corrupt them.
     */
    assert(nanos_loader_stack_layout_fits(ustack_va_base, ustack_va_end,
                                          string_bytes, pointer_words,
                                          &expected_sp_va));
    assert(nanos_loader_stack_layout_fits(ustack_pa_base, ustack_pa_end,
                                          string_bytes, pointer_words,
                                          &expected_sp_pa));

    uintptr_t sp_va = ustack_va_end;
    uintptr_t sp_pa = ustack_pa_end;

    // Copy strings into the stack memory.
    // Writes go to physical addresses because the kernel is building another
    // address space. The saved argv/envp pointers must be user virtual
    // addresses, because crt0 will dereference them after trap return in that
    // process address space.
    for (int i = 0; i < argc; i++)
    {
        size_t len = strlen(argv[i]);

        assert(len < (size_t)-1);
        len++;
        reserve_stack_bytes(&sp_va, ustack_va_base, &sp_pa, ustack_pa_base, len);
        memcpy((void *)sp_pa, argv[i], len);
        argv_ptrs[i] = (char *)sp_va;
    }

    for (int i = 0; i < envc; i++)
    {
        size_t len = strlen(envp[i]);

        assert(len < (size_t)-1);
        len++;
        reserve_stack_bytes(&sp_va, ustack_va_base, &sp_pa, ustack_pa_base, len);
        memcpy((void *)sp_pa, envp[i], len);
        envp_ptrs[i] = (char *)sp_va;
    }

    // Align for pushing uintptr_t values.
    sp_va = align_down(sp_va, sizeof(uintptr_t));
    sp_pa = align_down(sp_pa, sizeof(uintptr_t));
    assert(sp_va >= ustack_va_base && sp_pa >= ustack_pa_base);

#define PUSH_U(v) \
    do \
    { \
        reserve_stack_bytes(&sp_va, ustack_va_base, &sp_pa, ustack_pa_base, sizeof(uintptr_t)); \
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

    assert(sp_va == expected_sp_va && sp_pa == expected_sp_pa);

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
    uintptr_t ustack_va_base = user_stack_base(&pcb->as);

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
    uintptr_t args_va = build_user_stack(ustack_va_base, ustack_va_end,
                                         (uintptr_t)ustack_pa_base, ustack_pa_end,
                                         argv, envp);

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
