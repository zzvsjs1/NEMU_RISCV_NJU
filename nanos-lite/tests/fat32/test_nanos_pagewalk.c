#define _POSIX_C_SOURCE 200112L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/pagewalk.h"

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
};

static uint64_t make_pte(void *page, uint64_t flags)
{
    return (((uint64_t)(uintptr_t)page >> 12) << 10) | flags;
}

static void *alloc_page(void)
{
    void *page = NULL;
    int rc = posix_memalign(&page, PAGE_SIZE, PAGE_SIZE);
    if (rc != 0)
    {
        printf("posix_memalign failed: %d\n", rc);
        exit(1);
    }

    memset(page, 0, PAGE_SIZE);
    return page;
}

static int test_sv39_lookup_returns_leaf_physical_page(void)
{
    uint64_t *root = alloc_page();
    uint64_t *level1 = alloc_page();
    uint64_t *level0 = alloc_page();
    void *leaf = alloc_page();
    const uintptr_t va = 0x40049000u;
    const size_t vpn2 = (va >> 30) & 0x1ffu;
    const size_t vpn1 = (va >> 21) & 0x1ffu;
    const size_t vpn0 = (va >> 12) & 0x1ffu;

    root[vpn2] = make_pte(level1, PTE_V);
    level1[vpn1] = make_pte(level0, PTE_V);
    level0[vpn0] = make_pte(leaf, PTE_V | PTE_R | PTE_W | PTE_X | PTE_U | PTE_A | PTE_D);

    CHECK(nanos_pagewalk_lookup_page(root, va) == leaf);
    CHECK(nanos_pagewalk_lookup_page(root, va + 123) == leaf);

    free(leaf);
    free(level0);
    free(level1);
    free(root);
    return 0;
}

static int test_sv39_lookup_returns_null_for_missing_mapping(void)
{
    uint64_t *root = alloc_page();
    void *result = nanos_pagewalk_lookup_page(root, 0x40049000u);
    free(root);
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
