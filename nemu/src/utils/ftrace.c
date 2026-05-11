#include <utils.h>

#ifdef CONFIG_FTRACE
#include <elf.h>
#include <stdio.h>

typedef struct
{
    vaddr_t start;
    vaddr_t end;
    char *name;
} FuncSymbol;

static FuncSymbol *funcs = NULL;
static size_t nr_func = 0;
static int call_depth = 0;
static bool ftrace_ready = false;

/* ELF data is read directly so ftrace does not depend on readelf/objdump. */
static bool read_at(FILE *fp, long off, void *buf, size_t size)
{
    return fseek(fp, off, SEEK_SET) == 0 && fread(buf, size, 1, fp) == 1;
}

static char *dup_name(const char *s)
{
    size_t len = strlen(s) + 1;
    char *ret = malloc(len);

    if (ret != NULL)
    {
        memcpy(ret, s, len);
    }
    return ret;
}

static void clear_symbols()
{
    for (size_t i = 0; i < nr_func; i++)
    {
        free(funcs[i].name);
    }
    free(funcs);
    funcs = NULL;
    nr_func = 0;
    call_depth = 0;
    ftrace_ready = false;
}

static bool add_symbol(vaddr_t start, vaddr_t size, const char *name)
{
    FuncSymbol *new_funcs = realloc(funcs, (nr_func + 1) * sizeof(*funcs));

    if (new_funcs == NULL)
    {
        return false;
    }

    funcs = new_funcs;
    funcs[nr_func].start = start;
    /* Some toolchains emit zero-sized FUNC symbols; keep a one-byte lookup range. */
    funcs[nr_func].end = start + (size == 0 ? 1 : size);
    funcs[nr_func].name = dup_name(name);

    if (funcs[nr_func].name == NULL)
    {
        return false;
    }
    nr_func++;
    return true;
}

static const char *find_func(vaddr_t addr, vaddr_t *entry)
{
    for (size_t i = 0; i < nr_func; i++)
    {
        if (addr >= funcs[i].start && addr < funcs[i].end)
        {
            if (entry != NULL)
            {
                *entry = funcs[i].start;
            }
            return funcs[i].name;
        }
    }

    if (entry != NULL)
    {
        *entry = addr;
    }
    return "???";
}

static bool load_elf32(FILE *fp, const Elf32_Ehdr *eh)
{
    Elf32_Shdr *shdrs = malloc(eh->e_shnum * sizeof(*shdrs));

    if (shdrs == NULL)
    {
        return false;
    }

    bool ok = read_at(fp, eh->e_shoff, shdrs, eh->e_shnum * sizeof(*shdrs));
    for (int i = 0; ok && i < eh->e_shnum; i++)
    {
        /* .symtab points at its string table through sh_link. */

        if (shdrs[i].sh_type != SHT_SYMTAB)
        {
            continue;
        }

        Elf32_Shdr symtab = shdrs[i];
        Elf32_Shdr strtab = shdrs[symtab.sh_link];
        char *strs = malloc(strtab.sh_size);
        Elf32_Sym *syms = malloc(symtab.sh_size);
        ok = strs != NULL && syms != NULL &&
             read_at(fp, strtab.sh_offset, strs, strtab.sh_size) &&
             read_at(fp, symtab.sh_offset, syms, symtab.sh_size);

        size_t n = symtab.sh_size / sizeof(Elf32_Sym);
        for (size_t j = 0; ok && j < n; j++)
        {
            /* ftrace only needs named function ranges, not objects or section symbols. */

            if (ELF32_ST_TYPE(syms[j].st_info) == STT_FUNC &&
                syms[j].st_name < strtab.sh_size &&
                syms[j].st_value != 0)
            {
                ok = add_symbol(syms[j].st_value, syms[j].st_size, strs + syms[j].st_name);
            }
        }

        free(strs);
        free(syms);
    }

    free(shdrs);
    return ok;
}

static bool load_elf64(FILE *fp, const Elf64_Ehdr *eh)
{
    Elf64_Shdr *shdrs = malloc(eh->e_shnum * sizeof(*shdrs));

    if (shdrs == NULL)
    {
        return false;
    }

    bool ok = read_at(fp, eh->e_shoff, shdrs, eh->e_shnum * sizeof(*shdrs));
    for (int i = 0; ok && i < eh->e_shnum; i++)
    {
        /* .symtab points at its string table through sh_link. */

        if (shdrs[i].sh_type != SHT_SYMTAB)
        {
            continue;
        }

        Elf64_Shdr symtab = shdrs[i];
        Elf64_Shdr strtab = shdrs[symtab.sh_link];
        char *strs = malloc(strtab.sh_size);
        Elf64_Sym *syms = malloc(symtab.sh_size);
        ok = strs != NULL && syms != NULL &&
             read_at(fp, strtab.sh_offset, strs, strtab.sh_size) &&
             read_at(fp, symtab.sh_offset, syms, symtab.sh_size);

        size_t n = symtab.sh_size / sizeof(Elf64_Sym);
        for (size_t j = 0; ok && j < n; j++)
        {
            /* ftrace only needs named function ranges, not objects or section symbols. */

            if (ELF64_ST_TYPE(syms[j].st_info) == STT_FUNC &&
                syms[j].st_name < strtab.sh_size &&
                syms[j].st_value != 0)
            {
                ok = add_symbol(syms[j].st_value, syms[j].st_size, strs + syms[j].st_name);
            }
        }

        free(strs);
        free(syms);
    }

    free(shdrs);
    return ok;
}

void ftrace_init(const char *elf_file)
{
    clear_symbols();

    if (elf_file == NULL)
    {
        return;
    }

    FILE *fp = fopen(elf_file, "rb");

    if (fp == NULL)
    {
        perror("ftrace elf");
        return;
    }

    unsigned char ident[EI_NIDENT];
    /* Validate the ELF magic before reading class-specific headers. */
    bool ok = read_at(fp, 0, ident, sizeof(ident)) &&
              ident[EI_MAG0] == ELFMAG0 &&
              ident[EI_MAG1] == ELFMAG1 &&
              ident[EI_MAG2] == ELFMAG2 &&
              ident[EI_MAG3] == ELFMAG3;

    if (ok && ident[EI_CLASS] == ELFCLASS32)
    {
        Elf32_Ehdr eh;
        ok = read_at(fp, 0, &eh, sizeof(eh)) && load_elf32(fp, &eh);
    }
    else if (ok && ident[EI_CLASS] == ELFCLASS64)
    {
        Elf64_Ehdr eh;
        ok = read_at(fp, 0, &eh, sizeof(eh)) && load_elf64(fp, &eh);
    }
    else
    {
        ok = false;
    }

    fclose(fp);
    ftrace_ready = ok && nr_func > 0;
    Log("ftrace loaded %zu function symbols from %s", nr_func, elf_file);
}

void ftrace_call(vaddr_t pc, vaddr_t target)
{
    if (!ftrace_ready)
    {
        return;
    }

    vaddr_t entry;
    const char *name = find_func(target, &entry);
    /* Indentation mirrors dynamic call depth, which is independent of symbol nesting. */
    log_write(FMT_WORD ": %*scall [%s@" FMT_WORD "]\n", pc, call_depth * 2, "", name, entry);
    call_depth++;
}

void ftrace_ret(vaddr_t pc)
{
    if (!ftrace_ready)
    {
        return;
    }

    if (call_depth > 0)
    {
        call_depth--;
    }

    vaddr_t entry;
    const char *name = find_func(pc, &entry);
    /* Return attribution uses the current pc because the target is in the link register. */
    log_write(FMT_WORD ": %*sret  [%s]\n", pc, call_depth * 2, "", name);
}
#else
void ftrace_init(const char *elf_file) {}
void ftrace_call(vaddr_t pc, vaddr_t target) {}
void ftrace_ret(vaddr_t pc) {}
#endif
