#include "debug.h"
#include <isa.h>
#include <memory/paddr.h>
#include <memory/vaddr.h>

#ifdef CONFIG_ISA_riscv32
static inline word_t rv32_effective_mem_priv(int type)
{
    if (type == MEM_TYPE_IFETCH)
    {
        return cpu.prvi;
    }

    if (cpu.prvi == RISCV32_PRIV_M &&
        (cpu.csr.mstatus & RISCV_MSTATUS_MPRV) != 0)
    {
        return (cpu.csr.mstatus & RISCV_MSTATUS_MPP_MASK) >>
               RISCV_MSTATUS_MPP_SHIFT;
    }

    return cpu.prvi;
}

static inline bool rv32_mmu_direct_mode(int type)
{
    return likely((cpu.csr.satp & RISCV32_SATP_MODE_MASK) ==
                      RISCV_SATP_MODE_BARE ||
                  rv32_effective_mem_priv(type) == RISCV32_PRIV_M);
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

static void __attribute__((noreturn)) handle_translate_failure(int status, const char *caller)
{
#ifdef CONFIG_ISA_x86
    if (status == MEM_RET_FAIL)
    {
        x86_raise_page_fault();
    }
#endif

#ifdef CONFIG_ISA_mips32
    if (status == MEM_RET_FAIL)
    {
        mips32_raise_tlb_exception();
    }
#endif

    panic("%s: mmu translate failed", caller);
}

static paddr_t translated_paddr_or_panic(vaddr_t addr, int len, int type, const char *caller)
{
    paddr_t ret = isa_mmu_translate(addr, len, type);
    int st = mem_ret_status(ret);

    if (st != MEM_RET_OK)
    {
        handle_translate_failure(st, caller);
    }

    return mem_ret_pgaddr(ret) | (paddr_t)(addr & PAGE_MASK);
}

word_t vaddr_ifetch(vaddr_t addr, int len)
{
#ifdef CONFIG_ISA_riscv32
    /*
     * Bare-mode RISC-V instruction fetch is the hot path.  Bypass the generic
     * MMU check when satp.MODE is clear; paged mode still goes through the
     * common translation path so faults and cross-page limits stay centralised.
     */

    if (rv32_mmu_direct_mode(MEM_TYPE_IFETCH))
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
            word_t data = 0;

            for (int i = 0; i < len; i++)
            {
                data |= vaddr_ifetch(addr + (vaddr_t)i, 1) << (i * 8);
            }

            return data;
        }

        handle_translate_failure(st, "vaddr_ifetch");
    }

    // MMU_FAIL or others
    panic("vaddr_ifetch: mmu check failed");
    return 0;
}

word_t vaddr_read(vaddr_t addr, int len)
{
#ifdef CONFIG_ISA_riscv32
    if (rv32_mmu_direct_mode(MEM_TYPE_READ))
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
            word_t data = 0;

            for (int i = 0; i < len; i++)
            {
                data |= vaddr_read(addr + (vaddr_t)i, 1) << (i * 8);
            }

            return data;
        }

        handle_translate_failure(status, "vaddr_read");
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

    if (rv32_mmu_direct_mode(MEM_TYPE_WRITE))
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
            int first_len = PAGE_SIZE - (int)(addr & PAGE_MASK);
            int second_len = len - first_len;
            paddr_t first_pa = translated_paddr_or_panic(addr, first_len,
                                                         MEM_TYPE_WRITE, "vaddr_write");
            paddr_t second_pa = translated_paddr_or_panic(addr + (vaddr_t)first_len, second_len,
                                                          MEM_TYPE_WRITE, "vaddr_write");

            paddr_write(first_pa, first_len, data);
            paddr_write(second_pa, second_len, data >> (first_len * 8));
            return;
        }

        handle_translate_failure(st, "vaddr_write");
    }

    assert(0 && "vaddr_write: mmu check failed");
}
