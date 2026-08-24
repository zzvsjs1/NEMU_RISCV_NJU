#include "trap.h"
#include <stdint.h>

#define MTC0(reg, value) asm volatile("mtc0 %0, $" #reg : : "r"((uint32_t)(value)) : "memory")

/*
 * Each backing page is 4 KiB, matching the minimum MIPS32 page size.  Keeping
 * both arrays page-aligned also makes their low twelve physical address bits
 * zero before the PFN is encoded in EntryLo.
 */
static uint32_t even_page[1024] __attribute__((aligned(4096)));
static uint32_t odd_page[1024] __attribute__((aligned(4096)));

static uint32_t entrylo_for(void *page)
{
    /*
     * EntryLo.PFN occupies bits 25:6 and represents physical address bits
     * 31:12.  D=1 and V=1 make the selected page writable and valid; G remains
     * clear, so the entry continues to use EntryHi's ASID (zero in this test).
     */
    return (((uint32_t)(uintptr_t)page & 0xfffff000u) >> 6) | 0x6u;
}

int main(void)
{
    const uintptr_t even_va = 0x40000000u;
    const uintptr_t odd_va = 0x40001000u;
    volatile uint32_t *const even_mapping = (volatile uint32_t *)even_va;
    volatile uint32_t *const odd_mapping = (volatile uint32_t *)odd_va;

    even_page[0] = 0x11223344u;
    odd_page[0] = 0x55667788u;

    /*
     * One VPN2 entry covers the adjacent virtual pages.  EntryLo0 maps the
     * even page (virtual bit 12 clear), while EntryLo1 maps the odd page
     * (virtual bit 12 set).
     */
    MTC0(0, 3u);
    MTC0(10, even_va);
    MTC0(2, entrylo_for(even_page));
    MTC0(3, entrylo_for(odd_page));
    asm volatile("tlbwi" : : : "memory");

    check(*even_mapping == 0x11223344u);
    check(*odd_mapping == 0x55667788u);

    *even_mapping = 0xa1b2c3d4u;
    *odd_mapping = 0xe5f60718u;
    check(even_page[0] == 0xa1b2c3d4u);
    check(odd_page[0] == 0xe5f60718u);

    return 0;
}
