#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Compile loader.c in an ELF32 MIPS configuration on the host.  The mocked
 * AM and filesystem calls let this test exercise the actual static loader
 * functions without requiring the unavailable MIPS cross compiler.
 *
 * Assertions still call halt(), but logging is silent so the intentional
 * oversized-stack assertion does not look like a suite failure.
 */
#define __DEBUG_H__
#define Log(...) ((void)0)
#ifdef assert
#undef assert
#endif
#define assert(condition) \
    do \
    { \
        if (!(condition)) \
        { \
            halt(1); \
        } \
    } while (0)

#include "../../src/loader.c"

#define CHECK(condition) \
    do \
    { \
        if (!(condition)) \
        { \
            printf("check failed at line %d: %s\n", __LINE__, #condition); \
            return 1; \
        } \
    } while (0)

enum
{
    TEST_PAGE_SIZE = 4096u,
    TEST_STACK_PAGES = 8u,
    TEST_STACK_BYTES = TEST_STACK_PAGES * TEST_PAGE_SIZE,
    TEST_GUARD_BYTES = 64u,
    TEST_FD = 7,
};

static const uintptr_t test_user_start = 0x40000000u;
static const uintptr_t test_user_end = 0x80000000u;

static uint8_t elf_file[sizeof(Elf32_Ehdr) + sizeof(Elf32_Phdr)];
static size_t elf_file_size;
static size_t elf_file_offset;
static uint8_t allocated_page[TEST_PAGE_SIZE] __attribute__((aligned(TEST_PAGE_SIZE)));
static int new_page_calls;
static int map_calls;
static int read_calls;
static void *last_map_va;
static void *last_map_pa;
static Context test_context;
static jmp_buf halt_env;
static int expect_halt;

Area heap;

void halt(int code)
{
    if (expect_halt)
    {
        longjmp(halt_env, code == 0 ? 1 : code);
    }

    exit(code);
}

void putch(char ch)
{
    (void)ch;
}

void *new_page(size_t nr_page)
{
    if (nr_page != 1)
    {
        printf("unexpected multi-page allocation in loader test\n");
        exit(1);
    }

    new_page_calls++;
    return allocated_page;
}

void protect(AddrSpace *as)
{
    (void)as;
}

void map(AddrSpace *as, void *va, void *pa, int prot)
{
    (void)as;
    (void)prot;
    map_calls++;
    last_map_va = va;
    last_map_pa = pa;
}

Context *ucontext(AddrSpace *as, Area kstack, void *entry)
{
    (void)as;
    (void)kstack;
    (void)entry;
    return &test_context;
}

void *nanos_pagewalk_lookup_page(void *root, uintptr_t vaddr)
{
    (void)root;
    (void)vaddr;
    return NULL;
}

int fs_open(const char *pathname, int flags, int mode)
{
    (void)pathname;
    (void)flags;
    (void)mode;
    elf_file_offset = 0;
    return TEST_FD;
}

size_t fs_read(int fd, void *buf, size_t len)
{
    if (fd != TEST_FD || len > elf_file_size - elf_file_offset)
    {
        return (size_t)-1;
    }

    memcpy(buf, elf_file + elf_file_offset, len);
    elf_file_offset += len;
    read_calls++;
    return len;
}

size_t fs_lseek(int fd, size_t offset, int whence)
{
    if (fd != TEST_FD || whence != SEEK_SET || offset > elf_file_size)
    {
        return (size_t)-1;
    }

    elf_file_offset = offset;
    return offset;
}

int fs_close(int fd)
{
    return fd == TEST_FD ? 0 : -1;
}

static void reset_mock_state(void)
{
    memset(allocated_page, 0, sizeof(allocated_page));
    new_page_calls = 0;
    map_calls = 0;
    read_calls = 0;
    last_map_va = NULL;
    last_map_pa = NULL;
}

static void prepare_mips_elf(void)
{
    Elf32_Ehdr *header = (Elf32_Ehdr *)elf_file;
    Elf32_Phdr *program_header = (Elf32_Phdr *)(elf_file + sizeof(*header));

    memset(elf_file, 0, sizeof(elf_file));
    memcpy(header->e_ident, ELFMAG, SELFMAG);
    header->e_machine = EM_MIPS;
    header->e_type = ET_EXEC;
    header->e_phoff = sizeof(*header);
    header->e_phentsize = sizeof(*program_header);
    header->e_phnum = 1;
    header->e_entry = (Elf32_Addr)(test_user_start + TEST_PAGE_SIZE);

    program_header->p_type = PT_LOAD;
    program_header->p_flags = PF_X | PF_R;
    program_header->p_offset = sizeof(elf_file);
    program_header->p_vaddr = (Elf32_Addr)(test_user_start + TEST_PAGE_SIZE);
    program_header->p_filesz = 0;
    program_header->p_memsz = 1;

    elf_file_size = sizeof(elf_file);
}

static int test_mips_loader_accepts_em_mips(void)
{
    PCB pcb = {0};

    _Static_assert(EXPECT_TYPE == EM_MIPS, "MIPS ELF e_machine must use EM_MIPS rather than e_flags");

    prepare_mips_elf();
    reset_mock_state();
    pcb.as.ptr = allocated_page;
    pcb.as.area.start = (void *)test_user_start;
    pcb.as.area.end = (void *)test_user_end;

    CHECK(loader(&pcb, "fixture") == test_user_start + TEST_PAGE_SIZE);
    CHECK(new_page_calls == 1);
    CHECK(map_calls == 1);
    CHECK(read_calls == 2);
    CHECK((uintptr_t)last_map_va == test_user_start + TEST_PAGE_SIZE);
    CHECK(last_map_pa == allocated_page);

    return 0;
}

static int test_stack_preflight_halts_before_any_copy(void)
{
    static uint8_t stack_backing[TEST_GUARD_BYTES + TEST_STACK_BYTES];
    static char oversized_argv[TEST_STACK_BYTES / 2u];
    static char oversized_envp[TEST_STACK_BYTES / 2u];
    char *const argv[] = {oversized_argv, NULL};
    char *const envp[] = {oversized_envp, NULL};
    const uintptr_t va_base = test_user_end - TEST_STACK_BYTES;
    const uintptr_t va_end = test_user_end;
    const uintptr_t pa_base = (uintptr_t)stack_backing + TEST_GUARD_BYTES;
    const uintptr_t pa_end = pa_base + TEST_STACK_BYTES;

    memset(stack_backing, 0xa5, sizeof(stack_backing));
    memset(oversized_argv, 'a', sizeof(oversized_argv) - 1u);
    oversized_argv[sizeof(oversized_argv) - 1u] = '\0';
    memset(oversized_envp, 'e', sizeof(oversized_envp) - 1u);
    oversized_envp[sizeof(oversized_envp) - 1u] = '\0';

    expect_halt = 1;

    if (setjmp(halt_env) == 0)
    {
        (void)build_user_stack(va_base, va_end, pa_base, pa_end, argv, envp);
        expect_halt = 0;
        CHECK(false);
    }

    expect_halt = 0;

    for (size_t i = 0; i < sizeof(stack_backing); i++)
    {
        CHECK(stack_backing[i] == 0xa5);
    }

    return 0;
}

static int test_stack_exact_fit_preserves_the_abi_layout(void)
{
    static uint8_t stack_backing[TEST_STACK_BYTES];
    static char exact_arg[TEST_STACK_BYTES];
    char *const argv[] = {exact_arg, NULL};
    char *const envp[] = {NULL};
    const size_t pointer_words = 4u;
    const size_t string_bytes = TEST_STACK_BYTES - pointer_words * sizeof(uintptr_t);
    const uintptr_t va_base = test_user_end - TEST_STACK_BYTES;
    const uintptr_t va_end = test_user_end;
    const uintptr_t pa_base = (uintptr_t)stack_backing;
    const uintptr_t pa_end = pa_base + TEST_STACK_BYTES;
    const uintptr_t expected_string_va = va_base + pointer_words * sizeof(uintptr_t);
    uintptr_t sp;

    memset(stack_backing, 0, sizeof(stack_backing));
    memset(exact_arg, 'v', string_bytes - 1u);
    exact_arg[string_bytes - 1u] = '\0';

    sp = build_user_stack(va_base, va_end, pa_base, pa_end, argv, envp);

    CHECK(sp == va_base);
    CHECK(*(uintptr_t *)pa_base == 1u);
    CHECK(*(uintptr_t *)(pa_base + sizeof(uintptr_t)) == expected_string_va);
    CHECK(*(uintptr_t *)(pa_base + 2u * sizeof(uintptr_t)) == 0u);
    CHECK(*(uintptr_t *)(pa_base + 3u * sizeof(uintptr_t)) == 0u);
    CHECK(strcmp((char *)(pa_base + pointer_words * sizeof(uintptr_t)), exact_arg) == 0);

    return 0;
}

int main(void)
{
    if (test_mips_loader_accepts_em_mips() != 0 || test_stack_preflight_halts_before_any_copy() != 0 ||
        test_stack_exact_fit_preserves_the_abi_layout() != 0)
    {
        return 1;
    }

    printf("nanos loader integration tests passed\n");
    return 0;
}
