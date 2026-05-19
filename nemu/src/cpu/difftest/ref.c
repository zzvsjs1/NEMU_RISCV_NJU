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
#include <cpu/cpu.h>
#include <difftest-def.h>
#include <memory/paddr.h>
#include <utils.h>

static bool ref_pmem_range(paddr_t addr, size_t n)
{
    if (n == 0)
    {
        return true;
    }

    if (addr < (paddr_t)CONFIG_MBASE)
    {
        return false;
    }

    const uint64_t offset = (uint64_t)(addr - (paddr_t)CONFIG_MBASE);
    return offset < (uint64_t)CONFIG_MSIZE &&
           n <= (size_t)((uint64_t)CONFIG_MSIZE - offset);
}

__EXPORT void difftest_memcpy(paddr_t addr, void *buf, size_t n, bool direction)
{
    Assert(ref_pmem_range(addr, n),
           "DiffTest reference memory copy out of PMEM: addr=" FMT_PADDR ", size=%zu",
           addr, n);

    if (n == 0)
    {
        return;
    }

    assert(buf != NULL);
    if (direction == DIFFTEST_TO_REF)
    {
        memcpy(guest_to_host(addr), buf, n);
    }
    else
    {
        assert(direction == DIFFTEST_TO_DUT);
        memcpy(buf, guest_to_host(addr), n);
    }
}

__EXPORT void difftest_regcpy(void *dut, bool direction)
{
    assert(dut != NULL);
    if (direction == DIFFTEST_TO_REF)
    {
        memcpy(&cpu, dut, sizeof(cpu));
    }
    else
    {
        assert(direction == DIFFTEST_TO_DUT);
        memcpy(dut, &cpu, sizeof(cpu));
    }
}

__EXPORT void difftest_exec(uint64_t n)
{
    cpu_exec(n);
}

__EXPORT void difftest_raise_intr(uint64_t NO)
{
    cpu.pc = isa_raise_intr((word_t)NO, cpu.pc);
}

__EXPORT void difftest_init(int port)
{
    void init_mem();
    init_mem();
    /* Perform ISA dependent initialization. */
    init_isa();
}
