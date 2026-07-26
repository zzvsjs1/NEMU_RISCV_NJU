#include "trap.h"
#include <stdint.h>

#if !defined(__mips__)
#error "This test must be compiled for MIPS32"
#endif

#define PAGE_SIZE 4096u
#define PAGE_WORD_COUNT (PAGE_SIZE / sizeof(uint32_t))
#define PAGE_TABLE_PAGE_COUNT 6u
#define CONTEXT_STACK_SIZE 512u

#define USER_CODE_VA ((uintptr_t)0x40000000u)
#define USER_STACK_VA ((uintptr_t)0x7ffff000u)
#define USER_STACK_TOP (USER_STACK_VA + PAGE_SIZE)
#define USER_RESUME_EPC (USER_CODE_VA + 2u * sizeof(uint32_t))
#define USER_SECOND_RESUME_EPC (USER_CODE_VA + 4u * sizeof(uint32_t))
#define USER_A_MARKER ((uintptr_t)0x11u)
#define USER_B_MARKER ((uintptr_t)0x22u)
#define USER_A_RESUME_MARKER ((uintptr_t)0x33u)

/*
 * Each address space needs one root page, one code-region leaf table, and one
 * stack-region leaf table.  Dirty allocator output proves the test depends on
 * AM's page-table initialisation rather than host BSS behaviour.
 */
static uint8_t page_table_pages[PAGE_TABLE_PAGE_COUNT][PAGE_SIZE]
    __attribute__((aligned(PAGE_SIZE)));
static unsigned allocated_page_count;

/*
 * A and B intentionally map identical user virtual addresses to different
 * physical pages.  The distinct stack fill patterns also make an accidental
 * reload through the newly selected address space return obviously bad data.
 */
static uint32_t user_a_code_page[PAGE_WORD_COUNT]
    __attribute__((aligned(PAGE_SIZE)));
static uint32_t user_b_code_page[PAGE_WORD_COUNT]
    __attribute__((aligned(PAGE_SIZE)));
static uint8_t user_a_stack_page[PAGE_SIZE]
    __attribute__((aligned(PAGE_SIZE)));
static uint8_t user_b_stack_page[PAGE_SIZE]
    __attribute__((aligned(PAGE_SIZE)));

static uint8_t user_a_context_stack[CONTEXT_STACK_SIZE]
    __attribute__((aligned(16)));
static uint8_t user_b_context_stack[CONTEXT_STACK_SIZE]
    __attribute__((aligned(16)));

static AddrSpace address_space_a;
static AddrSpace address_space_b;
static Context *boot_context;
static Context *user_context_a;
static Context *user_context_b;
static volatile unsigned execution_stage;
static volatile bool user_a_executed;
static volatile bool user_b_executed;
static volatile bool user_a_resumed;

/*
 * Neither stub reads nor writes through SP.  Consequently its instruction
 * refill cannot accidentally populate the stack VPN2 before the normal
 * syscall trap tries to save a Context.  Distinct a0 markers prove which
 * physical code page ran at the shared virtual entry address.
 */
static const uint32_t user_a_code[] = {
    0x24040011u, /* addiu a0, zero, 0x11 */
    0x0000004cu, /* syscall 1 */
    0x24040033u, /* addiu a0, zero, 0x33 */
    0x0000004cu, /* syscall 1 */
    0x00000000u, /* nop */
};

static const uint32_t user_b_code[] = {
    0x24040022u, /* addiu a0, zero, 0x22 */
    0x0000004cu, /* syscall 1 */
    0x00000000u, /* nop */
};

_Static_assert(sizeof(user_a_code) <= sizeof(user_a_code_page),
               "User A code must fit in one page");
_Static_assert(sizeof(user_b_code) <= sizeof(user_b_code_page),
               "User B code must fit in one page");
_Static_assert((USER_STACK_TOP & 0x0fu) == 0u,
               "Initial user stacks must be 16-byte aligned");

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
    /* This test retains both address spaces until the final boot return. */
    (void)page;
}

static Context *handle_event(Event event, Context *context)
{
    check(event.event == EVENT_YIELD);

    switch (execution_stage)
    {
    case 0u:
        /* The direct-kernel boot Context must carry the kernel pdir marker. */
        check(context->pdir == NULL);
        check(context->np == 0u);
        check(boot_context == NULL);
        check(user_context_a != NULL);
        boot_context = context;
        execution_stage = 1u;
        return user_context_a;

    case 1u:
        /* User A reached its syscall without changing or touching its SP. */
        check(context == user_context_a);
        check(context->pdir == address_space_a.ptr);
        check(context->np == 1u);
        check(context->epc == USER_RESUME_EPC);
        check(context->GPRSP == USER_STACK_TOP);
        check(context->gpr[4] == USER_A_MARKER); /* Architectural a0. */
        check(!user_a_executed);
        check(!user_b_executed);
        user_a_executed = true;
        execution_stage = 2u;
        return user_context_b;

    case 2u:
        /*
         * Reaching this event proves trap return used B's Context and page
         * directory rather than reusing A's same-VA code or stack mapping.
         */
        check(context == user_context_b);
        check(context->pdir == address_space_b.ptr);
        check(context->np == 1u);
        check(context->epc == USER_RESUME_EPC);
        check(context->GPRSP == USER_STACK_TOP);
        check(context->gpr[4] == USER_B_MARKER); /* Architectural a0. */
        check(user_a_executed);
        check(!user_b_executed);
        user_b_executed = true;
        execution_stage = 3u;
        return user_context_a;

    case 3u:
        /*
         * A's second yield proves its original process-owned Context survived
         * B's complete trap round trip instead of aliasing shared scratch.
         */
        check(context == user_context_a);
        check(context->pdir == address_space_a.ptr);
        check(context->np == 1u);
        check(context->epc == USER_SECOND_RESUME_EPC);
        check(context->GPRSP == USER_STACK_TOP);
        check(context->gpr[4] == USER_A_RESUME_MARKER);
        check(user_a_executed);
        check(user_b_executed);
        check(!user_a_resumed);
        user_a_resumed = true;
        execution_stage = 4u;
        return boot_context;

    default:
        check(false);
        return context;
    }
}

int main(void)
{
    memcpy(user_a_code_page, user_a_code, sizeof(user_a_code));
    memcpy(user_b_code_page, user_b_code, sizeof(user_b_code));
    memset(user_a_stack_page, 0xa1, sizeof(user_a_stack_page));
    memset(user_b_stack_page, 0xb2, sizeof(user_b_stack_page));

    check((uintptr_t)user_a_code_page != (uintptr_t)user_b_code_page);
    check((uintptr_t)user_a_stack_page != (uintptr_t)user_b_stack_page);
    check(vme_init(allocate_page, free_page));

    protect(&address_space_a);
    protect(&address_space_b);
    check(address_space_a.ptr != address_space_b.ptr);

    map(&address_space_a, (void *)USER_CODE_VA, user_a_code_page, MMAP_READ);
    map(&address_space_a, (void *)USER_STACK_VA, user_a_stack_page,
        MMAP_READ | MMAP_WRITE);
    map(&address_space_b, (void *)USER_CODE_VA, user_b_code_page, MMAP_READ);
    map(&address_space_b, (void *)USER_STACK_VA, user_b_stack_page,
        MMAP_READ | MMAP_WRITE);
    check(allocated_page_count == PAGE_TABLE_PAGE_COUNT);

    const Area context_stack_a =
        RANGE(user_a_context_stack,
              user_a_context_stack + sizeof(user_a_context_stack));
    const Area context_stack_b =
        RANGE(user_b_context_stack,
              user_b_context_stack + sizeof(user_b_context_stack));

    user_context_a =
        ucontext(&address_space_a, context_stack_a, (void *)USER_CODE_VA);
    user_context_b =
        ucontext(&address_space_b, context_stack_b, (void *)USER_CODE_VA);
    check(user_context_a != NULL);
    check(user_context_b != NULL);
    check(user_context_a != user_context_b);
    check(user_context_a->pdir == address_space_a.ptr);
    check(user_context_b->pdir == address_space_b.ptr);
    check(user_context_a->np == 1u);
    check(user_context_b->np == 1u);

    user_context_a->GPRSP = USER_STACK_TOP;
    user_context_b->GPRSP = USER_STACK_TOP;

    check(cte_init(handle_event));
    yield();

    /* User B selected the preserved boot Context, so main resumes exactly once. */
    check(execution_stage == 4u);
    check(user_a_executed);
    check(user_b_executed);
    check(user_a_resumed);
    check(boot_context != NULL);
    return 0;
}
