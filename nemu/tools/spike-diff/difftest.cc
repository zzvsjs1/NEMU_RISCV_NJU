/***************************************************************************************
* Copyright (c) 2014-2022 Zihao Yu, Nanjing University
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

#include "mmu.h"
#include "sim.h"
#include "../../include/common.h"
#include <difftest-def.h>

#ifdef CONFIG_ISA_riscv32
#undef DEFAULT_ISA
#define DEFAULT_ISA "RV32IM"
#endif

#ifdef CONFIG_ISA_riscv64
#undef DEFAULT_ISA
#define DEFAULT_ISA "RV64IM_Zicsr_Zifencei"
#endif

static std::vector<std::pair<reg_t, abstract_device_t *>> difftest_plugin_devices;
static std::vector<std::string> difftest_htif_args;
static std::vector<std::pair<reg_t, mem_t *>> difftest_mem(
    1, std::make_pair(reg_t(DRAM_BASE), new mem_t(CONFIG_MSIZE)));
static std::vector<int> difftest_hartids;
static debug_module_config_t difftest_dm_config = {
    .progbufsize = 2,
    .max_sba_data_width = 0,
    .require_authentication = false,
    .abstract_rti = 0,
    .support_hasel = true,
    .support_abstract_csr_access = true,
    .support_abstract_fpr_access = true,
    .support_haltgroups = true,
    .support_impebreak = true};

struct diff_context_t
{
    word_t gpr[32];
    word_t pc;

    struct
    {
        // Supervisor address translation and protection register.
        // 0x180
        rtlreg_t satp;

        // Machine status register.
        // 0x300
        rtlreg_t mstatus;

        // Machine trap-handler base address.
        // 0x305
        rtlreg_t mtvec;

        // Machine scratch register.
        // 0x340
        rtlreg_t mscratch;

        // Machine exception program counter.
        // 0x341
        rtlreg_t mepc;

        // Machine trap cause
        // 0x342
        rtlreg_t mcause;

        // Machine trap value.
        // 0x343
        rtlreg_t mtval;
    } csr;

    rtlreg_t prvi;
    bool INTR;
};

static sim_t *s = NULL;
static processor_t *p = NULL;
static state_t *state = NULL;

static reg_t diff_read_csr(reg_t csr_addr)
{
    auto item = state->csrmap.find(csr_addr);
    return item == state->csrmap.end() ? 0 : item->second->read();
}

static void diff_write_csr(reg_t csr_addr, reg_t value)
{
    auto item = state->csrmap.find(csr_addr);
    if (item != state->csrmap.end())
    {
        item->second->write(value);
    }
}

void sim_t::diff_init(int port)
{
    p = get_core("0");
    state = p->get_state();
}

void sim_t::diff_step(uint64_t n)
{
    step(n);
    struct diff_context_t ctx;
    s->diff_get_regs(&ctx);
    // printf("Spike PC = 0x%x\n", ctx.pc);
}

void sim_t::diff_get_regs(void *diff_context)
{
    struct diff_context_t *ctx = (struct diff_context_t *)diff_context;
    for (int i = 0; i < NXPR; i++)
    {
        ctx->gpr[i] = state->XPR[i];
    }

    ctx->pc = state->pc;

    ctx->csr.satp = diff_read_csr(CSR_SATP);
    ctx->csr.mstatus = diff_read_csr(CSR_MSTATUS);
    ctx->csr.mtvec = diff_read_csr(CSR_MTVEC);
    ctx->csr.mscratch = diff_read_csr(CSR_MSCRATCH);
    ctx->csr.mepc = diff_read_csr(CSR_MEPC);
    ctx->csr.mcause = diff_read_csr(CSR_MCAUSE);
    ctx->csr.mtval = diff_read_csr(CSR_MTVAL);

    ctx->prvi = state->prv;
    ctx->INTR = false;
}

void sim_t::diff_set_regs(void *diff_context)
{
    struct diff_context_t *ctx = (struct diff_context_t *)diff_context;
    for (int i = 0; i < NXPR; i++)
    {
        state->XPR.write(i, (sword_t)ctx->gpr[i]);
    }
    state->pc = ctx->pc;

    diff_write_csr(CSR_SATP, ctx->csr.satp);
    diff_write_csr(CSR_MSTATUS, ctx->csr.mstatus);
    diff_write_csr(CSR_MTVEC, ctx->csr.mtvec);
    diff_write_csr(CSR_MSCRATCH, ctx->csr.mscratch);
    diff_write_csr(CSR_MEPC, ctx->csr.mepc);
    diff_write_csr(CSR_MCAUSE, ctx->csr.mcause);
    diff_write_csr(CSR_MTVAL, ctx->csr.mtval);
    state->prv = ctx->prvi;
}

void sim_t::diff_memcpy(reg_t dest, void *src, size_t n)
{
    mmu_t *mmu = p->get_mmu();
    for (size_t i = 0; i < n; i++)
    {
        mmu->store<uint8_t>(dest + i, *((uint8_t *)src + i));
    }
}

extern "C"
{

    __EXPORT void difftest_memcpy(paddr_t addr, void *buf, size_t n, bool direction)
    {
        if (direction == DIFFTEST_TO_REF)
        {
            s->diff_memcpy(addr, buf, n);
        }
        else
        {
            assert(0);
        }
    }

    __EXPORT void difftest_regcpy(void *dut, bool direction)
    {
        if (direction == DIFFTEST_TO_REF)
        {
            s->diff_set_regs(dut);
        }
        else
        {
            s->diff_get_regs(dut);
        }
    }

    __EXPORT void difftest_exec(uint64_t n)
    {
        s->diff_step(n);
    }

    __EXPORT void difftest_init(int port)
    {
        difftest_htif_args.push_back("");
        cfg_t cfg(/*default_initrd_bounds=*/std::make_pair((reg_t)0, (reg_t)0),
                  /*default_bootargs=*/nullptr,
                  /*default_isa=*/DEFAULT_ISA,
                  /*default_priv=*/DEFAULT_PRIV,
                  /*default_varch=*/DEFAULT_VARCH,
                  /*default_misaligned=*/false,
                  /*default_endianness*/ endianness_little,
                  /*default_pmpregions=*/16,
                  /*default_mem_layout=*/std::vector<mem_cfg_t>(),
                  /*default_hartids=*/std::vector<size_t>(1),
                  /*default_real_time_clint=*/false,
                  /*default_trigger_count=*/4);
        s = new sim_t(&cfg, false,
                      difftest_mem, difftest_plugin_devices, difftest_htif_args,
                      difftest_dm_config, nullptr, false, NULL,
                      false,
                      NULL,
                      true);
        s->diff_init(port);
    }

    __EXPORT void difftest_raise_intr(uint64_t NO)
    {
        trap_t t(NO);
        // printf("Before: %08lx\n", state->mtvec.get());
        p->take_trap_public(t, state->pc);
        // printf("After status: %08lx\n", state->mstatus.get()->read());
        // printf("After mcause: %08lx\n", state->mcause.get()->read());

        // **Before** any ref_difftest_exec, copy regs back and print PC**
        // struct diff_context_t ctx;
        // s->diff_get_regs(&ctx);
        // printf("Spike PC after raise_intr = 0x%x\n", ctx.pc);
    }
}
