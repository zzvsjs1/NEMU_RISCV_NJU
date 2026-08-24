#ifndef MIPS32_H__
#define MIPS32_H__

#include <stdint.h>

static inline uint8_t inb(uintptr_t addr)
{
    return *(volatile uint8_t *)addr;
}

static inline uint16_t inw(uintptr_t addr)
{
    return *(volatile uint16_t *)addr;
}

static inline uint32_t inl(uintptr_t addr)
{
    return *(volatile uint32_t *)addr;
}

static inline void outb(uintptr_t addr, uint8_t data)
{
    *(volatile uint8_t *)addr = data;
}

static inline void outw(uintptr_t addr, uint16_t data)
{
    *(volatile uint16_t *)addr = data;
}

static inline void outl(uintptr_t addr, uint32_t data)
{
    *(volatile uint32_t *)addr = data;
}

#define PTE_V 0x2u
#define PTE_D 0x4u

/*
 * MIPS32 uses 4 KiB pages and a two-level 10/10/12 page-table split.  Page
 * table entries keep the page-aligned address in bits 31:12; their low bits
 * deliberately use the same V and D positions as CP0 EntryLo.
 */
#define MIPS32_PTE_ADDR_MASK 0xfffff000u
#define MIPS32_TLB_NR 16u

// Page directory and page table constants
#define PTXSHFT 12u // Offset of PTX in a linear address
#define PDXSHFT 22u // Offset of PDX in a linear address

#define PDX(va) (((uintptr_t)(va) >> PDXSHFT) & 0x3ffu)
#define PTX(va) (((uintptr_t)(va) >> PTXSHFT) & 0x3ffu)

#endif
