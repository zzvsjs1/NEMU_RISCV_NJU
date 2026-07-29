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
#ifdef CONFIG_RISCV_D
#define DEFAULT_ISA "RV32IMFD_Zicsr_Zifencei"
#elif defined(CONFIG_RISCV_FPU)
#define DEFAULT_ISA "RV32IMF_Zicsr_Zifencei"
#else
#define DEFAULT_ISA MUXDEF(CONFIG_RVE, "RV32EM_Zicsr_Zifencei", "RV32IM_Zicsr_Zifencei")
#endif
#endif

#ifdef CONFIG_ISA_riscv64
#undef DEFAULT_ISA
#ifdef CONFIG_RV64_FPU
#define DEFAULT_ISA "RV64IMFD_Zicsr_Zifencei"
#else
#define DEFAULT_ISA "RV64IM_Zicsr_Zifencei"
#endif
#endif

#ifdef CONFIG_RISCV_FPU
/*
 * The privileged architecture places the two-bit FS state in mstatus[14:13].
 * Encoding 01 means Initial, so only the field's low bit (mstatus[13]) is set.
 * Spike exposes the complete MSTATUS_FS field mask but no state-specific name
 * through the interface used here; keep the encoding in one descriptive
 * constant and verify at compile time that it remains inside Spike's FS field.
 */
static constexpr reg_t SPIKE_MSTATUS_FS_INITIAL = (reg_t)1 << 13;
static_assert((SPIKE_MSTATUS_FS_INITIAL & MSTATUS_FS) ==
                  SPIKE_MSTATUS_FS_INITIAL,
              "Spike FS Initial encoding is outside mstatus.FS");
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
    riscv_difftest_state_t ctx;
    s->diff_get_regs(&ctx);
    // printf("Spike PC = 0x%x\n", ctx.pc);
}

void sim_t::diff_get_regs(void *diff_context)
{
    riscv_difftest_state_t *ctx = (riscv_difftest_state_t *)diff_context;

    for (int i = 0; i < RISCV_GPR_NUM; i++)
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

#ifdef CONFIG_RISCV_FPU
    for (int i = 0; i < RISCV_DIFFTEST_FPR_NUM; i++)
    {
        /*
         * Spike stores FPRs in a 128-bit container so it can also model Q.
         * DiffTest exposes only the architectural FLEN-width value: RV32F
         * takes the low 32 bits, while RV32D and RV64D use the complete low
         * 64-bit lane.
         */
#ifdef CONFIG_RISCV_D
        ctx->fpr[i] = state->FPR[i].v[0];
#else
        ctx->fpr[i] = (uint32_t)state->FPR[i].v[0];
#endif
    }

    ctx->fcsr =
        diff_read_csr(CSR_FCSR) & RISCV_DIFFTEST_FCSR_MASK;
#endif
}

void sim_t::diff_set_regs(void *diff_context)
{
    riscv_difftest_state_t *ctx = (riscv_difftest_state_t *)diff_context;

    for (int i = 0; i < RISCV_GPR_NUM; i++)
    {
        state->XPR.write(i, (sword_t)ctx->gpr[i]);
    }

    state->pc = ctx->pc;

    diff_write_csr(CSR_SATP, ctx->csr.satp);
#ifdef CONFIG_RISCV_FPU
    /*
     * Spike treats an fcsr write as architectural FP use: it marks
     * mstatus.FS Dirty and deliberately aborts if FS is Off.  DiffTest state
     * restoration is different from guest execution, though, and must also
     * be able to restore a reset state whose FS field is Off.
     *
     * Temporarily select the Initial state when necessary, restore fcsr, and
     * then write the requested mstatus again below.  The final write is
     * needed for every non-Off state too, because Spike's fcsr write changes
     * Clean or Initial to Dirty.
     */
    reg_t target_mstatus = ctx->csr.mstatus;
    reg_t fp_restore_mstatus = target_mstatus;

    if ((fp_restore_mstatus & MSTATUS_FS) == 0)
    {
        fp_restore_mstatus |= SPIKE_MSTATUS_FS_INITIAL;
    }

    diff_write_csr(CSR_MSTATUS, fp_restore_mstatus);
#else
    diff_write_csr(CSR_MSTATUS, ctx->csr.mstatus);
#endif
    diff_write_csr(CSR_MTVEC, ctx->csr.mtvec);
    diff_write_csr(CSR_MSCRATCH, ctx->csr.mscratch);
    diff_write_csr(CSR_MEPC, ctx->csr.mepc);
    diff_write_csr(CSR_MCAUSE, ctx->csr.mcause);
    diff_write_csr(CSR_MTVAL, ctx->csr.mtval);
    state->prv = ctx->prvi;

#ifdef CONFIG_RISCV_FPU
    for (int i = 0; i < RISCV_DIFFTEST_FPR_NUM; i++)
    {
        freg_t value;
#ifdef CONFIG_RISCV_D
        value.v[0] = ctx->fpr[i];
#else
        /*
         * Spike's internal freg_t is 128 bits.  A valid binary32 value must
         * therefore be NaN-boxed with every bit above the payload set to one.
         */
        value.v[0] = UINT64_C(0xffffffff00000000) | ctx->fpr[i];
#endif
        value.v[1] = UINT64_MAX;
        state->FPR.write(i, value);
    }

    diff_write_csr(CSR_FCSR,
                   ctx->fcsr & RISCV_DIFFTEST_FCSR_MASK);
    diff_write_csr(CSR_MSTATUS, target_mstatus);
#endif
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
        cfg_t *cfg = new cfg_t(/*default_initrd_bounds=*/std::make_pair((reg_t)0, (reg_t)0),
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
        s = new sim_t(cfg, false,
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
