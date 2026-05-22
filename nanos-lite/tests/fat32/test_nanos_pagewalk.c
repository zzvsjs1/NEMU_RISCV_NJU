#define _POSIX_C_SOURCE 200112L

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../src/pagewalk.h"

#ifndef PAGEWALK_TEST_MODE
#define PAGEWALK_TEST_MODE 64
#endif

#define CHECK(cond)                                                   \
    do                                                                \
    {                                                                 \
        if (!(cond))                                                  \
        {                                                             \
            printf("check failed at line %d: %s\n", __LINE__, #cond); \
            return 1;                                                 \
        }                                                             \
    } while (0)

enum
{
    PAGE_SIZE = 4096,
    PTE_V = 0x001,
    PTE_R = 0x002,
    PTE_W = 0x004,
    PTE_X = 0x008,
    PTE_U = 0x010,
    PTE_A = 0x040,
    PTE_D = 0x080,
    X86_PTE_P = 0x001,
    X86_PTE_W = 0x002,
    X86_PTE_U = 0x004,
};

static uint8_t root_page[PAGE_SIZE] __attribute__((aligned(PAGE_SIZE)));
static uint8_t level1_page[PAGE_SIZE] __attribute__((aligned(PAGE_SIZE)));
static uint8_t level0_page[PAGE_SIZE] __attribute__((aligned(PAGE_SIZE)));
static uint8_t leaf_page[PAGE_SIZE] __attribute__((aligned(PAGE_SIZE)));

static void clear_pages(void)
{
    memset(root_page, 0, sizeof(root_page));
    memset(level1_page, 0, sizeof(level1_page));
    memset(level0_page, 0, sizeof(level0_page));
    memset(leaf_page, 0, sizeof(leaf_page));
}

#if PAGEWALK_TEST_MODE == 64

static uint64_t make_rv_pte(void *page, uint64_t flags)
{
    return (((uint64_t)(uintptr_t)page >> 12) << 10) | flags;
}

static int test_sv39_lookup_returns_leaf_physical_page(void)
{
    clear_pages();

    uint64_t *root = (uint64_t *)root_page;
    uint64_t *level1 = (uint64_t *)level1_page;
    uint64_t *level0 = (uint64_t *)level0_page;
    void *leaf = leaf_page;
    const uintptr_t va = 0x40049000u;
    const size_t vpn2 = (va >> 30) & 0x1ffu;
    const size_t vpn1 = (va >> 21) & 0x1ffu;
    const size_t vpn0 = (va >> 12) & 0x1ffu;

    root[vpn2] = make_rv_pte(level1, PTE_V);
    level1[vpn1] = make_rv_pte(level0, PTE_V);
    level0[vpn0] = make_rv_pte(leaf, PTE_V | PTE_R | PTE_W | PTE_X | PTE_U | PTE_A | PTE_D);

    CHECK(nanos_pagewalk_lookup_page(root, va) == leaf);
    CHECK(nanos_pagewalk_lookup_page(root, va + 123) == leaf);

    return 0;
}

static int test_sv39_lookup_returns_null_for_missing_mapping(void)
{
    clear_pages();

    uint64_t *root = (uint64_t *)root_page;
    void *result = nanos_pagewalk_lookup_page(root, 0x40049000u);
    CHECK(result == NULL);
    return 0;
}

int main(void)
{
    if (test_sv39_lookup_returns_leaf_physical_page() != 0 ||
        test_sv39_lookup_returns_null_for_missing_mapping() != 0)
    {
        return 1;
    }

    puts("nanos pagewalk tests passed");
    return 0;
}

#elif PAGEWALK_TEST_MODE == 32

static uint64_t make_rv_pte(void *page, uint64_t flags)
{
    return (((uint64_t)(uintptr_t)page >> 12) << 10) | flags;
}

static int require_32_bit_page(void *page, const char *name)
{
    const uintptr_t addr = (uintptr_t)page;

    if ((addr & (PAGE_SIZE - 1u)) != 0 || addr > UINT32_MAX)
    {
        printf("%s is not a low aligned page: 0x%lx\n", name, (unsigned long)addr);
        return 1;
    }

    return 0;
}

static int test_sv32_lookup_returns_leaf_physical_page(void)
{
    clear_pages();

    uint32_t *root = (uint32_t *)root_page;
    uint32_t *level0 = (uint32_t *)level0_page;
    void *leaf = leaf_page;
    const uintptr_t va = 0x40049000u;
    const size_t vpn1 = (va >> 22) & 0x3ffu;
    const size_t vpn0 = (va >> 12) & 0x3ffu;

    CHECK(require_32_bit_page(root, "sv32 root") == 0);
    CHECK(require_32_bit_page(level0, "sv32 level0") == 0);
    CHECK(require_32_bit_page(leaf, "sv32 leaf") == 0);

    root[vpn1] = (uint32_t)make_rv_pte(level0, PTE_V);
    level0[vpn0] = (uint32_t)make_rv_pte(leaf, PTE_V | PTE_R | PTE_W | PTE_X | PTE_U | PTE_A | PTE_D);

    CHECK(nanos_pagewalk_lookup_page(root, va) == leaf);
    CHECK(nanos_pagewalk_lookup_page(root, va + 321) == leaf);

    return 0;
}

static int test_sv32_lookup_returns_null_for_missing_mapping(void)
{
    clear_pages();

    uint32_t *root = (uint32_t *)root_page;
    void *result = nanos_pagewalk_lookup_page(root, 0x40049000u);
    CHECK(result == NULL);
    return 0;
}

int main(void)
{
    if (test_sv32_lookup_returns_leaf_physical_page() != 0 ||
        test_sv32_lookup_returns_null_for_missing_mapping() != 0)
    {
        return 1;
    }

    puts("nanos pagewalk tests passed");
    return 0;
}

#elif PAGEWALK_TEST_MODE == 86

static int require_32_bit_page(void *page, const char *name)
{
    const uintptr_t addr = (uintptr_t)page;

    if ((addr & (PAGE_SIZE - 1u)) != 0 || addr > UINT32_MAX)
    {
        printf("%s is not a low aligned page: 0x%lx\n", name, (unsigned long)addr);
        return 1;
    }

    return 0;
}

static uint32_t make_x86_pte(void *page, uint32_t flags)
{
    return ((uint32_t)(uintptr_t)page & 0xfffff000u) | flags;
}

static int test_x86_lookup_returns_leaf_physical_page(void)
{
    clear_pages();

    uint32_t *root = (uint32_t *)root_page;
    uint32_t *level0 = (uint32_t *)level0_page;
    void *leaf = leaf_page;
    const uintptr_t va = 0x40049000u;
    const size_t pde = (va >> 22) & 0x3ffu;
    const size_t pte = (va >> 12) & 0x3ffu;

    CHECK(require_32_bit_page(root, "x86 root") == 0);
    CHECK(require_32_bit_page(level0, "x86 level0") == 0);
    CHECK(require_32_bit_page(leaf, "x86 leaf") == 0);

    root[pde] = make_x86_pte(level0, X86_PTE_P | X86_PTE_W | X86_PTE_U);
    level0[pte] = make_x86_pte(leaf, X86_PTE_P | X86_PTE_W | X86_PTE_U);

    CHECK(nanos_pagewalk_lookup_page(root, va) == leaf);
    CHECK(nanos_pagewalk_lookup_page(root, va + 99) == leaf);

    return 0;
}

static int test_x86_lookup_returns_null_for_missing_mapping(void)
{
    clear_pages();

    uint32_t *root = (uint32_t *)root_page;
    void *result = nanos_pagewalk_lookup_page(root, 0x40049000u);
    CHECK(result == NULL);
    return 0;
}

int main(void)
{
    if (test_x86_lookup_returns_leaf_physical_page() != 0 ||
        test_x86_lookup_returns_null_for_missing_mapping() != 0)
    {
        return 1;
    }

    puts("nanos pagewalk tests passed");
    return 0;
}

#else
#error "Unsupported PAGEWALK_TEST_MODE"
#endif
