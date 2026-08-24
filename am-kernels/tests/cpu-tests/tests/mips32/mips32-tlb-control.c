#include "trap.h"
#include <stdint.h>

#if !defined(__mips__)
#error "This test must be compiled for MIPS32"
#endif

/*
 * Keep the CP0 register number in the instruction as an immediate.  The
 * assembler cannot accept a run-time CP0 register number, so these helpers
 * deliberately stringify a compile-time constant at each call site.
 */
#define MTC0(reg, value) asm volatile("mtc0 %0, $" #reg : : "r"((uint32_t)(value)) : "memory")
#define MFC0(reg, value) asm volatile("mfc0 %0, $" #reg : "=r"(value) : : "memory")

static inline void tlbp(void)
{
    asm volatile("tlbp" : : : "memory");
}

static inline void tlbwi(void)
{
    asm volatile("tlbwi" : : : "memory");
}

static inline void tlbwr(void)
{
    asm volatile("tlbwr" : : : "memory");
}

static void write_entry(uint32_t index, uint32_t entryhi, uint32_t entrylo0, uint32_t entrylo1)
{
    MTC0(0, index);
    MTC0(10, entryhi);
    MTC0(2, entrylo0);
    MTC0(3, entrylo1);
    tlbwi();
}

static uint32_t probe(uint32_t entryhi)
{
    uint32_t index;

    MTC0(10, entryhi);
    tlbp();
    MFC0(0, index);
    return index;
}

int main(void)
{
    uint32_t value;

    /* First verify that all four CP0 control registers retain written data. */
    MTC0(0, 7u);
    MFC0(0, value);
    check(value == 7u);

    MTC0(2, 0x00123006u);
    MFC0(2, value);
    check(value == 0x00123006u);

    MTC0(3, 0x00456006u);
    MFC0(3, value);
    check(value == 0x00456006u);

    MTC0(10, 0x4000002au);
    MFC0(10, value);
    check(value == 0x4000002au);

    /* A failed probe is reported through the high-order P bit of Index. */
    check((probe(0x00400000u) & 0x80000000u) != 0u);

    /* NEMU uses VPN2 as the complete tag while only one address space is active. */
    write_entry(5u, 0x0040002au, 0x00123006u, 0x00456006u);
    check(probe(0x0040002au) == 5u);
    check(probe(0x0040002bu) == 5u);
    check((probe(0x0060002au) & 0x80000000u) != 0u);

    /*
     * Hardware normally chooses the TLBWR destination through Random, limited
     * by Wired.  NEMU does not expose that replacement state, so it uses
     * deterministic round-robin writes beginning at slot zero.  The test
     * records that model contract, not an ISA revision.
     */
    MTC0(10, 0x00800000u);
    MTC0(2, 0x00123006u);
    MTC0(3, 0x00456006u);
    tlbwr();
    check(probe(0x00800000u) == 0u);

    MTC0(10, 0x00a00000u);
    tlbwr();
    check(probe(0x00a00000u) == 1u);

    return 0;
}
