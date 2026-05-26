/***************************************************************************************
 * Copyright (c) 2014-2024 Zihao Yu, Nanjing University
 *
 * NEMU is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 *
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 *
 * See the Mulan PSL v2 for more details.
 ***************************************************************************************/

#include <isa.h>
#include <memory/paddr.h>
#include <memory/vaddr.h>

#define X86_CR0_WP 0x00010000u
#define X86_CR0_PG 0x80000000u
#define X86_CR4_PSE 0x00000010u
#define X86_PF_P 0x001u
#define X86_PF_WR 0x002u
#define X86_PF_US 0x004u
#define X86_PF_RSVD 0x008u
#define X86_PF_ID 0x010u
#define X86_PTE_P 0x001u
#define X86_PTE_W 0x002u
#define X86_PTE_U 0x004u
#define X86_PTE_A 0x020u
#define X86_PTE_D 0x040u
#define X86_PTE_PS 0x080u
#define X86_PAGE_MASK ((paddr_t)PAGE_MASK)
#define X86_PAGE_4M_MASK ((paddr_t)0x003fffffu)
#define X86_PTE_ADDR_MASK ((paddr_t)0xfffff000u)
#define X86_PDE_4M_ADDR_MASK ((paddr_t)0xffc00000u)

static int cpl_override = -1;

static int x86_mmu_cpl(void)
{
    return cpl_override >= 0 ? cpl_override : (cpu.cs & 0x3);
}

void x86_mmu_clear_cpl_override(void)
{
    cpl_override = -1;
}

static bool x86_page_perm_fail(uint32_t entry, int type)
{
    int cpl = x86_mmu_cpl();

    if (cpl == 3 && (entry & X86_PTE_U) == 0)
    {
        return true;
    }

    if (type == MEM_TYPE_WRITE && (entry & X86_PTE_W) == 0)
    {
        return cpl == 3 || (cpu.cr0 & X86_CR0_WP) != 0;
    }

    return false;
}

static uint32_t x86_page_fault_error_code(int type, bool protection, bool rsvd)
{
    uint32_t err = protection ? X86_PF_P : 0;

    if (type == MEM_TYPE_WRITE)
    {
        err |= X86_PF_WR;
    }

    if (x86_mmu_cpl() == 3)
    {
        err |= X86_PF_US;
    }

    if (rsvd)
    {
        err |= X86_PF_RSVD;
    }

    if (type == MEM_TYPE_IFETCH)
    {
        err |= X86_PF_ID;
    }

    return err;
}

static paddr_t x86_mmu_fail(vaddr_t vaddr, int type, bool protection, bool rsvd)
{
    cpu.cr2 = vaddr;
    cpu.pf_errcode = x86_page_fault_error_code(type, protection, rsvd);
    return MEM_RET_FAIL;
}

static uint32_t x86_set_ad_bits(paddr_t entry_addr, uint32_t entry, bool dirty)
{
    uint32_t updated = entry | X86_PTE_A;
    if (dirty)
    {
        updated |= X86_PTE_D;
    }

    if (updated != entry)
    {
        /*
         * Setting Accessed/Dirty bits does not change the physical page or loosen
         * permissions, so existing JIT DTLB entries remain valid.  The guest still
         * observes the architectural PTE/PDE update in PMEM.
         */
        paddr_write_no_jit_invalidate(entry_addr, 4, updated);
    }

    return updated;
}

word_t x86_vaddr_read_kernel(vaddr_t addr, int len)
{
    int old = cpl_override;
    cpl_override = 0;
    word_t data = vaddr_read(addr, len);
    cpl_override = old;
    return data;
}

void x86_vaddr_write_kernel(vaddr_t addr, int len, word_t data)
{
    int old = cpl_override;
    cpl_override = 0;
    vaddr_write(addr, len, data);
    cpl_override = old;
}

int isa_mmu_check(vaddr_t vaddr, int len, int type)
{
    (void)vaddr;
    (void)len;
    (void)type;

    return (cpu.cr0 & X86_CR0_PG) ? MMU_TRANSLATE : MMU_DIRECT;
}

paddr_t isa_mmu_translate(vaddr_t vaddr, int len, int type)
{
    if (((paddr_t)(vaddr & X86_PAGE_MASK) + (paddr_t)len) > PAGE_SIZE)
    {
        return MEM_RET_CROSS_PAGE;
    }

    paddr_t pd_base = (paddr_t)(cpu.cr3 & X86_PTE_ADDR_MASK);
    uint32_t pde_idx = (uint32_t)((vaddr >> 22) & 0x3ffu);
    uint32_t pte_idx = (uint32_t)((vaddr >> 12) & 0x3ffu);
    paddr_t pde_addr = pd_base + (paddr_t)pde_idx * 4u;
    uint32_t pde = (uint32_t)paddr_read(pde_addr, 4);

    if ((pde & X86_PTE_P) == 0)
    {
        return x86_mmu_fail(vaddr, type, false, false);
    }

    if ((pde & X86_PTE_PS) != 0 && (cpu.cr4 & X86_CR4_PSE) == 0)
    {
        /*
         * In 32-bit non-PAE paging, PDE.PS is reserved until CR4.PSE is enabled.
         * Treating it as a 4 MiB page unconditionally hides malformed page tables
         * from AM/Nanos page-fault tests and from any future reference difftest.
         */
        return x86_mmu_fail(vaddr, type, true, true);
    }

    if (x86_page_perm_fail(pde, type))
    {
        return x86_mmu_fail(vaddr, type, true, false);
    }

    if ((pde & X86_PTE_PS) != 0)
    {
        pde = x86_set_ad_bits(pde_addr, pde, type == MEM_TYPE_WRITE);
        paddr_t page = (paddr_t)(pde & X86_PDE_4M_ADDR_MASK);
        return (page | (paddr_t)(vaddr & X86_PAGE_4M_MASK)) & ~X86_PAGE_MASK;
    }

    pde = x86_set_ad_bits(pde_addr, pde, false);

    paddr_t pt_base = (paddr_t)(pde & X86_PTE_ADDR_MASK);
    paddr_t pte_addr = pt_base + (paddr_t)pte_idx * 4u;
    uint32_t pte = (uint32_t)paddr_read(pte_addr, 4);

    if ((pte & X86_PTE_P) == 0)
    {
        return x86_mmu_fail(vaddr, type, false, false);
    }

    if (x86_page_perm_fail(pte, type))
    {
        return x86_mmu_fail(vaddr, type, true, false);
    }

    pte = x86_set_ad_bits(pte_addr, pte, type == MEM_TYPE_WRITE);
    return (paddr_t)(pte & X86_PTE_ADDR_MASK) | MEM_RET_OK;
}
