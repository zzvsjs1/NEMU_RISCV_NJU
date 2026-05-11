#include "debug.h"
#include <isa.h>
#include <memory/paddr.h>
#include <memory/vaddr.h>

#ifdef CONFIG_ISA_riscv32
#define RISCV32_SATP_MODE_MASK 0x80000000u

static inline bool rv32_mmu_direct_mode()
{
    return likely((cpu.csr.satp & RISCV32_SATP_MODE_MASK) == 0);
}
#endif

/*
 * isa_mmu_translate() packs a page-aligned physical page address with a small
 * status code in the low PAGE_MASK bits.  Splitting those two parts here keeps
 * the virtual-memory users from depending on the exact bit layout directly.
 */
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
#ifdef CONFIG_ISA_riscv32
    /*
     * Bare-mode RISC-V instruction fetch is the hot path.  Bypass the generic
     * MMU check when satp.MODE is clear; paged mode still goes through the
     * common translation path so faults and cross-page limits stay centralised.
     */
    if (rv32_mmu_direct_mode())
    {
        return paddr_ifetch((paddr_t)addr);
    }
#endif

    int mmu = isa_mmu_check(addr, len, MEM_TYPE_IFETCH);

    if (mmu == MMU_DIRECT)
    {
#ifdef CONFIG_ISA_riscv32
        if (likely(len == 4))
        {
            return paddr_ifetch((paddr_t)addr);
        }
#endif
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
#ifdef CONFIG_ISA_riscv32
            if (likely(len == 4))
            {
                return paddr_ifetch(pa);
            }
#endif
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
#ifdef CONFIG_ISA_riscv32
    if (rv32_mmu_direct_mode())
    {
        return paddr_read((paddr_t)addr, len);
    }
#endif

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
#ifdef CONFIG_ISA_riscv32
    /*
     * Direct-mode writes intentionally still call paddr_write(), not pmem_write().
     * Physical memory decoding, device dispatch, and JIT invalidation hooks all
     * live below this boundary. That boundary matters for Bare-mode JIT tests:
     * an apparently simple store can still be self-modifying code or MMIO once
     * the physical address is decoded.
     */
    if (rv32_mmu_direct_mode())
    {
        paddr_write((paddr_t)addr, len, data);
        return;
    }
#endif

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
