#include "debug.h"
#include <isa.h>
#include <memory/paddr.h>
#include <memory/vaddr.h>

// translate returns (pg_paddr | status), pg_paddr is page-aligned so low 12 bits are free
static int mem_ret_status(paddr_t ret) 
{
    return (int)(ret & (paddr_t)PAGE_MASK);
}

static paddr_t mem_ret_pgaddr(paddr_t ret) 
{
    return (paddr_t)(ret & ~(paddr_t)PAGE_MASK);
}

word_t vaddr_ifetch(vaddr_t addr, int len) 
{
    int mmu = isa_mmu_check(addr, len, MEM_TYPE_IFETCH);

    if (mmu == MMU_DIRECT) 
    {
        return paddr_read((paddr_t)addr, len);
    }

    if (mmu == MMU_TRANSLATE) 
    {
        paddr_t ret = isa_mmu_translate(addr, len, MEM_TYPE_IFETCH);
        int st = mem_ret_status(ret);

        if (st == MEM_RET_OK) 
        {
            paddr_t pg = mem_ret_pgaddr(ret);
            paddr_t pa = pg | (paddr_t)(addr & PAGE_MASK);
            return paddr_read(pa, len);
        }

        if (st == MEM_RET_CROSS_PAGE) 
        {
            panic("vaddr_ifetch: cross-page access not supported yet");
        }

        // MEM_RET_FAIL or unknown code
        panic("vaddr_ifetch: mmu translate failed");
    }

    // MMU_FAIL or others
    panic("vaddr_ifetch: mmu check failed");
    return 0;
}

word_t vaddr_read(vaddr_t addr, int len) 
{
    const int mmu = isa_mmu_check(addr, len, MEM_TYPE_READ);

    if (mmu == MMU_DIRECT) 
    {
        return paddr_read((paddr_t)addr, len);
    }

    if (mmu == MMU_TRANSLATE) 
    {
        paddr_t ret = isa_mmu_translate(addr, len, MEM_TYPE_READ);
        int status = mem_ret_status(ret);

        if (status == MEM_RET_OK) 
        {
            paddr_t pg = mem_ret_pgaddr(ret);
            paddr_t pa = pg | (paddr_t)(addr & PAGE_MASK);
            return paddr_read(pa, len);
        }

        if (status == MEM_RET_CROSS_PAGE) 
        {
            panic("vaddr_read: cross-page access not supported yet");
        }

        panic("vaddr_read: mmu translate failed");
    }

    panic("vaddr_read: mmu check failed");
    return 0;
}

void vaddr_write(vaddr_t addr, int len, word_t data) 
{
    int mmu = isa_mmu_check(addr, len, MEM_TYPE_WRITE);

    if (mmu == MMU_DIRECT) 
    {
        paddr_write((paddr_t)addr, len, data);
        return;
    }

    if (mmu == MMU_TRANSLATE) 
    {
        paddr_t ret = isa_mmu_translate(addr, len, MEM_TYPE_WRITE);
        int st = mem_ret_status(ret);

        if (st == MEM_RET_OK) 
        {
            paddr_t pg = mem_ret_pgaddr(ret);
            paddr_t pa = pg | (paddr_t)(addr & PAGE_MASK);
            paddr_write(pa, len, data);
            return;
        }

        if (st == MEM_RET_CROSS_PAGE) 
        {
            assert(0 && "vaddr_write: cross-page access not supported yet");
        }

        assert(0 && "vaddr_write: mmu translate failed");
    }

    assert(0 && "vaddr_write: mmu check failed");
}
