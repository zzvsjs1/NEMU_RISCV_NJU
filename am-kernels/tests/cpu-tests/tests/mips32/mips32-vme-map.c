#include "trap.h"
#include <stdint.h>

#if !defined(__mips__)
#error "This test must be compiled for MIPS32"
#endif

enum
{
    PAGE_SIZE = 4096,
    PAGE_WORDS = PAGE_SIZE / sizeof(uintptr_t),
    PAGE_TABLE_PAGES = 2,
    CONTEXT_STACK_SIZE = 512,
    MIPS32_TLB_ENTRY_COUNT = 16,
    MIPS32_TLB_VPN2_STRIDE = 0x2000,
    MIPS32_INDEX_PROBE_FAILURE = 0x80000000u,
    MIPS32_PTE_VALID = 0x2u,
    MIPS32_PTE_DIRTY = 0x4u,
};

#define MIPS32_PTE_ADDRESS_MASK 0xfffff000u
#define PAGE_DIRECTORY_INDEX(va) (((uintptr_t)(va) >> 22) & 0x3ffu)
#define PAGE_TABLE_INDEX(va) (((uintptr_t)(va) >> 12) & 0x3ffu)

/*
 * These routines are the architecture-private half of AM's Context-switch
 * contract.  The trap path normally calls them, but this focused test invokes
 * them directly so it can distinguish a user Context from a kernel Context.
 */
extern void __am_get_cur_as(Context *context);
extern void __am_switch(Context *context);

/*
 * protect() needs one root page and map() needs one leaf-table page.  Filling
 * both pages with a non-zero pattern makes the test prove that AM explicitly
 * initialises page-table memory instead of relying on a zeroing allocator.
 */
static uint8_t page_table_pages[PAGE_TABLE_PAGES][PAGE_SIZE]
    __attribute__((aligned(PAGE_SIZE)));
static unsigned allocated_page_count;

static uint8_t data_page[PAGE_SIZE] __attribute__((aligned(PAGE_SIZE)));
static uint8_t context_stack[CONTEXT_STACK_SIZE]
    __attribute__((aligned(16)));

static void *allocate_page(int size)
{
    check(size == PAGE_SIZE);
    check(allocated_page_count < PAGE_TABLE_PAGES);

    void *const page = page_table_pages[allocated_page_count++];
    memset(page, 0xa5, PAGE_SIZE);
    return page;
}

static void free_page(void *page)
{
    /* unprotect() is outside this test's scope, so no page should be freed. */
    (void)page;
}

/*
 * CP0 register numbers must remain instruction immediates.  Stringifying the
 * compile-time register argument keeps the helpers usable with the MIPS
 * assembler while retaining a conventional C call at each test site.
 */
#define MTC0(reg, value)                                                       \
    asm volatile("mtc0 %0, $" #reg : : "r"((uint32_t)(value)) : "memory")
#define MFC0(reg, value)                                                       \
    asm volatile("mfc0 %0, $" #reg : "=r"(value) : : "memory")

static void install_global_tlb_entry(uint32_t index, uintptr_t virtual_address)
{
    /*
     * Both EntryLo G bits are set so a later probe cannot fail merely because
     * a TLB-clearing routine happened to leave a different ASID in EntryHi.
     * The entry is never used for memory access, so PFN zero is sufficient.
     */
    MTC0(0, index);
    MTC0(10, virtual_address);
    MTC0(2, MIPS32_PTE_VALID | MIPS32_PTE_DIRTY | 0x1u);
    MTC0(3, MIPS32_PTE_VALID | MIPS32_PTE_DIRTY | 0x1u);
    asm volatile("tlbwi" : : : "memory");
}

static uint32_t probe_tlb(uintptr_t virtual_address)
{
    uint32_t index;

    MTC0(10, virtual_address);
    asm volatile("tlbp" : : : "memory");
    MFC0(0, index);
    return index;
}

static void check_tlb_miss(uintptr_t virtual_address)
{
    check((probe_tlb(virtual_address) & MIPS32_INDEX_PROBE_FAILURE) != 0u);
}

static uintptr_t indexed_test_vpn2(uintptr_t base, unsigned index)
{
    /* Every 8 KiB step selects a distinct VPN2 while retaining ASID zero. */
    return base + (uintptr_t)index * MIPS32_TLB_VPN2_STRIDE;
}

static void seed_all_tlb_slots(uintptr_t base)
{
    /*
     * Populate every indexed slot, then probe every VPN2 before continuing.
     * This precondition prevents an incomplete invalidation routine from
     * passing merely because an unseeded slot happened to be invalid already.
     */
    for (unsigned index = 0; index < MIPS32_TLB_ENTRY_COUNT; index++)
    {
        install_global_tlb_entry(index, indexed_test_vpn2(base, index));
    }

    for (unsigned index = 0; index < MIPS32_TLB_ENTRY_COUNT; index++)
    {
        check(probe_tlb(indexed_test_vpn2(base, index)) == index);
    }
}

static void check_all_tlb_slots_invalid(uintptr_t base)
{
    for (unsigned index = 0; index < MIPS32_TLB_ENTRY_COUNT; index++)
    {
        check_tlb_miss(indexed_test_vpn2(base, index));
    }
}

int main(void)
{
    const uintptr_t mapped_virtual_address = 0x40002000u;
    const uintptr_t initial_stale_vpn2_base = 0x50000000u;
    const uintptr_t map_stale_vpn2_base = 0x52000000u;
    const uintptr_t switch_stale_vpn2_base = 0x54000000u;
    const uintptr_t user_entry = 0x40004000u;
    AddrSpace address_space = {0};

    /* vme_init() must discard all translations inherited from earlier work. */
    seed_all_tlb_slots(initial_stale_vpn2_base);
    check(vme_init(allocate_page, free_page));
    check_all_tlb_slots_invalid(initial_stale_vpn2_base);

    protect(&address_space);
    check(address_space.area.start == (void *)0x40000000u);
    check(address_space.area.end == (void *)0x80000000u);
    check(address_space.pgsize == PAGE_SIZE);
    check(address_space.ptr != NULL);
    check(allocated_page_count == 1u);

    /*
     * The allocator supplied 0xa5 bytes.  Every word in the new root must now
     * be zero, proving that protect() made a clean 1024-entry directory.
     */
    uintptr_t *const page_directory = address_space.ptr;

    for (unsigned i = 0; i < PAGE_WORDS; i++)
    {
        check(page_directory[i] == 0u);
    }

    /* map() changes translation state, so it must invalidate every stale hit. */
    seed_all_tlb_slots(map_stale_vpn2_base);
    map(&address_space, (void *)mapped_virtual_address, data_page,
        MMAP_READ | MMAP_WRITE);
    check_all_tlb_slots_invalid(map_stale_vpn2_base);
    check(allocated_page_count == 2u);

    /*
     * A MIPS32 4 KiB mapping uses ten page-directory bits, ten page-table
     * bits, and twelve offset bits.  Inspecting both indexed words catches an
     * accidental one-level table or a shifted PTE representation.
     */
    const unsigned directory_index =
        (unsigned)PAGE_DIRECTORY_INDEX(mapped_virtual_address);
    const unsigned table_index =
        (unsigned)PAGE_TABLE_INDEX(mapped_virtual_address);
    const uintptr_t directory_entry = page_directory[directory_index];

    check((directory_entry & MIPS32_PTE_VALID) != 0u);
    uintptr_t *const leaf_table =
        (uintptr_t *)(directory_entry & MIPS32_PTE_ADDRESS_MASK);
    check(leaf_table == (uintptr_t *)page_table_pages[1]);
    check((leaf_table[table_index] & MIPS32_PTE_ADDRESS_MASK) ==
          ((uintptr_t)data_page & MIPS32_PTE_ADDRESS_MASK));
    check((leaf_table[table_index] &
           (MIPS32_PTE_VALID | MIPS32_PTE_DIRTY)) ==
          (MIPS32_PTE_VALID | MIPS32_PTE_DIRTY));

    /* map() must also clear all untouched words in its newly allocated leaf. */
    for (unsigned i = 0; i < PAGE_WORDS; i++)
    {
        if (i != table_index)
        {
            check(leaf_table[i] == 0u);
        }
    }

    const Area kernel_stack =
        RANGE(context_stack, context_stack + sizeof(context_stack));
    Context *const user_context =
        ucontext(&address_space, kernel_stack, (void *)user_entry);
    check(user_context != NULL);

    const uintptr_t aligned_stack_top =
        (uintptr_t)kernel_stack.end & ~(uintptr_t)0x0fu;
    check((uintptr_t)user_context == aligned_stack_top - sizeof(Context));
    check(((uintptr_t)user_context & 0x7u) == 0u);
    check(user_context->pdir == address_space.ptr);
    check(user_context->epc == user_entry);
    check((user_context->status & 0x1u) != 0u);
    check((user_context->status & 0x2u) == 0u);
    check(user_context->GPRSP == 0u);
    check(user_context->GPRx == 0u);

    /*
     * Switching to a user Context records its root and invalidates stale TLB
     * state.  A later trap Context must receive that root from get_cur_as().
     */
    seed_all_tlb_slots(switch_stale_vpn2_base);
    __am_switch(user_context);
    check_all_tlb_slots_invalid(switch_stale_vpn2_base);

    Context observed_user_context = {0};
    __am_get_cur_as(&observed_user_context);
    check(observed_user_context.pdir == address_space.ptr);

    /*
     * A NULL pdir denotes a kernel Context.  The implementation may retain the
     * last user root internally, but get_cur_as() must expose NULL while the
     * current execution state is marked as kernel-only.
     */
    Context kernel_context = {0};
    __am_switch(&kernel_context);

    Context observed_kernel_context = {0};
    observed_kernel_context.pdir = (void *)0xdeadbeefu;
    __am_get_cur_as(&observed_kernel_context);
    check(observed_kernel_context.pdir == NULL);

    /* The user marker and saved root must remain reusable after a kernel hop. */
    __am_switch(user_context);
    memset(&observed_user_context, 0, sizeof(observed_user_context));
    __am_get_cur_as(&observed_user_context);
    check(observed_user_context.pdir == address_space.ptr);

    return 0;
}
