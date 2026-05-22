#include "pagewalk.h"

#if defined(__ISA__)
#include <klib.h>
#else
#include <assert.h>
#endif
#include <stddef.h>
#include <stdint.h>

#define PAGE_SHIFT 12u
#define PAGE_SIZE ((uintptr_t)1u << PAGE_SHIFT)
#define PTE_V ((uintptr_t)1u << 0)
#define PTE_R ((uintptr_t)1u << 1)
#define PTE_W ((uintptr_t)1u << 2)
#define PTE_X ((uintptr_t)1u << 3)
#define PTE_RWX (PTE_R | PTE_W | PTE_X)
#define PTE_PPN_SHIFT 10u
#define X86_PTE_P ((uintptr_t)1u << 0)
#define X86_PTE_ADDR_MASK (~(PAGE_SIZE - 1u))

#if defined(__ISA_X86__)
typedef uint32_t NanosPte;
#define PAGEWALK_LEVELS 2
#define VPN_BITS 10u
#elif defined(__riscv_xlen) && __riscv_xlen == 32
typedef uint32_t NanosPte;
#define PAGEWALK_LEVELS 2
#define VPN_BITS 10u
#elif defined(__riscv_xlen) && __riscv_xlen == 64
typedef uint64_t NanosPte;
#define PAGEWALK_LEVELS 3
#define VPN_BITS 9u
#else
#ifndef NANOS_PAGEWALK_XLEN
#if UINTPTR_MAX == UINT64_MAX
#define NANOS_PAGEWALK_XLEN 64
#else
#define NANOS_PAGEWALK_XLEN 32
#endif
#endif

#if NANOS_PAGEWALK_XLEN == 32
typedef uint32_t NanosPte;
#define PAGEWALK_LEVELS 2
#define VPN_BITS 10u
#elif NANOS_PAGEWALK_XLEN == 64
typedef uint64_t NanosPte;
#define PAGEWALK_LEVELS 3
#define VPN_BITS 9u
#else
#error "Unsupported Nanos page-walk XLEN"
#endif
#endif

#define VPN_MASK (((uintptr_t)1u << VPN_BITS) - 1u)

static uintptr_t pte_page_base(NanosPte pte)
{
#if defined(__ISA_X86__)
    return (uintptr_t)pte & X86_PTE_ADDR_MASK;
#else
    return ((uintptr_t)(pte >> PTE_PPN_SHIFT)) << PAGE_SHIFT;
#endif
}

static int pte_is_valid(NanosPte pte)
{
#if defined(__ISA_X86__)
    return (pte & X86_PTE_P) != 0;
#else
    return (pte & PTE_V) != 0;
#endif
}

#if defined(__ISA_X86__)
/* x86 marks both directory and leaf entries present with bit 0, so the level
 * number decides when the walk has reached a leaf PTE.
 */
#else
static int pte_is_leaf(NanosPte pte)
{
    return (pte & PTE_RWX) != 0;
}
#endif

void *nanos_pagewalk_lookup_page(void *root, uintptr_t vaddr)
{
    assert(root != NULL);

    NanosPte *table = (NanosPte *)root;
    const uintptr_t page_va = vaddr & ~(PAGE_SIZE - 1u);

    for (int level = PAGEWALK_LEVELS - 1; level >= 0; level--)
    {
        const uintptr_t vpn = (page_va >> (PAGE_SHIFT + (uintptr_t)level * VPN_BITS)) & VPN_MASK;
        const NanosPte pte = table[vpn];

        if (!pte_is_valid(pte))
        {
            return NULL;
        }

#if defined(__ISA_X86__)
        if (level == 0)
        {
            return (void *)pte_page_base(pte);
        }
#else
        if (pte_is_leaf(pte))
        {
            // AM maps user pages as 4 KiB leaves; upper-level leaves would be
            // superpages, which the Nanos loader does not create or need.
            assert(level == 0);
            return (void *)pte_page_base(pte);
        }
#endif

        assert(level > 0);
        table = (NanosPte *)pte_page_base(pte);
    }

    return NULL;
}
