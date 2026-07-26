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

/* This implementation uses one address space at a time, so VPN2 is the complete TLB tag. */
static bool tlb_entry_matches(const mips32_TLB_entry *entry, word_t entryhi)
{
    return (entry->entryhi & MIPS32_ENTRYHI_VPN2_MASK) ==
           (entryhi & MIPS32_ENTRYHI_VPN2_MASK);
}

void mips32_tlb_reset(void)
{
    /*
     * Give every invalid slot a distinct high VPN2 so ordinary low-address
     * probes cannot accidentally match zero-filled reset state.
     */
    for (uint32_t i = 0; i < MIPS32_TLB_NR; i++)
    {
        cpu.tlb[i].entryhi = 0x80000000u | (i << 13);
        cpu.tlb[i].entrylo0 = 0;
        cpu.tlb[i].entrylo1 = 0;
    }

    cpu.tlbwr_next = 0;
    cpu.index = MIPS32_INDEX_P;
}

void mips32_tlbp(void)
{
    for (uint32_t i = 0; i < MIPS32_TLB_NR; i++)
    {
        if (tlb_entry_matches(&cpu.tlb[i], cpu.entryhi))
        {
            /* Return the first match to keep duplicate-entry behaviour stable. */
            cpu.index = i;
            return;
        }
    }

    /* The P bit records that no TLB entry matched EntryHi. */
    cpu.index = MIPS32_INDEX_P;
}

static void write_tlb_slot(uint32_t index)
{
    Assert(index < MIPS32_TLB_NR,
           "MIPS32 TLB index out of range: %u", index);
    cpu.tlb[index].entryhi = cpu.entryhi & MIPS32_ENTRYHI_VPN2_MASK;
    cpu.tlb[index].entrylo0 = cpu.entrylo0;
    cpu.tlb[index].entrylo1 = cpu.entrylo1;
}

void mips32_tlbwi(void)
{
    /* Reserved Index bits are not a valid indexed-write destination. */
    Assert((cpu.index & ~0x0fu) == 0,
           "MIPS32 TLBWI invalid Index=" FMT_WORD, cpu.index);
    write_tlb_slot(cpu.index);
}

void mips32_tlbwr(void)
{
    /*
     * NEMU does not model the Random and Wired registers. A round-robin cursor
     * therefore supplies a deterministic replacement choice.
     */
    write_tlb_slot(cpu.tlbwr_next);
    cpu.tlbwr_next = (cpu.tlbwr_next + 1u) % MIPS32_TLB_NR;
}

int isa_mmu_check(vaddr_t vaddr, int len, int type)
{
    (void)len;
    (void)type;

    /* User addresses in kuseg always pass through the software-managed TLB. */
    if (vaddr < 0x80000000u)
    {
        return MMU_TRANSLATE;
    }

    /*
     * kseg0 (0x80000000--0x9fffffff) and kseg1
     * (0xa0000000--0xbfffffff) are the two unmapped kernel segments. This
     * MIPS32 MMU implementation does not support mapped kseg2 or kseg3
     * addresses.
     */
    if (vaddr < 0xc0000000u)
    {
        return MMU_DIRECT;
    }

    return MMU_FAIL;
}

static paddr_t record_tlb_fault(vaddr_t vaddr, mips32_TLB_fault fault)
{
    cpu.badvaddr = vaddr;
    cpu.entryhi = vaddr & MIPS32_ENTRYHI_VPN2_MASK;
    cpu.tlb_fault = fault;
    return (paddr_t)MEM_RET_FAIL;
}

paddr_t isa_mmu_translate(vaddr_t vaddr, int len, int type)
{
    const uint32_t page_offset = vaddr & 0xfffu;
    const bool store = type == MEM_TYPE_WRITE;

    /* Do not allow a previous failure to leak into a later translation. */
    cpu.tlb_fault = MIPS32_TLB_FAULT_NONE;

    /*
     * Translation returns a page base, so the common memory layer must split
     * an access that reaches into the next 4 KiB page before either half is
     * translated independently.
     */
    if (len <= 0 || (uint32_t)len > 4096u - page_offset)
    {
        return (paddr_t)MEM_RET_CROSS_PAGE;
    }

    for (uint32_t i = 0; i < MIPS32_TLB_NR; i++)
    {
        const mips32_TLB_entry *entry = &cpu.tlb[i];

        if (!tlb_entry_matches(entry, vaddr))
        {
            continue;
        }

        /* Virtual bit 12 selects the odd (EntryLo1) half of a VPN2 pair. */
        const word_t entrylo = (vaddr & 0x1000u) != 0
                                   ? entry->entrylo1
                                   : entry->entrylo0;

        Assert((entrylo & MIPS32_ENTRYLO_V) != 0,
               "MIPS32 invalid TLB mapping for vaddr=" FMT_WORD, vaddr);
        Assert(!store || (entrylo & MIPS32_ENTRYLO_D) != 0,
               "MIPS32 read-only TLB mapping written at vaddr=" FMT_WORD,
               vaddr);

        /* EntryLo.PFN bits 25:6 become physical address bits 31:12. */
        return (paddr_t)((entrylo & MIPS32_ENTRYLO_PFN_MASK) << 6) |
               (paddr_t)MEM_RET_OK;
    }

    return record_tlb_fault(
        vaddr, store ? MIPS32_TLB_FAULT_REFILL_STORE
                     : MIPS32_TLB_FAULT_REFILL_LOAD);
}

/* Translate one debugger byte without updating CP0 or the pending fault. */
static bool debug_translate(vaddr_t vaddr, paddr_t *paddr)
{
    const int mmu = isa_mmu_check(vaddr, 1, MEM_TYPE_READ);

    if (mmu == MMU_DIRECT)
    {
        *paddr = (paddr_t)vaddr;
        return true;
    }

    if (mmu != MMU_TRANSLATE)
    {
        return false;
    }

    for (uint32_t i = 0; i < MIPS32_TLB_NR; i++)
    {
        const mips32_TLB_entry *entry = &cpu.tlb[i];

        if (!tlb_entry_matches(entry, vaddr))
        {
            continue;
        }

        const word_t entrylo = (vaddr & 0x1000u) != 0
                                   ? entry->entrylo1
                                   : entry->entrylo0;

        if ((entrylo & MIPS32_ENTRYLO_V) == 0)
        {
            return false;
        }

        *paddr = (paddr_t)((entrylo & MIPS32_ENTRYLO_PFN_MASK) << 6) |
                 (paddr_t)(vaddr & 0xfffu);
        return true;
    }

    return false;
}

bool mips32_debug_vaddr_read(vaddr_t addr, int len, word_t *value)
{
    if (value == NULL || len <= 0 || len > (int)sizeof(word_t))
    {
        return false;
    }

    paddr_t first_paddr;
    if ((uint32_t)len <= 4096u - (addr & 0xfffu) &&
        debug_translate(addr, &first_paddr))
    {
        *value = paddr_read(first_paddr, len);
        return true;
    }

    word_t data = 0;

    for (int i = 0; i < len; i++)
    {
        paddr_t paddr;

        if (!debug_translate(addr + (vaddr_t)i, &paddr))
        {
            return false;
        }

        data |= paddr_read(paddr, 1) << (i * 8);
    }

    *value = data;
    return true;
}
