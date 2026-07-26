#include "trap.h"
#include <stdint.h>

#if !defined(__mips__)
#error "This test must be compiled for MIPS32"
#endif

#define PAGE_SIZE 4096u
#define PAGE_WORD_COUNT (PAGE_SIZE / sizeof(uint32_t))
#define PAGE_TABLE_PAGE_COUNT 3u
#define USER_CONTEXT_STACK_SIZE 512u

#define USER_CODE_VA ((uintptr_t)0x40000000u)
#define USER_DATA_VA ((uintptr_t)0x40002000u)
#define USER_STACK_LOWER_VA ((uintptr_t)0x7fffe000u)
#define USER_STACK_UPPER_VA ((uintptr_t)0x7ffff000u)
#define USER_INITIAL_SP ((uintptr_t)0x80000000u)
#define USER_STORED_SP (USER_INITIAL_SP - 16u)
#define USER_STACK_STORE_INDEX (0xff0u / sizeof(uint32_t))
#define USER_VALUE ((uint32_t)0x13579bdfu)

/*
 * The allocator services protect()'s root plus one leaf table for the code
 * and data region and one leaf table for the two-page stack region.  Supplying
 * visibly non-zero pages also keeps this test independent of BSS zeroing.
 */
static uint8_t page_table_pages[PAGE_TABLE_PAGE_COUNT][PAGE_SIZE]
    __attribute__((aligned(PAGE_SIZE)));
static unsigned allocated_page_count;

static uint32_t code_page[PAGE_WORD_COUNT]
    __attribute__((aligned(PAGE_SIZE)));
static uint32_t data_page[PAGE_WORD_COUNT]
    __attribute__((aligned(PAGE_SIZE)));
static uint32_t lower_stack_page[PAGE_WORD_COUNT]
    __attribute__((aligned(PAGE_SIZE)));
static uint32_t upper_stack_page[PAGE_WORD_COUNT]
    __attribute__((aligned(PAGE_SIZE)));
static uint8_t user_context_stack[USER_CONTEXT_STACK_SIZE]
    __attribute__((aligned(16)));

static AddrSpace user_address_space;
static Context *boot_context;
static Context *initial_user_context;
static volatile bool user_finished;

/*
 * These are literal little-endian MIPS32 instruction words.  The first
 * instruction reserves the O32 16-byte argument-save area while preserving
 * 16-byte stack alignment.  The two stores then force independent data and
 * stack refills before `syscall 1` yields through the normal exception path.
 */
static const uint32_t user_code[] = {
    0x27bdfff0u, /* addiu sp, sp, -16 */
    0x3c084000u, /* lui   t0, 0x4000 */
    0x35082000u, /* ori   t0, t0, 0x2000 */
    0xad040000u, /* sw    a0, 0(t0) */
    0xafa40000u, /* sw    a0, 0(sp) */
    0x0000004cu, /* syscall 1 */
    0x00000000u, /* nop */
};

_Static_assert(sizeof(user_code) <= sizeof(code_page),
               "MIPS32 user code must fit in one mapped page");
_Static_assert((USER_INITIAL_SP & 0x0fu) == 0u,
               "Initial MIPS32 user SP must be 16-byte aligned");
_Static_assert((USER_STORED_SP & 0x0fu) == 0u,
               "The raw user prologue must preserve stack alignment");

static void *allocate_page(int size)
{
    check(size == (int)PAGE_SIZE);
    check(allocated_page_count < PAGE_TABLE_PAGE_COUNT);

    void *const page = page_table_pages[allocated_page_count++];
    memset(page, 0xa5, PAGE_SIZE);
    return page;
}

static void free_page(void *page)
{
    /* This focused execution never destroys its single address space. */
    (void)page;
}

static Context *handle_event(Event event, Context *context)
{
    check(event.event == EVENT_YIELD);

    if (boot_context == NULL)
    {
        /*
         * The first yield comes from main() in the direct kernel segment.
         * Preserve its live trap Context and select the prepared user Context;
         * the assembly return path will switch page tables and enter kuseg.
         */
        check(initial_user_context != NULL);
        boot_context = context;
        return initial_user_context;
    }

    /* A second yield proves that instruction, data, and stack refills worked. */
    check(!user_finished);
    check(context != boot_context);
    check(context == initial_user_context);
    check(context->pdir == user_address_space.ptr);
    check(context->np == 1u);

    /*
     * CTE advances EPC past the `syscall 1` word before calling this handler.
     * The saved SP must likewise show exactly one execution of `addiu -16`.
     */
    check(context->epc == USER_CODE_VA + 6u * sizeof(uint32_t));
    check(context->GPRSP == USER_STORED_SP);
    check(context->gpr[4] == USER_VALUE); /* Architectural a0. */

    check(*(volatile uint32_t *)&data_page[0] == USER_VALUE);
    check(*(volatile uint32_t *)&upper_stack_page[USER_STACK_STORE_INDEX] ==
          USER_VALUE);

    user_finished = true;
    return boot_context;
}

int main(void)
{
    memcpy(code_page, user_code, sizeof(user_code));
    data_page[0] = 0u;
    upper_stack_page[USER_STACK_STORE_INDEX] = 0u;

    check(vme_init(allocate_page, free_page));
    protect(&user_address_space);

    /*
     * The lower and upper stack pages form one VPN2 pair.  Mapping both halves
     * makes the refill handler construct a complete EntryLo0/EntryLo1 pair,
     * although this raw program deliberately stores in the upper half.
     */
    map(&user_address_space, (void *)USER_CODE_VA, code_page, MMAP_READ);
    map(&user_address_space, (void *)USER_DATA_VA, data_page,
        MMAP_READ | MMAP_WRITE);
    map(&user_address_space, (void *)USER_STACK_LOWER_VA, lower_stack_page,
        MMAP_READ | MMAP_WRITE);
    map(&user_address_space, (void *)USER_STACK_UPPER_VA, upper_stack_page,
        MMAP_READ | MMAP_WRITE);
    check(allocated_page_count == PAGE_TABLE_PAGE_COUNT);

    const Area kernel_stack =
        RANGE(user_context_stack,
              user_context_stack + sizeof(user_context_stack));
    initial_user_context =
        ucontext(&user_address_space, kernel_stack, (void *)USER_CODE_VA);
    check(initial_user_context != NULL);

    /*
     * ucontext() supplies the page directory, EPC, and Status.  This test sets
     * the two user-visible inputs explicitly: a0 carries the value to store,
     * and SP begins at the aligned exclusive end of the mapped user area.
     */
    initial_user_context->gpr[4] = USER_VALUE; /* Architectural a0. */
    initial_user_context->GPRSP = USER_INITIAL_SP;

    check(cte_init(handle_event));
    yield();

    /* The user handler selected the saved boot Context, so execution resumes. */
    check(user_finished);
    check(boot_context != NULL);
    check(*(volatile uint32_t *)&data_page[0] == USER_VALUE);
    check(*(volatile uint32_t *)&upper_stack_page[USER_STACK_STORE_INDEX] ==
          USER_VALUE);
    return 0;
}
