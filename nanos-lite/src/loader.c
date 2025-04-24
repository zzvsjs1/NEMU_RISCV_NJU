#include <proc.h>
#include <elf.h>

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
#endif

size_t ramdisk_read(void *buf, size_t offset, size_t len);
size_t ramdisk_write(const void *buf, size_t offset, size_t len);
size_t get_ramdisk_size();

static uintptr_t loader(PCB *pcb, const char *filename) 
{
    // Read header.
    Elf_Ehdr elfH;
    ramdisk_read(&elfH, 0, sizeof(Elf_Ehdr));

    // Check header.
    assert(memcmp(elfH.e_ident, ELFMAG, SELFMAG) == 0);
    assert(elfH.e_machine == EXPECT_TYPE);
    assert(elfH.e_type == ET_EXEC);
    assert(elfH.e_phoff != 0);
    assert(elfH.e_phentsize == sizeof(Elf_Phdr));
    assert(elfH.e_phnum != 0);

    Elf_Phdr ph;

    for (int i = 0; i < (int)elfH.e_phnum; ++i)
    {
        const size_t address = elfH.e_phoff + i * elfH.e_phentsize;
        ramdisk_read(&ph, address, sizeof(Elf_Phdr));

        if (ph.p_type != PT_LOAD)
        {
            continue;
        }
        
        // Bulk copy
        void *dst = (void*)(uintptr_t)ph.p_vaddr;
        ramdisk_read(dst, ph.p_offset, ph.p_filesz);

        // Zero the BSS
        memset(
            (void*)(ph.p_vaddr + ph.p_filesz),
            0,
            ph.p_memsz - ph.p_filesz
          );
    }

    // Return entry point.
    return elfH.e_entry;
}

void naive_uload(PCB *pcb, const char *filename) 
{
  uintptr_t entry = loader(pcb, filename);
  Log("Jump to entry = %p", entry);
  ((void(*)())entry) ();
}

