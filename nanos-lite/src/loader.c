#include <proc.h>
#include <elf.h>
#include "debug.h"
#include "fs.h"

#ifdef __LP64__
# define Elf_Ehdr Elf64_Ehdr
# define Elf_Phdr Elf64_Phdr
#else
# define Elf_Ehdr Elf32_Ehdr
# define Elf_Phdr Elf32_Phdr
#endif

#if defined(__ISA_X86__)
# define EXPECT_TYPE EM_X86_64
#elif defined(__ISA_MIPS32__)
# define EXPECT_TYPE EF_MIPS_ARCH_32
#elif defined(__ISA_RISCV32__) || defined(__ISA_RISCV64__)
# define EXPECT_TYPE EM_RISCV
#else
# error unsupported ISA __ISA__
// # define EXPECT_TYPE EM_X86_64
#endif

// Provided by Nanos-lite memory system.
extern Area heap;

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

    for (int i = 0; i < (int)elfH.e_phnum; ++i)
    {
        Elf_Phdr phdr;
        const size_t phdrOffset = elfH.e_phoff + i * elfH.e_phentsize;

        assert(fs_lseek(fd, phdrOffset, SEEK_SET) != (size_t)-1);
        assert(fs_read(fd, &phdr, elfH.e_phentsize) == elfH.e_phentsize);

        // If cannot be loaded.
        if (phdr.p_type != PT_LOAD)
        {
            continue;
        }

        // Bulk copy, avoid paging.
        void *dst = (void*)(uintptr_t)phdr.p_vaddr;

        // Seek to the start of this segment on disk…
        assert(fs_lseek(fd, phdr.p_offset, SEEK_SET) != (size_t)-1);

        // …then read ph.p_filesz bytes into memory
        assert(fs_read(fd, dst, phdr.p_filesz) == phdr.p_filesz);

        // Zero the BSS.
        memset(
            (void*)(phdr.p_vaddr + phdr.p_filesz),
            0,
            phdr.p_memsz - phdr.p_filesz
          );
    }

    // Close fd.
    assert(fs_close(fd) == 0);

    // Return entry point.
    return elfH.e_entry;
}

// Align down helper, works for 32-bit and 64-bit.
static inline uintptr_t align_down(uintptr_t x, uintptr_t a) 
{
  return x & ~(a - 1);
}

// Build argc/argv/envp layout on user stack.
// Return the address where argc is stored, which will be passed via GPRx.
static uintptr_t build_user_stack(uintptr_t ustack_end, char *const argv[], char *const envp[]) 
{
  // Count argc, envc. argv/envp are expected to be NULL-terminated arrays.
  int argc = 0;
  int envc = 0;

  if (argv != NULL) 
  {
    while (argv[argc] != NULL) argc++;
  }
  
  if (envp != NULL) 
  {
    while (envp[envc] != NULL) envc++;
  }

  // We need to remember the final string addresses to build argv/envp pointer arrays.
  // For simplicity, use small fixed limits here, you can enlarge if needed.
  // Alternatively, you can allocate dynamically, but PA labs usually keep it simple.
  char *argv_ptrs[64];
  char *envp_ptrs[64];
  assert(argc < 64 && envc < 64);

  uintptr_t sp = ustack_end;

  // 1) Copy strings into the "string area" at high addresses, growing downward.
  // The string order does not matter as long as argv/envp pointers are correct.
  for (int i = 0; i < argc; i++) 
  {
    size_t len = strlen(argv[i]) + 1;   // include '\0'
    sp -= len;
    memcpy((void *)sp, argv[i], len);
    argv_ptrs[i] = (char *)sp;
  }

  for (int i = 0; i < envc; i++) 
  {
    size_t len = strlen(envp[i]) + 1;
    sp -= len;
    memcpy((void *)sp, envp[i], len);
    envp_ptrs[i] = (char *)sp;
  }

  // 2) Align for pointer pushing.
  sp = align_down(sp, sizeof(uintptr_t));

  // 3) Push envp pointers, then NULL, then argv pointers, then NULL, then argc.
  // Layout from low to high (stack grows down):
  //   [argc][argv0..argvN-1][NULL][envp0..envpM-1][NULL][string area...]
  //
  // Use a PUSH macro to avoid pointer arithmetic mistakes.
#define PUSH_U(v) do {                         \
    sp -= sizeof(uintptr_t);                     \
    *(uintptr_t *)sp = (uintptr_t)(v);           \
  } while (0)

  // envp NULL terminator
  PUSH_U(0);

  // envp pointers, keep original order in envp[0..envc-1]
  for (int i = envc - 1; i >= 0; i--) 
  {
    PUSH_U(envp_ptrs[i]);
  }

  // argv NULL terminator
  PUSH_U(0);

  // argv pointers, keep original order in argv[0..argc-1]
  for (int i = argc - 1; i >= 0; i--) 
  {
    PUSH_U(argv_ptrs[i]);
  }

  // argc value at the bottom
  PUSH_U((uintptr_t)argc);

#undef PUSH_U

  // Now sp points to argc, exactly what the lab convention requires.
  return sp;
}

void context_uload(PCB *pcb, const char *filename, char *const argv[], char *const envp[]) 
{
  // 1) Allocate fresh user stack first
  void *ustack_base = new_page(8);
  uintptr_t ustack_end = (uintptr_t)ustack_base + 8 * PGSIZE;

  // 2) Ensure envp is valid
  static char *const empty_envp[] = { NULL };
  if (envp == NULL) envp = empty_envp;

  // 3) Build argc/argv/envp NOW, before loader overwrites old address space
  uintptr_t args_addr = build_user_stack(ustack_end, argv, envp);

  // 4) Load program image
  uintptr_t entry = loader(pcb, filename);

  // 5) Create user context on kernel stack
  Area kstack = (Area){ .start = pcb->stack, .end = pcb + 1 };
  pcb->cp = ucontext(&pcb->as, kstack, (void *)entry);

  // 6) Pass argc address via GPRx, Navy _start will set sp and call call_main(args)
  pcb->cp->GPRx = args_addr;
}


void naive_uload(PCB *pcb, const char *filename) 
{
  uintptr_t entry = loader(pcb, filename);
  Log("Jump to entry = %p", entry);
  ((void(*)())entry) ();
}

