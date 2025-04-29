#include <proc.h>
#include <elf.h>
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

void naive_uload(PCB *pcb, const char *filename) 
{
  uintptr_t entry = loader(pcb, filename);
  Log("Jump to entry = %p", entry);
  ((void(*)())entry) ();
}

