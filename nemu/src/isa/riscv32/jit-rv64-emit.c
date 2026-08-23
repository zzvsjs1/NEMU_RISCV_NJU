#include <generated/autoconf.h>

#ifdef CONFIG_RV64

#include "jit-rv64-internal.h"

#ifdef CONFIG_DEVICE
#include <device/map.h>
#endif

#ifdef CONFIG_RISCV_FPU
#include "local-include/fpu.h"
#endif

/*
 * RV64 JIT emitter layer: x86-64 byte writing, generated-code ABI helpers,
 * register-cache emission, direct-link exits and instruction emitters.
 *
 * Reading map:
 *   1. x86-64 encoding and generated-code ABI primitives;
 *   2. compile-time guest-register cache;
 *   3. block exits, side exits, and guarded direct links;
 *   4. Bare/Sv39 load and store fast paths;
 *   5. RV64I/RV64M integer semantics;
 *   6. branch, JAL/JALR, and top-level opcode dispatch.
 *
 * Helpers named `emit_*` write x86-64 bytes.  Functions named after RV64
 * instructions define guest semantics by composing those host primitives.
 */
/*
 * x86-64 emitter ABI.
 *
 * Generated code is a normal C-callable function returning a 32-bit retired
 * guest-instruction count in EAX.  The prologue saves RBX, RBP and R12-R15 so
 * they can cache guest registers for the whole block.  R11 always points at the
 * global `CPU_state`, and R10 holds the PMEM host base for direct memory
 * operations. RAX, RCX, RDX and RDI are scratch. R8 is normally scratch, but a
 * proven helper-free self-loop may use it as one extra stable cache slot.
 *
 * The extra 8-byte stack adjustment keeps the System V stack aligned before
 * helper calls. Every dirty GPR which a helper can read is published before
 * that call. Audited non-memory FP helpers may retain unrelated dirty values
 * in the callee-saved cache registers, publishing them only on a precise trap
 * edge; unknown and memory effects retain the full barrier. Every native or
 * interpreter exit publishes complete architectural state. All RV64M
 * arithmetic is helper-free; its host instructions may use scratch registers
 * but preserve every stable-loop cache register.
 */

/* x86-64 host opcode suffixes used by the RV64 guest-code emitter. */
enum
{
    HOST_JCC_B = 0x82,
    HOST_JCC_AE = 0x83,
    HOST_JCC_E = 0x84,
    HOST_JCC_NE = 0x85,
    HOST_JCC_A = 0x87,
    HOST_JCC_L = 0x8c,
    HOST_JCC_GE = 0x8d,
    HOST_SETCC_B = 0x92,
    HOST_SETCC_L = 0x9c,
};

typedef enum
{
    HOST_REG_RAX = 0,
    HOST_REG_RCX = 1,
    HOST_REG_RDX = 2,
    HOST_REG_RBX = 3,
    HOST_REG_RBP = 5,
    HOST_REG_RSI = 6,
    HOST_REG_RDI = 7,
    HOST_REG_R8 = 8,
    HOST_REG_R10 = 10,
    HOST_REG_R11 = 11,
    HOST_REG_R12 = 12,
    HOST_REG_R13 = 13,
    HOST_REG_R14 = 14,
    HOST_REG_R15 = 15,
} host_reg_t;

#ifdef CONFIG_RISCV_FPU
/* FP memory helpers always form a conservative native-block boundary. */
static bool rv64_jit_fp_opcode_is_memory(uint32_t opcode) { return opcode == RV64_FP_OPCODE_LOAD || opcode == RV64_FP_OPCODE_STORE; }

#if RV64_JIT_STATS
static bool rv64_jit_test_fp_mmio_initialised = false;
static bool rv64_jit_test_fp_mmio_enabled = false;
static bool rv64_jit_test_fp_mmio_injected = false;

/*
 * Inject one deterministic post-MMIO interrupt edge for the focused boundary
 * regression. Production builds remove the hook, and statistics builds enable
 * it only through the explicit test environment variable.
 */
static bool rv64_jit_should_test_fp_mmio_boundary(void)
{
    if (!rv64_jit_test_fp_mmio_initialised)
    {
        const char *value =
            getenv("NEMU_RV64_JIT_TEST_FP_MMIO_BOUNDARY");
        rv64_jit_test_fp_mmio_enabled =
            value != NULL && value[0] != '\0' &&
            !(value[0] == '0' && value[1] == '\0');
        rv64_jit_test_fp_mmio_initialised = true;
    }

    return rv64_jit_test_fp_mmio_enabled &&
           !rv64_jit_test_fp_mmio_injected;
}

/* Consume the one-shot only after the FP memory helper has completed. */
static void rv64_jit_commit_test_fp_mmio_boundary(void)
{
    rv64_jit_test_fp_mmio_injected = true;
}
#endif

/*
 * Keep helper-entry and trap accounting beside the shared FP executor.
 * Successful outcomes are counted on their generated native edges below, so
 * the profile describes executed control flow rather than wrapper-side opcode
 * classification.
 */
static uint32_t rv64_jit_exec_fpu(uint32_t instr, vaddr_t pc)
{
    const bool is_memory =
        rv64_jit_fp_opcode_is_memory(instr & RV64_OPCODE_MASK);

#if RV64_JIT_STATS
    const bool pending_before_test_hook = cpu.INTR;
    const bool inject_test_interrupt =
        is_memory && rv64_jit_should_test_fp_mmio_boundary();

    if (inject_test_interrupt)
    {
        /*
         * Remove an asynchronous pre-existing edge so the focused hook always
         * creates a real false-to-true transition for the production predicate.
         */
        cpu.INTR = false;
    }
#endif

    const bool interrupt_was_pending = cpu.INTR;

    JIT_STAT_INC(fp_helper_calls);
    const uint32_t completed = riscv_fpu_jit_exec(instr, pc);

    if (completed == 0)
    {
        JIT_STAT_INC(fp_helper_trap_exits);
#if RV64_JIT_STATS
        if (inject_test_interrupt)
        {
            cpu.INTR = pending_before_test_hook;
        }
#endif
    }

    if (is_memory && completed != 0)
    {
#if RV64_JIT_STATS
        if (inject_test_interrupt)
        {
            rv64_jit_commit_test_fp_mmio_boundary();
            cpu.INTR = true;
        }
#endif

        if (nemu_state.state != NEMU_RUNNING ||
            (!interrupt_was_pending && cpu.INTR))
        {
            /*
             * Ending this generated block reaches only the JIT dispatcher.
             * The outer CPU loop must run before another native block so it
             * can observe a device stop or newly pending interrupt.
             */
            rv64_jit_cpu_boundary_requested = true;
        }
    }

    return completed;
}
#endif

enum
{
    HOST_REX_BASE = 0x40,
    HOST_REX_W = 0x08,
    HOST_REX_R = 0x04,
    HOST_REX_B = 0x01,
    HOST_REG_EXT_BIT = 0x08,
    HOST_MODRM_MOD_MEM = 0,
    HOST_MODRM_MOD_REG = 3,
    HOST_MODRM_MOD_DISP32 = 2,
    HOST_RM_SIB = 4,
    HOST_SIB_RCX_BASE_RDX_INDEX = 0x11,
    HOST_SIB_RDI_BASE_RDX_INDEX = 0x17,
    HOST_SIB_R10_BASE_RDX_INDEX = 0x12,
    HOST_ALU_ADD = 0x01,
    HOST_ALU_OR = 0x09,
    HOST_ALU_AND = 0x21,
    HOST_ALU_SUB = 0x29,
    HOST_ALU_XOR = 0x31,
    HOST_GROUP1_ADD = 0x0,
    HOST_GROUP1_OR = 0x1,
    HOST_GROUP1_AND = 0x4,
    HOST_GROUP1_XOR = 0x6,
    HOST_SHIFT_GROUP_SAL = 0x4,
    HOST_SHIFT_GROUP_SHR = 0x5,
    HOST_SHIFT_GROUP_SAR = 0x7,
    HOST_SHIFT_SAL = 0xe0,
    HOST_SHIFT_SHR = 0xe8,
    HOST_SHIFT_SAR = 0xf8,
};

/* Emit one byte into the current native block. */
static bool emit_u8(rv64_jit_writer_t *w, uint8_t value)
{
    if (w->cur >= w->end)
    {
        w->overflowed = true;
        return false;
    }

    *w->cur++ = value;
    return true;
}

/* Emit one little-endian 32-bit value into the current native block. */
static bool emit_u32(rv64_jit_writer_t *w, uint32_t value)
{
    for (size_t i = 0; i < sizeof(value); i++)
    {
        if (!emit_u8(w, (uint8_t)(value >> (i * 8))))
        {
            return false;
        }
    }

    return true;
}

/* Emit one little-endian 64-bit value into the current native block. */
static bool emit_u64(rv64_jit_writer_t *w, uint64_t value)
{
    for (size_t i = 0; i < sizeof(value); i++)
    {
        if (!emit_u8(w, (uint8_t)(value >> (i * 8))))
        {
            return false;
        }
    }

    return true;
}

/* Return the x86 register number backing one guest-register cache slot. */
static uint8_t jit_hreg_x86_reg(rv64_jit_hreg_t hreg)
{
    switch (hreg)
    {
    case RV64_JIT_HREG_RBX:
        return HOST_REG_RBX;
    case RV64_JIT_HREG_RBP:
        return HOST_REG_RBP;
    case RV64_JIT_HREG_R12:
        return HOST_REG_R12;
    case RV64_JIT_HREG_R13:
        return HOST_REG_R13;
    case RV64_JIT_HREG_R14:
        return HOST_REG_R14;
    case RV64_JIT_HREG_R15:
        return HOST_REG_R15;
    case RV64_JIT_HREG_R8:
        return HOST_REG_R8;
    default:
        Assert(0, "jit: invalid RV64 host register slot %d", hreg);
    }

    return HOST_REG_RBX;
}

/* Build an x86 ModRM byte from its mode, register, and r/m fields. */
static uint8_t jit_modrm(uint8_t mod, uint8_t reg, uint8_t rm)
{
    return (uint8_t)((mod << 6) | ((reg & 7u) << 3) | (rm & 7u));
}

/* Emit the fixed `[r10 + rdx]` addressing form used for direct PMEM access. */
static bool emit_pmem_sib_r10_rdx(rv64_jit_writer_t *w, host_reg_t reg)
{
    return emit_u8(w, jit_modrm(HOST_MODRM_MOD_MEM, (uint8_t)reg,
                                HOST_RM_SIB)) &&
           emit_u8(w, HOST_SIB_R10_BASE_RDX_INDEX);
}

/* Emit the fixed `[rcx + rdx]` addressing form used for direct MMIO reads. */
static bool emit_mmio_sib_rcx_rdx(rv64_jit_writer_t *w, host_reg_t reg)
{
    return emit_u8(w, jit_modrm(HOST_MODRM_MOD_MEM, (uint8_t)reg,
                                HOST_RM_SIB)) &&
           emit_u8(w, HOST_SIB_RCX_BASE_RDX_INDEX);
}

/* Emit the fixed `[rdi + rdx]` form used for direct MMIO writes. */
static bool emit_mmio_sib_rdi_rdx(rv64_jit_writer_t *w, host_reg_t reg)
{
    return emit_u8(w, jit_modrm(HOST_MODRM_MOD_MEM, (uint8_t)reg,
                                HOST_RM_SIB)) &&
           emit_u8(w, HOST_SIB_RDI_BASE_RDX_INDEX);
}

/* Emit a 64-bit REX.W prefix, including high-register extension bits. */
static bool emit_rex64(rv64_jit_writer_t *w, uint8_t reg, uint8_t rm)
{
    uint8_t rex = HOST_REX_BASE | HOST_REX_W;

    if ((reg & HOST_REG_EXT_BIT) != 0)
    {
        rex |= HOST_REX_R;
    }

    if ((rm & HOST_REG_EXT_BIT) != 0)
    {
        rex |= HOST_REX_B;
    }

    return emit_u8(w, rex);
}

/* Emit a REX prefix only when a 32-bit instruction references r8-r15. */
static bool emit_rex32_if_needed(rv64_jit_writer_t *w, uint8_t reg, uint8_t rm)
{
    uint8_t rex = HOST_REX_BASE;

    if ((reg & HOST_REG_EXT_BIT) != 0)
    {
        rex |= HOST_REX_R;
    }

    if ((rm & HOST_REG_EXT_BIT) != 0)
    {
        rex |= HOST_REX_B;
    }

    return rex == HOST_REX_BASE || emit_u8(w, rex);
}

/* Save all callee-saved host registers used as guest-register cache slots. */
static bool emit_push_saved_hregs(rv64_jit_writer_t *w)
{
    /*
     * Push order is RBX, RBP, R12, R13, R14, R15.  Six pushes would leave the
     * System V entry stack at rsp%16==8, so an extra 8-byte pad keeps helper
     * calls 16-byte aligned.
     */
    return emit_u8(w, 0x53) &&
           emit_u8(w, 0x55) &&
           emit_u8(w, 0x41) && emit_u8(w, 0x54) &&
           emit_u8(w, 0x41) && emit_u8(w, 0x55) &&
           emit_u8(w, 0x41) && emit_u8(w, 0x56) &&
           emit_u8(w, 0x41) && emit_u8(w, 0x57) &&
           /* sub rsp, 8 */
           emit_u8(w, 0x48) && emit_u8(w, 0x83) &&
           emit_u8(w, 0xec) && emit_u8(w, 0x08);
}

/* Restore callee-saved cache registers in the reverse of the push order. */
static bool emit_pop_saved_hregs(rv64_jit_writer_t *w)
{
    return
        /* add rsp, 8 */
        emit_u8(w, 0x48) && emit_u8(w, 0x83) &&
        emit_u8(w, 0xc4) && emit_u8(w, 0x08) &&
        emit_u8(w, 0x41) && emit_u8(w, 0x5f) &&
        emit_u8(w, 0x41) && emit_u8(w, 0x5e) &&
        emit_u8(w, 0x41) && emit_u8(w, 0x5d) &&
        emit_u8(w, 0x41) && emit_u8(w, 0x5c) &&
        emit_u8(w, 0x5d) &&
        emit_u8(w, 0x5b);
}

/* Forward declaration: the prologue needs R10 before the grouped move helpers. */
static bool emit_movabs_r10(rv64_jit_writer_t *w, uint64_t value);

/* Emit `movabs r11, &cpu`, restoring the fixed CPU-state base register. */
static bool emit_load_cpu_base(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x49) && emit_u8(w, 0xbb) &&
           emit_u64(w, (uint64_t)(uintptr_t)&cpu);
}

/*
 * Load the fixed host-side base registers used by generated RV64 code. Helper
 * calls may clobber R10/R11, so the same helper is used for prologue setup and
 * post-helper reloads.
 */
static bool emit_load_jit_bases(rv64_jit_writer_t *w)
{
    return emit_load_cpu_base(w) &&
           emit_movabs_r10(w, (uint64_t)(uintptr_t)guest_to_host(CONFIG_MBASE));
}

/* Emit the common native-block prologue and load long-lived base registers. */
bool rv64_jit_emit_prologue(rv64_jit_writer_t *w)
{
    /*
     * R11 is caller-saved, so generated code can dedicate it to `CPU_state *`
     * without saving it. R10 holds the host pointer for guest physical
     * CONFIG_MBASE, letting direct PMEM loads use `[r10 + offset]` after a
     * strict in-range guard. The saved host registers are the per-block guest
     * register cache and also provide 16-byte stack alignment for helper calls.
     */
    return emit_push_saved_hregs(w) &&
           emit_load_jit_bases(w);
}

/* Restore saved host registers and return to the C dispatcher. */
static bool emit_epilogue(rv64_jit_writer_t *w)
{
    return emit_pop_saved_hregs(w) && emit_u8(w, 0xc3);
}

/* Emit a native return when EAX already holds the completed instruction count. */
static bool emit_return_eax(rv64_jit_writer_t *w)
{
    return emit_epilogue(w);
}

/* Emit `movabs reg, imm64` for the fixed host registers used by the JIT ABI. */
static bool emit_movabs_reg(rv64_jit_writer_t *w, host_reg_t reg,
                            uint64_t value)
{
    const uint8_t rex =
        HOST_REX_BASE | HOST_REX_W |
        (((uint8_t)reg & HOST_REG_EXT_BIT) != 0 ? HOST_REX_B : 0);

    return emit_u8(w, rex) &&
           emit_u8(w, (uint8_t)(0xb8u + ((uint8_t)reg & 7u))) &&
           emit_u64(w, value);
}

/* Emit `movabs rax, imm64`, used for full-width constants and helper targets. */
static bool emit_movabs_rax(rv64_jit_writer_t *w, uint64_t value)
{
    return emit_movabs_reg(w, HOST_REG_RAX, value);
}

/* Emit `movabs rdx, imm64`, used for addresses of JIT loop counters. */
static bool emit_movabs_rdx(rv64_jit_writer_t *w, uint64_t value)
{
    return emit_movabs_reg(w, HOST_REG_RDX, value);
}

/* Emit `movabs rcx, imm64`, used for full-width PMEM range guards. */
static bool emit_movabs_rcx(rv64_jit_writer_t *w, uint64_t value)
{
    return emit_movabs_reg(w, HOST_REG_RCX, value);
}

/* Emit `movabs rdi, imm64`, used as the direct-MMIO write backing base. */
static bool emit_movabs_rdi(rv64_jit_writer_t *w, uint64_t value)
{
    return emit_movabs_reg(w, HOST_REG_RDI, value);
}

/*
 * Emit a sidecar address whose final arena location is not known until all
 * native bytes have been generated. The compiler patches the immediate before
 * publishing the block, using the same bounded fixup model as MMIO routes.
 */
static bool emit_movabs_indirect_pic_address(rv64_jit_writer_t *w,
                                             host_reg_t reg)
{
    rv64_jit_indirect_pic_builder_t *builder = w->indirect_pic;

    Assert(builder != NULL && builder->used,
           "jit: missing RV64 indirect PIC builder");
    Assert(builder->fixup_count < RV64_JIT_INDIRECT_PIC_MAX_FIXUPS,
           "jit: too many RV64 indirect PIC address fixups");

    const uint8_t rex =
        HOST_REX_BASE | HOST_REX_W |
        (((uint8_t)reg & HOST_REG_EXT_BIT) != 0 ? HOST_REX_B : 0);

    if (!emit_u8(w, rex) ||
        !emit_u8(w, (uint8_t)(0xb8u + ((uint8_t)reg & 7u))))
    {
        return false;
    }

    uint8_t *immediate = w->cur;

    if (!emit_u64(w, 0))
    {
        return false;
    }

    builder->address_immediates[builder->fixup_count++] = immediate;
    return true;
}

/* Record the one source-owned guarded-jump-cache base address for finalising. */
static bool emit_movabs_indirect_jump_cache_address(
    rv64_jit_writer_t *w, host_reg_t reg)
{
    rv64_jit_indirect_jump_cache_builder_t *builder =
        w->indirect_jump_cache;

    Assert(builder != NULL && builder->used,
           "jit: missing RV64 indirect jump cache builder");
    Assert(builder->fixup_count <
               RV64_JIT_INDIRECT_JUMP_CACHE_MAX_FIXUPS,
           "jit: too many RV64 indirect jump cache address fixups");

    const uint8_t rex =
        HOST_REX_BASE | HOST_REX_W |
        (((uint8_t)reg & HOST_REG_EXT_BIT) != 0 ? HOST_REX_B : 0);

    if (!emit_u8(w, rex) ||
        !emit_u8(w, (uint8_t)(0xb8u + ((uint8_t)reg & 7u))))
    {
        return false;
    }

    uint8_t *immediate = w->cur;

    if (!emit_u64(w, 0))
    {
        return false;
    }

    builder->address_immediates[builder->fixup_count++] = immediate;
    return true;
}

/* Emit `movabs r10, imm64`, the fixed host PMEM base for direct loads. */
static bool emit_movabs_r10(rv64_jit_writer_t *w, uint64_t value)
{
    return emit_movabs_reg(w, HOST_REG_R10, value);
}

#ifdef CONFIG_RISCV_FPU
/* Emit `movabs rsi, imm64`, preserving a full-width RV64 helper argument. */
static bool emit_movabs_rsi(rv64_jit_writer_t *w, uint64_t value) { return emit_movabs_reg(w, HOST_REG_RSI, value); }
#endif

/* Emit `mov eax, [rdx]`, loading one 32-bit JIT loop counter. */
static bool emit_mov_eax_m32_rdx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x8b) && emit_u8(w, 0x02);
}

/* Emit `mov rax, [rax]`, loading one live 64-bit runtime guard value. */
static bool emit_mov_rax_m64_rax(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x8b) && emit_u8(w, 0x00);
}

/* Emit `mov [rdx], eax`, storing one 32-bit JIT loop counter. */
static bool emit_mov_m32_rdx_eax(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x89) && emit_u8(w, 0x02);
}

/* Emit `mov r9d, [rdx]`, preloading the completed-lap count. */
static bool emit_mov_r9d_m32_rdx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x44) && emit_u8(w, 0x8b) && emit_u8(w, 0x0a);
}

/* Emit `mov esi, [rdx]`, preloading the current native-entry budget. */
static bool emit_mov_esi_m32_rdx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x8b) && emit_u8(w, 0x32);
}

/* Emit `mov [rdx], r9d`, synchronising completed laps before fall-through. */
static bool emit_mov_m32_rdx_r9d(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x44) && emit_u8(w, 0x89) && emit_u8(w, 0x0a);
}

/* Emit `mov ecx, eax`, copying the loop count for the budget look-ahead. */
static bool emit_mov_ecx_eax(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x89) && emit_u8(w, 0xc1);
}

/* Emit `mov eax, r9d`, preparing the completed count for a possible return. */
static bool emit_mov_eax_r9d(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x44) && emit_u8(w, 0x89) && emit_u8(w, 0xc8);
}

/* Emit `mov r9, rax`, preserving a dynamic JALR target across scratch work. */
static bool emit_mov_r9_rax(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x49) && emit_u8(w, 0x89) && emit_u8(w, 0xc1);
}

/* Emit `mov rdx, r9`, copying the preserved JALR target for cache hashing. */
static bool emit_mov_rdx_r9(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x4c) && emit_u8(w, 0x89) && emit_u8(w, 0xca);
}

/* Emit `mov rsi, r9`, preserving the dynamic target as helper argument two. */
static bool emit_mov_rsi_r9(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x4c) && emit_u8(w, 0x89) && emit_u8(w, 0xce);
}

/* Emit `mov rdx, r8`, passing the authoritative target slot as argument three. */
static bool emit_mov_rdx_r8(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x4c) && emit_u8(w, 0x89) && emit_u8(w, 0xc2);
}

/* Emit `mov rax, r9`, restoring the dynamic target for a committed miss. */
static bool emit_mov_rax_r9(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x4c) && emit_u8(w, 0x89) && emit_u8(w, 0xc8);
}

/* Emit `sub esi, r9d`, removing earlier completed work from the budget. */
static bool emit_sub_esi_r9d(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x44) && emit_u8(w, 0x29) && emit_u8(w, 0xce);
}

/* Emit `add r9d, imm32`, accounting for one completed native loop lap. */
static bool emit_add_r9d_imm32(rv64_jit_writer_t *w, uint32_t value)
{
    return emit_u8(w, 0x41) && emit_u8(w, 0x81) &&
           emit_u8(w, 0xc1) && emit_u32(w, value);
}

/* Emit `sub esi, imm32`, reserving budget for one more native loop lap. */
static bool emit_sub_esi_imm32(rv64_jit_writer_t *w, uint32_t value)
{
    return emit_u8(w, 0x81) && emit_u8(w, 0xee) && emit_u32(w, value);
}

/* Emit `mov eax, ecx`, restoring a saved dynamic return count. */
static bool emit_mov_eax_ecx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x89) && emit_u8(w, 0xc8);
}

/* Emit `test eax, eax`, commonly used after boolean helper returns. */
static bool emit_test_eax_eax(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x85) && emit_u8(w, 0xc0);
}

/* Emit `mov rcx, rax`, preserving a live RAX value across scratch operations. */
static bool emit_mov_rcx_rax(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x89) && emit_u8(w, 0xc1);
}

/* Emit `mov rax, rcx`, restoring a value previously preserved in RCX. */
static bool emit_mov_rax_rcx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x89) && emit_u8(w, 0xc8);
}

/* Emit `mov rdx, rax`, copying a 64-bit value into the RDX scratch register. */
static bool emit_mov_rdx_rax(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x89) && emit_u8(w, 0xc2);
}

/* Emit `mov rdx, rcx`, copying a 64-bit value into the RDX scratch register. */
static bool emit_mov_rdx_rcx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x89) && emit_u8(w, 0xca);
}

/* Emit `mov r8, rdx`, copying a VPN or PMEM offset into an index register. */
static bool emit_mov_r8_rdx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x49) && emit_u8(w, 0x89) && emit_u8(w, 0xd0);
}

/* Emit `mov r8d, edx`, used before indexing small refcount tables. */
static bool emit_mov_r8d_edx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x41) && emit_u8(w, 0x89) && emit_u8(w, 0xd0);
}

/* Emit `mov rdi, rdx`, preparing the first helper argument from a PMEM offset. */
static bool emit_mov_rdi_rdx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x89) && emit_u8(w, 0xd7);
}

/* Emit `mov rdi, rax`, preparing the first helper argument from a guest value. */
static bool emit_mov_rdi_rax(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x89) && emit_u8(w, 0xc7);
}

/* Form an exact direct-read host pointer from base RCX plus offset RDX. */
static bool emit_add_rdx_rcx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x01) && emit_u8(w, 0xca);
}

/* Form an exact direct-write host pointer from base RDI plus offset RDX. */
static bool emit_add_rdi_rdx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x01) && emit_u8(w, 0xd7);
}

/* Copy the high product or division remainder from RDX into RAX. */
static bool emit_mov_rax_rdx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x89) && emit_u8(w, 0xd0);
}

/* Copy the low 32-bit division remainder from EDX into EAX. */
static bool emit_mov_eax_edx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x89) && emit_u8(w, 0xd0);
}

/* Materialise one 32-bit value in EAX, clearing the upper half of RAX. */
static bool emit_mov_eax_imm32(rv64_jit_writer_t *w, uint32_t value)
{
    return emit_u8(w, 0xb8) && emit_u32(w, value);
}

/* Clear RDX before unsigned x86 division consumes RDX:RAX. */
static bool emit_zero_rdx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x31) && emit_u8(w, 0xd2);
}

/* Test a full-width divisor for the RISC-V divide-by-zero result arm. */
static bool emit_test_rcx_rcx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x85) && emit_u8(w, 0xc9);
}

/* Test only the low W-form divisor bits. */
static bool emit_test_ecx_ecx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x85) && emit_u8(w, 0xc9);
}

/* Compare RAX with a full-width exceptional value held in RDX. */
static bool emit_cmp_rax_rdx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x39) && emit_u8(w, 0xd0);
}

/* Compare the full-width signed divisor with -1. */
static bool emit_cmp_rcx_neg_one(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x83) &&
           emit_u8(w, 0xf9) && emit_u8(w, 0xff);
}

/* Compare only EAX with a W-form exceptional dividend. */
static bool emit_cmp_eax_imm32(rv64_jit_writer_t *w, uint32_t value)
{
    return emit_u8(w, 0x3d) && emit_u32(w, value);
}

/* Compare ECX with a full 32-bit immediate. */
static bool emit_cmp_ecx_imm32(rv64_jit_writer_t *w, uint32_t value)
{
    return emit_u8(w, 0x81) && emit_u8(w, 0xf9) &&
           emit_u32(w, value);
}

/* Compare EDX with -1 after extracting a binary32 NaN-box prefix. */
static bool emit_cmp_edx_neg_one(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x83) && emit_u8(w, 0xfa) && emit_u8(w, 0xff);
}

/* Mask EAX with a 32-bit immediate. */
static bool emit_and_eax_imm32(rv64_jit_writer_t *w, uint32_t value)
{
    return emit_u8(w, 0x25) && emit_u32(w, value);
}

/* Toggle selected EAX bits with a 32-bit immediate. */
static bool emit_xor_eax_imm32(rv64_jit_writer_t *w, uint32_t value)
{
    return emit_u8(w, 0x35) && emit_u32(w, value);
}

/* Mask ECX with a 32-bit immediate. */
static bool emit_and_ecx_imm32(rv64_jit_writer_t *w, uint32_t value)
{
    return emit_u8(w, 0x81) && emit_u8(w, 0xe1) &&
           emit_u32(w, value);
}

/* Test EAX against a 32-bit immediate without changing its raw FP bits. */
static bool emit_test_eax_imm32(rv64_jit_writer_t *w, uint32_t value)
{
    return emit_u8(w, 0xa9) && emit_u32(w, value);
}

/* Compare the low W-form divisor with -1. */
static bool emit_cmp_ecx_neg_one(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x83) && emit_u8(w, 0xf9) && emit_u8(w, 0xff);
}

/* Sign-extend RAX into RDX:RAX before a full-width signed divide. */
static bool emit_cqo(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x99);
}

/* Sign-extend EAX into EDX:EAX before a signed W-form divide. */
static bool emit_cdq(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x99);
}

/* Sign-extend a selected W-form EAX result to the architectural RV64 width. */
static bool emit_cdqe(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x98);
}

/* Emit unsigned RDX:RAX = RAX * RCX and retain both product halves. */
static bool emit_mul_rcx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0xf7) && emit_u8(w, 0xe1);
}

/* Emit signed RDX:RAX = RAX * RCX and retain both product halves. */
static bool emit_imul_rcx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0xf7) && emit_u8(w, 0xe9);
}

/* Divide unsigned RDX:RAX by RCX after the caller has excluded zero. */
static bool emit_div_rcx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0xf7) && emit_u8(w, 0xf1);
}

/* Divide signed RDX:RAX by RCX after both x86 trap cases are excluded. */
static bool emit_idiv_rcx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0xf7) && emit_u8(w, 0xf9);
}

/* Divide unsigned EDX:EAX by ECX for DIVUW and REMUW. */
static bool emit_div_ecx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0xf7) && emit_u8(w, 0xf1);
}

/* Divide signed EDX:EAX by ECX for DIVW and REMW. */
static bool emit_idiv_ecx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0xf7) && emit_u8(w, 0xf9);
}

/* Form an all-ones/zero correction mask from the signed lhs in RDI. */
static bool emit_sar_rdi_63(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0xc1) &&
           emit_u8(w, 0xff) && emit_u8(w, 63);
}

/* Keep the unsigned rhs only when the copied MULHSU lhs was negative. */
static bool emit_and_rdi_rcx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x21) && emit_u8(w, 0xcf);
}

/* Apply QEMU's mixed signed/unsigned high-product correction. */
static bool emit_sub_rdx_rdi(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x29) && emit_u8(w, 0xfa);
}

/* Emit `add eax, imm32`, used for completed-loop instruction accounting. */
static bool emit_add_eax_imm32(rv64_jit_writer_t *w, uint32_t imm)
{
    return emit_u8(w, 0x05) && emit_u32(w, imm);
}

/* Emit `add ecx, imm32`, used to test whether one more loop lap fits. */
static bool emit_add_ecx_imm32(rv64_jit_writer_t *w, uint32_t imm)
{
    return emit_u8(w, 0x81) && emit_u8(w, 0xc1) && emit_u32(w, imm);
}

/* Emit `sub rdx, rcx`, converting guest address to a PMEM byte offset. */
static bool emit_sub_rdx_rcx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x29) && emit_u8(w, 0xca);
}

/* Emit `sub rdx, rax`, converting translated paddr to a PMEM byte offset. */
static bool emit_sub_rdx_rax(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x29) && emit_u8(w, 0xc2);
}

/* Emit `add rdi, rax`, converting a PMEM offset while RCX keeps store data. */
static bool emit_add_rdi_rax(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x01) && emit_u8(w, 0xc7);
}

/* Emit `cmp rdx, rcx`, used by unsigned PMEM range guards. */
static bool emit_cmp_rdx_rcx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x39) && emit_u8(w, 0xca);
}

/* Emit `or rdx, rax`, combining a page base with a low page offset. */
static bool emit_or_rdx_rax(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x09) && emit_u8(w, 0xc2);
}

/* Shift RDX right by an immediate count. */
static bool emit_shr_rdx_imm(rv64_jit_writer_t *w, uint8_t value)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0xc1) && emit_u8(w, 0xea) && emit_u8(w, value);
}

/* Shift EDX left, also clearing the upper half of the resulting RDX offset. */
static bool emit_shl_edx_imm(rv64_jit_writer_t *w, uint8_t value)
{
    return emit_u8(w, 0xc1) && emit_u8(w, 0xe2) && emit_u8(w, value);
}

/* XOR a 32-bit immediate into EDX, which also clears the upper half of RDX. */
static bool emit_xor_edx_imm32(rv64_jit_writer_t *w, uint32_t value)
{
    return emit_u8(w, 0x81) && emit_u8(w, 0xf2) && emit_u32(w, value);
}

/* Mask EDX with an immediate to form a bounded direct-cache index. */
static bool emit_and_edx_imm32(rv64_jit_writer_t *w, uint32_t value)
{
    return emit_u8(w, 0x81) && emit_u8(w, 0xe2) && emit_u32(w, value);
}

/* AND EDX with a positive imm8, zero-extending the result through EDX. */
static bool emit_and_edx_imm8(rv64_jit_writer_t *w, uint8_t value)
{
    Assert(value <= INT8_MAX, "jit: RV64 EDX AND mask exceeds positive imm8");
    return emit_u8(w, 0x83) && emit_u8(w, 0xe2) && emit_u8(w, value);
}

/* Multiply RDX by a positive 32-bit structure size. */
static bool emit_imul_rdx_imm32(rv64_jit_writer_t *w, uint32_t value)
{
    Assert(value <= INT32_MAX, "jit: RV64 cache-slot stride is too large");
    return emit_u8(w, 0x48) && emit_u8(w, 0x69) &&
           emit_u8(w, 0xd2) && emit_u32(w, value);
}

/* Shift R8 left by an immediate count; DTLB entries are power-of-two sized. */
static bool emit_shl_r8_imm(rv64_jit_writer_t *w, uint8_t value)
{
    return emit_u8(w, 0x49) && emit_u8(w, 0xc1) && emit_u8(w, 0xe0) && emit_u8(w, value);
}

/* Shift R8 right by an immediate count while preserving high VPN tag bits. */
static bool emit_shr_r8_imm(rv64_jit_writer_t *w, uint8_t value)
{
    return emit_u8(w, 0x49) && emit_u8(w, 0xc1) && emit_u8(w, 0xe8) && emit_u8(w, value);
}

/* Shift R8D right by an immediate count for PMEM refcount table indexes. */
static bool emit_shr_r8d_imm(rv64_jit_writer_t *w, uint8_t value)
{
    return emit_u8(w, 0x41) && emit_u8(w, 0xc1) && emit_u8(w, 0xe8) && emit_u8(w, value);
}

/* Mask R8D with an immediate, usually to keep a direct-mapped table index. */
static bool emit_and_r8d_imm(rv64_jit_writer_t *w, uint32_t value)
{
    return emit_u8(w, 0x41) && emit_u8(w, 0x81) && emit_u8(w, 0xe0) && emit_u32(w, value);
}

/* Compare R8D with an immediate, used by store source-chunk guards. */
static bool emit_cmp_r8d_imm(rv64_jit_writer_t *w, uint32_t value)
{
    return emit_u8(w, 0x41) && emit_u8(w, 0x81) && emit_u8(w, 0xf8) && emit_u32(w, value);
}

/* Add RDX to R8, producing a pointer into the direct-mapped DTLB. */
static bool emit_add_r8_rdx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x49) && emit_u8(w, 0x01) && emit_u8(w, 0xd0);
}

/* XOR RDX into R8, matching the helper TLB hash mix. */
static bool emit_xor_r8_rdx(rv64_jit_writer_t *w)
{
    /* `49 31 d0` is `xor r8, rdx`; R8 holds the evolving TLB index. */
    return emit_u8(w, 0x49) && emit_u8(w, 0x31) && emit_u8(w, 0xd0);
}

/* Compare a byte field in the R8-pointed DTLB entry with an immediate. */
static bool emit_cmp_r8b_field_imm8(rv64_jit_writer_t *w, uint32_t offset,
                                    uint8_t value)
{
    Assert(offset <= INT8_MAX, "jit: RV64 DTLB byte field offset is too large");
    return emit_u8(w, 0x41) && emit_u8(w, 0x80) &&
           emit_u8(w, 0x78) && emit_u8(w, (uint8_t)offset) && emit_u8(w, value);
}

/* Compare a qword field in the R8-pointed DTLB entry with RDX. */
static bool emit_cmp_r8q_field_rdx(rv64_jit_writer_t *w, uint32_t offset)
{
    Assert(offset <= INT8_MAX, "jit: RV64 DTLB qword field offset is too large");
    return emit_u8(w, 0x49) && emit_u8(w, 0x39) &&
           emit_u8(w, 0x50) && emit_u8(w, (uint8_t)offset);
}

/* Compare a qword field in an R8-pointed block-cache slot with R9. */
static bool emit_cmp_r8q_field_r9(rv64_jit_writer_t *w, uint32_t offset)
{
    Assert(offset <= INT8_MAX, "jit: RV64 block qword field offset is too large");
    return emit_u8(w, 0x4d) && emit_u8(w, 0x39) &&
           emit_u8(w, 0x48) && emit_u8(w, (uint8_t)offset);
}

/* Compare a qword field in an R8-pointed block-cache slot with RAX. */
static bool emit_cmp_r8q_field_rax(rv64_jit_writer_t *w, uint32_t offset)
{
    Assert(offset <= INT8_MAX, "jit: RV64 block qword field offset is too large");
    return emit_u8(w, 0x49) && emit_u8(w, 0x39) &&
           emit_u8(w, 0x40) && emit_u8(w, (uint8_t)offset);
}

/* Load a qword field from the RDX-pointed PIC entry into R8. */
static bool emit_mov_r8_rdxq_field(rv64_jit_writer_t *w, uint32_t offset)
{
    Assert(offset <= INT8_MAX, "jit: RV64 PIC field offset is too large");
    return emit_u8(w, 0x4c) && emit_u8(w, 0x8b) &&
           emit_u8(w, 0x42) && emit_u8(w, (uint8_t)offset);
}

/* Compare a qword field in the RDX-pointed PIC entry with preserved target R9. */
static bool emit_cmp_rdxq_field_r9(rv64_jit_writer_t *w, uint32_t offset)
{
    Assert(offset <= INT8_MAX, "jit: RV64 PIC field offset is too large");
    return emit_u8(w, 0x4c) && emit_u8(w, 0x39) &&
           emit_u8(w, 0x4a) && emit_u8(w, (uint8_t)offset);
}

/* Load a qword field from the RDI-pointed jump-cache entry into R8. */
static bool emit_mov_r8_rdiq_field(rv64_jit_writer_t *w, uint32_t offset)
{
    Assert(offset <= INT8_MAX,
           "jit: RV64 indirect jump cache field offset is too large");
    return emit_u8(w, 0x4c) && emit_u8(w, 0x8b) &&
           emit_u8(w, 0x47) && emit_u8(w, (uint8_t)offset);
}

/* Load a qword field from the RDI-pointed jump-cache entry into RAX. */
static bool emit_mov_rax_rdiq_field(rv64_jit_writer_t *w, uint32_t offset)
{
    Assert(offset <= INT8_MAX,
           "jit: RV64 indirect jump cache field offset is too large");
    return emit_u8(w, 0x48) && emit_u8(w, 0x8b) &&
           emit_u8(w, 0x47) && emit_u8(w, (uint8_t)offset);
}

/* Store R8 into a qword field in the RDI-pointed jump-cache entry. */
static bool emit_mov_rdiq_field_r8(rv64_jit_writer_t *w, uint32_t offset)
{
    Assert(offset <= INT8_MAX,
           "jit: RV64 indirect jump cache field offset is too large");
    return emit_u8(w, 0x4c) && emit_u8(w, 0x89) &&
           emit_u8(w, 0x47) && emit_u8(w, (uint8_t)offset);
}

/* Store RAX into a qword field in the RDI-pointed jump-cache entry. */
static bool emit_mov_rdiq_field_rax(rv64_jit_writer_t *w, uint32_t offset)
{
    Assert(offset <= INT8_MAX,
           "jit: RV64 indirect jump cache field offset is too large");
    return emit_u8(w, 0x48) && emit_u8(w, 0x89) &&
           emit_u8(w, 0x47) && emit_u8(w, (uint8_t)offset);
}

/* Emit `test r8, r8`, rejecting an empty PIC entry before dereferencing it. */
static bool emit_test_r8_r8(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x4d) && emit_u8(w, 0x85) && emit_u8(w, 0xc0);
}

/* Test a full-width cached generation or prior slot pointer in RAX. */
static bool emit_test_rax_rax(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x85) && emit_u8(w, 0xc0);
}

/* Compare a dword field in the R8-pointed DTLB entry with an immediate. */
static bool emit_cmp_r8d_field_imm32(rv64_jit_writer_t *w, uint32_t offset,
                                     uint32_t value)
{
    Assert(offset <= INT8_MAX, "jit: RV64 DTLB dword field offset is too large");
    return emit_u8(w, 0x41) && emit_u8(w, 0x81) &&
           emit_u8(w, 0x78) && emit_u8(w, (uint8_t)offset) && emit_u32(w, value);
}

/* Compare a qword field in an R8-pointed block-cache slot with signed imm8. */
static bool emit_cmp_r8q_field_imm8(rv64_jit_writer_t *w, uint32_t offset,
                                    uint8_t value)
{
    Assert(offset <= INT8_MAX, "jit: RV64 block qword field offset is too large");
    return emit_u8(w, 0x49) && emit_u8(w, 0x83) &&
           emit_u8(w, 0x78) && emit_u8(w, (uint8_t)offset) &&
           emit_u8(w, value);
}

/* Load a qword field from an R8-pointed block-cache slot into RAX. */
static bool emit_mov_rax_r8q_field(rv64_jit_writer_t *w, uint32_t offset)
{
    Assert(offset <= INT8_MAX, "jit: RV64 block qword field offset is too large");
    return emit_u8(w, 0x49) && emit_u8(w, 0x8b) &&
           emit_u8(w, 0x40) && emit_u8(w, (uint8_t)offset);
}

/* Add a dword field from an R8-pointed block-cache slot into ECX. */
static bool emit_add_ecx_r8d_field(rv64_jit_writer_t *w, uint32_t offset)
{
    Assert(offset <= INT8_MAX, "jit: RV64 block dword field offset is too large");
    return emit_u8(w, 0x41) && emit_u8(w, 0x03) &&
           emit_u8(w, 0x48) && emit_u8(w, (uint8_t)offset);
}

/* Compare a byte field in the RDX-pointed direct-link block with an immediate. */
static bool emit_cmp_rdxb_field_imm8(rv64_jit_writer_t *w, uint32_t offset,
                                     uint8_t value)
{
    Assert(offset <= INT8_MAX, "jit: RV64 block byte field offset is too large");
    return emit_u8(w, 0x80) && emit_u8(w, 0x7a) &&
           emit_u8(w, (uint8_t)offset) && emit_u8(w, value);
}

/* Compare a dword field in the RDX-pointed direct-link block with an immediate. */
static bool emit_cmp_rdxd_field_imm32(rv64_jit_writer_t *w, uint32_t offset,
                                      uint32_t value)
{
    Assert(offset <= INT8_MAX, "jit: RV64 block dword field offset is too large");
    return emit_u8(w, 0x81) && emit_u8(w, 0x7a) &&
           emit_u8(w, (uint8_t)offset) && emit_u32(w, value);
}

/* Compare a qword field in the RDX-pointed direct-link block with RAX. */
static bool emit_cmp_rdxq_field_rax(rv64_jit_writer_t *w, uint32_t offset)
{
    Assert(offset <= INT8_MAX, "jit: RV64 block qword field offset is too large");
    return emit_u8(w, 0x48) && emit_u8(w, 0x39) &&
           emit_u8(w, 0x42) && emit_u8(w, (uint8_t)offset);
}

/* Compare a qword field in the RDX-pointed direct-link block with a sign imm8. */
static bool emit_cmp_rdxq_field_imm8(rv64_jit_writer_t *w, uint32_t offset,
                                     uint8_t value)
{
    Assert(offset <= INT8_MAX, "jit: RV64 block qword field offset is too large");
    return emit_u8(w, 0x48) && emit_u8(w, 0x83) &&
           emit_u8(w, 0x7a) && emit_u8(w, (uint8_t)offset) && emit_u8(w, value);
}

/* Load a qword field from the RDX-pointed direct-link block into RAX. */
static bool emit_mov_rax_rdxq_field(rv64_jit_writer_t *w, uint32_t offset)
{
    Assert(offset <= INT8_MAX, "jit: RV64 block qword field offset is too large");
    return emit_u8(w, 0x48) && emit_u8(w, 0x8b) &&
           emit_u8(w, 0x42) && emit_u8(w, (uint8_t)offset);
}

/* Add a dword field from the RDX-pointed direct-link block into ECX. */
static bool emit_add_ecx_rdxd_field(rv64_jit_writer_t *w, uint32_t offset)
{
    Assert(offset <= INT8_MAX, "jit: RV64 block dword field offset is too large");
    return emit_u8(w, 0x03) && emit_u8(w, 0x4a) && emit_u8(w, (uint8_t)offset);
}

/* Test permission bits in a dword field in the R8-pointed DTLB entry. */
static bool emit_test_r8d_field_imm32(rv64_jit_writer_t *w, uint32_t offset,
                                      uint32_t value)
{
    Assert(offset <= INT8_MAX, "jit: RV64 DTLB dword field offset is too large");
    return emit_u8(w, 0x41) && emit_u8(w, 0xf7) &&
           emit_u8(w, 0x40) && emit_u8(w, (uint8_t)offset) && emit_u32(w, value);
}

/* Load a qword field from the R8-pointed DTLB entry into RDX. */
static bool emit_mov_rdx_r8q_field(rv64_jit_writer_t *w, uint32_t offset)
{
    Assert(offset <= INT8_MAX, "jit: RV64 DTLB qword field offset is too large");
    return emit_u8(w, 0x49) && emit_u8(w, 0x8b) &&
           emit_u8(w, 0x50) && emit_u8(w, (uint8_t)offset);
}

/* Compare a 16-bit refcount in the RAX-based table indexed by R8D with zero. */
static bool emit_cmp_u16_ref_zero_rax_r8(rv64_jit_writer_t *w)
{
    /* `66 42 83 3c 40 00` is `cmp word ptr [rax + r8 * 2], 0`. */
    return emit_u8(w, 0x66) && emit_u8(w, 0x42) &&
           emit_u8(w, 0x83) && emit_u8(w, 0x3c) &&
           emit_u8(w, 0x40) && emit_u8(w, 0x00);
}

/* Compare a 32-bit refcount in the RAX-based table indexed by R8D with zero. */
static bool emit_cmp_u32_ref_zero_rax_r8(rv64_jit_writer_t *w)
{
    /* `42 83 3c 80 00` is `cmp dword ptr [rax + r8 * 4], 0`. */
    return emit_u8(w, 0x42) && emit_u8(w, 0x83) &&
           emit_u8(w, 0x3c) && emit_u8(w, 0x80) && emit_u8(w, 0x00);
}

/* Emit `mov esi, imm32`, preparing the second helper argument. */
static bool emit_mov_esi_imm32(rv64_jit_writer_t *w, uint32_t imm)
{
    return emit_u8(w, 0xbe) && emit_u32(w, imm);
}

#ifdef CONFIG_RISCV_FPU
/* Emit `mov edi, imm32`, preparing the first helper argument. */
static bool emit_mov_edi_imm32(rv64_jit_writer_t *w, uint32_t imm) { return emit_u8(w, 0xbf) && emit_u32(w, imm); }
#endif

/* Emit `cmp ecx, [rdx]`, comparing proposed work with the entry budget. */
static bool emit_cmp_ecx_m32_rdx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x3b) && emit_u8(w, 0x0a);
}

/* Return this path's retired count plus any earlier native loop laps. */
static bool emit_return_total_retired(rv64_jit_writer_t *w, uint32_t count)
{
    return emit_movabs_rdx(w, (uint64_t)(uintptr_t)&rv64_jit_loop_extra) &&
           emit_mov_eax_m32_rdx(w) &&
           emit_add_eax_imm32(w, count) &&
           emit_return_eax(w);
}

/* Emit a 32-bit zeroing idiom for RAX. */
static bool emit_zero_rax(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x31) && emit_u8(w, 0xc0);
}

/* Emit `test al, imm8`, checking low address alignment bits. */
static bool emit_test_al_imm8(rv64_jit_writer_t *w, uint8_t mask)
{
    return emit_u8(w, 0xa8) && emit_u8(w, mask);
}

/* Load `cpu.gpr[reg]` into one 64-bit cached host register. */
static bool emit_load_gpr_hreg(rv64_jit_writer_t *w, rv64_jit_hreg_t hreg,
                               uint32_t reg)
{
    const uint8_t dst = jit_hreg_x86_reg(hreg);
    const uint8_t base = 11;

    /* `REX.W 8b /r` is `mov r64, qword ptr [r11 + disp32]`. */
    return emit_rex64(w, dst, base) &&
           emit_u8(w, 0x8b) &&
           emit_u8(w, jit_modrm(2, dst, base)) &&
           emit_u32(w, jit_gpr_offset(reg));
}

/* Store one cached 64-bit host register back into `cpu.gpr[reg]`. */
static bool emit_store_gpr_hreg(rv64_jit_writer_t *w, uint32_t reg,
                                rv64_jit_hreg_t hreg)
{
    const uint8_t src = jit_hreg_x86_reg(hreg);
    const uint8_t base = 11;

    /* `REX.W 89 /r` is `mov qword ptr [r11 + disp32], r64`. */
    return emit_rex64(w, src, base) &&
           emit_u8(w, 0x89) &&
           emit_u8(w, jit_modrm(2, src, base)) &&
           emit_u32(w, jit_gpr_offset(reg));
}

#ifdef CONFIG_RISCV_FPU
/* Load one raw 64-bit CPU-state field through the fixed R11 base. */
static bool emit_load_cpu_u64_reg(rv64_jit_writer_t *w, host_reg_t dst,
                                  uint32_t offset)
{
    return emit_rex64(w, (uint8_t)dst, HOST_REG_R11) &&
           emit_u8(w, 0x8b) &&
           emit_u8(w, jit_modrm(HOST_MODRM_MOD_DISP32, (uint8_t)dst,
                                HOST_REG_R11)) &&
           emit_u32(w, offset);
}

/* Load a 32-bit CPU-state field, zero-extending it in the selected host reg. */
static bool emit_load_cpu_u32_reg(rv64_jit_writer_t *w, host_reg_t dst,
                                  uint32_t offset)
{
    return emit_rex32_if_needed(w, (uint8_t)dst, HOST_REG_R11) &&
           emit_u8(w, 0x8b) &&
           emit_u8(w, jit_modrm(HOST_MODRM_MOD_DISP32, (uint8_t)dst,
                                HOST_REG_R11)) &&
           emit_u32(w, offset);
}

/* Store one raw 64-bit host register into CPU state through R11. */
static bool emit_store_cpu_u64_reg(rv64_jit_writer_t *w, uint32_t offset,
                                   host_reg_t src)
{
    return emit_rex64(w, (uint8_t)src, HOST_REG_R11) &&
           emit_u8(w, 0x89) &&
           emit_u8(w, jit_modrm(HOST_MODRM_MOD_DISP32, (uint8_t)src,
                                HOST_REG_R11)) &&
           emit_u32(w, offset);
}

/* Test one 64-bit CPU-state field against a sign-extended imm32 mask. */
static bool emit_test_cpu_u64_imm32(rv64_jit_writer_t *w, uint32_t offset,
                                    uint32_t mask)
{
    /*
     * `REX.W f7 /0` is TEST r/m64, imm32.  The FS mask is positive and fits
     * imm32, so x86 sign extension cannot introduce unrelated high bits.
     */
    return emit_rex64(w, 0, HOST_REG_R11) &&
           emit_u8(w, 0xf7) &&
           emit_u8(w, jit_modrm(HOST_MODRM_MOD_DISP32, 0,
                                HOST_REG_R11)) &&
           emit_u32(w, offset) && emit_u32(w, mask);
}
#endif

/* Copy one cached host-register value into RAX for generic emitters. */
static bool emit_mov_rax_hreg(rv64_jit_writer_t *w, rv64_jit_hreg_t hreg)
{
    const uint8_t src = jit_hreg_x86_reg(hreg);

    /* `mov rax, hreg` is encoded as `REX.W 89 /r` with RAX in the r/m field. */
    return emit_rex64(w, src, 0) &&
           emit_u8(w, 0x89) &&
           emit_u8(w, jit_modrm(3, src, 0));
}

/* Copy one cached host-register value into RCX for second operands. */
static bool emit_mov_rcx_hreg(rv64_jit_writer_t *w, rv64_jit_hreg_t hreg)
{
    const uint8_t src = jit_hreg_x86_reg(hreg);

    /* RCX is r/m field 1 in `mov rcx, hreg`. */
    return emit_rex64(w, src, 1) &&
           emit_u8(w, 0x89) &&
           emit_u8(w, jit_modrm(3, src, 1));
}

/* Copy the RAX temporary result into a cached host register. */
static bool emit_mov_hreg_rax(rv64_jit_writer_t *w, rv64_jit_hreg_t hreg)
{
    const uint8_t dst = jit_hreg_x86_reg(hreg);

    /* `mov hreg, rax` is `REX.W 89 /r` with RAX in the reg field. */
    return emit_rex64(w, 0, dst) &&
           emit_u8(w, 0x89) &&
           emit_u8(w, jit_modrm(3, 0, dst));
}

/* Copy one cached host register to another. */
static bool emit_mov_hreg_hreg(rv64_jit_writer_t *w, rv64_jit_hreg_t dst,
                               rv64_jit_hreg_t src)
{
    const uint8_t dst_reg = jit_hreg_x86_reg(dst);
    const uint8_t src_reg = jit_hreg_x86_reg(src);

    if (dst == src)
    {
        return true;
    }

    /* `mov dst, src` keeps both operands in 64-bit host registers. */
    return emit_rex64(w, src_reg, dst_reg) &&
           emit_u8(w, 0x89) &&
           emit_u8(w, jit_modrm(3, src_reg, dst_reg));
}

/* Emit `hreg op imm32` using x86 group-1 immediate ALU operations. */
static bool emit_hreg_imm32_alu64(rv64_jit_writer_t *w, rv64_jit_hreg_t hreg,
                                  uint8_t subop, int32_t imm)
{
    const uint8_t dst = jit_hreg_x86_reg(hreg);

    return emit_rex64(w, subop, dst) &&
           emit_u8(w, 0x81) &&
           emit_u8(w, jit_modrm(3, subop, dst)) &&
           emit_u32(w, (uint32_t)imm);
}

/* Emit `dst op src` directly between two cached 64-bit host registers. */
static bool emit_hreg_hreg_alu64(rv64_jit_writer_t *w, rv64_jit_hreg_t dst,
                                 rv64_jit_hreg_t src, uint8_t opcode)
{
    const uint8_t dst_reg = jit_hreg_x86_reg(dst);
    const uint8_t src_reg = jit_hreg_x86_reg(src);

    return emit_rex64(w, src_reg, dst_reg) &&
           emit_u8(w, opcode) &&
           emit_u8(w, jit_modrm(3, src_reg, dst_reg));
}

/* Compare two cached 64-bit values directly without scratch-register moves. */
static bool emit_cmp_hreg_hreg(rv64_jit_writer_t *w,
                               rv64_jit_hreg_t lhs,
                               rv64_jit_hreg_t rhs)
{
    const uint8_t lhs_reg = jit_hreg_x86_reg(lhs);
    const uint8_t rhs_reg = jit_hreg_x86_reg(rhs);

    return emit_rex64(w, rhs_reg, lhs_reg) &&
           emit_u8(w, 0x39) &&
           emit_u8(w, jit_modrm(3, rhs_reg, lhs_reg));
}

/* Set equality flags by testing one cached 64-bit value against itself. */
static bool emit_test_hreg_hreg(rv64_jit_writer_t *w,
                                rv64_jit_hreg_t hreg)
{
    const uint8_t reg = jit_hreg_x86_reg(hreg);

    return emit_rex64(w, reg, reg) &&
           emit_u8(w, 0x85) &&
           emit_u8(w, jit_modrm(3, reg, reg));
}

/* Materialise one condition-code result directly in a cached host register. */
static bool emit_setcc_hreg(rv64_jit_writer_t *w,
                            rv64_jit_hreg_t hreg,
                            uint8_t setcc_opcode)
{
    const uint8_t reg = jit_hreg_x86_reg(hreg);
    const bool needs_byte_rex =
        (reg & 7u) >= 4u || (reg & HOST_REG_EXT_BIT) != 0;

    /*
     * A REX prefix selects BPL and the other modern low-byte names instead of
     * AH-CH-DH-BH. Extended registers additionally need B for SETcc, then both
     * R and B while MOVZX widens the result into the same 32-bit register.
     */
    uint8_t setcc_rex = HOST_REX_BASE;
    uint8_t movzx_rex = HOST_REX_BASE;

    if ((reg & HOST_REG_EXT_BIT) != 0)
    {
        setcc_rex |= HOST_REX_B;
        movzx_rex |= HOST_REX_R | HOST_REX_B;
    }

    return (!needs_byte_rex || emit_u8(w, setcc_rex)) &&
           emit_u8(w, 0x0f) &&
           emit_u8(w, setcc_opcode) &&
           emit_u8(w, jit_modrm(3, 0, reg)) &&
           (!needs_byte_rex || emit_u8(w, movzx_rex)) &&
           emit_u8(w, 0x0f) &&
           emit_u8(w, 0xb6) &&
           emit_u8(w, jit_modrm(3, reg, reg));
}

/* Emit `dst32 op src32` directly between cached host registers. */
static bool emit_hreg_hreg_alu32(rv64_jit_writer_t *w, rv64_jit_hreg_t dst,
                                 rv64_jit_hreg_t src, uint8_t opcode)
{
    const uint8_t dst_reg = jit_hreg_x86_reg(dst);
    const uint8_t src_reg = jit_hreg_x86_reg(src);

    return emit_rex32_if_needed(w, src_reg, dst_reg) &&
           emit_u8(w, opcode) &&
           emit_u8(w, jit_modrm(3, src_reg, dst_reg));
}

/* Emit `imul dst32, src32`, keeping the low 32-bit product in dst32. */
static bool emit_hreg_hreg_imul32(rv64_jit_writer_t *w, rv64_jit_hreg_t dst,
                                  rv64_jit_hreg_t src)
{
    const uint8_t dst_reg = jit_hreg_x86_reg(dst);
    const uint8_t src_reg = jit_hreg_x86_reg(src);

    return emit_rex32_if_needed(w, dst_reg, src_reg) &&
           emit_u8(w, 0x0f) && emit_u8(w, 0xaf) &&
           emit_u8(w, jit_modrm(3, dst_reg, src_reg));
}

/* Sign-extend one cached 32-bit W-form result back to RV64 XLEN. */
static bool emit_hreg_sext32(rv64_jit_writer_t *w, rv64_jit_hreg_t hreg)
{
    const uint8_t reg = jit_hreg_x86_reg(hreg);

    return emit_rex64(w, reg, reg) &&
           emit_u8(w, 0x63) &&
           emit_u8(w, jit_modrm(3, reg, reg));
}

/* Emit one immediate shift directly into a cached 64-bit host register. */
static bool emit_shift_hreg_imm(rv64_jit_writer_t *w, rv64_jit_hreg_t hreg,
                                uint8_t subop, uint8_t shamt)
{
    const uint8_t dst = jit_hreg_x86_reg(hreg);

    return emit_rex64(w, subop, dst) &&
           emit_u8(w, 0xc1) &&
           emit_u8(w, jit_modrm(3, subop, dst)) &&
           emit_u8(w, shamt);
}

/* Load a full-width constant into one cached host register. */
static bool emit_mov_hreg_imm64(rv64_jit_writer_t *w, rv64_jit_hreg_t hreg,
                                uint64_t value)
{
    const uint8_t dst = jit_hreg_x86_reg(hreg);

    if ((int64_t)value >= INT32_MIN && (int64_t)value <= INT32_MAX)
    {
        /*
         * `REX.W c7 /0 imm32` sign-extends a 32-bit immediate to 64 bits, which
         * is shorter than movabs and exactly matches small RV64 constants.
         */
        return emit_rex64(w, 0, dst) &&
               emit_u8(w, 0xc7) &&
               emit_u8(w, jit_modrm(3, 0, dst)) &&
               emit_u32(w, (uint32_t)value);
    }

    /* `REX.W b8+rd imm64` is the full movabs form for arbitrary RV64 values. */
    return emit_rex64(w, 0, dst) &&
           emit_u8(w, (uint8_t)(0xb8u + (dst & 7u))) &&
           emit_u64(w, value);
}

/*
 * Guest-register cache.
 *
 * The register cache is a compile-time description of which guest GPR is
 * currently held in each callee-saved host register.  `valid` means the slot is
 * assigned to a guest register, `loaded` means native code has materialised its
 * current value, and `dirty` means `CPU_state.gpr[]` is stale until a flush.
 * The monotonically increasing age gives a simple spill choice when all slots
 * are occupied.  Guest x0 is special: reads materialise zero and writes are
 * discarded, so it never needs a dirty slot.
 *
 * Emitters snapshot this metadata before instructions that may fail emission.
 * If a later byte write would exceed the arena or an unsupported sub-case is
 * found, the snapshot is restored so the next fallback path still sees the
 * register state that matches the bytes already emitted.
 */
/* Initialise per-block guest-register cache metadata. */
void rv64_jit_reg_cache_init(rv64_jit_reg_cache_t *regs)
{
    regs->slot_count = RV64_JIT_HREG_BASE_COUNT;
    regs->next_age = 1;
    regs->current_use_mask = 0;
    regs->live_after_mask = 0;

    for (uint32_t i = 0; i < RV64_JIT_HREG_COUNT; i++)
    {
        regs->slots[i] = (rv64_jit_reg_slot_t){
            .valid = false,
            .loaded = false,
            .dirty = false,
            .guest_reg = 0,
            .age = 0,
            .hreg = (rv64_jit_hreg_t)i,
        };
    }
}

/* Restore compile-time cache metadata after an instruction emitter rolls back. */
void rv64_jit_reg_cache_restore(rv64_jit_reg_cache_t *regs,
                                const rv64_jit_reg_cache_t *snapshot)
{
    *regs = *snapshot;
}

/* Install conservative per-instruction hints for register victim selection. */
void rv64_jit_reg_cache_set_liveness(rv64_jit_reg_cache_t *regs,
                                     uint32_t current_use_mask,
                                     uint32_t live_after_mask)
{
    /* Guest x0 never owns a cache slot. */
    regs->current_use_mask = current_use_mask & ~1u;
    regs->live_after_mask = live_after_mask & ~1u;
}

/* Find the host-register slot currently assigned to one guest register. */
static rv64_jit_reg_slot_t *jit_reg_find(rv64_jit_reg_cache_t *regs,
                                         uint32_t reg)
{
    for (uint32_t i = 0; i < regs->slot_count; i++)
    {
        rv64_jit_reg_slot_t *slot = &regs->slots[i];

        if (slot->valid && slot->guest_reg == reg)
        {
            return slot;
        }
    }

    return NULL;
}

#ifdef CONFIG_RISCV_FPU
/* Count mappings whose guest value is already materialised in a host slot. */
static uint32_t jit_reg_loaded_mapping_count(
    const rv64_jit_reg_cache_t *regs)
{
    uint32_t count = 0;

    for (uint32_t i = 0; i < regs->slot_count; i++)
    {
        const rv64_jit_reg_slot_t *slot = &regs->slots[i];
        count += slot->valid && slot->loaded ? 1u : 0u;
    }

    return count;
}

/* Count mappings whose architectural memory copy is currently stale. */
static uint32_t jit_reg_dirty_mapping_count(
    const rv64_jit_reg_cache_t *regs)
{
    uint32_t count = 0;

    for (uint32_t i = 0; i < regs->slot_count; i++)
    {
        const rv64_jit_reg_slot_t *slot = &regs->slots[i];
        count += slot->valid && slot->loaded && slot->dirty ? 1u : 0u;
    }

    return count;
}

/*
 * Discard one cached guest register after successful helper writeback.
 *
 * The old cached value is allowed to be dirty: publishing it would overwrite
 * the helper's new architectural result. The separately emitted trap stub is
 * built before this success-only metadata change, so a trapping instruction
 * still stores the pre-instruction value. Return one when a materialised host
 * value was discarded; all unaffected slots retain value and dirty state.
 */
static uint32_t jit_reg_discard_helper_written_guest(
    rv64_jit_reg_cache_t *regs, uint32_t reg)
{
    rv64_jit_reg_slot_t *slot = jit_reg_find(regs, reg);

    if (slot == NULL)
    {
        return 0;
    }

    const uint32_t invalidated = slot->loaded ? 1u : 0u;
    const rv64_jit_hreg_t hreg = slot->hreg;

    *slot = (rv64_jit_reg_slot_t){
        .valid = false,
        .loaded = false,
        .dirty = false,
        .guest_reg = 0,
        .age = 0,
        .hreg = hreg,
    };
    return invalidated;
}

/* Apply one precise helper effect to the successful native continuation. */
static void jit_reg_apply_fp_helper_effect(
    rv64_jit_reg_cache_t *regs, riscv_fpu_gpr_effect_t effect,
    uint32_t input_flushes, uint32_t trap_stores)
{
    uint32_t invalidated = 0;

    for (uint32_t reg = 1; reg < RISCV_GPR_NUM; reg++)
    {
        if ((effect.success_write_mask & (UINT32_C(1) << reg)) != 0)
        {
            invalidated +=
                jit_reg_discard_helper_written_guest(regs, reg);
        }
    }

    JIT_STAT_INC(emitted_sites.fp_helper_gpr_effect_sites);
    JIT_STAT_ADD(emitted_sites.fp_helper_gpr_mappings_preserved,
                 jit_reg_loaded_mapping_count(regs));
    JIT_STAT_ADD(emitted_sites.fp_helper_gpr_selective_invalidations,
                 invalidated);
    JIT_STAT_ADD(emitted_sites.fp_helper_gpr_input_flushes, input_flushes);
    JIT_STAT_ADD(emitted_sites.fp_helper_gpr_dirty_mappings_preserved,
                 jit_reg_dirty_mapping_count(regs));
    JIT_STAT_ADD(emitted_sites.fp_helper_gpr_trap_stores, trap_stores);
}

/*
 * DiffTest trap entry invokes an opaque reference callback before generated
 * code can publish deferred values. Keep that build on the full barrier path;
 * ordinary production builds use the audited closed FP helper graph.
 */
static bool jit_reg_can_defer_fp_helper_sync(
    riscv_fpu_gpr_effect_t effect)
{
#ifdef CONFIG_DIFFTEST
    (void)effect;
    return false;
#else
    return rv64_jit_fp_gpr_effects_enabled() && effect.precise &&
           effect.trap_preserves_gprs;
#endif
}
#endif

/* Emit a store-back for one dirty cached slot without changing metadata. */
static bool jit_reg_emit_flush_slot(rv64_jit_writer_t *w,
                                    const rv64_jit_reg_slot_t *slot)
{
    if (!slot->valid || !slot->loaded || !slot->dirty ||
        slot->guest_reg == RV64_GPR_ZERO)
    {
        return true;
    }

    return emit_store_gpr_hreg(w, slot->guest_reg, slot->hreg);
}

/* Flush one dirty slot and mark it clean once the native bytes are emitted. */
static bool jit_reg_flush_slot(rv64_jit_writer_t *w, rv64_jit_reg_slot_t *slot)
{
    if (!jit_reg_emit_flush_slot(w, slot))
    {
        return false;
    }

    slot->dirty = false;
    return true;
}

/*
 * Publish only dirty mappings named by an audited helper read mask. Successful
 * stores become clean in continuing metadata; an enclosing instruction
 * snapshot restores both bytes and metadata if later emission fails.
 */
static bool jit_reg_flush_mask(rv64_jit_writer_t *w,
                               rv64_jit_reg_cache_t *regs,
                               uint32_t mask, uint32_t *flushed)
{
    Assert(flushed != NULL, "jit: missing selective-flush count");
    *flushed = 0;

    for (uint32_t i = 0; i < regs->slot_count; i++)
    {
        rv64_jit_reg_slot_t *slot = &regs->slots[i];

        if (!slot->valid || slot->guest_reg == RV64_GPR_ZERO ||
            (mask & (UINT32_C(1) << slot->guest_reg)) == 0)
        {
            continue;
        }

        const bool emitted_store = slot->loaded && slot->dirty;
        if (!jit_reg_flush_slot(w, slot))
        {
            return false;
        }

        *flushed += emitted_store ? 1u : 0u;
    }

    return true;
}

/* Flush every dirty cached guest register before helper-visible exits. */
static bool jit_reg_flush_all_dirty(rv64_jit_writer_t *w,
                                    rv64_jit_reg_cache_t *regs)
{
    for (uint32_t i = 0; i < regs->slot_count; i++)
    {
        if (!jit_reg_flush_slot(w, &regs->slots[i]))
        {
            return false;
        }
    }

    return true;
}

/* Emit all dirty store-backs without changing the continuing path metadata. */
static bool jit_reg_emit_flush_all_dirty(rv64_jit_writer_t *w,
                                         const rv64_jit_reg_cache_t *regs)
{
    for (uint32_t i = 0; i < regs->slot_count; i++)
    {
        if (!jit_reg_emit_flush_slot(w, &regs->slots[i]))
        {
            return false;
        }
    }

    return true;
}

/*
 * Select a free slot or a liveness-guided LRU victim.
 *
 * Current operands are pinned because several emitters retain their slot
 * pointers while allocating the destination. Among the remaining slots, prefer
 * a value with no later fall-through use. Dirty victims are still written back
 * by jit_reg_alloc(), so this hint cannot discard architectural state needed by
 * a taken side exit, trap, helper, or later block.
 */
static rv64_jit_reg_slot_t *jit_reg_choose_slot(rv64_jit_reg_cache_t *regs)
{
    rv64_jit_reg_slot_t *oldest_unpinned = NULL;
    rv64_jit_reg_slot_t *oldest_dead = NULL;

    for (uint32_t i = 0; i < regs->slot_count; i++)
    {
        rv64_jit_reg_slot_t *slot = &regs->slots[i];

        if (!slot->valid)
        {
            return slot;
        }

        const uint32_t guest_bit = 1u << slot->guest_reg;

        if ((regs->current_use_mask & guest_bit) != 0)
        {
            continue;
        }

        if (!slot->loaded)
        {
            return slot;
        }

        if (oldest_unpinned == NULL ||
            slot->age < oldest_unpinned->age)
        {
            oldest_unpinned = slot;
        }

        if ((regs->live_after_mask & guest_bit) == 0 &&
            (oldest_dead == NULL || slot->age < oldest_dead->age))
        {
            oldest_dead = slot;
        }
    }

    if (oldest_dead != NULL)
    {
        JIT_STAT_INC(emitted_sites.reg_cache_dead_victims);

        if (oldest_unpinned != oldest_dead &&
            oldest_unpinned != NULL &&
            (regs->live_after_mask &
             (1u << oldest_unpinned->guest_reg)) != 0)
        {
            JIT_STAT_INC(emitted_sites.reg_cache_live_lru_avoided);
        }

        return oldest_dead;
    }

    return oldest_unpinned;
}

/* Reserve a cache slot for one guest register, spilling the LRU victim if needed. */
static rv64_jit_reg_slot_t *jit_reg_alloc(rv64_jit_writer_t *w,
                                          rv64_jit_reg_cache_t *regs,
                                          uint32_t reg)
{
    rv64_jit_reg_slot_t *slot = jit_reg_find(regs, reg);

    if (slot != NULL)
    {
        slot->age = regs->next_age++;
        return slot;
    }

    slot = jit_reg_choose_slot(regs);
    if (slot == NULL)
    {
        return NULL;
    }

    const bool spill = slot->valid && slot->loaded && slot->dirty &&
                       slot->guest_reg != RV64_GPR_ZERO;

    if (!jit_reg_flush_slot(w, slot))
    {
        return NULL;
    }

    if (spill)
    {
        JIT_STAT_INC(emitted_sites.reg_cache_spills);
    }

    slot->valid = true;
    slot->loaded = false;
    slot->dirty = false;
    slot->guest_reg = reg;
    slot->age = regs->next_age++;
    return slot;
}

/* Return a slot whose host register definitely contains the guest value. */
static rv64_jit_reg_slot_t *jit_reg_loaded_slot(rv64_jit_writer_t *w,
                                                rv64_jit_reg_cache_t *regs,
                                                uint32_t reg)
{
    rv64_jit_reg_slot_t *slot = jit_reg_alloc(w, regs, reg);

    if (slot == NULL)
    {
        return NULL;
    }

    if (!slot->loaded)
    {
        if (!emit_load_gpr_hreg(w, slot->hreg, reg))
        {
            return NULL;
        }

        slot->loaded = true;
    }

    slot->age = regs->next_age++;
    return slot;
}

/*
 * Materialise every loop-carried GPR once before the native loop header.
 *
 * The strict compile pre-scan guarantees that this block cannot call a C
 * helper or use R8 as a memory scratch register before its self-backedge. With
 * every referenced GPR already assigned, the loop body cannot spill or change
 * the mapping, so a taken backedge can safely retain the host-register values.
 *
 * R9D and ESI are otherwise-unused caller-saved registers in this helper-free
 * path. Keeping the completed-lap count and budget remaining after the current
 * lap there avoids serial accesses to the tiny loop-ABI globals on every taken
 * lap.
 */
bool rv64_jit_prepare_stable_loop_regs(rv64_jit_writer_t *w,
                                       rv64_jit_reg_cache_t *regs,
                                       uint32_t reg_mask,
                                       uint32_t loop_insn_count)
{
    if ((reg_mask & 1u) != 0 ||
        (uint32_t)__builtin_popcount(reg_mask) > RV64_JIT_HREG_COUNT ||
        loop_insn_count == 0)
    {
        return false;
    }

    regs->slot_count = RV64_JIT_HREG_COUNT;

    for (uint32_t reg = 1; reg < 32u; reg++)
    {
        if ((reg_mask & (1u << reg)) != 0 &&
            jit_reg_loaded_slot(w, regs, reg) == NULL)
        {
            return false;
        }
    }

    return emit_movabs_rdx(
               w, (uint64_t)(uintptr_t)&rv64_jit_loop_extra) &&
           emit_mov_r9d_m32_rdx(w) &&
           emit_movabs_rdx(
               w, (uint64_t)(uintptr_t)&rv64_jit_entry_budget) &&
           emit_mov_esi_m32_rdx(w) &&
           emit_sub_esi_r9d(w) &&
           emit_sub_esi_imm32(w, loop_insn_count);
}

/* Materialise a guest register in RAX, treating x0 as constant zero. */
static bool jit_reg_read_rax(rv64_jit_writer_t *w,
                             rv64_jit_reg_cache_t *regs, uint32_t reg)
{
    if (reg == RV64_GPR_ZERO)
    {
        return emit_zero_rax(w);
    }

    rv64_jit_reg_slot_t *slot = jit_reg_loaded_slot(w, regs, reg);
    return slot != NULL && emit_mov_rax_hreg(w, slot->hreg);
}

/* Materialise a guest register in RCX, treating x0 as constant zero. */
static bool jit_reg_read_rcx(rv64_jit_writer_t *w,
                             rv64_jit_reg_cache_t *regs, uint32_t reg)
{
    if (reg == RV64_GPR_ZERO)
    {
        return emit_u8(w, 0x31) && emit_u8(w, 0xc9);
    }

    rv64_jit_reg_slot_t *slot = jit_reg_loaded_slot(w, regs, reg);
    return slot != NULL && emit_mov_rcx_hreg(w, slot->hreg);
}

/* Write the current RAX result into one guest-register cache slot. */
static bool jit_reg_write_rax(rv64_jit_writer_t *w,
                              rv64_jit_reg_cache_t *regs, uint32_t reg)
{
    if (reg == RV64_GPR_ZERO)
    {
        return true;
    }

    rv64_jit_reg_slot_t *slot = jit_reg_alloc(w, regs, reg);

    if (slot == NULL || !emit_mov_hreg_rax(w, slot->hreg))
    {
        return false;
    }

    slot->loaded = true;
    slot->dirty = true;
    slot->age = regs->next_age++;
    return true;
}

/* Write a constant value into one guest-register cache slot. */
static bool jit_reg_write_imm(rv64_jit_writer_t *w,
                              rv64_jit_reg_cache_t *regs, uint32_t reg,
                              uint64_t value)
{
    if (reg == RV64_GPR_ZERO)
    {
        return true;
    }

    rv64_jit_reg_slot_t *slot = jit_reg_alloc(w, regs, reg);

    if (slot == NULL || !emit_mov_hreg_imm64(w, slot->hreg, value))
    {
        return false;
    }

    slot->loaded = true;
    slot->dirty = true;
    slot->age = regs->next_age++;
    return true;
}

/*
 * Lower SLT/SLTU entirely between existing cache slots.
 *
 * SETcc does not alter flags, so it is safe for the destination to alias either
 * source: the comparison reads both old values before the low-byte result is
 * written and zero-extended.
 */
static bool jit_reg_try_emit_cached_compare(
    rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs,
    uint32_t rd, uint32_t rs1, uint32_t rs2,
    uint8_t setcc_opcode, bool *handled)
{
    *handled = false;

    if (rd == RV64_GPR_ZERO ||
        rs1 == RV64_GPR_ZERO ||
        rs2 == RV64_GPR_ZERO)
    {
        return true;
    }

    rv64_jit_reg_slot_t *dst = jit_reg_find(regs, rd);
    rv64_jit_reg_slot_t *lhs = jit_reg_find(regs, rs1);
    rv64_jit_reg_slot_t *rhs = jit_reg_find(regs, rs2);

    if (dst == NULL || lhs == NULL || rhs == NULL ||
        !dst->loaded || !lhs->loaded || !rhs->loaded)
    {
        return true;
    }

    *handled = true;

    if (!emit_cmp_hreg_hreg(w, lhs->hreg, rhs->hreg) ||
        !emit_setcc_hreg(w, dst->hreg, setcc_opcode))
    {
        return false;
    }

    lhs->age = regs->next_age++;
    rhs->age = regs->next_age++;
    dst->loaded = true;
    dst->dirty = true;
    dst->age = regs->next_age++;
    return true;
}

/* Mark a cache slot as the freshly written value of its assigned guest register. */
static void jit_reg_mark_written(rv64_jit_reg_cache_t *regs,
                                 rv64_jit_reg_slot_t *slot)
{
    slot->loaded = true;
    slot->dirty = true;
    slot->age = regs->next_age++;
}

/* Copy a guest register value to another cache slot without touching memory. */
static bool jit_reg_copy(rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs,
                         uint32_t dst_reg, uint32_t src_reg)
{
    if (dst_reg == RV64_GPR_ZERO)
    {
        return true;
    }

    if (src_reg == RV64_GPR_ZERO)
    {
        return jit_reg_write_imm(w, regs, dst_reg, 0);
    }

    rv64_jit_reg_slot_t *src = jit_reg_loaded_slot(w, regs, src_reg);
    if (src == NULL)
    {
        return false;
    }

    if (dst_reg == src_reg)
    {
        return true;
    }

    rv64_jit_reg_slot_t *dst = jit_reg_alloc(w, regs, dst_reg);
    if (dst == NULL || !emit_mov_hreg_hreg(w, dst->hreg, src->hreg))
    {
        return false;
    }

    dst->loaded = true;
    dst->dirty = true;
    dst->age = regs->next_age++;
    return true;
}

/* Store an immediate guest PC by materialising it in RAX first. */
static bool emit_store_pc_imm(rv64_jit_writer_t *w, vaddr_t pc)
{
    return emit_movabs_rax(w, pc) &&
           /* `49 89 83 disp32` stores RAX into `cpu.pc` through R11. */
           emit_u8(w, 0x49) && emit_u8(w, 0x89) &&
           emit_u8(w, 0x83) && emit_u32(w, jit_pc_offset());
}

/* Store a dynamic guest PC already held in RAX. */
static bool emit_store_rax_pc(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x49) && emit_u8(w, 0x89) &&
           emit_u8(w, 0x83) && emit_u32(w, jit_pc_offset());
}

/* Emit `add rax, imm32`, whose immediate is sign-extended by x86-64. */
static bool emit_add_rax_imm32(rv64_jit_writer_t *w, int32_t imm)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x05) && emit_u32(w, (uint32_t)imm);
}

/* Emit `and rax, imm32`, whose immediate is sign-extended by x86-64. */
static bool emit_and_rax_imm32(rv64_jit_writer_t *w, int32_t imm)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x25) && emit_u32(w, (uint32_t)imm);
}

/* Emit one RAX op RCX 64-bit ALU instruction selected by the opcode byte. */
static bool emit_rax_rcx_alu64(rv64_jit_writer_t *w, uint8_t opcode)
{
    /*
     * Opcodes use ModRM C8 (`rax, rcx`): 01=ADD, 29=SUB, 31=XOR,
     * 09=OR and 21=AND. REX.W makes the operation full 64-bit.
     */
    return emit_u8(w, 0x48) && emit_u8(w, opcode) && emit_u8(w, 0xc8);
}

/* Emit one RAX op RDX 64-bit ALU instruction selected by the opcode byte. */
static bool emit_rax_rdx_alu64(rv64_jit_writer_t *w, uint8_t opcode)
{
    return emit_u8(w, 0x48) && emit_u8(w, opcode) && emit_u8(w, 0xd0);
}

/* Emit one RCX op RDX 64-bit ALU instruction selected by the opcode byte. */
static bool emit_rcx_rdx_alu64(rv64_jit_writer_t *w, uint8_t opcode)
{
    return emit_u8(w, 0x48) && emit_u8(w, opcode) && emit_u8(w, 0xd1);
}

/* Emit one EAX op ECX 32-bit ALU instruction without sign extension. */
static bool emit_eax_ecx_alu32(rv64_jit_writer_t *w, uint8_t opcode)
{
    return emit_u8(w, opcode) && emit_u8(w, 0xc8);
}

/* Emit one EAX op ECX 32-bit ALU instruction, then sign-extend to 64 bits. */
static bool emit_eax_ecx_alu32_sext(rv64_jit_writer_t *w, uint8_t opcode)
{
    /* W-form RV64 ALU operations keep low 32 bits, then CDQE sign-extends EAX. */
    return emit_u8(w, opcode) && emit_u8(w, 0xc8) &&
           emit_u8(w, 0x48) && emit_u8(w, 0x98);
}

/* Emit a 32-bit immediate shift of EAX, then sign-extend to 64 bits. */
static bool emit_shift_eax_imm_sext(rv64_jit_writer_t *w, uint8_t subop, uint8_t shamt)
{
    /* Group-2 ModRM subops are e0=SHL, e8=SHR and f8=SAR on EAX. */
    return emit_u8(w, 0xc1) && emit_u8(w, subop) && emit_u8(w, shamt) &&
           emit_u8(w, 0x48) && emit_u8(w, 0x98);
}

/* Emit a 64-bit variable shift of RAX by CL. */
static bool emit_shift_rax_cl(rv64_jit_writer_t *w, uint8_t subop)
{
    /* D3 uses CL as the variable shift count; RISC-V masks the count similarly. */
    return emit_u8(w, 0x48) && emit_u8(w, 0xd3) && emit_u8(w, subop);
}

/* Emit a 32-bit variable shift of EAX by CL, then sign-extend to 64 bits. */
static bool emit_shift_eax_cl_sext(rv64_jit_writer_t *w, uint8_t subop)
{
    /* D3 uses CL as the variable shift count; CDQE sign-extends W-form results. */
    return emit_u8(w, 0xd3) && emit_u8(w, subop) &&
           emit_u8(w, 0x48) && emit_u8(w, 0x98);
}

/* Emit `cmp rax, rcx` for signed or unsigned setcc operations. */
static bool emit_cmp_rax_rcx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x39) && emit_u8(w, 0xc8);
}

/* Compare RCX with RDX, used by binary64 exponent classification. */
static bool emit_cmp_rcx_rdx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x39) && emit_u8(w, 0xd1);
}

/* Test RAX against a mask in RDX without changing either operand. */
static bool emit_test_rax_rdx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x85) && emit_u8(w, 0xd0);
}

/* Emit `cmp rax, imm32`, using x86-64 sign-extension of the immediate. */
static bool emit_cmp_rax_imm32(rv64_jit_writer_t *w, int32_t imm)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x3d) && emit_u32(w, (uint32_t)imm);
}

/* Materialise a condition-code result as 0 or 1 in RAX. */
static bool emit_setcc_rax(rv64_jit_writer_t *w, uint8_t setcc_opcode)
{
    /* `0f setcc c0` writes AL, then `0f b6 c0` zero-extends AL into EAX/RAX. */
    return emit_u8(w, 0x0f) && emit_u8(w, setcc_opcode) &&
           emit_u8(w, 0xc0) &&
           emit_u8(w, 0x0f) && emit_u8(w, 0xb6) && emit_u8(w, 0xc0);
}

/* Emit a conditional branch with a rel32 placeholder and return its patch site. */
static bool emit_jcc_rel32_placeholder(rv64_jit_writer_t *w, uint8_t jcc_opcode,
                                       uint8_t **disp)
{
    /* x86 near conditional branches are `0f 8x disp32`; `disp` points at disp32. */
    if (!emit_u8(w, 0x0f) || !emit_u8(w, jcc_opcode))
    {
        return false;
    }

    *disp = w->cur;
    return emit_u32(w, 0);
}

/* Emit an unconditional `jmp rel32` and return its displacement patch site. */
static bool emit_jmp_rel32_placeholder(rv64_jit_writer_t *w, uint8_t **disp)
{
    /* `e9 disp32` jumps relative to the byte after the 32-bit displacement. */
    if (!emit_u8(w, 0xe9))
    {
        return false;
    }

    *disp = w->cur;
    return emit_u32(w, 0);
}

/* Emit `movabs rax, target; call rax` for rare helper-backed side paths. */
static bool emit_call_abs(rv64_jit_writer_t *w, uintptr_t target)
{
    return emit_movabs_rax(w, (uint64_t)target) &&
           emit_u8(w, 0xff) && emit_u8(w, 0xd0);
}

/* Emit `jmp rax`, used by direct links to enter another block body. */
static bool emit_jmp_rax(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0xff) && emit_u8(w, 0xe0);
}

/* Emit `jmp r9`, retaining a helper-returned body pointer across stat updates. */
static bool emit_jmp_r9(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x41) && emit_u8(w, 0xff) && emit_u8(w, 0xe1);
}

#if RV64_JIT_STATS
enum
{
    RV64_JIT_CHAIN_STATS_DIRECT = 0,
    RV64_JIT_CHAIN_STATS_RETURN_PRIMARY,
    RV64_JIT_CHAIN_STATS_RETURN_SECONDARY,
    RV64_JIT_CHAIN_STATS_JALR_PRIMARY,
    RV64_JIT_CHAIN_STATS_JALR_SECONDARY,
};

/* Clear the ESI route/way discriminator used by statistics-only branches. */
static bool emit_zero_esi(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x31) && emit_u8(w, 0xf6);
}

/* Test whether the statistics route/way discriminator is zero. */
static bool emit_test_esi_esi(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x85) && emit_u8(w, 0xf6);
}

/* Compare the statistics route in ESI with one small mode value. */
static bool emit_cmp_esi_imm8(rv64_jit_writer_t *w, uint8_t value)
{
    return emit_u8(w, 0x83) && emit_u8(w, 0xfe) && emit_u8(w, value);
}
#endif

#if RV64_JIT_STATS
/* Emit a native-side increment for one 64-bit counter. */
static bool emit_inc_u64_counter(rv64_jit_writer_t *w, uint64_t *counter)
{
    /*
     * `48 ff 00` is `inc qword ptr [rax]`.  The helper deliberately clobbers
     * RAX; callers place it after address proof and before instructions that
     * overwrite RAX or no longer need it.  The increment is non-atomic because
     * NEMU executes this CPU and its generated code on one execution thread.
     */
    return emit_movabs_rax(w, (uint64_t)(uintptr_t)counter) &&
           emit_u8(w, 0x48) && emit_u8(w, 0xff) && emit_u8(w, 0x00);
}

/* Increment a counter through RDI when a live load address/result occupies RAX. */
static bool emit_inc_u64_counter_preserve_rax(
    rv64_jit_writer_t *w, uint64_t *counter)
{
    return emit_movabs_rdi(w, (uint64_t)(uintptr_t)counter) &&
           emit_u8(w, 0x48) && emit_u8(w, 0xff) && emit_u8(w, 0x07);
}

/* Set or test the optional source-specific counter carried by a patched edge. */
static bool emit_zero_edi(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x31) && emit_u8(w, 0xff);
}

static bool emit_test_rdi_rdi(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x85) && emit_u8(w, 0xff);
}

static bool emit_inc_m64_rdi(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0xff) && emit_u8(w, 0x07);
}
#endif

/* Emit an optional native-side increment for one 64-bit JIT stat counter. */
static bool emit_inc_jit_stat_counter(rv64_jit_writer_t *w, uint64_t *counter)
{
#if RV64_JIT_STATS
    return emit_inc_u64_counter(w, counter);
#else
    (void)w;
    (void)counter;
    return true;
#endif
}

/* Increment a statistic while retaining the guest address or load result in RAX. */
static bool emit_inc_jit_stat_counter_preserve_rax(
    rv64_jit_writer_t *w, uint64_t *counter)
{
#if RV64_JIT_STATS
    return emit_inc_u64_counter_preserve_rax(w, counter);
#else
    (void)w;
    (void)counter;
    return true;
#endif
}

/* Count one runtime load that completed through the inline translated-PMEM path. */
static bool emit_inline_paged_load_hit_stats(rv64_jit_writer_t *w)
{
    return emit_inc_jit_stat_counter(w, &rv64_jit_stats.data_tlb_hits) &&
           emit_inc_jit_stat_counter(w, &rv64_jit_stats.inline_paged_load_hits);
}

/* Count one runtime store that completed through the inline translated-PMEM path. */
static bool emit_inline_paged_store_hit_stats(rv64_jit_writer_t *w)
{
    return emit_inc_jit_stat_counter(w, &rv64_jit_stats.data_tlb_hits) &&
           emit_inc_jit_stat_counter(w, &rv64_jit_stats.inline_paged_store_hits);
}

/* Patch a rel32 displacement emitted by a previous branch helper. */
static void patch_rel32(uint8_t *disp, const uint8_t *target)
{
    /*
     * `disp` points at the four-byte signed displacement itself.  x86 measures
     * a relative branch from the byte immediately after that field, hence the
     * addition of its C type size before subtracting from the target.
     */
    int64_t rel = target - (disp + sizeof(int32_t));
    Assert(rel >= INT32_MIN && rel <= INT32_MAX, "jit: rel32 target is out of range");
    int32_t rel32 = (int32_t)rel;
    memcpy(disp, &rel32, sizeof(rel32));
}

/* Reserve one private direct-MMIO route; excess sites keep the old classifier. */
static uint8_t mmio_route_reserve_site(
    rv64_jit_mmio_route_builder_t *routes, uint64_t guest_addr,
    uint64_t host_ptr)
{
    if (routes == NULL ||
        routes->site_count >= RV64_JIT_MMIO_ROUTE_MAX_SITES)
    {
        return RV64_JIT_MMIO_ROUTE_NO_SITE;
    }

    Assert(host_ptr != 0, "jit: direct-MMIO route has a null host pointer");

    const uint8_t site = routes->site_count++;
    routes->initial_routes[site] =
        (rv64_jit_mmio_route_t){
            .guest_addr_tag = guest_addr,
            .host_ptr = host_ptr,
        };
    return site;
}

/* Record one RIP-relative field reference for patching after native emission. */
static bool mmio_route_record_fixup(
    rv64_jit_mmio_route_builder_t *routes, uint8_t site,
    rv64_jit_mmio_route_field_t field, uint8_t *disp32,
    const uint8_t *next_ip)
{
    Assert(routes != NULL && site < routes->site_count,
           "jit: invalid direct-MMIO route site %u", site);
    Assert(field == RV64_JIT_MMIO_ROUTE_TAG ||
               field == RV64_JIT_MMIO_ROUTE_HOST,
           "jit: invalid direct-MMIO route field %u", field);
    Assert(routes->fixup_count < RV64_JIT_MMIO_ROUTE_MAX_FIXUPS,
           "jit: too many direct-MMIO route fixups");

    routes->fixups[routes->fixup_count++] =
        (rv64_jit_mmio_route_fixup_t){
            .disp32 = disp32,
            .next_ip = next_ip,
            .site = site,
            .field = (uint8_t)field,
        };
    return true;
}

/* Emit `mov rdx, qword ptr [rip + route.host]`. */
static bool emit_mmio_route_load_host_rdx(
    rv64_jit_writer_t *w, rv64_jit_mmio_route_builder_t *routes,
    uint8_t site)
{
    if (!emit_u8(w, 0x48) || !emit_u8(w, 0x8b) || !emit_u8(w, 0x15))
    {
        return false;
    }

    uint8_t *disp32 = w->cur;
    return emit_u32(w, 0) &&
           mmio_route_record_fixup(
               routes, site, RV64_JIT_MMIO_ROUTE_HOST, disp32, w->cur);
}

/* Emit `mov rdi, qword ptr [rip + route.host]`. */
static bool emit_mmio_route_load_host_rdi(
    rv64_jit_writer_t *w, rv64_jit_mmio_route_builder_t *routes,
    uint8_t site)
{
    if (!emit_u8(w, 0x48) || !emit_u8(w, 0x8b) || !emit_u8(w, 0x3d))
    {
        return false;
    }

    uint8_t *disp32 = w->cur;
    return emit_u32(w, 0) &&
           mmio_route_record_fixup(
               routes, site, RV64_JIT_MMIO_ROUTE_HOST, disp32, w->cur);
}

/* Emit `cmp rax, qword ptr [rip + route.tag]`. */
static bool emit_mmio_route_cmp_tag_rax(
    rv64_jit_writer_t *w, rv64_jit_mmio_route_builder_t *routes,
    uint8_t site)
{
    if (!emit_u8(w, 0x48) || !emit_u8(w, 0x3b) || !emit_u8(w, 0x05))
    {
        return false;
    }

    uint8_t *disp32 = w->cur;
    return emit_u32(w, 0) &&
           mmio_route_record_fixup(
               routes, site, RV64_JIT_MMIO_ROUTE_TAG, disp32, w->cur);
}

/* Emit `mov qword ptr [rip + route.tag], rax`. */
static bool emit_mmio_route_store_tag_rax(
    rv64_jit_writer_t *w, rv64_jit_mmio_route_builder_t *routes,
    uint8_t site)
{
    if (!emit_u8(w, 0x48) || !emit_u8(w, 0x89) || !emit_u8(w, 0x05))
    {
        return false;
    }

    uint8_t *disp32 = w->cur;
    return emit_u32(w, 0) &&
           mmio_route_record_fixup(
               routes, site, RV64_JIT_MMIO_ROUTE_TAG, disp32, w->cur);
}

/* Emit `mov qword ptr [rip + route.tag], rdi`. */
static bool emit_mmio_route_store_tag_rdi(
    rv64_jit_writer_t *w, rv64_jit_mmio_route_builder_t *routes,
    uint8_t site)
{
    if (!emit_u8(w, 0x48) || !emit_u8(w, 0x89) || !emit_u8(w, 0x3d))
    {
        return false;
    }

    uint8_t *disp32 = w->cur;
    return emit_u32(w, 0) &&
           mmio_route_record_fixup(
               routes, site, RV64_JIT_MMIO_ROUTE_TAG, disp32, w->cur);
}

/* Emit `mov qword ptr [rip + route.host], rdx`. */
static bool emit_mmio_route_store_host_rdx(
    rv64_jit_writer_t *w, rv64_jit_mmio_route_builder_t *routes,
    uint8_t site)
{
    if (!emit_u8(w, 0x48) || !emit_u8(w, 0x89) || !emit_u8(w, 0x15))
    {
        return false;
    }

    uint8_t *disp32 = w->cur;
    return emit_u32(w, 0) &&
           mmio_route_record_fixup(
               routes, site, RV64_JIT_MMIO_ROUTE_HOST, disp32, w->cur);
}

/* Emit `mov qword ptr [rip + route.host], rdi`. */
static bool emit_mmio_route_store_host_rdi(
    rv64_jit_writer_t *w, rv64_jit_mmio_route_builder_t *routes,
    uint8_t site)
{
    if (!emit_u8(w, 0x48) || !emit_u8(w, 0x89) || !emit_u8(w, 0x3d))
    {
        return false;
    }

    uint8_t *disp32 = w->cur;
    return emit_u32(w, 0) &&
           mmio_route_record_fixup(
               routes, site, RV64_JIT_MMIO_ROUTE_HOST, disp32, w->cur);
}

/*
 * Append one cache-line sidecar after terminal native code and resolve every
 * recorded data reference. Referenced entries start with an explicitly
 * contracted exact route observed at compilation; unused tail entries remain
 * zero. The allocation cursor includes the sidecar, while the compiler
 * separately retains the executable end for perf attribution.
 */
bool rv64_jit_finalise_mmio_routes(
    rv64_jit_writer_t *w, rv64_jit_mmio_route_builder_t *routes)
{
    Assert(routes != NULL, "jit: missing direct-MMIO route builder");

    if (routes->site_count == 0)
    {
        Assert(routes->fixup_count == 0,
               "jit: route fixups exist without route sites");
        return true;
    }

    Assert(routes->site_count <= RV64_JIT_MMIO_ROUTE_MAX_SITES,
           "jit: too many direct-MMIO route sites");

    const uintptr_t aligned =
        rv64_jit_align_up((uintptr_t)w->cur,
                          RV64_JIT_MMIO_ROUTE_LINE_SIZE);

    while ((uintptr_t)w->cur < aligned)
    {
        if (!emit_u8(w, 0))
        {
            return false;
        }
    }

    uint8_t *route_base = w->cur;

    for (uint32_t i = 0; i < RV64_JIT_MMIO_ROUTE_MAX_SITES; i++)
    {
        const rv64_jit_mmio_route_t initial =
            i < routes->site_count
                ? routes->initial_routes[i]
                : (rv64_jit_mmio_route_t){0};

        if (!emit_u64(w, initial.guest_addr_tag) ||
            !emit_u64(w, initial.host_ptr))
        {
            return false;
        }
    }

    for (uint32_t i = 0; i < routes->fixup_count; i++)
    {
        const rv64_jit_mmio_route_fixup_t *fixup = &routes->fixups[i];
        Assert(fixup->site < routes->site_count,
               "jit: route fixup refers to absent site %u", fixup->site);

        const size_t field_offset =
            fixup->field == RV64_JIT_MMIO_ROUTE_HOST
                ? offsetof(rv64_jit_mmio_route_t, host_ptr)
                : offsetof(rv64_jit_mmio_route_t, guest_addr_tag);
        const uint8_t *target =
            route_base +
            (size_t)fixup->site * sizeof(rv64_jit_mmio_route_t) +
            field_offset;
        const int64_t rel = target - fixup->next_ip;

        Assert(rel >= INT32_MIN && rel <= INT32_MAX,
               "jit: direct-MMIO route disp32 is out of range");
        const int32_t rel32 = (int32_t)rel;
        memcpy(fixup->disp32, &rel32, sizeof(rel32));
    }

    return true;
}

/*
 * Append and resolve the source-owned guarded jump cache. Generated code is
 * the only owner of this address: source invalidation retires that code, and an
 * arena reset retires both bytes together, so no block descriptor or reverse
 * target links are needed. Every entry starts empty and is populated only by a
 * successful run-time authoritative lookup.
 */
bool rv64_jit_finalise_indirect_jump_cache(
    rv64_jit_writer_t *w,
    rv64_jit_indirect_jump_cache_builder_t *builder)
{
    Assert(builder != NULL,
           "jit: missing RV64 indirect jump cache builder");

    if (!builder->used)
    {
        Assert(builder->fixup_count == 0,
               "jit: indirect jump cache fixup exists without a site");
        return true;
    }

    Assert(builder->fixup_count ==
               RV64_JIT_INDIRECT_JUMP_CACHE_MAX_FIXUPS,
           "jit: invalid RV64 indirect jump cache fixup count %u",
           builder->fixup_count);

    const uintptr_t aligned =
        rv64_jit_align_up((uintptr_t)w->cur,
                          RV64_JIT_INDIRECT_JUMP_CACHE_LINE_SIZE);
    const size_t padding = (size_t)(aligned - (uintptr_t)w->cur);
    const size_t available = (size_t)(w->end - w->cur);

    if (padding > available ||
        sizeof(rv64_jit_indirect_jump_cache_t) > available - padding)
    {
        w->overflowed = true;
        return false;
    }

    memset(w->cur, 0, padding);
    w->cur += padding;

    rv64_jit_indirect_jump_cache_t *persistent =
        (rv64_jit_indirect_jump_cache_t *)w->cur;
    memset(persistent, 0, sizeof(*persistent));
    w->cur += sizeof(*persistent);

    const uint64_t address = (uint64_t)(uintptr_t)persistent;
    memcpy(builder->address_immediates[0], &address, sizeof(address));
    return true;
}

/*
 * Append the single per-block indirect PIC after native bytes and patch each
 * `movabs` reference emitted before its arena address was known. The sidecar is
 * cache-line aligned so both ways remain together and never share mutable data
 * with the following block allocation.
 */
bool rv64_jit_finalise_indirect_pic(
    rv64_jit_writer_t *w, rv64_jit_indirect_pic_builder_t *builder,
    rv64_jit_indirect_pic_t **pic)
{
    Assert(builder != NULL, "jit: missing RV64 indirect PIC builder");
    Assert(pic != NULL, "jit: missing RV64 indirect PIC result");
    *pic = NULL;

    if (!builder->used)
    {
        Assert(builder->fixup_count == 0,
               "jit: indirect PIC fixups exist without a site");
        return true;
    }

    Assert(builder->kind < RV64_JIT_INDIRECT_PIC_KIND_COUNT,
           "jit: invalid RV64 indirect PIC builder kind %u", builder->kind);
    Assert(builder->fixup_count > 0 &&
               builder->fixup_count <= RV64_JIT_INDIRECT_PIC_MAX_FIXUPS,
           "jit: invalid RV64 indirect PIC fixup count %u",
           builder->fixup_count);

    const uintptr_t aligned =
        rv64_jit_align_up((uintptr_t)w->cur,
                          RV64_JIT_INDIRECT_PIC_LINE_SIZE);
    const size_t padding = (size_t)(aligned - (uintptr_t)w->cur);
    const size_t available = (size_t)(w->end - w->cur);

    if (padding > available ||
        sizeof(rv64_jit_indirect_pic_t) > available - padding)
    {
        w->overflowed = true;
        return false;
    }

    memset(w->cur, 0, padding);
    w->cur += padding;

    rv64_jit_indirect_pic_t *persistent =
        (rv64_jit_indirect_pic_t *)w->cur;
    memset(persistent, 0, sizeof(*persistent));
    persistent->kind = builder->kind;

    for (uint32_t i = 0; i < RV64_JIT_INDIRECT_PIC_WAYS; i++)
    {
        Assert(builder->selector_disps[i] != NULL &&
                   builder->target_disps[i] != NULL &&
                   builder->guarded_paths[i] != NULL &&
                   builder->patched_paths[i] != NULL,
               "jit: incomplete RV64 indirect PIC way %u", i);
        persistent->links[i] = (rv64_jit_link_t){
            .selector_disp = builder->selector_disps[i],
            .target_disp = builder->target_disps[i],
            .guarded_path = builder->guarded_paths[i],
            .patched_path = builder->patched_paths[i],
            .target_slot_index = UINT32_MAX,
            .pic_kind = builder->kind,
            .pic_way = (uint8_t)i,
            .patch_eligible = true,
            .dynamic = true,
        };
    }

    w->cur += sizeof(*persistent);

    const uint64_t address = (uint64_t)(uintptr_t)persistent;

    for (uint32_t i = 0; i < builder->fixup_count; i++)
    {
        memcpy(builder->address_immediates[i], &address, sizeof(address));
    }

    *pic = persistent;
    return true;
}

/* Forward declaration for exact-FP runtime permission failures below. */
static bool emit_interpreter_side_exit(rv64_jit_writer_t *w,
                                       rv64_jit_reg_cache_t *regs, vaddr_t pc,
                                       uint32_t completed_count,
                                       rv64_jit_side_exit_reason_t reason);

/*
 * FP memory lowering is placed beside the other FP emitters, before the shared
 * integer-memory definitions below. These declarations let it compose the
 * already-audited address, PMEM, and invalidation primitives without moving or
 * duplicating them.
 */
static bool emit_direct_pmem_load_rax(rv64_jit_writer_t *w,
                                      uint32_t funct3);
static bool emit_direct_pmem_store_from_rcx(rv64_jit_writer_t *w,
                                            uint32_t len);
static bool emit_guard_store_not_compiled_source(
    rv64_jit_writer_t *w, uint32_t len, uint8_t **cross_chunk_disp,
    uint8_t **source_chunk_disp);
static bool emit_guard_store_not_translation_dependency(
    rv64_jit_writer_t *w, uint8_t **data_page_table_disp,
    uint8_t **ifetch_page_table_disp);
static bool emit_alignment_guard_al(rv64_jit_writer_t *w, uint32_t len,
                                    uint8_t **slow_disp);
static bool emit_guard_bare_address_in_pmem(rv64_jit_writer_t *w,
                                            uint32_t len,
                                            uint8_t **slow_disp);
static void patch_tlb_guard(const rv64_jit_tlb_guard_patch_t *patch,
                            const uint8_t *slow_path);
static bool emit_inline_sv39_load_fast_path(
    rv64_jit_writer_t *w, uint32_t funct3, uint32_t len,
    rv64_jit_tlb_guard_patch_t *patch);
static bool emit_inline_sv39_store_address(
    rv64_jit_writer_t *w, uint32_t len,
    rv64_jit_tlb_guard_patch_t *patch);

#ifdef CONFIG_RISCV_FPU
#define RV64_JIT_FP32_SIGN_MASK UINT32_C(0x80000000)
#define RV64_JIT_FP32_EXPONENT_MASK UINT32_C(0x7f800000)
#define RV64_JIT_FP32_FRACTION_MASK UINT32_C(0x007fffff)
#define RV64_JIT_FP32_QUIET_NAN_BIT UINT32_C(0x00400000)
#define RV64_JIT_FP32_CANONICAL_NAN UINT32_C(0x7fc00000)
#define RV64_JIT_FP32_BOX_MASK UINT64_C(0xffffffff00000000)
#define RV64_JIT_FP64_SIGN_MASK UINT64_C(0x8000000000000000)
#define RV64_JIT_FP64_EXPONENT_MASK UINT64_C(0x7ff0000000000000)
#define RV64_JIT_FP64_FRACTION_MASK UINT64_C(0x000fffffffffffff)
#define RV64_JIT_FP64_QUIET_NAN_BIT UINT64_C(0x0008000000000000)

/*
 * Replace a malformed binary32 NaN box in RAX with canonical positive qNaN.
 * A valid box retains its raw low word, including signalling NaN payloads.
 */
static bool emit_fp_unbox_s_rax(rv64_jit_writer_t *w)
{
    uint8_t *boxed_disp = NULL;

    if (!emit_mov_rdx_rax(w) ||
        !emit_shr_rdx_imm(w, 32) ||
        !emit_cmp_edx_neg_one(w) ||
        !emit_jcc_rel32_placeholder(w, HOST_JCC_E, &boxed_disp) ||
        !emit_mov_eax_imm32(w, RV64_JIT_FP32_CANONICAL_NAN))
    {
        return false;
    }

    patch_rel32(boxed_disp, w->cur);
    return true;
}

/* Mark NEMU's stored FS field Dirty and keep the derived SD bit coherent. */
static bool emit_mark_fp_dirty(rv64_jit_writer_t *w)
{
    const uint64_t dirty_mask =
        (uint64_t)RISCV_MSTATUS_FS_DIRTY | (uint64_t)RISCV_MSTATUS_SD;

    return emit_load_cpu_u64_reg(w, HOST_REG_RAX,
                                 jit_mstatus_offset()) &&
           emit_movabs_rdx(w, dirty_mask) &&
           emit_rax_rdx_alu64(w, HOST_ALU_OR) &&
           emit_store_cpu_u64_reg(w, jit_mstatus_offset(),
                                  HOST_REG_RAX);
}

/* Emit one terminal FCLASS result arm and remember its jump to the join. */
static bool emit_fp_class_result_arm(rv64_jit_writer_t *w, uint32_t bit,
                                     uint8_t **done_disps,
                                     uint32_t *done_count)
{
    Assert(bit < 10u, "jit: invalid FCLASS result bit");
    Assert(*done_count < 10u, "jit: too many FCLASS result arms");

    return emit_mov_eax_imm32(w, 1u << bit) &&
           emit_jmp_rel32_placeholder(w,
                                      &done_disps[(*done_count)++]);
}

/* Classify an unboxed binary32 value in EAX without touching guest fflags. */
static bool emit_fp_classify_s_rax(rv64_jit_writer_t *w)
{
    uint8_t *exponent_all_ones_disp = NULL;
    uint8_t *exponent_zero_disp = NULL;
    uint8_t *positive_normal_disp = NULL;
    uint8_t *fraction_zero_disp = NULL;
    uint8_t *signalling_nan_disp = NULL;
    uint8_t *positive_infinity_disp = NULL;
    uint8_t *zero_fraction_disp = NULL;
    uint8_t *positive_subnormal_disp = NULL;
    uint8_t *positive_zero_disp = NULL;
    uint8_t *done_disps[10];
    uint32_t done_count = 0;

    if (!emit_mov_ecx_eax(w) ||
        !emit_and_ecx_imm32(w, RV64_JIT_FP32_EXPONENT_MASK) ||
        !emit_cmp_ecx_imm32(w, RV64_JIT_FP32_EXPONENT_MASK) ||
        !emit_jcc_rel32_placeholder(
            w, HOST_JCC_E, &exponent_all_ones_disp) ||
        !emit_test_ecx_ecx(w) ||
        !emit_jcc_rel32_placeholder(
            w, HOST_JCC_E, &exponent_zero_disp) ||
        !emit_test_eax_imm32(w, RV64_JIT_FP32_SIGN_MASK) ||
        !emit_jcc_rel32_placeholder(
            w, HOST_JCC_E, &positive_normal_disp) ||
        !emit_fp_class_result_arm(w, 1, done_disps, &done_count))
    {
        return false;
    }

    patch_rel32(positive_normal_disp, w->cur);
    if (!emit_fp_class_result_arm(w, 6, done_disps, &done_count))
    {
        return false;
    }

    patch_rel32(exponent_all_ones_disp, w->cur);
    if (!emit_mov_ecx_eax(w) ||
        !emit_and_ecx_imm32(w, RV64_JIT_FP32_FRACTION_MASK) ||
        !emit_test_ecx_ecx(w) ||
        !emit_jcc_rel32_placeholder(
            w, HOST_JCC_E, &fraction_zero_disp) ||
        !emit_test_eax_imm32(w, RV64_JIT_FP32_QUIET_NAN_BIT) ||
        !emit_jcc_rel32_placeholder(
            w, HOST_JCC_E, &signalling_nan_disp) ||
        !emit_fp_class_result_arm(w, 9, done_disps, &done_count))
    {
        return false;
    }

    patch_rel32(signalling_nan_disp, w->cur);
    if (!emit_fp_class_result_arm(w, 8, done_disps, &done_count))
    {
        return false;
    }

    patch_rel32(fraction_zero_disp, w->cur);
    if (!emit_test_eax_imm32(w, RV64_JIT_FP32_SIGN_MASK) ||
        !emit_jcc_rel32_placeholder(
            w, HOST_JCC_E, &positive_infinity_disp) ||
        !emit_fp_class_result_arm(w, 0, done_disps, &done_count))
    {
        return false;
    }

    patch_rel32(positive_infinity_disp, w->cur);
    if (!emit_fp_class_result_arm(w, 7, done_disps, &done_count))
    {
        return false;
    }

    patch_rel32(exponent_zero_disp, w->cur);
    if (!emit_mov_ecx_eax(w) ||
        !emit_and_ecx_imm32(w, RV64_JIT_FP32_FRACTION_MASK) ||
        !emit_test_ecx_ecx(w) ||
        !emit_jcc_rel32_placeholder(
            w, HOST_JCC_E, &zero_fraction_disp) ||
        !emit_test_eax_imm32(w, RV64_JIT_FP32_SIGN_MASK) ||
        !emit_jcc_rel32_placeholder(
            w, HOST_JCC_E, &positive_subnormal_disp) ||
        !emit_fp_class_result_arm(w, 2, done_disps, &done_count))
    {
        return false;
    }

    patch_rel32(positive_subnormal_disp, w->cur);
    if (!emit_fp_class_result_arm(w, 5, done_disps, &done_count))
    {
        return false;
    }

    patch_rel32(zero_fraction_disp, w->cur);
    if (!emit_test_eax_imm32(w, RV64_JIT_FP32_SIGN_MASK) ||
        !emit_jcc_rel32_placeholder(
            w, HOST_JCC_E, &positive_zero_disp) ||
        !emit_fp_class_result_arm(w, 3, done_disps, &done_count))
    {
        return false;
    }

    patch_rel32(positive_zero_disp, w->cur);
    if (!emit_fp_class_result_arm(w, 4, done_disps, &done_count))
    {
        return false;
    }

    for (uint32_t i = 0; i < done_count; i++)
    {
        patch_rel32(done_disps[i], w->cur);
    }
    return true;
}

/* Classify a raw binary64 value in RAX without touching guest fflags. */
static bool emit_fp_classify_d_rax(rv64_jit_writer_t *w)
{
    uint8_t *exponent_all_ones_disp = NULL;
    uint8_t *exponent_zero_disp = NULL;
    uint8_t *positive_normal_disp = NULL;
    uint8_t *fraction_zero_disp = NULL;
    uint8_t *signalling_nan_disp = NULL;
    uint8_t *positive_infinity_disp = NULL;
    uint8_t *zero_fraction_disp = NULL;
    uint8_t *positive_subnormal_disp = NULL;
    uint8_t *positive_zero_disp = NULL;
    uint8_t *done_disps[10];
    uint32_t done_count = 0;

    if (!emit_mov_rcx_rax(w) ||
        !emit_movabs_rdx(w, RV64_JIT_FP64_EXPONENT_MASK) ||
        !emit_rcx_rdx_alu64(w, HOST_ALU_AND) ||
        !emit_cmp_rcx_rdx(w) ||
        !emit_jcc_rel32_placeholder(
            w, HOST_JCC_E, &exponent_all_ones_disp) ||
        !emit_test_rcx_rcx(w) ||
        !emit_jcc_rel32_placeholder(
            w, HOST_JCC_E, &exponent_zero_disp) ||
        !emit_movabs_rdx(w, RV64_JIT_FP64_SIGN_MASK) ||
        !emit_test_rax_rdx(w) ||
        !emit_jcc_rel32_placeholder(
            w, HOST_JCC_E, &positive_normal_disp) ||
        !emit_fp_class_result_arm(w, 1, done_disps, &done_count))
    {
        return false;
    }

    patch_rel32(positive_normal_disp, w->cur);
    if (!emit_fp_class_result_arm(w, 6, done_disps, &done_count))
    {
        return false;
    }

    patch_rel32(exponent_all_ones_disp, w->cur);
    if (!emit_mov_rcx_rax(w) ||
        !emit_movabs_rdx(w, RV64_JIT_FP64_FRACTION_MASK) ||
        !emit_rcx_rdx_alu64(w, HOST_ALU_AND) ||
        !emit_test_rcx_rcx(w) ||
        !emit_jcc_rel32_placeholder(
            w, HOST_JCC_E, &fraction_zero_disp) ||
        !emit_movabs_rdx(w, RV64_JIT_FP64_QUIET_NAN_BIT) ||
        !emit_test_rax_rdx(w) ||
        !emit_jcc_rel32_placeholder(
            w, HOST_JCC_E, &signalling_nan_disp) ||
        !emit_fp_class_result_arm(w, 9, done_disps, &done_count))
    {
        return false;
    }

    patch_rel32(signalling_nan_disp, w->cur);
    if (!emit_fp_class_result_arm(w, 8, done_disps, &done_count))
    {
        return false;
    }

    patch_rel32(fraction_zero_disp, w->cur);
    if (!emit_movabs_rdx(w, RV64_JIT_FP64_SIGN_MASK) ||
        !emit_test_rax_rdx(w) ||
        !emit_jcc_rel32_placeholder(
            w, HOST_JCC_E, &positive_infinity_disp) ||
        !emit_fp_class_result_arm(w, 0, done_disps, &done_count))
    {
        return false;
    }

    patch_rel32(positive_infinity_disp, w->cur);
    if (!emit_fp_class_result_arm(w, 7, done_disps, &done_count))
    {
        return false;
    }

    patch_rel32(exponent_zero_disp, w->cur);
    if (!emit_mov_rcx_rax(w) ||
        !emit_movabs_rdx(w, RV64_JIT_FP64_FRACTION_MASK) ||
        !emit_rcx_rdx_alu64(w, HOST_ALU_AND) ||
        !emit_test_rcx_rcx(w) ||
        !emit_jcc_rel32_placeholder(
            w, HOST_JCC_E, &zero_fraction_disp) ||
        !emit_movabs_rdx(w, RV64_JIT_FP64_SIGN_MASK) ||
        !emit_test_rax_rdx(w) ||
        !emit_jcc_rel32_placeholder(
            w, HOST_JCC_E, &positive_subnormal_disp) ||
        !emit_fp_class_result_arm(w, 2, done_disps, &done_count))
    {
        return false;
    }

    patch_rel32(positive_subnormal_disp, w->cur);
    if (!emit_fp_class_result_arm(w, 5, done_disps, &done_count))
    {
        return false;
    }

    patch_rel32(zero_fraction_disp, w->cur);
    if (!emit_movabs_rdx(w, RV64_JIT_FP64_SIGN_MASK) ||
        !emit_test_rax_rdx(w) ||
        !emit_jcc_rel32_placeholder(
            w, HOST_JCC_E, &positive_zero_disp) ||
        !emit_fp_class_result_arm(w, 3, done_disps, &done_count))
    {
        return false;
    }

    patch_rel32(positive_zero_disp, w->cur);
    if (!emit_fp_class_result_arm(w, 4, done_disps, &done_count))
    {
        return false;
    }

    for (uint32_t i = 0; i < done_count; i++)
    {
        patch_rel32(done_disps[i], w->cur);
    }
    return true;
}

/* Lower one S sign-injection instruction using independently unboxed inputs. */
static bool emit_fp_sign_s(rv64_jit_writer_t *w,
                           rv64_jit_fp_exact_op_t op,
                           uint32_t rd, uint32_t rs1, uint32_t rs2)
{
    if (!emit_load_cpu_u64_reg(w, HOST_REG_RAX,
                               jit_fpr_offset(rs1)) ||
        !emit_fp_unbox_s_rax(w) ||
        !emit_mov_ecx_eax(w) ||
        !emit_load_cpu_u64_reg(w, HOST_REG_RAX,
                               jit_fpr_offset(rs2)) ||
        !emit_fp_unbox_s_rax(w))
    {
        return false;
    }

    switch (op)
    {
    case RV64_JIT_FP_EXACT_FSGNJ_S:
        if (!emit_eax_ecx_alu32(w, HOST_ALU_XOR) ||
            !emit_and_eax_imm32(w, RV64_JIT_FP32_SIGN_MASK) ||
            !emit_eax_ecx_alu32(w, HOST_ALU_XOR))
        {
            return false;
        }
        break;
    case RV64_JIT_FP_EXACT_FSGNJN_S:
        if (!emit_eax_ecx_alu32(w, HOST_ALU_XOR) ||
            !emit_xor_eax_imm32(w, RV64_JIT_FP32_SIGN_MASK) ||
            !emit_and_eax_imm32(w, RV64_JIT_FP32_SIGN_MASK) ||
            !emit_eax_ecx_alu32(w, HOST_ALU_XOR))
        {
            return false;
        }
        break;
    case RV64_JIT_FP_EXACT_FSGNJX_S:
        if (!emit_and_eax_imm32(w, RV64_JIT_FP32_SIGN_MASK) ||
            !emit_eax_ecx_alu32(w, HOST_ALU_XOR))
        {
            return false;
        }
        break;
    default:
        return false;
    }

    return emit_movabs_rdx(w, RV64_JIT_FP32_BOX_MASK) &&
           emit_rax_rdx_alu64(w, HOST_ALU_OR) &&
           emit_store_cpu_u64_reg(w, jit_fpr_offset(rd),
                                  HOST_REG_RAX) &&
           emit_mark_fp_dirty(w);
}

/* Lower one D sign-injection instruction as a raw 64-bit bit operation. */
static bool emit_fp_sign_d(rv64_jit_writer_t *w,
                           rv64_jit_fp_exact_op_t op,
                           uint32_t rd, uint32_t rs1, uint32_t rs2)
{
    if (!emit_load_cpu_u64_reg(w, HOST_REG_RCX,
                               jit_fpr_offset(rs1)) ||
        !emit_load_cpu_u64_reg(w, HOST_REG_RAX,
                               jit_fpr_offset(rs2)) ||
        !emit_movabs_rdx(w, RV64_JIT_FP64_SIGN_MASK))
    {
        return false;
    }

    switch (op)
    {
    case RV64_JIT_FP_EXACT_FSGNJ_D:
        if (!emit_rax_rcx_alu64(w, HOST_ALU_XOR) ||
            !emit_rax_rdx_alu64(w, HOST_ALU_AND) ||
            !emit_rax_rcx_alu64(w, HOST_ALU_XOR))
        {
            return false;
        }
        break;
    case RV64_JIT_FP_EXACT_FSGNJN_D:
        if (!emit_rax_rcx_alu64(w, HOST_ALU_XOR) ||
            !emit_rax_rdx_alu64(w, HOST_ALU_XOR) ||
            !emit_rax_rdx_alu64(w, HOST_ALU_AND) ||
            !emit_rax_rcx_alu64(w, HOST_ALU_XOR))
        {
            return false;
        }
        break;
    case RV64_JIT_FP_EXACT_FSGNJX_D:
        if (!emit_rax_rdx_alu64(w, HOST_ALU_AND) ||
            !emit_rax_rcx_alu64(w, HOST_ALU_XOR))
        {
            return false;
        }
        break;
    default:
        return false;
    }

    return emit_store_cpu_u64_reg(w, jit_fpr_offset(rd),
                                  HOST_REG_RAX) &&
           emit_mark_fp_dirty(w);
}

/* Emit the successful body of one validated exact-FP operation. */
static bool emit_native_fp_exact_body(rv64_jit_writer_t *w,
                                      rv64_jit_reg_cache_t *regs,
                                      uint32_t instr,
                                      rv64_jit_fp_exact_op_t op)
{
    const uint32_t rd = rv64_instr_rd(instr);
    const uint32_t rs1 = rv64_instr_rs1(instr);
    const uint32_t rs2 = rv64_instr_rs2(instr);

    switch (op)
    {
    case RV64_JIT_FP_EXACT_FMV_X_W:
        return emit_load_cpu_u32_reg(w, HOST_REG_RAX,
                                     jit_fpr_offset(rs1)) &&
               emit_cdqe(w) &&
               jit_reg_write_rax(w, regs, rd);
    case RV64_JIT_FP_EXACT_FMV_W_X:
        return jit_reg_read_rax(w, regs, rs1) &&
               emit_movabs_rdx(w, RV64_JIT_FP32_BOX_MASK) &&
               emit_rax_rdx_alu64(w, HOST_ALU_OR) &&
               emit_store_cpu_u64_reg(w, jit_fpr_offset(rd),
                                      HOST_REG_RAX) &&
               emit_mark_fp_dirty(w);
    case RV64_JIT_FP_EXACT_FMV_X_D:
        return emit_load_cpu_u64_reg(w, HOST_REG_RAX,
                                     jit_fpr_offset(rs1)) &&
               jit_reg_write_rax(w, regs, rd);
    case RV64_JIT_FP_EXACT_FMV_D_X:
        return jit_reg_read_rax(w, regs, rs1) &&
               emit_store_cpu_u64_reg(w, jit_fpr_offset(rd),
                                      HOST_REG_RAX) &&
               emit_mark_fp_dirty(w);
    case RV64_JIT_FP_EXACT_FSGNJ_S:
    case RV64_JIT_FP_EXACT_FSGNJN_S:
    case RV64_JIT_FP_EXACT_FSGNJX_S:
        return emit_fp_sign_s(w, op, rd, rs1, rs2);
    case RV64_JIT_FP_EXACT_FSGNJ_D:
    case RV64_JIT_FP_EXACT_FSGNJN_D:
    case RV64_JIT_FP_EXACT_FSGNJX_D:
        return emit_fp_sign_d(w, op, rd, rs1, rs2);
    case RV64_JIT_FP_EXACT_FCLASS_S:
        return emit_load_cpu_u64_reg(w, HOST_REG_RAX,
                                     jit_fpr_offset(rs1)) &&
               emit_fp_unbox_s_rax(w) &&
               emit_fp_classify_s_rax(w) &&
               jit_reg_write_rax(w, regs, rd);
    case RV64_JIT_FP_EXACT_FCLASS_D:
        return emit_load_cpu_u64_reg(w, HOST_REG_RAX,
                                     jit_fpr_offset(rs1)) &&
               emit_fp_classify_d_rax(w) &&
               jit_reg_write_rax(w, regs, rd);
    default:
        return false;
    }
}

/*
 * Guard FS at run time because NEMU's block key does not include mstatus.FS.
 * The cold edge returns before the instruction, allowing the interpreter to
 * raise the precise illegal-instruction trap.  Its register flush uses the
 * pre-instruction mapping because the fast emitter may allocate or overwrite a
 * destination cache slot after the guard.
 */
static bool emit_native_fp_exact(rv64_jit_writer_t *w,
                                 rv64_jit_reg_cache_t *regs,
                                 uint32_t instr, vaddr_t pc,
                                 uint32_t completed_count,
                                 rv64_jit_fp_exact_op_t op)
{
    rv64_jit_reg_cache_t side_exit_regs = *regs;
    uint8_t *fs_off_disp = NULL;
    uint8_t *done_disp = NULL;

    if (!emit_test_cpu_u64_imm32(
            w, jit_mstatus_offset(),
            (uint32_t)RISCV_MSTATUS_FS_MASK) ||
        !emit_jcc_rel32_placeholder(
            w, HOST_JCC_E, &fs_off_disp) ||
        !emit_native_fp_exact_body(w, regs, instr, op) ||
        !emit_inc_jit_stat_counter(
            w, &rv64_jit_stats.native_fp_exact_executions[op]) ||
        !emit_jmp_rel32_placeholder(w, &done_disp))
    {
        return false;
    }

    patch_rel32(fs_off_disp, w->cur);
    if (!emit_interpreter_side_exit(
            w, &side_exit_regs, pc, completed_count,
            RV64_JIT_SIDE_EXIT_FP_FS_OFF))
    {
        return false;
    }

    patch_rel32(done_disp, w->cur);
    return true;
}

/*
 * Execute one uncertain Bare FP-memory access through the architectural FPU
 * path and end this native block. The helper rechecks FS, alignment, address
 * decoding, MMIO callbacks, and trap delivery, so the cold edge must arrive
 * before any memory or FPR effect from the instruction.
 */
static bool emit_fp_memory_helper_terminal(rv64_jit_writer_t *w,
                                           const rv64_jit_reg_cache_t *regs,
                                           uint32_t instr, vaddr_t pc,
                                           uint32_t completed_count)
{
    if (!jit_reg_emit_flush_all_dirty(w, regs) ||
        !emit_mov_edi_imm32(w, instr) ||
        !emit_movabs_rsi(w, pc) ||
        !emit_call_abs(w, (uintptr_t)rv64_jit_exec_fpu))
    {
        return false;
    }

#if RV64_JIT_STATS
    uint8_t *skip_memory_exit_stat_disp = NULL;

    /*
     * `rv64_jit_exec_fpu()` already counts calls and trap exits. Count a
     * memory exit only when the whole instruction completed successfully.
     */
    if (!emit_test_eax_eax(w) ||
        !emit_jcc_rel32_placeholder(
            w, HOST_JCC_E, &skip_memory_exit_stat_disp) ||
        !emit_inc_jit_stat_counter(
            w, &rv64_jit_stats.fp_helper_memory_exits))
    {
        return false;
    }

    patch_rel32(skip_memory_exit_stat_disp, w->cur);
#endif

    if (!emit_return_total_retired(w, completed_count + 1u))
    {
        return false;
    }

    JIT_STAT_INC(emitted_sites.fp_helper_sites);
    return true;
}

/* Emit an aligned Bare-PMEM FLW or FLD which falls through after completion. */
static bool emit_native_fp_memory_bare_load(
    rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs,
    uint32_t instr, vaddr_t pc, uint32_t completed_count,
    rv64_jit_fp_memory_op_t op)
{
    const uint32_t rd = rv64_instr_rd(instr);
    const uint32_t rs1 = rv64_instr_rs1(instr);
    const int32_t imm = (int32_t)imm_i(instr);
    const uint32_t len =
        op == RV64_JIT_FP_MEMORY_FLW ? 4u : 8u;
    const uint32_t host_load_funct3 =
        op == RV64_JIT_FP_MEMORY_FLW
            ? RV64_FUNCT3_LWU
            : RV64_FUNCT3_LD;
    rv64_jit_reg_cache_t fs_off_regs = *regs;
    rv64_jit_reg_cache_t side_exit_regs;
    uint8_t *fs_off_disp = NULL;
    uint8_t *align_slow_disp = NULL;
    uint8_t *range_slow_disp = NULL;
    uint8_t *done_disp = NULL;

    /*
     * FS has architectural priority over address/alignment failures. The guard
     * therefore precedes even the base-register read and uses the untouched
     * cache snapshot on its interpreter edge.
     */
    if (!emit_test_cpu_u64_imm32(
            w, jit_mstatus_offset(),
            (uint32_t)RISCV_MSTATUS_FS_MASK) ||
        !emit_jcc_rel32_placeholder(
            w, HOST_JCC_E, &fs_off_disp) ||
        !jit_reg_read_rax(w, regs, rs1) ||
        !emit_add_rax_imm32(w, imm))
    {
        return false;
    }

    /*
     * Reading an uncached base can evict a dirty GPR and change which guest
     * value each host slot contains. FS Off branches before that read, while
     * every address-related exit must use this post-read cache description.
     */
    side_exit_regs = *regs;

    if (!emit_alignment_guard_al(w, len, &align_slow_disp) ||
        !emit_mov_rdx_rax(w) ||
        !emit_guard_bare_address_in_pmem(
            w, len, &range_slow_disp) ||
        !emit_direct_pmem_load_rax(w, host_load_funct3))
    {
        return false;
    }

    if (op == RV64_JIT_FP_MEMORY_FLW &&
        (!emit_movabs_rdx(w, RV64_JIT_FP32_BOX_MASK) ||
         !emit_rax_rdx_alu64(w, HOST_ALU_OR)))
    {
        return false;
    }

    if (!emit_store_cpu_u64_reg(
            w, jit_fpr_offset(rd), HOST_REG_RAX) ||
        !emit_mark_fp_dirty(w) ||
        !emit_inc_jit_stat_counter(
            w, &rv64_jit_stats.native_fp_memory_executions[op]) ||
        !emit_jmp_rel32_placeholder(w, &done_disp))
    {
        return false;
    }

    patch_rel32(range_slow_disp, w->cur);
    if (!emit_fp_memory_helper_terminal(
            w, regs, instr, pc, completed_count))
    {
        return false;
    }

    patch_rel32(align_slow_disp, w->cur);
    if (!emit_interpreter_side_exit(
            w, &side_exit_regs, pc, completed_count,
            RV64_JIT_SIDE_EXIT_LOAD_GUARD))
    {
        return false;
    }

    patch_rel32(fs_off_disp, w->cur);
    if (!emit_interpreter_side_exit(
            w, &fs_off_regs, pc, completed_count,
            RV64_JIT_SIDE_EXIT_FP_FS_OFF))
    {
        return false;
    }

    patch_rel32(done_disp, w->cur);
    return true;
}

/* Emit an aligned Bare-PMEM FSW or FSD which falls through after completion. */
static bool emit_native_fp_memory_bare_store(
    rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs,
    uint32_t instr, vaddr_t pc, uint32_t completed_count,
    rv64_jit_fp_memory_op_t op)
{
    const uint32_t rs1 = rv64_instr_rs1(instr);
    const uint32_t rs2 = rv64_instr_rs2(instr);
    const int32_t imm = (int32_t)imm_s(instr);
    const uint32_t len =
        op == RV64_JIT_FP_MEMORY_FSW ? 4u : 8u;
    rv64_jit_reg_cache_t fs_off_regs = *regs;
    rv64_jit_reg_cache_t side_exit_regs;
    uint8_t *fs_off_disp = NULL;
    uint8_t *align_slow_disp = NULL;
    uint8_t *range_slow_disp = NULL;
    uint8_t *cross_chunk_disp = NULL;
    uint8_t *source_chunk_disp = NULL;
    uint8_t *data_page_table_disp = NULL;
    uint8_t *ifetch_page_table_disp = NULL;
    uint8_t *done_disp = NULL;

    if (!emit_test_cpu_u64_imm32(
            w, jit_mstatus_offset(),
            (uint32_t)RISCV_MSTATUS_FS_MASK) ||
        !emit_jcc_rel32_placeholder(
            w, HOST_JCC_E, &fs_off_disp) ||
        !jit_reg_read_rax(w, regs, rs1) ||
        !emit_add_rax_imm32(w, imm))
    {
        return false;
    }

    side_exit_regs = *regs;

    if (!emit_alignment_guard_al(w, len, &align_slow_disp) ||
        !emit_mov_rdx_rax(w) ||
        !emit_guard_bare_address_in_pmem(
            w, len, &range_slow_disp) ||
        /*
         * Range proof uses RCX as scratch. Load the raw FPR only after that
         * proof, then preserve it across the source and page-table guards.
         */
        !emit_load_cpu_u64_reg(
            w, HOST_REG_RCX, jit_fpr_offset(rs2)) ||
        !emit_guard_store_not_compiled_source(
            w, len, &cross_chunk_disp, &source_chunk_disp) ||
        !emit_guard_store_not_translation_dependency(
            w, &data_page_table_disp, &ifetch_page_table_disp) ||
        !emit_direct_pmem_store_from_rcx(w, len) ||
        !emit_inc_jit_stat_counter(
            w, &rv64_jit_stats.native_fp_memory_executions[op]) ||
        !emit_jmp_rel32_placeholder(w, &done_disp))
    {
        return false;
    }

    /*
     * A sensitive store has not committed yet. Return to the interpreter,
     * which performs the write and invalidation exactly once before any later
     * translated instruction can run.
     */
    const uint8_t *source_slow_path = w->cur;
    patch_rel32(cross_chunk_disp, source_slow_path);
    patch_rel32(source_chunk_disp, source_slow_path);
    patch_rel32(data_page_table_disp, source_slow_path);
    patch_rel32(ifetch_page_table_disp, source_slow_path);
    if (!emit_interpreter_side_exit(
            w, &side_exit_regs, pc, completed_count,
            RV64_JIT_SIDE_EXIT_STORE_SOURCE))
    {
        return false;
    }

    patch_rel32(range_slow_disp, w->cur);
    if (!emit_fp_memory_helper_terminal(
            w, regs, instr, pc, completed_count))
    {
        return false;
    }

    patch_rel32(align_slow_disp, w->cur);
    if (!emit_interpreter_side_exit(
            w, &side_exit_regs, pc, completed_count,
            RV64_JIT_SIDE_EXIT_STORE_GUARD))
    {
        return false;
    }

    patch_rel32(fs_off_disp, w->cur);
    if (!emit_interpreter_side_exit(
            w, &fs_off_regs, pc, completed_count,
            RV64_JIT_SIDE_EXIT_FP_FS_OFF))
    {
        return false;
    }

    patch_rel32(done_disp, w->cur);
    return true;
}

/* Dispatch one strictly decoded FP memory operation to its Bare fast path. */
static bool emit_native_fp_memory_bare(
    rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs,
    uint32_t instr, vaddr_t pc, uint32_t completed_count,
    rv64_jit_fp_memory_op_t op)
{
    switch (op)
    {
    case RV64_JIT_FP_MEMORY_FLW:
    case RV64_JIT_FP_MEMORY_FLD:
        return emit_native_fp_memory_bare_load(
            w, regs, instr, pc, completed_count, op);
    case RV64_JIT_FP_MEMORY_FSW:
    case RV64_JIT_FP_MEMORY_FSD:
        return emit_native_fp_memory_bare_store(
            w, regs, instr, pc, completed_count, op);
    default:
        return false;
    }
}

/*
 * Probe an Sv39 translation without executing the FP memory instruction, then
 * return to the dispatcher at that instruction. An ordinary-PMEM retry may
 * now hit natively; every uncertain case still reaches the architectural
 * fallback. The C call may clobber the caller-saved JIT bases, so reload them
 * before the cache snapshot is flushed.
 */
static bool emit_fp_memory_probe_side_exit(
    rv64_jit_writer_t *w, rv64_jit_reg_cache_t *side_exit_regs,
    bool address_in_rcx, uint32_t len, bool is_store,
    vaddr_t pc, uint32_t completed_count,
    rv64_jit_side_exit_reason_t reason)
{
    if ((address_in_rcx &&
         (!emit_mov_rax_rcx(w) || !emit_mov_rdi_rax(w))) ||
        !emit_mov_esi_imm32(w, len) ||
        !emit_call_abs(
            w, (uintptr_t)(is_store
                               ? rv64_jit_data_tlb_probe_write
                               : rv64_jit_data_tlb_probe_read)) ||
        !emit_load_jit_bases(w))
    {
        return false;
    }

    return emit_interpreter_side_exit(
        w, side_exit_regs, pc, completed_count, reason);
}

/* Emit a translated FLW or FLD for an already-filled inline data-TLB entry. */
static bool emit_native_fp_memory_paged_load(
    rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs,
    uint32_t instr, vaddr_t pc, uint32_t completed_count,
    rv64_jit_fp_memory_op_t op)
{
    const uint32_t rd = rv64_instr_rd(instr);
    const uint32_t rs1 = rv64_instr_rs1(instr);
    const int32_t imm = (int32_t)imm_i(instr);
    const uint32_t len =
        op == RV64_JIT_FP_MEMORY_FLW ? 4u : 8u;
    const uint32_t host_load_funct3 =
        op == RV64_JIT_FP_MEMORY_FLW
            ? RV64_FUNCT3_LWU
            : RV64_FUNCT3_LD;
    rv64_jit_reg_cache_t fs_off_regs = *regs;
    rv64_jit_reg_cache_t side_exit_regs;
    rv64_jit_tlb_guard_patch_t tlb_guard = {0};
    uint8_t *fs_off_disp = NULL;
    uint8_t *align_slow_disp = NULL;
    uint8_t *done_disp = NULL;

    if (!emit_test_cpu_u64_imm32(
            w, jit_mstatus_offset(),
            (uint32_t)RISCV_MSTATUS_FS_MASK) ||
        !emit_jcc_rel32_placeholder(
            w, HOST_JCC_E, &fs_off_disp) ||
        !jit_reg_read_rax(w, regs, rs1) ||
        !emit_add_rax_imm32(w, imm))
    {
        return false;
    }

    side_exit_regs = *regs;

    if (!emit_alignment_guard_al(w, len, &align_slow_disp) ||
        /*
         * RCX retains the original virtual address for every DTLB miss. The
         * fast proof leaves the final PMEM offset in RDX and the raw value in
         * RAX.
         */
        !emit_mov_rcx_rax(w) ||
        !emit_inline_sv39_load_fast_path(
            w, host_load_funct3, len, &tlb_guard))
    {
        return false;
    }

    if (op == RV64_JIT_FP_MEMORY_FLW &&
        (!emit_movabs_rdx(w, RV64_JIT_FP32_BOX_MASK) ||
         !emit_rax_rdx_alu64(w, HOST_ALU_OR)))
    {
        return false;
    }

    if (!emit_store_cpu_u64_reg(
            w, jit_fpr_offset(rd), HOST_REG_RAX) ||
        !emit_mark_fp_dirty(w) ||
        !emit_inc_jit_stat_counter(
            w, &rv64_jit_stats.native_fp_memory_executions[op]) ||
        !emit_jmp_rel32_placeholder(w, &done_disp))
    {
        return false;
    }

    const uint8_t *translation_slow_path = w->cur;
    patch_tlb_guard(&tlb_guard, translation_slow_path);
    if (!emit_fp_memory_probe_side_exit(
            w, &side_exit_regs, true, len, false, pc,
            completed_count, RV64_JIT_SIDE_EXIT_LOAD_GUARD))
    {
        return false;
    }

    patch_rel32(align_slow_disp, w->cur);
    if (!emit_interpreter_side_exit(
            w, &side_exit_regs, pc, completed_count,
            RV64_JIT_SIDE_EXIT_LOAD_GUARD))
    {
        return false;
    }

    patch_rel32(fs_off_disp, w->cur);
    if (!emit_interpreter_side_exit(
            w, &fs_off_regs, pc, completed_count,
            RV64_JIT_SIDE_EXIT_FP_FS_OFF))
    {
        return false;
    }

    patch_rel32(done_disp, w->cur);
    JIT_STAT_INC(emitted_sites.native_paged_loads);
    JIT_STAT_INC(emitted_sites.inline_paged_loads);
    return true;
}

/* Emit a translated FSW or FSD for an already-filled inline data-TLB entry. */
static bool emit_native_fp_memory_paged_store(
    rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs,
    uint32_t instr, vaddr_t pc, uint32_t completed_count,
    rv64_jit_fp_memory_op_t op)
{
    const uint32_t rs1 = rv64_instr_rs1(instr);
    const uint32_t rs2 = rv64_instr_rs2(instr);
    const int32_t imm = (int32_t)imm_s(instr);
    const uint32_t len =
        op == RV64_JIT_FP_MEMORY_FSW ? 4u : 8u;
    rv64_jit_reg_cache_t fs_off_regs = *regs;
    rv64_jit_reg_cache_t side_exit_regs;
    rv64_jit_tlb_guard_patch_t tlb_guard = {0};
    uint8_t *fs_off_disp = NULL;
    uint8_t *align_slow_disp = NULL;
    uint8_t *cross_chunk_disp = NULL;
    uint8_t *source_chunk_disp = NULL;
    uint8_t *data_page_table_disp = NULL;
    uint8_t *ifetch_page_table_disp = NULL;
    uint8_t *done_disp = NULL;

    if (!emit_test_cpu_u64_imm32(
            w, jit_mstatus_offset(),
            (uint32_t)RISCV_MSTATUS_FS_MASK) ||
        !emit_jcc_rel32_placeholder(
            w, HOST_JCC_E, &fs_off_disp) ||
        !jit_reg_read_rax(w, regs, rs1) ||
        !emit_add_rax_imm32(w, imm))
    {
        return false;
    }

    side_exit_regs = *regs;

    if (!emit_alignment_guard_al(w, len, &align_slow_disp) ||
        /*
         * RDI retains the original virtual address for a probe; RCX retains
         * the raw FPR source while the fast proof derives the PMEM offset.
         */
        !emit_mov_rdi_rax(w) ||
        !emit_load_cpu_u64_reg(
            w, HOST_REG_RCX, jit_fpr_offset(rs2)) ||
        !emit_inline_sv39_store_address(w, len, &tlb_guard) ||
        !emit_guard_store_not_compiled_source(
            w, len, &cross_chunk_disp, &source_chunk_disp) ||
        !emit_guard_store_not_translation_dependency(
            w, &data_page_table_disp, &ifetch_page_table_disp) ||
        !emit_inline_paged_store_hit_stats(w) ||
        !emit_direct_pmem_store_from_rcx(w, len) ||
        !emit_inc_jit_stat_counter(
            w, &rv64_jit_stats.native_fp_memory_executions[op]) ||
        !emit_jmp_rel32_placeholder(w, &done_disp))
    {
        return false;
    }

    const uint8_t *translation_slow_path = w->cur;
    patch_tlb_guard(&tlb_guard, translation_slow_path);
    if (!emit_fp_memory_probe_side_exit(
            w, &side_exit_regs, false, len, true, pc,
            completed_count, RV64_JIT_SIDE_EXIT_STORE_GUARD))
    {
        return false;
    }

    /*
     * A source or page-table hazard follows a successful DTLB proof, so its
     * translation is already warm. The interpreter owns the one real store and
     * its invalidation without another C probe.
     */
    const uint8_t *source_slow_path = w->cur;
    patch_rel32(cross_chunk_disp, source_slow_path);
    patch_rel32(source_chunk_disp, source_slow_path);
    patch_rel32(data_page_table_disp, source_slow_path);
    patch_rel32(ifetch_page_table_disp, source_slow_path);
    if (!emit_interpreter_side_exit(
            w, &side_exit_regs, pc, completed_count,
            RV64_JIT_SIDE_EXIT_STORE_SOURCE))
    {
        return false;
    }

    patch_rel32(align_slow_disp, w->cur);
    if (!emit_interpreter_side_exit(
            w, &side_exit_regs, pc, completed_count,
            RV64_JIT_SIDE_EXIT_STORE_GUARD))
    {
        return false;
    }

    patch_rel32(fs_off_disp, w->cur);
    if (!emit_interpreter_side_exit(
            w, &fs_off_regs, pc, completed_count,
            RV64_JIT_SIDE_EXIT_FP_FS_OFF))
    {
        return false;
    }

    patch_rel32(done_disp, w->cur);
    JIT_STAT_INC(emitted_sites.native_paged_stores);
    JIT_STAT_INC(emitted_sites.inline_paged_stores);
    return true;
}

/* Dispatch one strictly decoded FP memory operation to its translated path. */
static bool emit_native_fp_memory_paged(
    rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs,
    uint32_t instr, vaddr_t pc, uint32_t completed_count,
    rv64_jit_fp_memory_op_t op)
{
    switch (op)
    {
    case RV64_JIT_FP_MEMORY_FLW:
    case RV64_JIT_FP_MEMORY_FLD:
        return emit_native_fp_memory_paged_load(
            w, regs, instr, pc, completed_count, op);
    case RV64_JIT_FP_MEMORY_FSW:
    case RV64_JIT_FP_MEMORY_FSD:
        return emit_native_fp_memory_paged_store(
            w, regs, instr, pc, completed_count, op);
    default:
        return false;
    }
}

/*
 * Emit one scalar F/D instruction through the shared SoftFloat executor.
 *
 * A strict helper footprint publishes only integer inputs which C can read.
 * Other dirty mappings remain in SysV callee-saved host registers: successful
 * execution retains them, while the terminal trap stub stores them before the
 * dispatcher runs guest trap code. A successful helper-written destination is
 * discarded without publishing its stale old value. Unknown effects, memory,
 * DiffTest builds, and the runtime ablation switch retain the full pre-call
 * barrier and full post-success reset. Guarded
 * FLW/FLD/FSW/FSD use the native paths above; unsupported FP memory encodings
 * end the block after the helper so MMIO, faults, page-table effects, and
 * source invalidation remain ordered exactly as they are in the interpreter.
 */
bool rv64_jit_emit_fp_instr(rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs, uint32_t instr, vaddr_t pc, uint32_t completed_count,
                            bool *ends_block)
{
    const uint32_t opcode = instr & RV64_OPCODE_MASK;
    const bool is_memory = rv64_jit_fp_opcode_is_memory(opcode);
    const rv64_jit_fp_exact_op_t exact_op =
        rv64_jit_decode_fp_exact(instr);
    const rv64_jit_fp_memory_op_t memory_op =
        rv64_jit_decode_fp_memory(instr);
    uint8_t *continue_disp = NULL;

    *ends_block = false;

    if (exact_op != RV64_JIT_FP_EXACT_INVALID)
    {
        if (!emit_native_fp_exact(
                w, regs, instr, pc, completed_count, exact_op))
        {
            return false;
        }

        JIT_STAT_INC(emitted_sites.native_fp_exact_sites);
        return true;
    }

    if (memory_op != RV64_JIT_FP_MEMORY_INVALID)
    {
        const bool bare =
            (cpu.csr.satp >> RV64_JIT_SATP_MODE_SHIFT) ==
            RV64_JIT_SATP_MODE_BARE;
        const bool emitted =
            bare
                ? emit_native_fp_memory_bare(
                      w, regs, instr, pc, completed_count, memory_op)
                : emit_native_fp_memory_paged(
                      w, regs, instr, pc, completed_count, memory_op);

        if (!emitted)
        {
            return false;
        }

        JIT_STAT_INC(emitted_sites.native_fp_memory_sites);
        return true;
    }

    const riscv_fpu_gpr_effect_t gpr_effect =
        riscv_fpu_gpr_effect(instr);
    const bool defer_gpr_sync =
        !is_memory && jit_reg_can_defer_fp_helper_sync(gpr_effect);
    uint32_t input_flushes = 0;
    uint32_t trap_stores = 0;

    if (defer_gpr_sync)
    {
        /* Helper-backed blocks must never admit the caller-saved R8 slot. */
        Assert(regs->slot_count == RV64_JIT_HREG_BASE_COUNT,
               "jit: deferred FP helper sync saw an R8 cache slot");
        if (!jit_reg_flush_mask(
                w, regs, gpr_effect.read_mask, &input_flushes))
        {
            return false;
        }
    }
    else if (!jit_reg_flush_all_dirty(w, regs))
    {
        return false;
    }

    /*
     * System V passes the raw instruction in EDI and the full guest PC in RSI.
     * The helper returns one in EAX on normal completion and zero on a trap.
     */
    if (!emit_mov_edi_imm32(w, instr) || !emit_movabs_rsi(w, pc) ||
        !emit_call_abs(w, (uintptr_t)rv64_jit_exec_fpu))
    {
        return false;
    }

    if (is_memory)
    {
#if RV64_JIT_STATS
        uint8_t *skip_memory_exit_stat_disp = NULL;

        /*
         * A zero helper result is the trap edge. Only normal FP memory
         * completion reaches the native increment before both outcomes join
         * the terminal return path.
         */
        if (!emit_test_eax_eax(w) || !emit_jcc_rel32_placeholder(w, HOST_JCC_E, &skip_memory_exit_stat_disp) ||
            !emit_inc_jit_stat_counter(w, &rv64_jit_stats.fp_helper_memory_exits))
        {
            return false;
        }

        patch_rel32(skip_memory_exit_stat_disp, w->cur);
#endif

        if (!emit_return_total_retired(w, completed_count + 1u))
        {
            return false;
        }

        *ends_block = true;
    }
    else
    {
        /*
         * A non-zero result skips the trap-only synchronisation and return.
         * The helper already published the trap PC and CSRs. Reload R11 only
         * on the zero edge, then store every dirty mapping before guest trap
         * code can execute from a separately compiled native entry.
         */
        if (!emit_test_eax_eax(w) ||
            !emit_jcc_rel32_placeholder(
                w, HOST_JCC_NE, &continue_disp))
        {
            return false;
        }

        if (defer_gpr_sync)
        {
            trap_stores = jit_reg_dirty_mapping_count(regs);
            if (trap_stores != 0 &&
                (!emit_load_cpu_base(w) ||
                 !jit_reg_emit_flush_all_dirty(w, regs)))
            {
                return false;
            }
        }

        if (!emit_return_total_retired(w, completed_count + 1u))
        {
            return false;
        }

        patch_rel32(continue_disp, w->cur);

        if (!emit_inc_jit_stat_counter(w, &rv64_jit_stats.fp_helper_continuations) || !emit_load_jit_bases(w))
        {
            return false;
        }

        /*
         * Emit the terminal trap stores before changing this metadata. The
         * continuing edge may now discard helper-written GPRs, including a
         * dirty old mapping, while every unaffected clean or dirty value stays
         * live in RBX, RBP, and R12-R15.
         */
        if (defer_gpr_sync)
        {
            jit_reg_apply_fp_helper_effect(
                regs, gpr_effect, input_flushes, trap_stores);
        }
        else
        {
            rv64_jit_reg_cache_init(regs);
        }
    }

    JIT_STAT_INC(emitted_sites.fp_helper_sites);
    return true;
}
#endif

/*
 * Block exits and direct links.
 *
 * Every exit has the same architectural contract: all completed guest
 * instructions have committed, no later instruction has partially committed,
 * dirty guest registers are visible in `CPU_state`, `cpu.pc` names the next
 * instruction for C or the interpreter, and the return count includes any
 * chained loop laps.
 *
 * A direct link may jump to another native block only after checking the cache
 * slot still has the expected PC, `satp`, ifetch privilege, data privilege tag
 * when relevant, body entry pointer and ifetch generation.  Any failed guard
 * returns to the dispatcher; the dispatcher then performs the slower byte and
 * page-table revalidation before running or recompiling the target.
 */
/* Flush cached registers and side-exit so the interpreter executes this PC. */
static bool emit_interpreter_side_exit(rv64_jit_writer_t *w,
                                       rv64_jit_reg_cache_t *regs, vaddr_t pc,
                                       uint32_t completed_count,
                                       rv64_jit_side_exit_reason_t reason)
{
    return jit_reg_emit_flush_all_dirty(w, regs) &&
           emit_store_pc_imm(w, pc) &&
           emit_inc_jit_stat_counter(w, &rv64_jit_stats.side_exit_by_reason[reason]) &&
           emit_return_total_retired(w, completed_count);
}

/* Add one conditional jump to the shared direct-link miss path. */
static bool emit_direct_link_miss_jcc(rv64_jit_writer_t *w, uint8_t jcc_opcode,
                                      uint8_t **miss_disps, uint32_t *miss_count)
{
    Assert(*miss_count < RV64_JIT_DIRECT_LINK_MAX_MISS_PATCHES,
           "jit: RV64 direct-link miss patch list overflow");
    return emit_jcc_rel32_placeholder(w, jcc_opcode, &miss_disps[(*miss_count)++]);
}

/*
 * A known direct edge keeps its block-cache slot in RDX, while a dynamic JALR
 * computes a slot in R8 and preserves the architectural target in R9.  The
 * distinction is an emitter-time choice: both forms must implement one safety
 * policy without adding a generated run-time mode branch.
 */
typedef enum
{
    RV64_JIT_LINK_CANDIDATE_KNOWN_RDX,
    RV64_JIT_LINK_CANDIDATE_RUNTIME_R8,
} rv64_jit_link_candidate_kind_t;

typedef struct
{
    rv64_jit_link_candidate_kind_t kind;

    /* Used only by the known-target form. */
    rv64_jit_block_t *known_slot;
    vaddr_t target_pc;

    /* Used only by the runtime-target form. */
    uint32_t context_mix;
} rv64_jit_link_candidate_t;

typedef struct
{
    word_t satp;
    uint32_t ifetch_state;
    uint32_t data_state;
    uint32_t completed_count;
    bool source_uses_data_state;
    bool count_already_accumulated;
} rv64_jit_link_guard_config_t;

typedef struct
{
    uint8_t *disps[RV64_JIT_DIRECT_LINK_MAX_MISS_PATCHES];
    uint32_t count;
} rv64_jit_link_guard_fixups_t;

static bool emit_link_guard_miss_jcc(
    rv64_jit_writer_t *w, uint8_t jcc_opcode,
    rv64_jit_link_guard_fixups_t *fixups)
{
    return emit_direct_link_miss_jcc(
        w, jcc_opcode, fixups->disps, &fixups->count);
}

static void patch_link_guard_misses(
    const rv64_jit_link_guard_fixups_t *fixups,
    const uint8_t *miss_path)
{
    for (uint32_t i = 0; i < fixups->count; i++)
    {
        patch_rel32(fixups->disps[i], miss_path);
    }
}

static bool emit_link_target_guards(
    rv64_jit_writer_t *w,
    const rv64_jit_link_candidate_t *candidate,
    const rv64_jit_link_guard_config_t *config,
    rv64_jit_link_guard_fixups_t *fixups);

#if RV64_JIT_STATS
static bool emit_guarded_link_taken_stats(
    rv64_jit_writer_t *w,
    const rv64_jit_link_candidate_t *candidate);
#endif

/* Emit the conservative block exit used when cross-block direct links are off. */
bool rv64_jit_emit_plain_block_exit(rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs,
                                    vaddr_t target_pc, uint32_t completed_count)
{
    return jit_reg_emit_flush_all_dirty(w, regs) &&
           emit_store_pc_imm(w, target_pc) &&
           emit_return_total_retired(w, completed_count);
}

/*
 * Commit one source path's completed instructions to the active native entry.
 * EAX deliberately retains the new total: every patched source can pass it
 * straight to the destination chain entry without reloading the same global.
 */
static bool emit_accumulate_loop_extra(rv64_jit_writer_t *w,
                                       uint32_t completed_count)
{
    return emit_movabs_rdx(
               w, (uint64_t)(uintptr_t)&rv64_jit_loop_extra) &&
           emit_mov_eax_m32_rdx(w) &&
           emit_add_eax_imm32(w, completed_count) &&
           emit_mov_m32_rdx_eax(w);
}

#if RV64_JIT_STATS
/* Count one target-budget-accepted chain according to its source route. */
static bool emit_chain_accepted_stats(rv64_jit_writer_t *w)
{
    uint8_t *direct_disp = NULL;
    uint8_t *jalr_disp = NULL;
    uint8_t *return_primary_disp = NULL;
    uint8_t *jalr_primary_disp = NULL;
    uint8_t *return_done_disp = NULL;
    uint8_t *jalr_done_disp = NULL;
    uint8_t *direct_done_disp = NULL;

    if (!emit_test_esi_esi(w) ||
        !emit_jcc_rel32_placeholder(w, HOST_JCC_E, &direct_disp) ||
        !emit_cmp_esi_imm8(w, RV64_JIT_CHAIN_STATS_RETURN_SECONDARY) ||
        !emit_jcc_rel32_placeholder(w, HOST_JCC_A, &jalr_disp) ||
        !emit_inc_jit_stat_counter(
            w, &rv64_jit_stats.indirect_pic_patched_entries
                    [RV64_JIT_INDIRECT_PIC_RETURN]) ||
        !emit_inc_jit_stat_counter(
            w, &rv64_jit_stats.indirect_pic_hits[RV64_JIT_INDIRECT_PIC_RETURN]) ||
        !emit_cmp_esi_imm8(w, RV64_JIT_CHAIN_STATS_RETURN_SECONDARY) ||
        !emit_jcc_rel32_placeholder(
            w, HOST_JCC_NE, &return_primary_disp) ||
        !emit_inc_jit_stat_counter(
            w, &rv64_jit_stats.indirect_pic_secondary_hits[RV64_JIT_INDIRECT_PIC_RETURN]))
    {
        return false;
    }

    patch_rel32(return_primary_disp, w->cur);

    if (!emit_inc_jit_stat_counter(
            w, &rv64_jit_stats.direct_return_link_taken_count) ||
        !emit_jmp_rel32_placeholder(w, &return_done_disp))
    {
        return false;
    }

    patch_rel32(jalr_disp, w->cur);

    if (!emit_inc_jit_stat_counter(
            w, &rv64_jit_stats.indirect_pic_patched_entries
                    [RV64_JIT_INDIRECT_PIC_JALR]) ||
        !emit_inc_jit_stat_counter(
            w, &rv64_jit_stats.indirect_pic_hits[RV64_JIT_INDIRECT_PIC_JALR]) ||
        !emit_cmp_esi_imm8(w, RV64_JIT_CHAIN_STATS_JALR_SECONDARY) ||
        !emit_jcc_rel32_placeholder(
            w, HOST_JCC_NE, &jalr_primary_disp) ||
        !emit_inc_jit_stat_counter(
            w, &rv64_jit_stats.indirect_pic_secondary_hits[RV64_JIT_INDIRECT_PIC_JALR]))
    {
        return false;
    }

    patch_rel32(jalr_primary_disp, w->cur);

    if (!emit_inc_jit_stat_counter(
            w, &rv64_jit_stats.direct_jalr_link_taken_count) ||
        !emit_jmp_rel32_placeholder(w, &jalr_done_disp))
    {
        return false;
    }

    patch_rel32(direct_disp, w->cur);

    if (!emit_inc_jit_stat_counter(
            w, &rv64_jit_stats.patched_direct_link_taken_count) ||
        !emit_test_rdi_rdi(w) ||
        !emit_jcc_rel32_placeholder(
            w, HOST_JCC_E, &direct_done_disp) ||
        !emit_inc_m64_rdi(w))
    {
        return false;
    }

    const uint8_t *done = w->cur;
    patch_rel32(return_done_disp, done);
    patch_rel32(jalr_done_disp, done);
    patch_rel32(direct_done_disp, done);
    return true;
}

/* Count a patched PIC whose exact target could not fit this native entry. */
static bool emit_chain_rejected_stats(rv64_jit_writer_t *w)
{
    uint8_t *done_disp = NULL;
    uint8_t *jalr_disp = NULL;
    uint8_t *return_done_disp = NULL;

    if (!emit_test_esi_esi(w) ||
        !emit_jcc_rel32_placeholder(w, HOST_JCC_E, &done_disp) ||
        !emit_cmp_esi_imm8(w, RV64_JIT_CHAIN_STATS_RETURN_SECONDARY) ||
        !emit_jcc_rel32_placeholder(w, HOST_JCC_A, &jalr_disp) ||
        !emit_inc_jit_stat_counter(
            w, &rv64_jit_stats.indirect_pic_budget_rejections[RV64_JIT_INDIRECT_PIC_RETURN]) ||
        !emit_inc_jit_stat_counter(
            w, &rv64_jit_stats.direct_return_link_miss_count) ||
        !emit_jmp_rel32_placeholder(w, &return_done_disp))
    {
        return false;
    }

    patch_rel32(jalr_disp, w->cur);

    if (!emit_inc_jit_stat_counter(
            w, &rv64_jit_stats.indirect_pic_budget_rejections[RV64_JIT_INDIRECT_PIC_JALR]) ||
        !emit_inc_jit_stat_counter(
            w, &rv64_jit_stats.direct_jalr_link_miss_count))
    {
        return false;
    }

    const uint8_t *done = w->cur;
    patch_rel32(done_disp, done);
    patch_rel32(return_done_disp, done);
    return true;
}
#endif

/*
 * Enter a block from an already-active native frame.
 *
 * The source edge has committed its own retired count into `loop_extra` before
 * arriving here. Check the complete destination block against the remaining
 * dispatcher budget before entering its normal body. The body deliberately
 * starts after the destination prologue, so it reuses the source frame and
 * eventually returns through the ordinary epilogue.
 */
bool rv64_jit_emit_chain_entry(rv64_jit_writer_t *w, vaddr_t pc,
                               uint32_t insn_count,
                               const uint8_t *body_entry,
                               const uint8_t **chain_entry)
{
    uint8_t *over_budget_disp = NULL;
    uint8_t *body_disp = NULL;

    Assert(chain_entry != NULL, "jit: missing RV64 chain-entry result");
    Assert(body_entry != NULL, "jit: missing RV64 chain-entry body");
    *chain_entry = w->cur;

    if (!emit_mov_ecx_eax(w) ||
        !emit_add_ecx_imm32(w, insn_count) ||
        !emit_movabs_rdx(w, (uint64_t)(uintptr_t)&rv64_jit_entry_budget) ||
        !emit_cmp_ecx_m32_rdx(w) ||
        !emit_jcc_rel32_placeholder(w, HOST_JCC_A, &over_budget_disp) ||
#if RV64_JIT_STATS
        !emit_chain_accepted_stats(w) ||
#endif
        !emit_inc_jit_stat_counter(w, &rv64_jit_stats.direct_link_taken_count))
    {
        return false;
    }

    if (!emit_jmp_rel32_placeholder(w, &body_disp))
    {
        return false;
    }

    patch_rel32(body_disp, body_entry);
    patch_rel32(over_budget_disp, w->cur);

#if RV64_JIT_STATS
    if (!emit_chain_rejected_stats(w))
    {
        return false;
    }
#endif

    return emit_store_pc_imm(w, pc) &&
           emit_inc_jit_stat_counter(w, &rv64_jit_stats.direct_link_miss_count) &&
           emit_return_total_retired(w, 0);
}

/*
 * Emit the complete guarded known-target path.
 *
 * A patchable source selector reaches this same path until a target is
 * published, and again after target invalidation. Such a source has already
 * added `completed_count` to `loop_extra`; ordinary guarded links retain the
 * older ABI and perform that addition here.
 */
static bool emit_guarded_direct_link_exit(
    rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs,
    vaddr_t target_pc, uint32_t completed_count,
    bool source_uses_data_state, bool registers_already_flushed,
    bool count_already_accumulated,
    uint64_t *extra_taken_counter)
{
    const word_t satp = cpu.csr.satp;
    const uint32_t ifetch_state = rv64_jit_ifetch_state();
    rv64_jit_block_t *target =
        rv64_jit_cache_slot_context(target_pc, satp, ifetch_state);
    const uint32_t body_entry_off =
        (uint32_t)offsetof(rv64_jit_block_t, body_entry);
    const rv64_jit_link_candidate_t candidate = {
        .kind = RV64_JIT_LINK_CANDIDATE_KNOWN_RDX,
        .known_slot = target,
        .target_pc = target_pc,
    };
    const rv64_jit_link_guard_config_t guard_config = {
        .satp = satp,
        .ifetch_state = ifetch_state,
        .data_state = rv64_jit_data_tlb_state(MEM_TYPE_READ),
        .completed_count = completed_count,
        .source_uses_data_state = source_uses_data_state,
        .count_already_accumulated = count_already_accumulated,
    };
    rv64_jit_link_guard_fixups_t guard_fixups = {0};

    if ((!registers_already_flushed &&
         !jit_reg_emit_flush_all_dirty(w, regs)) ||
        !emit_link_target_guards(
            w, &candidate, &guard_config, &guard_fixups))
    {
        return false;
    }

#if RV64_JIT_STATS
    if (!emit_guarded_link_taken_stats(w, &candidate))
    {
        return false;
    }
#endif

    if (!emit_inc_jit_stat_counter(w, &rv64_jit_stats.direct_link_taken_count) ||
        !(extra_taken_counter == NULL ||
          emit_inc_jit_stat_counter(w, extra_taken_counter)) ||
        !emit_movabs_rdx(w, (uint64_t)(uintptr_t)target) ||
        !emit_mov_rax_rdxq_field(w, body_entry_off) ||
        !emit_jmp_rax(w))
    {
        return false;
    }

    patch_link_guard_misses(&guard_fixups, w->cur);

    return emit_store_pc_imm(w, target_pc) &&
           emit_inc_jit_stat_counter(w, &rv64_jit_stats.direct_link_miss_count) &&
           emit_return_total_retired(
               w, count_already_accumulated ? 0 : completed_count);
}

/*
 * Emit a selector which can be rewritten from the guarded path to a published
 * target's chain entry. The source count is committed before the selector, so
 * invalidation can always restore the guarded path without changing its ABI.
 */
static bool emit_patchable_direct_link_exit(
    rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs,
    vaddr_t target_pc, uint32_t completed_count,
    bool source_uses_data_state, uint64_t *extra_taken_counter)
{
    Assert(w->links != NULL, "jit: missing RV64 direct-link builder");
    Assert(w->links->count < RV64_JIT_BLOCK_MAX_LINKS,
           "jit: RV64 direct-link builder overflow");

    const word_t satp = cpu.csr.satp;
    const uint32_t ifetch_state = rv64_jit_ifetch_state();
    uint8_t *selector_disp = NULL;
    uint8_t *target_disp = NULL;
    const uint8_t *patched_path = NULL;

    if (!jit_reg_emit_flush_all_dirty(w, regs) ||
        !emit_accumulate_loop_extra(w, completed_count) ||
        !emit_jmp_rel32_placeholder(w, &selector_disp))
    {
        return false;
    }

#if RV64_JIT_STATS
    /*
     * The target chain entry owns every taken increment after its budget check.
     * This local thunk only carries the optional source-subset counter in RDI.
     */
    patched_path = w->cur;

    if (!emit_zero_esi(w) ||
        !(extra_taken_counter != NULL
              ? emit_movabs_rdi(
                    w, (uint64_t)(uintptr_t)extra_taken_counter)
              : emit_zero_edi(w)) ||
        !emit_jmp_rel32_placeholder(w, &target_disp))
    {
        return false;
    }
#else
    (void)extra_taken_counter;
#endif

    const uint8_t *guarded_path = w->cur;

    if (!emit_guarded_direct_link_exit(
            w, regs, target_pc, completed_count, source_uses_data_state,
            true, true, extra_taken_counter))
    {
        return false;
    }

    patch_rel32(selector_disp, guarded_path);

    rv64_jit_link_build_record_t *record =
        &w->links->records[w->links->count++];
    *record = (rv64_jit_link_build_record_t){
        .selector_disp = selector_disp,
        .target_disp = target_disp,
        .guarded_path = guarded_path,
        .patched_path = patched_path,
        .target_pc = target_pc,
        .target_satp = satp,
        .target_ifetch_state = ifetch_state,
        .patch_eligible = true,
    };
    return true;
}

/* Choose the mutable fast path only for the first, context-simple stage. */
bool rv64_jit_emit_direct_link_exit(rv64_jit_writer_t *w,
                                    rv64_jit_reg_cache_t *regs,
                                    vaddr_t target_pc,
                                    uint32_t completed_count,
                                    bool source_uses_data_state,
                                    uint64_t *extra_taken_counter)
{
    const bool bare_context =
        (cpu.csr.satp >> RV64_JIT_SATP_MODE_SHIFT) ==
        RV64_JIT_SATP_MODE_BARE;

    if (w->links != NULL &&
        w->links->count < RV64_JIT_BLOCK_MAX_LINKS &&
        bare_context && !source_uses_data_state)
    {
        return emit_patchable_direct_link_exit(
            w, regs, target_pc, completed_count,
            source_uses_data_state, extra_taken_counter);
    }

    return emit_guarded_direct_link_exit(
        w, regs, target_pc, completed_count, source_uses_data_state,
        false, false, extra_taken_counter);
}

/* Reserve the complete R8-pointed target, then commit this source's count. */
static bool emit_indirect_target_budget(rv64_jit_writer_t *w,
                                        uint32_t completed_count,
                                        uint8_t **over_budget_disp)
{
    const uint32_t insn_count_off =
        (uint32_t)offsetof(rv64_jit_block_t, insn_count);

    return emit_movabs_rdx(
               w, (uint64_t)(uintptr_t)&rv64_jit_loop_extra) &&
           emit_mov_eax_m32_rdx(w) &&
           emit_add_eax_imm32(w, completed_count) &&
           emit_mov_ecx_eax(w) &&
           emit_add_ecx_r8d_field(w, insn_count_off) &&
           emit_movabs_rdx(
               w, (uint64_t)(uintptr_t)&rv64_jit_entry_budget) &&
           emit_cmp_ecx_m32_rdx(w) &&
           emit_jcc_rel32_placeholder(
               w, HOST_JCC_A, over_budget_disp) &&
           emit_movabs_rdx(
               w, (uint64_t)(uintptr_t)&rv64_jit_loop_extra) &&
           emit_mov_m32_rdx_eax(w);
}

/* Materialise the cache slot while preserving each path's established ABI. */
static bool emit_link_candidate_address(
    rv64_jit_writer_t *w,
    const rv64_jit_link_candidate_t *candidate)
{
    switch (candidate->kind)
    {
    case RV64_JIT_LINK_CANDIDATE_KNOWN_RDX:
        Assert(candidate->known_slot != NULL,
               "jit: missing known RV64 direct-link slot");
        return emit_movabs_rdx(
            w, (uint64_t)(uintptr_t)candidate->known_slot);
    case RV64_JIT_LINK_CANDIDATE_RUNTIME_R8:
        /* Match rv64_jit_cache_hash_context() using the target in R9. */
        return emit_mov_rdx_r9(w) &&
               emit_shr_rdx_imm(w, RV64_JIT_CACHE_PC_SHIFT) &&
               emit_xor_edx_imm32(w, candidate->context_mix) &&
               emit_and_edx_imm32(w, RV64_JIT_CACHE_SIZE - 1u) &&
               emit_imul_rdx_imm32(
                   w, (uint32_t)sizeof(rv64_jit_block_t)) &&
               emit_movabs_reg(
                   w, HOST_REG_R8,
                   (uint64_t)(uintptr_t)rv64_jit_cache) &&
               emit_add_r8_rdx(w);
    default:
        Assert(false, "jit: invalid RV64 direct-link candidate kind %u",
               (unsigned)candidate->kind);
        return false;
    }
}

static bool emit_link_candidate_cmp_byte_imm(
    rv64_jit_writer_t *w,
    const rv64_jit_link_candidate_t *candidate,
    uint32_t offset, uint8_t value)
{
    if (candidate->kind == RV64_JIT_LINK_CANDIDATE_KNOWN_RDX)
    {
        return emit_cmp_rdxb_field_imm8(w, offset, value);
    }

    Assert(candidate->kind == RV64_JIT_LINK_CANDIDATE_RUNTIME_R8,
           "jit: invalid RV64 byte-guard candidate kind %u",
           (unsigned)candidate->kind);
    return emit_cmp_r8b_field_imm8(w, offset, value);
}

static bool emit_link_candidate_cmp_dword_imm(
    rv64_jit_writer_t *w,
    const rv64_jit_link_candidate_t *candidate,
    uint32_t offset, uint32_t value)
{
    if (candidate->kind == RV64_JIT_LINK_CANDIDATE_KNOWN_RDX)
    {
        return emit_cmp_rdxd_field_imm32(w, offset, value);
    }

    Assert(candidate->kind == RV64_JIT_LINK_CANDIDATE_RUNTIME_R8,
           "jit: invalid RV64 dword-guard candidate kind %u",
           (unsigned)candidate->kind);
    return emit_cmp_r8d_field_imm32(w, offset, value);
}

static bool emit_link_candidate_cmp_qword_rax(
    rv64_jit_writer_t *w,
    const rv64_jit_link_candidate_t *candidate,
    uint32_t offset)
{
    if (candidate->kind == RV64_JIT_LINK_CANDIDATE_KNOWN_RDX)
    {
        return emit_cmp_rdxq_field_rax(w, offset);
    }

    Assert(candidate->kind == RV64_JIT_LINK_CANDIDATE_RUNTIME_R8,
           "jit: invalid RV64 qword-guard candidate kind %u",
           (unsigned)candidate->kind);
    return emit_cmp_r8q_field_rax(w, offset);
}

static bool emit_link_candidate_cmp_qword_imm8(
    rv64_jit_writer_t *w,
    const rv64_jit_link_candidate_t *candidate,
    uint32_t offset, uint8_t value)
{
    if (candidate->kind == RV64_JIT_LINK_CANDIDATE_KNOWN_RDX)
    {
        return emit_cmp_rdxq_field_imm8(w, offset, value);
    }

    Assert(candidate->kind == RV64_JIT_LINK_CANDIDATE_RUNTIME_R8,
           "jit: invalid RV64 qword-guard candidate kind %u",
           (unsigned)candidate->kind);
    return emit_cmp_r8q_field_imm8(w, offset, value);
}

static bool emit_link_candidate_pc(
    rv64_jit_writer_t *w,
    const rv64_jit_link_candidate_t *candidate,
    uint32_t pc_offset)
{
    if (candidate->kind == RV64_JIT_LINK_CANDIDATE_KNOWN_RDX)
    {
        return emit_movabs_rax(w, candidate->target_pc) &&
               emit_cmp_rdxq_field_rax(w, pc_offset);
    }

    Assert(candidate->kind == RV64_JIT_LINK_CANDIDATE_RUNTIME_R8,
           "jit: invalid RV64 PC-guard candidate kind %u",
           (unsigned)candidate->kind);
    return emit_cmp_r8q_field_r9(w, pc_offset);
}

static bool emit_link_candidate_satp(
    rv64_jit_writer_t *w,
    const rv64_jit_link_candidate_t *candidate,
    uint32_t satp_offset, word_t satp)
{
    if (candidate->kind == RV64_JIT_LINK_CANDIDATE_KNOWN_RDX)
    {
        return emit_movabs_rax(w, satp) &&
               emit_cmp_rdxq_field_rax(w, satp_offset);
    }

    Assert(candidate->kind == RV64_JIT_LINK_CANDIDATE_RUNTIME_R8,
           "jit: invalid RV64 satp-guard candidate kind %u",
           (unsigned)candidate->kind);
    return emit_movabs_rdx(w, satp) &&
           emit_cmp_r8q_field_rdx(w, satp_offset);
}

/* Emit the path-specific form of the final whole-target budget guard. */
static bool emit_link_target_budget_guard(
    rv64_jit_writer_t *w,
    const rv64_jit_link_candidate_t *candidate,
    const rv64_jit_link_guard_config_t *config,
    rv64_jit_link_guard_fixups_t *fixups)
{
    if (candidate->kind == RV64_JIT_LINK_CANDIDATE_KNOWN_RDX)
    {
        const uint32_t insn_count_off =
            (uint32_t)offsetof(rv64_jit_block_t, insn_count);

        if (!emit_movabs_rdx(
                w, (uint64_t)(uintptr_t)&rv64_jit_loop_extra) ||
            !emit_mov_eax_m32_rdx(w))
        {
            return false;
        }

        /*
         * A loop fallthrough may already have published this source count.
         * Add it only on ordinary exits so one direct link cannot retire the
         * same guest instructions twice.
         */
        if (!config->count_already_accumulated &&
            !emit_add_eax_imm32(w, config->completed_count))
        {
            return false;
        }

        if (!emit_mov_ecx_eax(w) ||
            !emit_movabs_rdx(
                w, (uint64_t)(uintptr_t)candidate->known_slot) ||
            !emit_add_ecx_rdxd_field(w, insn_count_off) ||
            !emit_movabs_rdx(
                w, (uint64_t)(uintptr_t)&rv64_jit_entry_budget) ||
            !emit_cmp_ecx_m32_rdx(w) ||
            !emit_link_guard_miss_jcc(w, HOST_JCC_A, fixups))
        {
            return false;
        }

        /* Commit the new source count only after the complete target fits. */
        if (!config->count_already_accumulated &&
            (!emit_movabs_rdx(
                 w, (uint64_t)(uintptr_t)&rv64_jit_loop_extra) ||
             !emit_mov_m32_rdx_eax(w)))
        {
            return false;
        }

        return true;
    }

    Assert(candidate->kind == RV64_JIT_LINK_CANDIDATE_RUNTIME_R8,
           "jit: invalid RV64 budget-guard candidate kind %u",
           (unsigned)candidate->kind);
    Assert(!config->count_already_accumulated,
           "jit: runtime RV64 direct link received an accumulated count");

    uint8_t *over_budget_disp = NULL;
    if (!emit_indirect_target_budget(
            w, config->completed_count, &over_budget_disp))
    {
        return false;
    }

    Assert(fixups->count < RV64_JIT_DIRECT_LINK_MAX_MISS_PATCHES,
           "jit: runtime-target budget guard list overflow");
    fixups->disps[fixups->count++] = over_budget_disp;
    return true;
}

/*
 * Emit the one authoritative direct-link safety policy for both operand ABIs.
 * Each candidate differs only in how its slot and comparison operands are
 * encoded; the order and number of rejection checks are shared here.
 */
static bool emit_link_target_guards(
    rv64_jit_writer_t *w,
    const rv64_jit_link_candidate_t *candidate,
    const rv64_jit_link_guard_config_t *config,
    rv64_jit_link_guard_fixups_t *fixups)
{
    const uint32_t valid_off =
        (uint32_t)offsetof(rv64_jit_block_t, valid);
    const uint32_t translated_off =
        (uint32_t)offsetof(rv64_jit_block_t, translated);
    const uint32_t uses_data_state_off =
        (uint32_t)offsetof(rv64_jit_block_t, uses_data_state);
    const uint32_t pc_off = (uint32_t)offsetof(rv64_jit_block_t, pc);
    const uint32_t satp_off = (uint32_t)offsetof(rv64_jit_block_t, satp);
    const uint32_t ifetch_state_off =
        (uint32_t)offsetof(rv64_jit_block_t, ifetch_state);
    const uint32_t data_state_off =
        (uint32_t)offsetof(rv64_jit_block_t, data_state);
    const uint32_t ifetch_generation_off =
        (uint32_t)offsetof(rv64_jit_block_t, ifetch_generation);
    const uint32_t body_entry_off =
        (uint32_t)offsetof(rv64_jit_block_t, body_entry);
    uint8_t *data_state_ok_disp = NULL;
    uint8_t *ifetch_generation_ok_disp = NULL;

    Assert(candidate != NULL, "jit: missing RV64 direct-link candidate");
    Assert(config != NULL, "jit: missing RV64 direct-link guard config");
    Assert(fixups != NULL, "jit: missing RV64 direct-link guard fixups");
    *fixups = (rv64_jit_link_guard_fixups_t){0};

    if (!emit_link_candidate_address(w, candidate) ||
        !emit_link_candidate_cmp_byte_imm(
            w, candidate, valid_off, 1) ||
        !emit_link_guard_miss_jcc(w, HOST_JCC_NE, fixups) ||
        !emit_link_candidate_pc(w, candidate, pc_off) ||
        !emit_link_guard_miss_jcc(w, HOST_JCC_NE, fixups) ||
        !emit_link_candidate_satp(
            w, candidate, satp_off, config->satp) ||
        !emit_link_guard_miss_jcc(w, HOST_JCC_NE, fixups) ||
        !emit_link_candidate_cmp_dword_imm(
            w, candidate, ifetch_state_off,
            config->ifetch_state) ||
        !emit_link_guard_miss_jcc(w, HOST_JCC_NE, fixups) ||
        !emit_link_candidate_cmp_byte_imm(
            w, candidate, uses_data_state_off, 0))
    {
        return false;
    }

    if (config->source_uses_data_state)
    {
        /*
         * A source that captures data-translation state proves the current
         * value for a sensitive target. An insensitive target needs no such
         * comparison and takes the short branch around it.
         */
        if (!emit_jcc_rel32_placeholder(
                w, HOST_JCC_E, &data_state_ok_disp) ||
            !emit_link_candidate_cmp_dword_imm(
                w, candidate, data_state_off, config->data_state) ||
            !emit_link_guard_miss_jcc(
                w, HOST_JCC_NE, fixups))
        {
            return false;
        }

        patch_rel32(data_state_ok_disp, w->cur);
    }
    else
    {
        /*
         * A source without this dependency survives a data-state change, so
         * it may link only to a target that is data-state insensitive.
         */
        if (!emit_link_guard_miss_jcc(w, HOST_JCC_NE, fixups))
        {
            return false;
        }
    }

    /*
     * A changed translated-fetch generation returns to C, where
     * rv64_jit_block_matches() owns the slower page-table and source proof.
     */
    if (!emit_link_candidate_cmp_byte_imm(
            w, candidate, translated_off, 0) ||
        !emit_jcc_rel32_placeholder(
            w, HOST_JCC_E, &ifetch_generation_ok_disp) ||
        !emit_movabs_rax(
            w, (uint64_t)(uintptr_t)&rv64_jit_ifetch_generation) ||
        !emit_mov_rax_m64_rax(w) ||
        !emit_link_candidate_cmp_qword_rax(
            w, candidate, ifetch_generation_off) ||
        !emit_link_guard_miss_jcc(
            w, HOST_JCC_NE, fixups))
    {
        return false;
    }

    patch_rel32(ifetch_generation_ok_disp, w->cur);

    if (!emit_link_candidate_cmp_qword_imm8(
            w, candidate, body_entry_off, 0) ||
        !emit_link_guard_miss_jcc(w, HOST_JCC_E, fixups) ||
        !emit_link_target_budget_guard(
            w, candidate, config, fixups))
    {
        return false;
    }

    Assert(fixups->count == RV64_JIT_DIRECT_LINK_GUARD_COUNT,
           "jit: shared RV64 direct-link guard policy drifted");
    return true;
}

#if RV64_JIT_STATS
/* Count links whose target requires translation or data-state validation. */
static bool emit_guarded_link_taken_stats(
    rv64_jit_writer_t *w,
    const rv64_jit_link_candidate_t *candidate)
{
    const uint32_t translated_off =
        (uint32_t)offsetof(rv64_jit_block_t, translated);
    const uint32_t uses_data_state_off =
        (uint32_t)offsetof(rv64_jit_block_t, uses_data_state);
    uint8_t *guarded_taken_disp = NULL;
    uint8_t *guarded_done_disp = NULL;

    if ((candidate->kind == RV64_JIT_LINK_CANDIDATE_KNOWN_RDX &&
         !emit_link_candidate_address(w, candidate)) ||
        !emit_link_candidate_cmp_byte_imm(
            w, candidate, translated_off, 0) ||
        !emit_jcc_rel32_placeholder(
            w, HOST_JCC_NE, &guarded_taken_disp) ||
        !emit_link_candidate_cmp_byte_imm(
            w, candidate, uses_data_state_off, 0) ||
        !emit_jcc_rel32_placeholder(
            w, HOST_JCC_E, &guarded_done_disp))
    {
        return false;
    }

    patch_rel32(guarded_taken_disp, w->cur);

    if (!emit_inc_jit_stat_counter(
            w, &rv64_jit_stats.direct_guarded_link_taken_count))
    {
        return false;
    }

    patch_rel32(guarded_done_disp, w->cur);
    return true;
}
#endif

/*
 * Relocations which connect one generated PIC-way probe to its neighbouring
 * probe, the shared miss path, and the later hot-hit region.  Keeping these
 * four exits together makes the loop which owns their final destinations the
 * only place where cross-way control flow is assembled.
 */
typedef struct
{
    uint8_t *tag_miss_disp;
    uint8_t *empty_disp;
    uint8_t *stale_to_miss_disp;
    uint8_t *hit_disp;
} rv64_jit_indirect_pic_way_fixups_t;

/*
 * Emit one way of the guarded indirect PIC probe.
 *
 * RDX already contains the sidecar base and must remain live for every later
 * way.  A matching tag enters the mutable selector, whose initial destination
 * is this way's guarded path.  That path accepts only a non-empty, live slot
 * with the exact generation published by the refill; all other outcomes stay
 * unresolved for the wrapper to route either to the next way or to the one
 * common miss path.  The instruction order intentionally matches the former
 * hand-written way-zero and way-one sequences byte for byte.
 */
static bool emit_indirect_pic_way_probe(
    rv64_jit_writer_t *w, uint32_t way,
    rv64_jit_indirect_pic_kind_t pic_kind,
    rv64_jit_indirect_pic_way_fixups_t *fixups)
{
    Assert(w->indirect_pic != NULL && w->indirect_pic->used,
           "jit: missing active RV64 indirect PIC builder");
    Assert(way < RV64_JIT_INDIRECT_PIC_WAYS,
           "jit: invalid RV64 indirect PIC way %u", way);
    Assert(pic_kind < RV64_JIT_INDIRECT_PIC_KIND_COUNT,
           "jit: invalid RV64 indirect PIC kind %u", pic_kind);
    Assert(fixups != NULL, "jit: missing RV64 indirect PIC way fixups");

    const uint32_t way_base =
        (uint32_t)offsetof(rv64_jit_indirect_pic_t, ways) +
        way * (uint32_t)sizeof(rv64_jit_indirect_pic_entry_t);
    const uint32_t target_pc_off =
        (uint32_t)offsetof(rv64_jit_indirect_pic_entry_t, target_pc);
    const uint32_t target_generation_off =
        (uint32_t)offsetof(
            rv64_jit_indirect_pic_entry_t, target_generation);
    const uint32_t target_slot_off =
        (uint32_t)offsetof(rv64_jit_indirect_pic_entry_t, target_slot);
    const uint32_t valid_off =
        (uint32_t)offsetof(rv64_jit_block_t, valid);
    const uint32_t generation_off =
        (uint32_t)offsetof(rv64_jit_block_t, generation);
    uint8_t *stale_disp = NULL;

    *fixups = (rv64_jit_indirect_pic_way_fixups_t){0};

    if (!emit_cmp_rdxq_field_r9(w, way_base + target_pc_off) ||
        !emit_jcc_rel32_placeholder(
            w, HOST_JCC_NE, &fixups->tag_miss_disp) ||
        !emit_jmp_rel32_placeholder(
            w, &w->indirect_pic->selector_disps[way]))
    {
        return false;
    }

    w->indirect_pic->guarded_paths[way] = w->cur;
    patch_rel32(w->indirect_pic->selector_disps[way], w->cur);

    if (!emit_mov_r8_rdxq_field(w, way_base + target_slot_off) ||
        !emit_test_r8_r8(w) ||
        !emit_jcc_rel32_placeholder(
            w, HOST_JCC_E, &fixups->empty_disp) ||
        !emit_cmp_r8b_field_imm8(w, valid_off, 1) ||
        !emit_jcc_rel32_placeholder(
            w, HOST_JCC_NE, &stale_disp) ||
        !emit_mov_rax_rdxq_field(
            w, way_base + target_generation_off) ||
        !emit_cmp_r8q_field_rax(w, generation_off) ||
        !emit_jcc_rel32_placeholder(
            w, HOST_JCC_E, &fixups->hit_disp))
    {
        return false;
    }

    patch_rel32(stale_disp, w->cur);

    return emit_inc_jit_stat_counter(
               w, &rv64_jit_stats.indirect_pic_stale_rejections[pic_kind]) &&
           emit_jmp_rel32_placeholder(
               w, &fixups->stale_to_miss_disp);
}

/*
 * Assemble the complete two-way probe around the uniform way emitter above.
 * There is one sidecar-address fixup, as before: RDX remains the base while a
 * tag miss advances to the next generated way.  Empty entries and stale
 * certificates bypass later tags and converge directly on the shared miss
 * counter, while hit branches remain unresolved for the later primary and
 * secondary hot-hit paths.
 */
static bool emit_indirect_pic_probe(
    rv64_jit_writer_t *w, rv64_jit_indirect_pic_kind_t pic_kind,
    uint8_t *hit_disps[RV64_JIT_INDIRECT_PIC_WAYS])
{
    rv64_jit_indirect_pic_way_fixups_t
        way_fixups[RV64_JIT_INDIRECT_PIC_WAYS] = {0};

    if (!emit_movabs_indirect_pic_address(w, HOST_REG_RDX))
    {
        return false;
    }

    for (uint32_t way = 0; way < RV64_JIT_INDIRECT_PIC_WAYS; way++)
    {
        if (way != 0u)
        {
            /* A tag miss alone is allowed to inspect the following way. */
            patch_rel32(way_fixups[way - 1u].tag_miss_disp, w->cur);
        }

        if (!emit_indirect_pic_way_probe(
                w, way, pic_kind, &way_fixups[way]))
        {
            return false;
        }
    }

    const uint8_t *pic_miss = w->cur;

    for (uint32_t way = 0; way < RV64_JIT_INDIRECT_PIC_WAYS; way++)
    {
        if (way + 1u == RV64_JIT_INDIRECT_PIC_WAYS)
        {
            patch_rel32(way_fixups[way].tag_miss_disp, pic_miss);
        }

        patch_rel32(way_fixups[way].empty_disp, pic_miss);
        patch_rel32(way_fixups[way].stale_to_miss_disp, pic_miss);
        hit_disps[way] = way_fixups[way].hit_disp;
    }

    return emit_inc_jit_stat_counter(
        w, &rv64_jit_stats.indirect_pic_misses[pic_kind]);
}

/*
 * Probe the data-only direct-mapped jump cache for the target held in R9.
 *
 * RDI is deliberately left pointing at the selected cache entry on every
 * non-terminal route.  The authoritative-success region can therefore fill
 * that exact entry without repeating the index calculation.  A valid hit is
 * terminal; empty entries and tag misses join the authoritative lookup, while
 * stale certificates first account for their rejection.  Only the budget
 * rejection bypasses the lookup and remains unresolved for the common miss.
 */
static bool emit_indirect_jump_cache_probe(
    rv64_jit_writer_t *w, uint32_t completed_count,
    uint64_t *subset_taken_counter,
    uint8_t **budget_to_miss_disp)
{
    const uint32_t cache_generation_off =
        (uint32_t)offsetof(
            rv64_jit_indirect_jump_cache_entry_t, target_generation);
    const uint32_t cache_slot_off =
        (uint32_t)offsetof(
            rv64_jit_indirect_jump_cache_entry_t, target_slot);
    const uint32_t valid_off =
        (uint32_t)offsetof(rv64_jit_block_t, valid);
    const uint32_t generation_off =
        (uint32_t)offsetof(rv64_jit_block_t, generation);
    const uint32_t pc_off =
        (uint32_t)offsetof(rv64_jit_block_t, pc);
    const uint32_t body_entry_off =
        (uint32_t)offsetof(rv64_jit_block_t, body_entry);
    uint8_t *empty_disp = NULL;
    uint8_t *tag_miss_disp = NULL;
    uint8_t *invalid_stale_disp = NULL;
    uint8_t *zero_generation_stale_disp = NULL;
    uint8_t *changed_generation_stale_disp = NULL;
    uint8_t *budget_disp = NULL;

    Assert(w->indirect_jump_cache != NULL &&
               w->indirect_jump_cache->used,
           "jit: missing active RV64 indirect jump cache builder");
    Assert(budget_to_miss_disp != NULL,
           "jit: missing RV64 jump-cache budget fixup result");
    *budget_to_miss_disp = NULL;

    /*
     * Compute (((target >> 2) & 15) << 4) as
     * ((target & 0x3c) << 2). RISC-V instruction alignment makes these
     * expressions identical, while the latter removes one hot shift and uses
     * compact 32-bit operations whose result is a zero-extended sidecar byte
     * offset.  Keeping RDI live through all miss routes is part of this
     * helper's interface, not incidental scratch-register behaviour.
     */
    if (!emit_mov_rdx_r9(w) ||
        !emit_and_edx_imm8(
            w, RV64_JIT_INDIRECT_JUMP_CACHE_BYTE_MASK) ||
        !emit_shl_edx_imm(
            w, RV64_JIT_INDIRECT_JUMP_CACHE_OFFSET_SHIFT) ||
        !emit_movabs_indirect_jump_cache_address(
            w, HOST_REG_RDI) ||
        !emit_add_rdi_rdx(w) ||
        !emit_mov_r8_rdiq_field(w, cache_slot_off) ||
        !emit_test_r8_r8(w) ||
        !emit_jcc_rel32_placeholder(
            w, HOST_JCC_E, &empty_disp) ||
        !emit_cmp_r8q_field_r9(w, pc_off) ||
        !emit_jcc_rel32_placeholder(
            w, HOST_JCC_NE, &tag_miss_disp) ||
        !emit_cmp_r8b_field_imm8(w, valid_off, 1) ||
        !emit_jcc_rel32_placeholder(
            w, HOST_JCC_NE, &invalid_stale_disp) ||
        !emit_mov_rax_rdiq_field(w, cache_generation_off) ||
        !emit_test_rax_rax(w) ||
        !emit_jcc_rel32_placeholder(
            w, HOST_JCC_E, &zero_generation_stale_disp) ||
        !emit_cmp_r8q_field_rax(w, generation_off) ||
        !emit_jcc_rel32_placeholder(
            w, HOST_JCC_NE, &changed_generation_stale_disp) ||
        !emit_indirect_target_budget(
            w, completed_count, &budget_disp) ||
        !emit_inc_jit_stat_counter(
            w, &rv64_jit_stats.indirect_jump_cache_hits) ||
        !emit_inc_jit_stat_counter(
            w, &rv64_jit_stats.direct_link_taken_count) ||
        !(subset_taken_counter == NULL ||
          emit_inc_jit_stat_counter(w, subset_taken_counter)) ||
        !emit_mov_rax_r8q_field(w, body_entry_off) ||
        !emit_jmp_rax(w))
    {
        return false;
    }

    patch_rel32(budget_disp, w->cur);

    if (!emit_inc_jit_stat_counter(
            w, &rv64_jit_stats.indirect_jump_cache_budget_rejections) ||
        !emit_jmp_rel32_placeholder(
            w, budget_to_miss_disp))
    {
        return false;
    }

    const uint8_t *stale_path = w->cur;
    patch_rel32(invalid_stale_disp, stale_path);
    patch_rel32(zero_generation_stale_disp, stale_path);
    patch_rel32(changed_generation_stale_disp, stale_path);

    if (!emit_inc_jit_stat_counter(
            w, &rv64_jit_stats.indirect_jump_cache_stale_rejections))
    {
        return false;
    }

    const uint8_t *cache_miss = w->cur;
    patch_rel32(empty_disp, cache_miss);
    patch_rel32(tag_miss_disp, cache_miss);

    return emit_inc_jit_stat_counter(
        w, &rv64_jit_stats.indirect_jump_cache_misses);
}

/*
 * Emit the terminal success reached after the authoritative R8 slot passes
 * every shared link guard and the whole-target budget check.
 *
 * R9 remains the architectural target.  A PIC-enabled site calls its cold
 * refill helper and jumps through the returned body pointer.  A jump-cache
 * site consumes the RDI entry retained by its probe and publishes the slot
 * before the non-zero generation certificate.  A site with neither sidecar
 * simply enters the authoritative body.  Every generated route is terminal.
 */
static bool emit_indirect_authoritative_hit(
    rv64_jit_writer_t *w, bool pic_enabled,
    bool jump_cache_enabled, uint64_t *subset_taken_counter)
{
    const uint32_t generation_off =
        (uint32_t)offsetof(rv64_jit_block_t, generation);
    const uint32_t body_entry_off =
        (uint32_t)offsetof(rv64_jit_block_t, body_entry);

    Assert(!(pic_enabled && jump_cache_enabled),
           "jit: authoritative RV64 hit selected both indirect caches");

    if (pic_enabled)
    {
        Assert(w->indirect_pic != NULL && w->indirect_pic->used,
               "jit: missing active RV64 indirect PIC builder");

        /*
         * Refill is cold: hot hits never cross the C ABI. Preserve the returned
         * body pointer in R9 while restoring R10/R11 and updating statistics.
         */
        return emit_mov_rsi_r9(w) &&
               emit_mov_rdx_r8(w) &&
               emit_movabs_indirect_pic_address(w, HOST_REG_RDI) &&
               emit_call_abs(
                   w, (uintptr_t)rv64_jit_indirect_pic_refill) &&
               emit_mov_r9_rax(w) &&
               emit_load_jit_bases(w) &&
               emit_inc_jit_stat_counter(
                   w, &rv64_jit_stats.direct_link_taken_count) &&
               (subset_taken_counter == NULL ||
                emit_inc_jit_stat_counter(
                    w, subset_taken_counter)) &&
               emit_jmp_r9(w);
    }

    if (jump_cache_enabled)
    {
        const uint32_t cache_generation_off =
            (uint32_t)offsetof(
                rv64_jit_indirect_jump_cache_entry_t,
                target_generation);
        const uint32_t cache_slot_off =
            (uint32_t)offsetof(
                rv64_jit_indirect_jump_cache_entry_t,
                target_slot);

        Assert(w->indirect_jump_cache != NULL &&
                   w->indirect_jump_cache->used,
               "jit: missing active RV64 indirect jump cache builder");

#if RV64_JIT_STATS
        uint8_t *empty_entry_disp = NULL;

        /* Count any occupied direct-map entry overwritten by this fill. */
        if (!emit_mov_rax_rdiq_field(w, cache_slot_off) ||
            !emit_test_rax_rax(w) ||
            !emit_jcc_rel32_placeholder(
                w, HOST_JCC_E, &empty_entry_disp) ||
            !emit_inc_jit_stat_counter(
                w, &rv64_jit_stats.indirect_jump_cache_replacements))
        {
            return false;
        }

        patch_rel32(empty_entry_disp, w->cur);
#endif

        /*
         * Generated code and its sidecar currently execute on one vCPU
         * thread. Clear the certificate, install the stable slot, then
         * publish the exact non-zero generation last. A future concurrent
         * implementation must make this release/acquire or per-vCPU.
         */
        if (!emit_zero_rax(w) ||
            !emit_mov_rdiq_field_rax(w, cache_generation_off) ||
            !emit_mov_rdiq_field_r8(w, cache_slot_off) ||
            !emit_inc_jit_stat_counter(
                w, &rv64_jit_stats.indirect_jump_cache_fills) ||
            !emit_mov_rax_r8q_field(w, generation_off) ||
            !emit_mov_rdiq_field_rax(w, cache_generation_off))
        {
            return false;
        }
    }

    return emit_inc_jit_stat_counter(
               w, &rv64_jit_stats.direct_link_taken_count) &&
           (subset_taken_counter == NULL ||
            emit_inc_jit_stat_counter(
                w, subset_taken_counter)) &&
           emit_mov_rax_r8q_field(w, body_entry_off) &&
           emit_jmp_rax(w);
}

/*
 * Emit the later hot-hit adapters reached by the unresolved PIC-way branches.
 * R8 still holds the matching authoritative slot.  Statistics builds assign
 * ESI route zero/one before converging, so the primary and secondary counters
 * retain their existing layout; non-statistics builds converge immediately.
 * A successful budget check is terminal, while its one rejection is returned
 * unresolved for the shared architectural miss path.
 */
static bool emit_indirect_pic_fast_hits(
    rv64_jit_writer_t *w,
    uint8_t *hit_disps[RV64_JIT_INDIRECT_PIC_WAYS],
    uint32_t completed_count, uint64_t *subset_taken_counter,
    rv64_jit_indirect_pic_kind_t pic_kind,
    uint8_t **budget_to_miss_disp)
{
    const uint32_t body_entry_off =
        (uint32_t)offsetof(rv64_jit_block_t, body_entry);
    uint8_t *budget_disp = NULL;

    Assert(w->indirect_pic != NULL && w->indirect_pic->used,
           "jit: missing active RV64 indirect PIC builder");
    Assert(hit_disps != NULL && hit_disps[0] != NULL &&
               hit_disps[1] != NULL,
           "jit: missing RV64 indirect PIC hit fixups");
    Assert(budget_to_miss_disp != NULL,
           "jit: missing RV64 indirect PIC budget fixup result");
    *budget_to_miss_disp = NULL;

#if RV64_JIT_STATS
    uint8_t *secondary_to_common_disp = NULL;
    uint8_t *no_secondary_stat_disp = NULL;
    const uint8_t *secondary_hit = w->cur;

    if (!emit_mov_esi_imm32(w, 1) ||
        !emit_jmp_rel32_placeholder(
            w, &secondary_to_common_disp))
    {
        return false;
    }

    const uint8_t *primary_hit = w->cur;

    if (!emit_zero_esi(w))
    {
        return false;
    }

    const uint8_t *fast_hit = w->cur;
    patch_rel32(hit_disps[0], primary_hit);
    patch_rel32(hit_disps[1], secondary_hit);
    patch_rel32(secondary_to_common_disp, fast_hit);
#else
    const uint8_t *fast_hit = w->cur;
    patch_rel32(hit_disps[0], fast_hit);
    patch_rel32(hit_disps[1], fast_hit);
#endif

    if (!emit_indirect_target_budget(
            w, completed_count, &budget_disp) ||
        !emit_inc_jit_stat_counter(
            w, &rv64_jit_stats.indirect_pic_hits[pic_kind]))
    {
        return false;
    }

#if RV64_JIT_STATS
    if (!emit_test_esi_esi(w) ||
        !emit_jcc_rel32_placeholder(
            w, HOST_JCC_E, &no_secondary_stat_disp) ||
        !emit_inc_jit_stat_counter(
            w, &rv64_jit_stats.indirect_pic_secondary_hits[pic_kind]))
    {
        return false;
    }

    patch_rel32(no_secondary_stat_disp, w->cur);
#endif

    if (!emit_inc_jit_stat_counter(
            w, &rv64_jit_stats.direct_link_taken_count) ||
        !(subset_taken_counter == NULL ||
          emit_inc_jit_stat_counter(w, subset_taken_counter)) ||
        !emit_mov_rax_r8q_field(w, body_entry_off) ||
        !emit_jmp_rax(w))
    {
        return false;
    }

    patch_rel32(budget_disp, w->cur);

    return emit_inc_jit_stat_counter(
               w, &rv64_jit_stats.indirect_pic_budget_rejections[pic_kind]) &&
           emit_jmp_rel32_placeholder(
               w, budget_to_miss_disp);
}

/*
 * Resolve every non-terminal rejection to the one architectural JALR miss.
 * R9 has survived every probe and guard and is therefore the value committed
 * to cpu.pc.  Returning completed_count includes the already-executed JALR so
 * the dispatcher resumes at that target rather than repeating the transfer.
 */
static bool emit_indirect_miss_path(
    rv64_jit_writer_t *w,
    const rv64_jit_link_guard_fixups_t *guard_fixups,
    uint8_t *pic_budget_to_miss_disp,
    uint8_t *jump_cache_budget_to_miss_disp,
    uint32_t completed_count, uint64_t *subset_miss_counter)
{
    const uint8_t *miss_path = w->cur;

    Assert(guard_fixups != NULL,
           "jit: missing RV64 indirect guard fixups");
    patch_link_guard_misses(guard_fixups, miss_path);

    if (pic_budget_to_miss_disp != NULL)
    {
        patch_rel32(pic_budget_to_miss_disp, miss_path);
    }

    if (jump_cache_budget_to_miss_disp != NULL)
    {
        patch_rel32(jump_cache_budget_to_miss_disp, miss_path);
    }

    return emit_mov_rax_r9(w) &&
           emit_store_rax_pc(w) &&
           emit_inc_jit_stat_counter(
               w, &rv64_jit_stats.direct_link_miss_count) &&
           (subset_miss_counter == NULL ||
            emit_inc_jit_stat_counter(
                w, subset_miss_counter)) &&
           emit_return_total_retired(w, completed_count);
}

/*
 * Append the unreachable mutable-PIC patch thunks after the ordinary miss.
 * Each way records the thunk start, commits this source's retired count, and
 * leaves one target-owned rel32 displacement for publication.  In statistics
 * builds ESI carries the stable route value expected by the destination chain
 * entry; the selector and target displacement arrays must stay index-aligned.
 */
static bool emit_indirect_pic_patch_thunks(
    rv64_jit_writer_t *w, uint32_t completed_count,
    rv64_jit_indirect_pic_kind_t pic_kind)
{
    Assert(w->indirect_pic != NULL && w->indirect_pic->used,
           "jit: missing active RV64 indirect PIC builder");

#if !RV64_JIT_STATS
    (void)pic_kind;
#endif

    for (uint32_t i = 0; i < RV64_JIT_INDIRECT_PIC_WAYS; i++)
    {
        w->indirect_pic->patched_paths[i] = w->cur;

        if (!emit_accumulate_loop_extra(w, completed_count)
#if RV64_JIT_STATS
            || !emit_mov_esi_imm32(
                w, 1u + (uint32_t)pic_kind *
                             RV64_JIT_INDIRECT_PIC_WAYS + i)
#endif
            || !emit_jmp_rel32_placeholder(
                w, &w->indirect_pic->target_disps[i]))
        {
            return false;
        }
    }

    return true;
}

/*
 * Probe and enter the native block named by an aligned runtime JALR target.
 *
 * Runtime register contract:
 *   - RAX initially contains the full architectural target;
 *   - R9 preserves that target until either the hit jump or miss PC store;
 *   - R8 holds the candidate authoritative block-cache slot;
 *   - RAX, RCX, RDX, RSI, and RDI are scratch after dirty GPRs are flushed.
 *
 * An eligible context-simple call or return first checks two per-exit PIC ways.
 * Each way retains only a target tag, a stable block-cache-slot address, and
 * the exact non-zero generation that passed the complete lookup below. No
 * native pointer is cached. Any empty, invalid, replaced, translated, or
 * otherwise stale target therefore falls through to the authoritative guards
 * and ordinary dispatcher miss path.
 *
 * A non-linking JALR without an architectural return-stack hint deliberately
 * skips the two-way PIC. Large switch tables cannot fit it and would otherwise
 * call the C refill helper on nearly every dispatch. Eligible Bare-mode sites
 * instead use the guarded data-only cache above; all other sites retain the
 * same inline authoritative guards and ordinary dispatcher miss path without
 * installing mutable native edges.
 */
static bool emit_indirect_link_exit(
    rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs,
    uint32_t rd, vaddr_t link_pc, uint32_t completed_count,
    bool source_uses_data_state, bool pic_eligible,
    uint64_t *subset_taken_counter, uint64_t *subset_miss_counter,
    rv64_jit_indirect_pic_kind_t pic_kind)
{
    const word_t satp = cpu.csr.satp;
    const uint32_t ifetch_state = rv64_jit_ifetch_state();
    const uint32_t context_mix =
        rv64_jit_cache_context_mix(satp, ifetch_state);
    const bool bare_context =
        (satp >> RV64_JIT_SATP_MODE_SHIFT) ==
        RV64_JIT_SATP_MODE_BARE;
    const bool pic_enabled =
        w->indirect_pic != NULL && pic_eligible &&
        !source_uses_data_state && bare_context;
    const bool jump_cache_enabled =
        w->indirect_jump_cache != NULL && !pic_eligible &&
        !source_uses_data_state && bare_context;
    const rv64_jit_link_candidate_t candidate = {
        .kind = RV64_JIT_LINK_CANDIDATE_RUNTIME_R8,
        .context_mix = context_mix,
    };
    const rv64_jit_link_guard_config_t guard_config = {
        .satp = satp,
        .ifetch_state = ifetch_state,
        .data_state = rv64_jit_data_tlb_state(MEM_TYPE_READ),
        .completed_count = completed_count,
        .source_uses_data_state = source_uses_data_state,
        .count_already_accumulated = false,
    };
    rv64_jit_link_guard_fixups_t guard_fixups = {0};
    uint8_t *pic_hit_disps[RV64_JIT_INDIRECT_PIC_WAYS] = {0};
    uint8_t *jump_cache_budget_to_miss_disp = NULL;

    Assert(pic_kind < RV64_JIT_INDIRECT_PIC_KIND_COUNT,
           "jit: invalid RV64 indirect PIC kind %u", pic_kind);
    Assert(!(pic_enabled && jump_cache_enabled),
           "jit: one RV64 JALR selected both indirect caches");

    if (pic_enabled)
    {
        Assert(!w->indirect_pic->used,
               "jit: more than one indirect PIC in an RV64 block");
        w->indirect_pic->used = true;
        w->indirect_pic->kind = (uint8_t)pic_kind;
        JIT_STAT_INC(emitted_sites.indirect_pic_sites);
    }

    if (jump_cache_enabled)
    {
        Assert(!w->indirect_jump_cache->used,
               "jit: more than one indirect jump cache in an RV64 block");
        w->indirect_jump_cache->used = true;
        JIT_STAT_INC(emitted_sites.indirect_jump_cache_sites);
    }

    if (!emit_mov_r9_rax(w) ||
        !jit_reg_write_imm(w, regs, rd, link_pc) ||
        !jit_reg_flush_all_dirty(w, regs))
    {
        return false;
    }

    if (jump_cache_enabled)
    {
        if (!emit_indirect_jump_cache_probe(
                w, completed_count, subset_taken_counter,
                &jump_cache_budget_to_miss_disp))
        {
            return false;
        }
    }

    if (pic_enabled)
    {
        if (!emit_indirect_pic_probe(w, pic_kind, pic_hit_disps))
        {
            return false;
        }
    }

    if (!emit_link_target_guards(
            w, &candidate, &guard_config, &guard_fixups))
    {
        return false;
    }

#if RV64_JIT_STATS
    if (!emit_guarded_link_taken_stats(w, &candidate))
    {
        return false;
    }
#endif

    if (!emit_indirect_authoritative_hit(
            w, pic_enabled, jump_cache_enabled,
            subset_taken_counter))
    {
        return false;
    }

    uint8_t *pic_budget_to_miss_disp = NULL;

    if (pic_enabled)
    {
        if (!emit_indirect_pic_fast_hits(
                w, pic_hit_disps, completed_count,
                subset_taken_counter, pic_kind,
                &pic_budget_to_miss_disp))
        {
            return false;
        }
    }

    if (!emit_indirect_miss_path(
            w, &guard_fixups, pic_budget_to_miss_disp,
            jump_cache_budget_to_miss_disp, completed_count,
            subset_miss_counter))
    {
        return false;
    }

    if (pic_enabled &&
        !emit_indirect_pic_patch_thunks(
            w, completed_count, pic_kind))
    {
        return false;
    }

    return true;
}

/*
 * Inline memory emitters.
 *
 * Direct PMEM memory code is emitted only after earlier guards have proved
 * alignment, ordinary RAM range and no source-code write hazard.  The paged
 * variant adds an inline DTLB-hit proof: matching `satp`, VPN, permission state
 * and access rights, plus a same-page range check.  The slow edge from any
 * failed guard goes to the existing helper path, so page faults, MMIO, fresh
 * walks, cross-page accesses and invalidation side effects remain centralised in
 * the C implementation.
 */
/* Emit the x86 load instruction matching one RV64 load funct3 field. */
static bool emit_direct_pmem_load_rax(rv64_jit_writer_t *w, uint32_t funct3)
{
    /*
     * RDX is the byte offset from CONFIG_MBASE and R10 is the host pointer for
     * CONFIG_MBASE. Signed byte/half/word forms use x86 sign-extension loads;
     * unsigned forms write EAX, which zeroes the upper half of RAX by x86-64
     * rule. LD is a plain 64-bit load.
     */
    switch (funct3)
    {
    case RV64_FUNCT3_LB: /* LB: movsx rax, byte ptr [r10 + rdx]. */
        return emit_u8(w, 0x49) && emit_u8(w, 0x0f) && emit_u8(w, 0xbe) &&
               emit_pmem_sib_r10_rdx(w, HOST_REG_RAX);
    case RV64_FUNCT3_LH: /* LH: movsx rax, word ptr [r10 + rdx]. */
        return emit_u8(w, 0x49) && emit_u8(w, 0x0f) && emit_u8(w, 0xbf) &&
               emit_pmem_sib_r10_rdx(w, HOST_REG_RAX);
    case RV64_FUNCT3_LW: /* LW: movsxd rax, dword ptr [r10 + rdx]. */
        return emit_u8(w, 0x49) && emit_u8(w, 0x63) &&
               emit_pmem_sib_r10_rdx(w, HOST_REG_RAX);
    case RV64_FUNCT3_LD: /* LD: mov rax, qword ptr [r10 + rdx]. */
        return emit_u8(w, 0x49) && emit_u8(w, 0x8b) &&
               emit_pmem_sib_r10_rdx(w, HOST_REG_RAX);
    case RV64_FUNCT3_LBU: /* LBU: movzx eax, byte ptr [r10 + rdx]. */
        return emit_u8(w, 0x41) && emit_u8(w, 0x0f) && emit_u8(w, 0xb6) &&
               emit_pmem_sib_r10_rdx(w, HOST_REG_RAX);
    case RV64_FUNCT3_LHU: /* LHU: movzx eax, word ptr [r10 + rdx]. */
        return emit_u8(w, 0x41) && emit_u8(w, 0x0f) && emit_u8(w, 0xb7) &&
               emit_pmem_sib_r10_rdx(w, HOST_REG_RAX);
    case RV64_FUNCT3_LWU: /* LWU: mov eax, dword ptr [r10 + rdx]. */
        return emit_u8(w, 0x41) && emit_u8(w, 0x8b) &&
               emit_pmem_sib_r10_rdx(w, HOST_REG_RAX);
    default:
        return false;
    }
}

/* Emit the x86 load matching one RV64 load from `[rcx + rdx]`. */
static bool emit_direct_mmio_load_rax(rv64_jit_writer_t *w, uint32_t funct3)
{
    /*
     * RCX is the stable device-backing base and RDX is the proved map offset.
     * The encodings mirror the PMEM forms above, without REX.B because RCX is a
     * low host register.  Writing EAX zero-extends unsigned results to RV64.
     */
    switch (funct3)
    {
    case RV64_FUNCT3_LB: /* LB: movsx rax, byte ptr [rcx + rdx]. */
        return emit_u8(w, 0x48) && emit_u8(w, 0x0f) && emit_u8(w, 0xbe) &&
               emit_mmio_sib_rcx_rdx(w, HOST_REG_RAX);
    case RV64_FUNCT3_LH: /* LH: movsx rax, word ptr [rcx + rdx]. */
        return emit_u8(w, 0x48) && emit_u8(w, 0x0f) && emit_u8(w, 0xbf) &&
               emit_mmio_sib_rcx_rdx(w, HOST_REG_RAX);
    case RV64_FUNCT3_LW: /* LW: movsxd rax, dword ptr [rcx + rdx]. */
        return emit_u8(w, 0x48) && emit_u8(w, 0x63) &&
               emit_mmio_sib_rcx_rdx(w, HOST_REG_RAX);
    case RV64_FUNCT3_LD: /* LD: mov rax, qword ptr [rcx + rdx]. */
        return emit_u8(w, 0x48) && emit_u8(w, 0x8b) &&
               emit_mmio_sib_rcx_rdx(w, HOST_REG_RAX);
    case RV64_FUNCT3_LBU: /* LBU: movzx eax, byte ptr [rcx + rdx]. */
        return emit_u8(w, 0x0f) && emit_u8(w, 0xb6) &&
               emit_mmio_sib_rcx_rdx(w, HOST_REG_RAX);
    case RV64_FUNCT3_LHU: /* LHU: movzx eax, word ptr [rcx + rdx]. */
        return emit_u8(w, 0x0f) && emit_u8(w, 0xb7) &&
               emit_mmio_sib_rcx_rdx(w, HOST_REG_RAX);
    case RV64_FUNCT3_LWU: /* LWU: mov eax, dword ptr [rcx + rdx]. */
        return emit_u8(w, 0x8b) &&
               emit_mmio_sib_rcx_rdx(w, HOST_REG_RAX);
    default:
        return false;
    }
}

/* Emit the typed RV64 load from one exact host pointer held in RDX. */
static bool emit_direct_mmio_load_rax_from_rdx(
    rv64_jit_writer_t *w, uint32_t funct3)
{
    switch (funct3)
    {
    case RV64_FUNCT3_LB: /* movsx rax, byte ptr [rdx]. */
        return emit_u8(w, 0x48) && emit_u8(w, 0x0f) &&
               emit_u8(w, 0xbe) && emit_u8(w, 0x02);
    case RV64_FUNCT3_LH: /* movsx rax, word ptr [rdx]. */
        return emit_u8(w, 0x48) && emit_u8(w, 0x0f) &&
               emit_u8(w, 0xbf) && emit_u8(w, 0x02);
    case RV64_FUNCT3_LW: /* movsxd rax, dword ptr [rdx]. */
        return emit_u8(w, 0x48) && emit_u8(w, 0x63) &&
               emit_u8(w, 0x02);
    case RV64_FUNCT3_LD: /* mov rax, qword ptr [rdx]. */
        return emit_u8(w, 0x48) && emit_u8(w, 0x8b) &&
               emit_u8(w, 0x02);
    case RV64_FUNCT3_LBU: /* movzx eax, byte ptr [rdx]. */
        return emit_u8(w, 0x0f) && emit_u8(w, 0xb6) &&
               emit_u8(w, 0x02);
    case RV64_FUNCT3_LHU: /* movzx eax, word ptr [rdx]. */
        return emit_u8(w, 0x0f) && emit_u8(w, 0xb7) &&
               emit_u8(w, 0x02);
    case RV64_FUNCT3_LWU: /* mov eax, dword ptr [rdx]. */
        return emit_u8(w, 0x8b) && emit_u8(w, 0x02);
    default:
        return false;
    }
}

/* Calculate the address visible at block entry as an optimisation hint only. */
static uint64_t observed_bare_address(uint32_t rs1, int32_t imm)
{
    Assert(rs1 < 32u, "jit: invalid RV64 base register %u", rs1);

    const uint64_t base =
        rs1 == RV64_GPR_ZERO ? 0 : cpu.gpr[rs1]._64;
    return base + (uint64_t)(int64_t)imm;
}

/*
 * Return whether the block-entry address names a complete explicit direct-read
 * span. A positive answer controls route emission only; generated tag, PMEM,
 * classifier, and helper guards remain the authority at execution time.
 */
static bool direct_mmio_load_address_observed(
    uint64_t addr, uint32_t len, uint64_t *host_ptr)
{
    Assert(host_ptr != NULL, "jit: missing observed direct-read host output");

#if defined(CONFIG_DEVICE) && RV64_JIT_ENABLED
    const size_t direct_map_count = mmio_direct_read_map_count();

    for (size_t i = 0; i < direct_map_count; i++)
    {
        const IOMap *map = mmio_direct_read_map(i);

        if (!map_supports_direct_read(map, (int)len))
        {
            continue;
        }

        const uint64_t map_size =
            (uint64_t)map->high - (uint64_t)map->low + 1u;

        if (map_size >= len &&
            addr >= (uint64_t)map->low &&
            addr - (uint64_t)map->low <= map_size - len)
        {
            *host_ptr =
                (uint64_t)(uintptr_t)map->space +
                (addr - (uint64_t)map->low);
            return true;
        }
    }
#else
    (void)addr;
    (void)len;
    (void)host_ptr;
#endif

    return false;
}

/*
 * Probe one warmed exact route after alignment but before the PMEM proof.
 * A tag miss enters the unchanged PMEM proof and later classifier with the
 * original address still in RAX.
 */
static bool emit_direct_mmio_load_route_probe(
    rv64_jit_writer_t *w, rv64_jit_mmio_route_builder_t *routes,
    uint8_t site, uint32_t funct3, uint8_t **success_disp)
{
    uint8_t *tag_miss_disp = NULL;

    if (!emit_mmio_route_cmp_tag_rax(w, routes, site) ||
        !emit_jcc_rel32_placeholder(
            w, HOST_JCC_NE, &tag_miss_disp) ||
        !emit_mmio_route_load_host_rdx(w, routes, site) ||
        !emit_direct_mmio_load_rax_from_rdx(w, funct3) ||
        !emit_inc_jit_stat_counter_preserve_rax(
            w, &rv64_jit_stats.direct_mmio_load_route_hits) ||
        !emit_inc_jit_stat_counter_preserve_rax(
            w, &rv64_jit_stats.inline_direct_mmio_load_hits) ||
        !emit_jmp_rel32_placeholder(w, success_disp))
    {
        return false;
    }

    patch_rel32(tag_miss_disp, w->cur);

    return emit_inc_jit_stat_counter_preserve_rax(
        w, &rv64_jit_stats.direct_mmio_load_route_misses);
}

/*
 * Emit run-time range checks and direct host loads for explicitly approved
 * MMIO maps.  RAX keeps the original guest address across every miss so the
 * final fallback can enter the existing physical helper unchanged.
 */
static bool emit_inline_direct_mmio_loads(
    rv64_jit_writer_t *w, uint32_t funct3, uint32_t len,
    rv64_jit_mmio_route_builder_t *routes, uint8_t route_site,
    uint8_t *success_disps[RV64_JIT_DIRECT_MMIO_MAX_MAPS],
    uint32_t *success_count)
{
    *success_count = 0;

#if defined(CONFIG_DEVICE) && RV64_JIT_ENABLED
    const size_t direct_map_count = mmio_direct_read_map_count();

    for (size_t i = 0; i < direct_map_count; i++)
    {
        const IOMap *map = mmio_direct_read_map(i);
        uint8_t *next_map_disp = NULL;

        Assert(map != NULL, "jit: direct MMIO map index %zu disappeared", i);

        if (!map_supports_direct_read(map, (int)len))
        {
            continue;
        }

        if (*success_count >= RV64_JIT_DIRECT_MMIO_MAX_MAPS)
        {
            break;
        }

        const uint64_t map_size =
            (uint64_t)map->high - (uint64_t)map->low + 1u;
        Assert(map_size >= len,
               "jit: direct MMIO map %s is smaller than width %u",
               map->name, len);

        /*
         * Unsigned subtraction plus JA rejects addresses below `low`, above
         * `high`, and accesses whose final byte crosses the inclusive end.
         */
        if (!emit_mov_rdx_rax(w) ||
            !emit_movabs_rcx(w, (uint64_t)map->low) ||
            !emit_sub_rdx_rcx(w) ||
            !emit_movabs_rcx(w, map_size - len) ||
            !emit_cmp_rdx_rcx(w) ||
            !emit_jcc_rel32_placeholder(w, HOST_JCC_A, &next_map_disp))
        {
            return false;
        }

        if (route_site != RV64_JIT_MMIO_ROUTE_NO_SITE)
        {
            /*
             * Preserve the guest address in RDI while the typed load replaces
             * RAX with its architectural result.  Publish the host first and
             * the non-PMEM tag last, after the backing access commits.
             */
            if (!emit_movabs_rcx(
                    w, (uint64_t)(uintptr_t)map->space) ||
                !emit_add_rdx_rcx(w) ||
                !emit_mov_rdi_rax(w) ||
                !emit_direct_mmio_load_rax_from_rdx(w, funct3) ||
                !emit_mmio_route_store_host_rdx(
                    w, routes, route_site) ||
                !emit_mmio_route_store_tag_rdi(
                    w, routes, route_site) ||
                !emit_inc_jit_stat_counter_preserve_rax(
                    w, &rv64_jit_stats.direct_mmio_load_route_fills) ||
                !emit_inc_jit_stat_counter_preserve_rax(
                    w, &rv64_jit_stats.inline_direct_mmio_load_hits) ||
                !emit_jmp_rel32_placeholder(
                    w, &success_disps[*success_count]))
            {
                return false;
            }
        }
        else if (
            /*
             * Counter emission clobbers RAX, so count before the backing load
             * recreates the architectural result.
             */
            !emit_inc_jit_stat_counter(
                w, &rv64_jit_stats.inline_direct_mmio_load_hits) ||
            !emit_movabs_rcx(w, (uint64_t)(uintptr_t)map->space) ||
            !emit_direct_mmio_load_rax(w, funct3) ||
            !emit_jmp_rel32_placeholder(
                w, &success_disps[*success_count]))
        {
            return false;
        }

        (*success_count)++;
        patch_rel32(next_map_disp, w->cur);
    }
#else
    (void)w;
    (void)funct3;
    (void)len;
    (void)routes;
    (void)route_site;
    (void)success_disps;
#endif

    return true;
}

/* Emit one conservative fallback branch for an inline RV64 data-TLB guard. */
static bool emit_tlb_guard_slow_jcc(rv64_jit_writer_t *w,
                                    rv64_jit_tlb_guard_patch_t *patch,
                                    uint8_t jcc_opcode)
{
    Assert(patch->count < sizeof(patch->slow_disps) / sizeof(patch->slow_disps[0]),
           "jit: too many RV64 DTLB slow-path branches");
    return emit_jcc_rel32_placeholder(w, jcc_opcode,
                                      &patch->slow_disps[patch->count++]);
}

/* Patch every fallback branch emitted by an inline RV64 data-TLB guard. */
static void patch_tlb_guard(const rv64_jit_tlb_guard_patch_t *patch,
                            const uint8_t *slow_path)
{
    for (uint32_t i = 0; i < patch->count; i++)
    {
        patch_rel32(patch->slow_disps[i], slow_path);
    }
}

/* Emit the shared inline DTLB-hit proof for translated PMEM data accesses. */
static bool emit_inline_dtlb_lookup_to_pmem_offset(
    rv64_jit_writer_t *w, uint32_t len, uint32_t need_access,
    rv64_jit_tlb_guard_patch_t *patch)
{
    Assert(len >= 1 && len <= 8, "jit: unsupported RV64 DTLB width %u", len);

    const word_t satp = cpu.csr.satp;
    const uint32_t state = rv64_jit_data_tlb_state(MEM_TYPE_READ);
    const uint32_t valid_off = (uint32_t)offsetof(rv64_jit_data_tlb_entry_t, valid);
    const uint32_t satp_off = (uint32_t)offsetof(rv64_jit_data_tlb_entry_t, satp);
    const uint32_t vpn_off = (uint32_t)offsetof(rv64_jit_data_tlb_entry_t, vpn);
    const uint32_t state_off = (uint32_t)offsetof(rv64_jit_data_tlb_entry_t, state);
    const uint32_t access_off = (uint32_t)offsetof(rv64_jit_data_tlb_entry_t, access);
    const uint32_t pg_paddr_off =
        (uint32_t)offsetof(rv64_jit_data_tlb_entry_t, pg_paddr);
    const uint8_t entry_shift = RV64_JIT_DATA_TLB_ENTRY_SHIFT;

    /*
     * The generated proof mirrors jit_translate_pmem()'s TLB-hit half:
     *   vpn = vaddr >> PAGE_SHIFT
     *   entry = &rv64_jit_data_tlb[(vpn ^ mixed vpn/satp/state) & mask]
     *   entry byte offset = index << RV64_JIT_DATA_TLB_ENTRY_SHIFT
     *   require valid, exact satp, exact VPN, exact permission state and access
     *   require the byte range to stay inside the translated 4 KiB page
     *
     * RCX is reserved by the caller for the original guest address or store
     * value, so the guard uses RAX/RDX/R8 only.  Any failed guard branches to
     * the old helper path, which still owns faults, MMIO, and fresh TLB fills.
     *
     * The host Jcc constants are near-Jcc low bytes. HOST_JCC_A is an unsigned
     * range check for page-offset overflow.
     */
    if (!emit_mov_rdx_rax(w) ||
        !emit_shr_rdx_imm(w, PAGE_SHIFT) ||
        !emit_mov_r8_rdx(w) ||
        !emit_shr_r8_imm(w, RV64_JIT_DATA_TLB_VPN_MIX_SHIFT) ||
        !emit_xor_r8_rdx(w) ||
        !emit_movabs_rdx(w, satp ^
                                (satp >> RV64_JIT_DATA_TLB_SATP_MIX_SHIFT) ^
                                state) ||
        !emit_xor_r8_rdx(w) ||
        !emit_and_r8d_imm(w, RV64_JIT_DATA_TLB_SIZE - 1u) ||
        !emit_shl_r8_imm(w, entry_shift) ||
        !emit_movabs_rdx(w, (uint64_t)(uintptr_t)rv64_jit_data_tlb) ||
        !emit_add_r8_rdx(w) ||
        !emit_cmp_r8b_field_imm8(w, valid_off, 0) ||
        !emit_tlb_guard_slow_jcc(w, patch, HOST_JCC_E) ||
        !emit_movabs_rdx(w, satp) ||
        !emit_cmp_r8q_field_rdx(w, satp_off) ||
        !emit_tlb_guard_slow_jcc(w, patch, HOST_JCC_NE) ||
        !emit_mov_rdx_rax(w) ||
        !emit_shr_rdx_imm(w, PAGE_SHIFT) ||
        !emit_cmp_r8q_field_rdx(w, vpn_off) ||
        !emit_tlb_guard_slow_jcc(w, patch, HOST_JCC_NE) ||
        !emit_cmp_r8d_field_imm32(w, state_off, state) ||
        !emit_tlb_guard_slow_jcc(w, patch, HOST_JCC_NE) ||
        !emit_test_r8d_field_imm32(w, access_off, need_access) ||
        !emit_tlb_guard_slow_jcc(w, patch, HOST_JCC_E) ||
        !emit_and_rax_imm32(w, PAGE_MASK) ||
        !emit_cmp_rax_imm32(w, PAGE_SIZE - len) ||
        !emit_tlb_guard_slow_jcc(w, patch, HOST_JCC_A) ||
        !emit_mov_rdx_r8q_field(w, pg_paddr_off) ||
        !emit_or_rdx_rax(w) ||
        !emit_movabs_rax(w, (uint64_t)CONFIG_MBASE) ||
        !emit_sub_rdx_rax(w))
    {
        return false;
    }

    return true;
}

/* Emit an inline translated-PMEM load using a previously filled RV64 data TLB. */
static bool emit_inline_sv39_load_fast_path(
    rv64_jit_writer_t *w, uint32_t funct3, uint32_t len,
    rv64_jit_tlb_guard_patch_t *patch)
{
    /*
     * RCX must contain the original guest virtual address before entry.  The
     * common guard may clobber RAX while computing the page offset; on success
     * RDX is the byte offset from CONFIG_MBASE for emit_direct_pmem_load_rax().
     */
    return emit_inline_dtlb_lookup_to_pmem_offset(
               w, len, RV64_JIT_DATA_TLB_READ, patch) &&
           emit_inline_paged_load_hit_stats(w) &&
           emit_direct_pmem_load_rax(w, funct3);
}

/* Emit an inline PMEM store from RCX using the selected RV64 store width. */
static bool emit_direct_pmem_store_from_rcx(rv64_jit_writer_t *w, uint32_t len)
{
    /*
     * The low part of RCX naturally supplies SB/SH/SW truncation.  SD uses the
     * full 64-bit register.  The caller has already proved that RDX is an
     * in-PMEM byte offset and that the write is not to tracked source or page
     * table bytes.
     */
    switch (len)
    {
    case 1: /* mov byte ptr [r10 + rdx], cl. */
        return emit_u8(w, 0x41) && emit_u8(w, 0x88) &&
               emit_pmem_sib_r10_rdx(w, HOST_REG_RCX);
    case 2: /* mov word ptr [r10 + rdx], cx. */
        return emit_u8(w, 0x66) && emit_u8(w, 0x41) &&
               emit_u8(w, 0x89) &&
               emit_pmem_sib_r10_rdx(w, HOST_REG_RCX);
    case 4: /* mov dword ptr [r10 + rdx], ecx. */
        return emit_u8(w, 0x41) && emit_u8(w, 0x89) &&
               emit_pmem_sib_r10_rdx(w, HOST_REG_RCX);
    case 8: /* mov qword ptr [r10 + rdx], rcx. */
        return emit_u8(w, 0x49) && emit_u8(w, 0x89) &&
               emit_pmem_sib_r10_rdx(w, HOST_REG_RCX);
    default:
        return false;
    }
}

/* Emit one exact-width MMIO backing store from RCX to `[rdi + rdx]`. */
static bool emit_direct_mmio_store_from_rcx(rv64_jit_writer_t *w,
                                            uint32_t len)
{
    /*
     * RDI is the stable backing pointer for the contracted region and RDX is
     * the proved byte offset. The x86 store naturally truncates RCX for the
     * narrower RV64 store forms and performs one host access.
     */
    switch (len)
    {
    case 1: /* mov byte ptr [rdi + rdx], cl. */
        return emit_u8(w, 0x88) &&
               emit_mmio_sib_rdi_rdx(w, HOST_REG_RCX);
    case 2: /* mov word ptr [rdi + rdx], cx. */
        return emit_u8(w, 0x66) && emit_u8(w, 0x89) &&
               emit_mmio_sib_rdi_rdx(w, HOST_REG_RCX);
    case 4: /* mov dword ptr [rdi + rdx], ecx. */
        return emit_u8(w, 0x89) &&
               emit_mmio_sib_rdi_rdx(w, HOST_REG_RCX);
    case 8: /* mov qword ptr [rdi + rdx], rcx. */
        return emit_u8(w, 0x48) && emit_u8(w, 0x89) &&
               emit_mmio_sib_rdi_rdx(w, HOST_REG_RCX);
    default:
        return false;
    }
}

/* Emit one exact-width MMIO store from RCX to exact host pointer RDI. */
static bool emit_direct_mmio_store_from_rcx_to_rdi(
    rv64_jit_writer_t *w, uint32_t len)
{
    switch (len)
    {
    case 1: /* mov byte ptr [rdi], cl. */
        return emit_u8(w, 0x88) && emit_u8(w, 0x0f);
    case 2: /* mov word ptr [rdi], cx. */
        return emit_u8(w, 0x66) && emit_u8(w, 0x89) &&
               emit_u8(w, 0x0f);
    case 4: /* mov dword ptr [rdi], ecx. */
        return emit_u8(w, 0x89) && emit_u8(w, 0x0f);
    case 8: /* mov qword ptr [rdi], rcx. */
        return emit_u8(w, 0x48) && emit_u8(w, 0x89) &&
               emit_u8(w, 0x0f);
    default:
        return false;
    }
}

/*
 * Return whether the block-entry address names a complete explicit
 * direct-write span. This hint never bypasses the generated exact tag check.
 */
static bool direct_mmio_store_address_observed(
    uint64_t addr, uint32_t len, uint64_t *host_ptr)
{
    Assert(host_ptr != NULL, "jit: missing observed direct-write host output");

#if defined(CONFIG_DEVICE) && RV64_JIT_ENABLED
    const size_t direct_region_count =
        mmio_direct_write_region_count();

    for (size_t i = 0; i < direct_region_count; i++)
    {
        const IODirectWriteRegion *region =
            mmio_direct_write_region(i);

        if (!io_direct_write_region_supports(region, (int)len))
        {
            continue;
        }

        const uint64_t region_size =
            (uint64_t)region->high - (uint64_t)region->low + 1u;

        if (region_size >= len &&
            addr >= (uint64_t)region->low &&
            addr - (uint64_t)region->low <= region_size - len)
        {
            *host_ptr =
                (uint64_t)(uintptr_t)region->space +
                (addr - (uint64_t)region->low);
            return true;
        }
    }
#else
    (void)addr;
    (void)len;
    (void)host_ptr;
#endif

    return false;
}

/* Probe a warmed exact write route while preserving RAX on every miss. */
static bool emit_direct_mmio_store_route_probe(
    rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs,
    rv64_jit_mmio_route_builder_t *routes, uint8_t site,
    uint32_t rs2, uint32_t len, uint8_t **success_disp)
{
    uint8_t *tag_miss_disp = NULL;

    if (!emit_mmio_route_cmp_tag_rax(w, routes, site) ||
        !emit_jcc_rel32_placeholder(
            w, HOST_JCC_NE, &tag_miss_disp) ||
        !emit_mmio_route_load_host_rdi(w, routes, site) ||
        !jit_reg_read_rcx(w, regs, rs2) ||
        !emit_direct_mmio_store_from_rcx_to_rdi(w, len) ||
        !emit_inc_jit_stat_counter(
            w, &rv64_jit_stats.direct_mmio_store_route_hits) ||
        !emit_inc_jit_stat_counter(
            w, &rv64_jit_stats.inline_direct_mmio_store_hits) ||
        !emit_jmp_rel32_placeholder(w, success_disp))
    {
        return false;
    }

    patch_rel32(tag_miss_disp, w->cur);

    return emit_inc_jit_stat_counter_preserve_rax(
        w, &rv64_jit_stats.direct_mmio_store_route_misses);
}

/*
 * Emit run-time range checks and exact host stores for explicitly approved
 * MMIO subregions. RAX retains the original guest address across all misses so
 * the final fallback can enter the existing physical helper unchanged.
 */
static bool emit_inline_direct_mmio_stores(
    rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs,
    uint32_t rs2, uint32_t len,
    rv64_jit_mmio_route_builder_t *routes, uint8_t route_site,
    uint8_t *success_disps[RV64_JIT_DIRECT_MMIO_MAX_REGIONS],
    uint32_t *success_count)
{
    *success_count = 0;

#if defined(CONFIG_DEVICE) && RV64_JIT_ENABLED
    const size_t direct_region_count = mmio_direct_write_region_count();

    for (size_t i = 0; i < direct_region_count; i++)
    {
        const IODirectWriteRegion *region =
            mmio_direct_write_region(i);
        uint8_t *next_region_disp = NULL;

        Assert(region != NULL,
               "jit: direct MMIO write region index %zu disappeared", i);

        if (!io_direct_write_region_supports(region, (int)len))
        {
            continue;
        }

        if (*success_count >= RV64_JIT_DIRECT_MMIO_MAX_REGIONS)
        {
            break;
        }

        const uint64_t region_size =
            (uint64_t)region->high - (uint64_t)region->low + 1u;
        Assert(region_size >= len,
               "jit: direct MMIO write region %s is smaller than width %u",
               region->map_name, len);

        /*
         * Unsigned subtraction plus JA rejects addresses below `low`, above
         * `high`, and accesses whose final byte crosses the inclusive end.
         * Only the matched arm materialises rs2 into RCX and changes RAX for
         * statistics, so every miss preserves the helper ABI.
         */
        if (!emit_mov_rdx_rax(w) ||
            !emit_movabs_rcx(w, (uint64_t)region->low) ||
            !emit_sub_rdx_rcx(w) ||
            !emit_movabs_rcx(w, region_size - len) ||
            !emit_cmp_rdx_rcx(w) ||
            !emit_jcc_rel32_placeholder(
                w, HOST_JCC_A, &next_region_disp))
        {
            return false;
        }

        if (route_site != RV64_JIT_MMIO_ROUTE_NO_SITE)
        {
            if (!jit_reg_read_rcx(w, regs, rs2) ||
                !emit_movabs_rdi(
                    w, (uint64_t)(uintptr_t)region->space) ||
                !emit_add_rdi_rdx(w) ||
                !emit_direct_mmio_store_from_rcx_to_rdi(w, len) ||
                !emit_mmio_route_store_host_rdi(
                    w, routes, route_site) ||
                !emit_mmio_route_store_tag_rax(
                    w, routes, route_site) ||
                !emit_inc_jit_stat_counter(
                    w, &rv64_jit_stats.direct_mmio_store_route_fills) ||
                !emit_inc_jit_stat_counter(
                    w, &rv64_jit_stats.inline_direct_mmio_store_hits) ||
                !emit_jmp_rel32_placeholder(
                    w, &success_disps[*success_count]))
            {
                return false;
            }
        }
        else if (!jit_reg_read_rcx(w, regs, rs2) ||
                 !emit_movabs_rdi(
                     w, (uint64_t)(uintptr_t)region->space) ||
                 !emit_direct_mmio_store_from_rcx(w, len) ||
                 !emit_inc_jit_stat_counter(
                     w, &rv64_jit_stats.inline_direct_mmio_store_hits) ||
                 !emit_jmp_rel32_placeholder(
                     w, &success_disps[*success_count]))
        {
            return false;
        }

        (*success_count)++;
        patch_rel32(next_region_disp, w->cur);
    }
#else
    (void)w;
    (void)regs;
    (void)rs2;
    (void)len;
    (void)routes;
    (void)route_site;
    (void)success_disps;
#endif

    return true;
}

/* Emit guards that keep inline stores away from compiled source chunks. */
static bool emit_guard_store_not_compiled_source(
    rv64_jit_writer_t *w, uint32_t len, uint8_t **cross_chunk_disp,
    uint8_t **source_chunk_disp)
{
    Assert(len >= 1 && len <= 8, "jit: unsupported RV64 store width %u", len);

    /*
     * Direct inline stores only continue when they stay within one source-ref
     * chunk and that chunk currently has no compiled block references.  The
     * helper path performs exact invalidation and exits for every ambiguous
     * store, preserving self-modifying-code ordering.
     */
    return emit_mov_r8d_edx(w) &&
           emit_and_r8d_imm(w, RV64_JIT_SOURCE_CHUNK_MASK) &&
           emit_cmp_r8d_imm(w, RV64_JIT_SOURCE_CHUNK_SIZE - len) &&
           emit_jcc_rel32_placeholder(w, HOST_JCC_A, cross_chunk_disp) &&
           emit_mov_r8d_edx(w) &&
           emit_shr_r8d_imm(w, RV64_JIT_SOURCE_CHUNK_SHIFT) &&
           emit_movabs_rax(w, (uint64_t)(uintptr_t)rv64_jit_source_chunk_refs) &&
           emit_cmp_u32_ref_zero_rax_r8(w) &&
           emit_jcc_rel32_placeholder(w, HOST_JCC_NE, source_chunk_disp);
}

/* Emit a guard that keeps inline stores away from cached page-table pages. */
static bool emit_guard_store_not_translation_dependency(
    rv64_jit_writer_t *w, uint8_t **data_page_table_disp,
    uint8_t **ifetch_page_table_disp)
{
    /*
     * RDX is the PMEM byte offset.  A non-zero page-table refcount means a
     * direct write could stale a data-TLB entry or an instruction-fetch mapping,
     * so the helper must perform the store and run the invalidation hook.
     */
    return emit_mov_r8d_edx(w) &&
           emit_shr_r8d_imm(w, PAGE_SHIFT) &&
           emit_movabs_rax(w, (uint64_t)(uintptr_t)rv64_jit_data_tlb_pt_page_refs) &&
           emit_cmp_u16_ref_zero_rax_r8(w) &&
           emit_jcc_rel32_placeholder(w, HOST_JCC_NE, data_page_table_disp) &&
           emit_movabs_rax(w, (uint64_t)(uintptr_t)rv64_jit_ifetch_pt_page_refs) &&
           emit_cmp_u32_ref_zero_rax_r8(w) &&
           emit_jcc_rel32_placeholder(w, HOST_JCC_NE, ifetch_page_table_disp);
}

/* Emit an inline translated-PMEM store address proof through the RV64 data TLB. */
static bool emit_inline_sv39_store_address(
    rv64_jit_writer_t *w, uint32_t len,
    rv64_jit_tlb_guard_patch_t *patch)
{
    /*
     * RDI must hold the original guest virtual address and RCX the store value.
     * The common guard may clobber RAX while proving the page offset.  On
     * success RDX is the byte offset from CONFIG_MBASE for the direct store.
     */
    return emit_inline_dtlb_lookup_to_pmem_offset(
        w, len, RV64_JIT_DATA_TLB_WRITE, patch);
}

/* Emit the common low-bit alignment guard for multi-byte RV64 memory ops. */
static bool emit_alignment_guard_al(rv64_jit_writer_t *w, uint32_t len,
                                    uint8_t **slow_disp)
{
    if (len <= 1)
    {
        return true;
    }

    return emit_test_al_imm8(w, (uint8_t)(len - 1u)) &&
           emit_jcc_rel32_placeholder(w, HOST_JCC_NE, slow_disp);
}

/* Emit the Bare-mode PMEM range proof shared by direct loads and stores. */
static bool emit_guard_bare_address_in_pmem(rv64_jit_writer_t *w, uint32_t len,
                                            uint8_t **slow_disp)
{
    /*
     * The unsigned JA branch catches underflow before CONFIG_MBASE, ordinary
     * out-of-range addresses, and byte ranges that would run past PMEM.
     */
    return emit_movabs_rcx(w, (uint64_t)CONFIG_MBASE) &&
           emit_sub_rdx_rcx(w) &&
           emit_movabs_rcx(w, (uint64_t)CONFIG_MSIZE - len) &&
           emit_cmp_rdx_rcx(w) &&
           emit_jcc_rel32_placeholder(w, HOST_JCC_A, slow_disp);
}

/* Emit one helper-backed RV64 load for non-Bare address translation modes. */
static bool emit_paged_load_instr(rv64_jit_writer_t *w,
                                  rv64_jit_reg_cache_t *regs,
                                  uint32_t rd, uint32_t rs1,
                                  uint32_t funct3,
                                  int32_t imm, uint32_t len,
                                  uintptr_t helper, vaddr_t pc,
                                  uint32_t completed_count)
{
    uint8_t *align_slow_disp = NULL;
    uint8_t *fast_done_disp = NULL;
    uint8_t *done_disp = NULL;
    rv64_jit_tlb_guard_patch_t tlb_guard = {0};
    rv64_jit_reg_cache_t side_exit_regs;

    if (!jit_reg_read_rax(w, regs, rs1) ||
        !emit_add_rax_imm32(w, imm))
    {
        return false;
    }

    side_exit_regs = *regs;

    if (!emit_alignment_guard_al(w, len, &align_slow_disp))
    {
        return false;
    }

    /*
     * The inline path consumes a TLB hit without leaving generated code.  RCX
     * preserves the original guest address until every fallback branch has
     * reached the helper path; the helper remains the only place that fills the
     * TLB or reports faults/MMIO.
     */
    if (!emit_mov_rcx_rax(w) ||
        !emit_inline_sv39_load_fast_path(w, funct3, len, &tlb_guard))
    {
        return false;
    }

    if (!emit_jmp_rel32_placeholder(w, &fast_done_disp))
    {
        return false;
    }

    const uint8_t *slow_path = w->cur;
    patch_tlb_guard(&tlb_guard, slow_path);

    if (!emit_mov_rax_rcx(w) ||
        !jit_reg_emit_flush_all_dirty(w, regs) ||
        !emit_mov_rdi_rax(w) ||
        !emit_store_pc_imm(w, pc) ||
        !emit_call_abs(w, helper) ||
        !emit_load_jit_bases(w))
    {
        return false;
    }

    patch_rel32(fast_done_disp, w->cur);

    if (!jit_reg_write_rax(w, regs, rd))
    {
        return false;
    }

    if (align_slow_disp != NULL)
    {
        /*
         * The alignment side exit is only valid before the load changes RD.
         * Fast and helper-backed success must skip it; otherwise an instruction
         * such as `ld a4, imm(a4)` would re-enter the interpreter with the
         * loaded value already in the base register.
         */
        if (!emit_jmp_rel32_placeholder(w, &done_disp))
        {
            return false;
        }

        patch_rel32(align_slow_disp, w->cur);

        if (!emit_interpreter_side_exit(w, &side_exit_regs, pc, completed_count,
                                        RV64_JIT_SIDE_EXIT_LOAD_GUARD))
        {
            return false;
        }

        patch_rel32(done_disp, w->cur);
    }

    JIT_STAT_INC(emitted_sites.native_loads);
    JIT_STAT_INC(emitted_sites.native_paged_loads);
    JIT_STAT_INC(emitted_sites.inline_paged_loads);
    return true;
}

/* Emit one guarded bare-mode RV64 load that falls back before unsafe accesses. */
bool rv64_jit_emit_load_instr(rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs,
                              uint32_t instr, vaddr_t pc,
                              uint32_t completed_count,
                              rv64_jit_mmio_route_builder_t *mmio_routes)
{
    const uint32_t rd = rv64_instr_rd(instr);
    const uint32_t funct3 = rv64_instr_funct3(instr);
    const uint32_t rs1 = rv64_instr_rs1(instr);
    const int32_t imm = (int32_t)imm_i(instr);
    uint32_t len = 0;
    uintptr_t helper = 0;
    uintptr_t bare_helper = 0;
    uint8_t *align_slow_disp = NULL;
    uint8_t *range_slow_disp = NULL;
    uint8_t *fast_done_disp = NULL;
    uint8_t *cached_mmio_done_disp = NULL;
    uint8_t *direct_mmio_done_disps[RV64_JIT_DIRECT_MMIO_MAX_MAPS] = {0};
    uint32_t direct_mmio_done_count = 0;
    uint8_t *done_disp = NULL;
    rv64_jit_reg_cache_t side_exit_regs;

    switch (funct3)
    {
    case RV64_FUNCT3_LB:
        helper = (uintptr_t)rv64_jit_load_i8;
        bare_helper = (uintptr_t)rv64_jit_load_bare_i8;
        len = 1;
        break;
    case RV64_FUNCT3_LBU:
        helper = (uintptr_t)rv64_jit_load_u8;
        bare_helper = (uintptr_t)rv64_jit_load_bare_u8;
        len = 1;
        break;
    case RV64_FUNCT3_LH:
        helper = (uintptr_t)rv64_jit_load_i16;
        bare_helper = (uintptr_t)rv64_jit_load_bare_i16;
        len = 2;
        break;
    case RV64_FUNCT3_LHU:
        helper = (uintptr_t)rv64_jit_load_u16;
        bare_helper = (uintptr_t)rv64_jit_load_bare_u16;
        len = 2;
        break;
    case RV64_FUNCT3_LW:
        helper = (uintptr_t)rv64_jit_load_i32;
        bare_helper = (uintptr_t)rv64_jit_load_bare_i32;
        len = 4;
        break;
    case RV64_FUNCT3_LWU:
        helper = (uintptr_t)rv64_jit_load_u32;
        bare_helper = (uintptr_t)rv64_jit_load_bare_u32;
        len = 4;
        break;
    case RV64_FUNCT3_LD:
        helper = (uintptr_t)rv64_jit_load_u64;
        bare_helper = (uintptr_t)rv64_jit_load_bare_u64;
        len = 8;
        break;
    default:
        return false;
    }

    /*
     * The direct PMEM tier is intentionally Bare-mode only.  Non-Bare modes use
     * helper calls below, because Sv39 permission and effective-privilege checks
     * are subtler than this physical-address range proof.
     */
    if ((cpu.csr.satp >> RV64_JIT_SATP_MODE_SHIFT) !=
        RV64_JIT_SATP_MODE_BARE)
    {
        return emit_paged_load_instr(w, regs, rd, rs1, funct3, imm, len, helper, pc,
                                     completed_count);
    }

    if (!jit_reg_read_rax(w, regs, rs1) ||
        !emit_add_rax_imm32(w, imm))
    {
        return false;
    }

    side_exit_regs = *regs;

    if (!emit_alignment_guard_al(w, len, &align_slow_disp))
    {
        return false;
    }

    uint8_t route_site = RV64_JIT_MMIO_ROUTE_NO_SITE;
    const uint64_t observed_addr = observed_bare_address(rs1, imm);
    uint64_t observed_host = 0;

    if (direct_mmio_load_address_observed(
            observed_addr, len, &observed_host))
    {
        route_site = mmio_route_reserve_site(
            mmio_routes, observed_addr, observed_host);
    }

    /*
     * Only a site observed as direct MMIO at block entry gets this pre-PMEM
     * specialisation. A changed tag falls through to the byte-for-byte PMEM
     * proof and complete classifier/helper path.
     */
    if (route_site != RV64_JIT_MMIO_ROUTE_NO_SITE &&
        !emit_direct_mmio_load_route_probe(
            w, mmio_routes, route_site, funct3,
            &cached_mmio_done_disp))
    {
        return false;
    }

    if (!emit_mov_rdx_rax(w) ||
        !emit_guard_bare_address_in_pmem(w, len, &range_slow_disp) ||
        !emit_direct_pmem_load_rax(w, funct3) ||
        !emit_jmp_rel32_placeholder(w, &fast_done_disp))
    {
        return false;
    }

    patch_rel32(range_slow_disp, w->cur);

    if (!emit_inline_direct_mmio_loads(
            w, funct3, len, mmio_routes, route_site,
            direct_mmio_done_disps,
            &direct_mmio_done_count))
    {
        return false;
    }

    /*
     * An aligned out-of-PMEM Bare-mode load may be MMIO. Enter at the physical
     * helper because the emitted guards already completed the alignment, mode,
     * and PMEM-range checks. paddr_read() still preserves device callbacks and
     * invalid-address behaviour before native execution continues.
     */
    if (!jit_reg_emit_flush_all_dirty(w, regs) ||
        !emit_mov_rdi_rax(w) ||
        !emit_store_pc_imm(w, pc) ||
        !emit_call_abs(w, bare_helper) ||
        !emit_load_jit_bases(w))
    {
        return false;
    }

    /*
     * Both successful arms still describe the pre-load register mapping here.
     * Allocate and write RD only after they merge, so an LRU spill is emitted on
     * both paths instead of being hidden in the skipped direct-PMEM arm.
     */
    patch_rel32(fast_done_disp, w->cur);
    if (cached_mmio_done_disp != NULL)
    {
        patch_rel32(cached_mmio_done_disp, w->cur);
    }
    for (uint32_t i = 0; i < direct_mmio_done_count; i++)
    {
        patch_rel32(direct_mmio_done_disps[i], w->cur);
    }

    if (!jit_reg_write_rax(w, regs, rd))
    {
        return false;
    }

    if (align_slow_disp != NULL)
    {
        /*
         * The alignment side exit is valid only before the common RD write.
         * Successful direct and helper-backed loads must skip the interpreter.
         */
        if (!emit_jmp_rel32_placeholder(w, &done_disp))
        {
            return false;
        }

        patch_rel32(align_slow_disp, w->cur);

        if (!emit_interpreter_side_exit(w, &side_exit_regs, pc, completed_count,
                                        RV64_JIT_SIDE_EXIT_LOAD_GUARD))
        {
            return false;
        }

        patch_rel32(done_disp, w->cur);
    }

    JIT_STAT_INC(emitted_sites.native_loads);
    if (direct_mmio_done_count != 0u)
    {
        JIT_STAT_INC(emitted_sites.direct_mmio_load_sites);
    }
    return true;
}

/* Emit one helper-backed RV64 store for non-Bare address translation modes. */
static bool emit_paged_store_instr(rv64_jit_writer_t *w,
                                   rv64_jit_reg_cache_t *regs,
                                   uint32_t rs1, uint32_t rs2,
                                   int32_t imm, uint32_t len,
                                   vaddr_t pc, vaddr_t next_pc,
                                   uint32_t completed_count)
{
    uint8_t *align_slow_disp = NULL;
    uint8_t *cross_chunk_disp = NULL;
    uint8_t *source_chunk_disp = NULL;
    uint8_t *data_page_table_disp = NULL;
    uint8_t *ifetch_page_table_disp = NULL;
    uint8_t *fast_done_disp = NULL;
    uint8_t *helper_exit_disp = NULL;
    uint8_t *helper_done_disp = NULL;
    uint8_t *done_disp = NULL;
    rv64_jit_tlb_guard_patch_t tlb_guard = {0};
    rv64_jit_reg_cache_t side_exit_regs;

    if (!jit_reg_read_rax(w, regs, rs1) ||
        !emit_add_rax_imm32(w, imm))
    {
        return false;
    }

    side_exit_regs = *regs;

    if (!emit_alignment_guard_al(w, len, &align_slow_disp))
    {
        return false;
    }

    /*
     * A DTLB-hit store can commit inline only when the final physical bytes are
     * ordinary PMEM and are not tracked as compiled source or page-table pages.
     * A miss refills through the helper. It may rejoin only after proving the
     * completed PMEM store touched no source or page-table dependency;
     * sensitive writes, MMIO, and faults retain the dispatcher boundary.
     */
    if (!emit_mov_rdi_rax(w) ||
        !jit_reg_read_rcx(w, regs, rs2) ||
        !emit_inline_sv39_store_address(w, len, &tlb_guard) ||
        !emit_guard_store_not_compiled_source(w, len, &cross_chunk_disp,
                                              &source_chunk_disp) ||
        !emit_guard_store_not_translation_dependency(
            w, &data_page_table_disp, &ifetch_page_table_disp) ||
        !emit_inline_paged_store_hit_stats(w) ||
        !emit_direct_pmem_store_from_rcx(w, len) ||
        !emit_jmp_rel32_placeholder(w, &fast_done_disp))
    {
        return false;
    }

    const uint8_t *slow_path = w->cur;
    patch_tlb_guard(&tlb_guard, slow_path);
    patch_rel32(cross_chunk_disp, slow_path);
    patch_rel32(source_chunk_disp, slow_path);
    patch_rel32(data_page_table_disp, slow_path);
    patch_rel32(ifetch_page_table_disp, slow_path);

    if (!jit_reg_emit_flush_all_dirty(w, regs) ||
        !emit_mov_rdx_rcx(w) ||
        !emit_mov_esi_imm32(w, len) ||
        !emit_store_pc_imm(w, pc) ||
        !emit_call_abs(
            w, (uintptr_t)rv64_jit_store_vaddr_continue) ||
        !emit_load_jit_bases(w) ||
        !emit_test_eax_eax(w) ||
        !emit_jcc_rel32_placeholder(
            w, HOST_JCC_E, &helper_exit_disp) ||
        !emit_jmp_rel32_placeholder(w, &helper_done_disp))
    {
        return false;
    }

    patch_rel32(helper_exit_disp, w->cur);

    if (!emit_store_pc_imm(w, next_pc) ||
        !emit_inc_jit_stat_counter(w,
                                   &rv64_jit_stats.side_exit_by_reason[RV64_JIT_SIDE_EXIT_PAGED_STORE_HELPER]) ||
        !emit_return_total_retired(w, completed_count + 1u))
    {
        return false;
    }

    patch_rel32(fast_done_disp, w->cur);
    patch_rel32(helper_done_disp, w->cur);

    if (align_slow_disp != NULL)
    {
        /*
         * Only the pre-store alignment guard may enter this side exit. Both a
         * successful inline store and a safe helper refill continue in native
         * code; a sensitive or non-PMEM helper store has already returned after
         * updating cpu.pc.
         */
        if (!emit_jmp_rel32_placeholder(w, &done_disp))
        {
            return false;
        }

        patch_rel32(align_slow_disp, w->cur);

        if (!emit_interpreter_side_exit(w, &side_exit_regs, pc, completed_count,
                                        RV64_JIT_SIDE_EXIT_STORE_GUARD))
        {
            return false;
        }

        patch_rel32(done_disp, w->cur);
    }

    JIT_STAT_INC(emitted_sites.native_stores);
    JIT_STAT_INC(emitted_sites.native_paged_stores);
    JIT_STAT_INC(emitted_sites.inline_paged_stores);
    return true;
}

/* Emit one guarded bare-mode RV64 store that normally commits inline. */
bool rv64_jit_emit_store_instr(rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs,
                               uint32_t instr, vaddr_t pc,
                               vaddr_t next_pc, uint32_t completed_count,
                               rv64_jit_mmio_route_builder_t *mmio_routes)
{
    const uint32_t funct3 = rv64_instr_funct3(instr);
    const uint32_t rs1 = rv64_instr_rs1(instr);
    const uint32_t rs2 = rv64_instr_rs2(instr);
    const int32_t imm = (int32_t)imm_s(instr);
    uint32_t len = 0;
    uint8_t *align_slow_disp = NULL;
    uint8_t *range_slow_disp = NULL;
    uint8_t *cross_chunk_disp = NULL;
    uint8_t *source_chunk_disp = NULL;
    uint8_t *data_page_table_disp = NULL;
    uint8_t *ifetch_page_table_disp = NULL;
    uint8_t *exit_disp = NULL;
    uint8_t *bare_helper_exit_disp = NULL;
    uint8_t *direct_done_disp = NULL;
    uint8_t *cached_mmio_done_disp = NULL;
    uint8_t *continue_disp = NULL;
    uint8_t *bare_helper_continue_disp = NULL;
    uint8_t *direct_mmio_done_disps[RV64_JIT_DIRECT_MMIO_MAX_REGIONS] = {0};
    uint32_t direct_mmio_done_count = 0;
    rv64_jit_reg_cache_t side_exit_regs;

    switch (funct3)
    {
    case RV64_FUNCT3_SB:
        len = 1;
        break;
    case RV64_FUNCT3_SH:
        len = 2;
        break;
    case RV64_FUNCT3_SW:
        len = 4;
        break;
    case RV64_FUNCT3_SD:
        len = 8;
        break;
    default:
        return false;
    }

    if ((cpu.csr.satp >> RV64_JIT_SATP_MODE_SHIFT) !=
        RV64_JIT_SATP_MODE_BARE)
    {
        return emit_paged_store_instr(w, regs, rs1, rs2, imm, len, pc, next_pc,
                                      completed_count);
    }

    if (!jit_reg_read_rax(w, regs, rs1) ||
        !emit_add_rax_imm32(w, imm))
    {
        return false;
    }

    /*
     * Both sides of the PMEM range guard must agree on which host register
     * contains rs2. Materialise it before the branch; otherwise the compiler
     * could record a load emitted only on the PMEM path and the MMIO path would
     * read an uninitialised cache slot.
     */
    if (rs2 != RV64_GPR_ZERO &&
        jit_reg_loaded_slot(w, regs, rs2) == NULL)
    {
        return false;
    }

    side_exit_regs = *regs;

    if (!emit_alignment_guard_al(w, len, &align_slow_disp))
    {
        return false;
    }

    uint8_t route_site = RV64_JIT_MMIO_ROUTE_NO_SITE;
    const uint64_t observed_addr = observed_bare_address(rs1, imm);
    uint64_t observed_host = 0;

    if (direct_mmio_store_address_observed(
            observed_addr, len, &observed_host))
    {
        route_site = mmio_route_reserve_site(
            mmio_routes, observed_addr, observed_host);
    }

    /*
     * A warmed exact MMIO store bypasses the PMEM proof. Any changed address
     * retains RAX and executes the complete established PMEM or MMIO path.
     */
    if (route_site != RV64_JIT_MMIO_ROUTE_NO_SITE &&
        !emit_direct_mmio_store_route_probe(
            w, regs, mmio_routes, route_site, rs2, len,
            &cached_mmio_done_disp))
    {
        return false;
    }

    if (!emit_mov_rdx_rax(w) ||
        !emit_guard_bare_address_in_pmem(w, len, &range_slow_disp))
    {
        return false;
    }

    /*
     * Ordinary PMEM stores can write directly and keep executing.  Stores that
     * cross a source-tracking chunk, overlap compiled source, or touch cached
     * page-table bytes use the helper so exact invalidation happens after the
     * write and before any later translated fetch.
     */
    if (!jit_reg_read_rcx(w, regs, rs2) ||
        !emit_guard_store_not_compiled_source(w, len, &cross_chunk_disp,
                                              &source_chunk_disp) ||
        !emit_guard_store_not_translation_dependency(
            w, &data_page_table_disp, &ifetch_page_table_disp) ||
        !emit_direct_pmem_store_from_rcx(w, len) ||
        !emit_jmp_rel32_placeholder(w, &direct_done_disp))
    {
        return false;
    }

    const uint8_t *helper_path = w->cur;
    patch_rel32(cross_chunk_disp, helper_path);
    patch_rel32(source_chunk_disp, helper_path);
    patch_rel32(data_page_table_disp, helper_path);
    patch_rel32(ifetch_page_table_disp, helper_path);

    /*
     * The helper's EAX result follows the named cross-unit store ABI.  `test
     * eax,eax` therefore sends RV64_JIT_STORE_MUST_EXIT (zero) to the side exit
     * and lets RV64_JIT_STORE_MAY_CONTINUE (one) rejoin native execution.
     */
    if (!jit_reg_emit_flush_all_dirty(w, regs) ||
        !emit_mov_rdi_rdx(w) ||
        !emit_movabs_rax(w, (uint64_t)CONFIG_MBASE) ||
        !emit_add_rdi_rax(w) ||
        !emit_mov_rdx_rcx(w) ||
        !emit_mov_esi_imm32(w, len) ||
        !emit_store_pc_imm(w, pc) ||
        !emit_call_abs(w, (uintptr_t)rv64_jit_store_pmem_continue) ||
        !emit_load_jit_bases(w) ||
        !emit_test_eax_eax(w) ||
        !emit_jcc_rel32_placeholder(w, HOST_JCC_E, &exit_disp) ||
        !emit_jmp_rel32_placeholder(w, &continue_disp))
    {
        return false;
    }

    patch_rel32(exit_disp, w->cur);

    if (!emit_load_cpu_base(w) ||
        !emit_store_pc_imm(w, next_pc) ||
        !emit_inc_jit_stat_counter(w,
                                   &rv64_jit_stats.side_exit_by_reason[RV64_JIT_SIDE_EXIT_STORE_SOURCE]) ||
        !emit_return_total_retired(w, completed_count + 1u))
    {
        return false;
    }

    if (align_slow_disp != NULL)
    {
        patch_rel32(align_slow_disp, w->cur);

        if (!emit_interpreter_side_exit(
                w, &side_exit_regs, pc, completed_count,
                RV64_JIT_SIDE_EXIT_STORE_GUARD))
        {
            return false;
        }
    }

    patch_rel32(range_slow_disp, w->cur);

    /*
     * An explicit direct-write subregion can commit without exposing dirty GPRs
     * to C. Every unmatched address and width retains RAX and falls through to
     * the existing exact-once helper path.
     */
    if (!emit_inline_direct_mmio_stores(
            w, regs, rs2, len, mmio_routes, route_site,
            direct_mmio_done_disps,
            &direct_mmio_done_count))
    {
        return false;
    }

    /*
     * The range guard leaves the original physical address in RAX. Commit the
     * MMIO access exactly once through paddr_write(), then either rejoin the
     * continuing native path or retire the completed store at a safe boundary.
     */
    if (!jit_reg_read_rcx(w, regs, rs2) ||
        !jit_reg_emit_flush_all_dirty(w, regs) ||
        !emit_mov_rdi_rax(w) ||
        !emit_mov_rdx_rcx(w) ||
        !emit_mov_esi_imm32(w, len) ||
        !emit_store_pc_imm(w, pc) ||
        !emit_call_abs(w, (uintptr_t)rv64_jit_store_bare_continue) ||
        !emit_load_jit_bases(w) ||
        !emit_test_eax_eax(w) ||
        !emit_jcc_rel32_placeholder(w, HOST_JCC_E,
                                    &bare_helper_exit_disp) ||
        !emit_jmp_rel32_placeholder(w, &bare_helper_continue_disp))
    {
        return false;
    }

    patch_rel32(bare_helper_exit_disp, w->cur);

    if (!emit_store_pc_imm(w, next_pc) ||
        !emit_inc_jit_stat_counter(w,
                                   &rv64_jit_stats.side_exit_by_reason[RV64_JIT_SIDE_EXIT_STORE_HELPER]) ||
        !emit_return_total_retired(w, completed_count + 1u))
    {
        return false;
    }

    patch_rel32(direct_done_disp, w->cur);
    patch_rel32(continue_disp, w->cur);
    patch_rel32(bare_helper_continue_disp, w->cur);
    if (cached_mmio_done_disp != NULL)
    {
        patch_rel32(cached_mmio_done_disp, w->cur);
    }

    for (uint32_t i = 0; i < direct_mmio_done_count; i++)
    {
        patch_rel32(direct_mmio_done_disps[i], w->cur);
    }

    JIT_STAT_INC(emitted_sites.native_stores);
    JIT_STAT_INC(emitted_sites.native_store_continuations);

    if (direct_mmio_done_count != 0u)
    {
        JIT_STAT_INC(emitted_sites.direct_mmio_store_sites);
    }

    return true;
}

/*
 * Complete one native M instruction after its architectural result is safe in
 * the guest-register cache. Statistics builds count the generated path at run
 * time; ordinary builds emit no extra instruction. Keeping the increment last
 * also permits it to clobber RAX without damaging an aliased destination.
 */
static bool emit_finish_native_m(rv64_jit_writer_t *w, rv64_jit_m_op_t op)
{
    if (!emit_inc_jit_stat_counter(
            w, &rv64_jit_stats.native_m_executions[op]))
    {
        return false;
    }

    JIT_STAT_INC(emitted_sites.native_m_ops);
    return true;
}

/* Emit MULH, MULHSU, or MULHU and copy the high product half to RAX. */
static bool emit_rv64_mul_high(rv64_jit_writer_t *w,
                               rv64_jit_reg_cache_t *regs,
                               uint32_t rd, rv64_jit_m_op_t op)
{
    switch (op)
    {
    case RV64_JIT_M_OP_MULH:
        if (!emit_imul_rcx(w))
        {
            return false;
        }
        break;
    case RV64_JIT_M_OP_MULHU:
        if (!emit_mul_rcx(w))
        {
            return false;
        }
        break;
    case RV64_JIT_M_OP_MULHSU:
        /*
         * QEMU uses the identity
         *
         *   signed_high(lhs, unsigned rhs)
         *       = unsigned_high(lhs, rhs)
         *         - (((int64_t)lhs >> 63) & rhs).
         *
         * RDI is a per-instruction scratch register. R8 remains untouched so a
         * proven helper-free loop can keep its seventh cached guest register.
         */
        if (!emit_mov_rdi_rax(w) ||
            !emit_sar_rdi_63(w) ||
            !emit_and_rdi_rcx(w) ||
            !emit_mul_rcx(w) ||
            !emit_sub_rdx_rdi(w))
        {
            return false;
        }
        break;
    default:
        return false;
    }

    return emit_mov_rax_rdx(w) &&
           jit_reg_write_rax(w, regs, rd) &&
           emit_finish_native_m(w, op);
}

/* Emit full-width DIV/DIVU/REM/REMU with RISC-V's non-trapping edge results. */
static bool emit_rv64_divrem(rv64_jit_writer_t *w,
                             rv64_jit_reg_cache_t *regs,
                             uint32_t rd, bool is_signed,
                             bool want_remainder, rv64_jit_m_op_t op)
{
    uint8_t *zero_disp = NULL;
    uint8_t *normal_done_disp = NULL;

    if (!emit_test_rcx_rcx(w) ||
        !emit_jcc_rel32_placeholder(w, HOST_JCC_E, &zero_disp))
    {
        return false;
    }

    if (!is_signed)
    {
        if (!emit_zero_rdx(w) ||
            !emit_div_rcx(w) ||
            (want_remainder && !emit_mov_rax_rdx(w)) ||
            !emit_jmp_rel32_placeholder(w, &normal_done_disp))
        {
            return false;
        }

        patch_rel32(zero_disp, w->cur);

        /*
         * REMU by zero keeps the untouched dividend already in RAX. DIVU by
         * zero materialises the architecturally defined all-ones quotient.
         */
        if ((!want_remainder && !emit_movabs_rax(w, UINT64_MAX)))
        {
            return false;
        }

        patch_rel32(normal_done_disp, w->cur);
        return jit_reg_write_rax(w, regs, rd) &&
               emit_finish_native_m(w, op);
    }

    uint8_t *normal_disp = NULL;
    uint8_t *overflow_disp = NULL;
    uint8_t *zero_done_disp = NULL;

    /*
     * IDIV has two host exceptions which are ordinary RISC-V results. Compare
     * against a full-width INT64_MIN constant because x86 has no cmp r64, imm64.
     */
    if (!emit_movabs_rdx(w, UINT64_C(0x8000000000000000)) ||
        !emit_cmp_rax_rdx(w) ||
        !emit_jcc_rel32_placeholder(w, HOST_JCC_NE, &normal_disp) ||
        !emit_cmp_rcx_neg_one(w) ||
        !emit_jcc_rel32_placeholder(w, HOST_JCC_E, &overflow_disp))
    {
        return false;
    }

    patch_rel32(normal_disp, w->cur);

    if (!emit_cqo(w) ||
        !emit_idiv_rcx(w) ||
        (want_remainder && !emit_mov_rax_rdx(w)) ||
        !emit_jmp_rel32_placeholder(w, &normal_done_disp))
    {
        return false;
    }

    patch_rel32(zero_disp, w->cur);

    if ((!want_remainder && !emit_movabs_rax(w, UINT64_MAX)) ||
        !emit_jmp_rel32_placeholder(w, &zero_done_disp))
    {
        return false;
    }

    patch_rel32(overflow_disp, w->cur);

    /*
     * On signed overflow DIV returns the original INT64_MIN already in RAX;
     * REM returns zero. Neither case reaches host IDIV.
     */
    if (want_remainder && !emit_zero_rax(w))
    {
        return false;
    }

    patch_rel32(normal_done_disp, w->cur);
    patch_rel32(zero_done_disp, w->cur);
    return jit_reg_write_rax(w, regs, rd) &&
           emit_finish_native_m(w, op);
}

/* Emit DIVW/DIVUW/REMW/REMUW and sign-extend every selected 32-bit result. */
static bool emit_rv64_divrem_word(rv64_jit_writer_t *w,
                                  rv64_jit_reg_cache_t *regs,
                                  uint32_t rd, bool is_signed,
                                  bool want_remainder,
                                  rv64_jit_m_op_t op)
{
    uint8_t *zero_disp = NULL;
    uint8_t *normal_done_disp = NULL;

    if (!emit_test_ecx_ecx(w) ||
        !emit_jcc_rel32_placeholder(w, HOST_JCC_E, &zero_disp))
    {
        return false;
    }

    if (!is_signed)
    {
        if (!emit_zero_rdx(w) ||
            !emit_div_ecx(w) ||
            (want_remainder && !emit_mov_eax_edx(w)) ||
            !emit_jmp_rel32_placeholder(w, &normal_done_disp))
        {
            return false;
        }

        patch_rel32(zero_disp, w->cur);

        if (!want_remainder && !emit_mov_eax_imm32(w, UINT32_MAX))
        {
            return false;
        }

        patch_rel32(normal_done_disp, w->cur);
    }
    else
    {
        uint8_t *normal_disp = NULL;
        uint8_t *overflow_disp = NULL;
        uint8_t *zero_done_disp = NULL;

        if (!emit_cmp_eax_imm32(w, (uint32_t)INT32_MIN) ||
            !emit_jcc_rel32_placeholder(w, HOST_JCC_NE, &normal_disp) ||
            !emit_cmp_ecx_neg_one(w) ||
            !emit_jcc_rel32_placeholder(w, HOST_JCC_E, &overflow_disp))
        {
            return false;
        }

        patch_rel32(normal_disp, w->cur);

        if (!emit_cdq(w) ||
            !emit_idiv_ecx(w) ||
            (want_remainder && !emit_mov_eax_edx(w)) ||
            !emit_jmp_rel32_placeholder(w, &normal_done_disp))
        {
            return false;
        }

        patch_rel32(zero_disp, w->cur);

        if ((!want_remainder &&
             !emit_mov_eax_imm32(w, UINT32_MAX)) ||
            !emit_jmp_rel32_placeholder(w, &zero_done_disp))
        {
            return false;
        }

        patch_rel32(overflow_disp, w->cur);

        /*
         * EAX still contains INT32_MIN for an overflowing quotient. A
         * remainder is zero and joins the same mandatory sign-extension step.
         */
        if (want_remainder && !emit_mov_eax_imm32(w, 0))
        {
            return false;
        }

        patch_rel32(normal_done_disp, w->cur);
        patch_rel32(zero_done_disp, w->cur);
    }

    return emit_cdqe(w) &&
           jit_reg_write_rax(w, regs, rd) &&
           emit_finish_native_m(w, op);
}

/* Emit a one-source integer op directly in the destination cache slot. */
static bool emit_op_imm_hreg(rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs,
                             uint32_t rd, uint32_t rs1, uint8_t subop,
                             int32_t imm)
{
    if (rd == RV64_GPR_ZERO)
    {
        return true;
    }

    if (rs1 == RV64_GPR_ZERO)
    {
        switch (subop)
        {
        case HOST_GROUP1_ADD: /* ADD: 0 + imm */
        case HOST_GROUP1_OR:  /* OR: 0 | imm */
        case HOST_GROUP1_XOR: /* XOR: 0 ^ imm */
            return jit_reg_write_imm(w, regs, rd, (uint64_t)(int64_t)imm);
        case HOST_GROUP1_AND: /* AND: 0 & imm */
            return jit_reg_write_imm(w, regs, rd, 0);
        default:
            return false;
        }
    }

    rv64_jit_reg_slot_t *src = jit_reg_loaded_slot(w, regs, rs1);
    if (src == NULL)
    {
        return false;
    }

    rv64_jit_reg_slot_t *dst = jit_reg_alloc(w, regs, rd);
    if (dst == NULL ||
        (dst != src && !emit_mov_hreg_hreg(w, dst->hreg, src->hreg)) ||
        !emit_hreg_imm32_alu64(w, dst->hreg, subop, imm))
    {
        return false;
    }

    jit_reg_mark_written(regs, dst);
    return true;
}

/* Emit a shift-immediate op directly in the destination cache slot. */
static bool emit_shift_imm_hreg(rv64_jit_writer_t *w,
                                rv64_jit_reg_cache_t *regs,
                                uint32_t rd, uint32_t rs1,
                                uint8_t subop, uint8_t shamt)
{
    if (rd == RV64_GPR_ZERO)
    {
        return true;
    }

    if (rs1 == RV64_GPR_ZERO)
    {
        return jit_reg_write_imm(w, regs, rd, 0);
    }

    rv64_jit_reg_slot_t *src = jit_reg_loaded_slot(w, regs, rs1);
    if (src == NULL)
    {
        return false;
    }

    rv64_jit_reg_slot_t *dst = jit_reg_alloc(w, regs, rd);
    if (dst == NULL ||
        (dst != src && !emit_mov_hreg_hreg(w, dst->hreg, src->hreg)) ||
        !emit_shift_hreg_imm(w, dst->hreg, subop, shamt))
    {
        return false;
    }

    jit_reg_mark_written(regs, dst);
    return true;
}

/* Emit a two-source integer ALU op directly between cached host registers. */
static bool emit_op_hreg(rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs,
                         uint32_t rd, uint32_t rs1, uint32_t rs2,
                         uint8_t opcode, bool commutative)
{
    if (rd == RV64_GPR_ZERO)
    {
        return true;
    }

    rv64_jit_reg_slot_t *src1 = jit_reg_loaded_slot(w, regs, rs1);
    rv64_jit_reg_slot_t *src2 = jit_reg_loaded_slot(w, regs, rs2);

    if (src1 == NULL || src2 == NULL)
    {
        return false;
    }

    rv64_jit_reg_slot_t *dst = NULL;
    rv64_jit_reg_slot_t *rhs = NULL;

    if (rd == rs1)
    {
        dst = src1;
        rhs = src2;
    }
    else if (commutative && rd == rs2)
    {
        dst = src2;
        rhs = src1;
    }
    else
    {
        dst = jit_reg_alloc(w, regs, rd);
        if (dst == NULL || !emit_mov_hreg_hreg(w, dst->hreg, src1->hreg))
        {
            return false;
        }

        rhs = src2;
    }

    if (!emit_hreg_hreg_alu64(w, dst->hreg, rhs->hreg, opcode))
    {
        return false;
    }

    jit_reg_mark_written(regs, dst);
    return true;
}

/* Emit one commutative RV64 W-form op directly in cached host registers. */
static bool emit_op32_hreg_commutative(rv64_jit_writer_t *w,
                                       rv64_jit_reg_cache_t *regs,
                                       uint32_t rd, uint32_t rs1,
                                       uint32_t rs2, uint8_t opcode,
                                       bool multiply)
{
    if (rd == RV64_GPR_ZERO)
    {
        return true;
    }

    if (rs1 == RV64_GPR_ZERO || rs2 == RV64_GPR_ZERO)
    {
        return false;
    }

    rv64_jit_reg_slot_t *src1 = jit_reg_loaded_slot(w, regs, rs1);
    rv64_jit_reg_slot_t *src2 = jit_reg_loaded_slot(w, regs, rs2);

    if (src1 == NULL || src2 == NULL)
    {
        return false;
    }

    rv64_jit_reg_slot_t *dst = NULL;
    rv64_jit_reg_slot_t *rhs = NULL;

    if (rd == rs1)
    {
        dst = src1;
        rhs = src2;
    }
    else if (rd == rs2)
    {
        dst = src2;
        rhs = src1;
    }
    else
    {
        dst = jit_reg_alloc(w, regs, rd);
        if (dst == NULL)
        {
            return false;
        }

        if (dst == src1)
        {
            rhs = src2;
        }
        else if (dst == src2)
        {
            rhs = src1;
        }
        else
        {
            if (!emit_mov_hreg_hreg(w, dst->hreg, src1->hreg))
            {
                return false;
            }

            rhs = src2;
        }
    }

    if (!((multiply ? emit_hreg_hreg_imul32(w, dst->hreg, rhs->hreg)
                    : emit_hreg_hreg_alu32(w, dst->hreg, rhs->hreg, opcode)) &&
          emit_hreg_sext32(w, dst->hreg)))
    {
        return false;
    }

    jit_reg_mark_written(regs, dst);
    return true;
}

/* Emit a 64-bit RISC-V OP-IMM instruction into native code. */
static bool emit_op_imm(rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs,
                        uint32_t instr)
{
    const uint32_t rd = rv64_instr_rd(instr);
    const uint32_t funct3 = rv64_instr_funct3(instr);
    const uint32_t rs1 = rv64_instr_rs1(instr);
    const int32_t imm = (int32_t)imm_i(instr);

    if (funct3 == RV64_FUNCT3_ADD_SUB && imm == 0)
    {
        return jit_reg_copy(w, regs, rd, rs1);
    }

    switch (funct3)
    {
    case RV64_FUNCT3_ADD_SUB: /* ADDI */
        return emit_op_imm_hreg(w, regs, rd, rs1, HOST_GROUP1_ADD, imm);
    case RV64_FUNCT3_SLT: /* SLTI, signed compare. */
        if (!jit_reg_read_rax(w, regs, rs1))
        {
            return false;
        }

        return emit_cmp_rax_imm32(w, imm) &&
               emit_setcc_rax(w, HOST_SETCC_L) &&
               jit_reg_write_rax(w, regs, rd);
    case RV64_FUNCT3_SLTU: /* SLTIU, unsigned compare. */
        if (!jit_reg_read_rax(w, regs, rs1))
        {
            return false;
        }

        return emit_cmp_rax_imm32(w, imm) &&
               emit_setcc_rax(w, HOST_SETCC_B) &&
               jit_reg_write_rax(w, regs, rd);
    case RV64_FUNCT3_XOR: /* XORI */
        return emit_op_imm_hreg(w, regs, rd, rs1, HOST_GROUP1_XOR, imm);
    case RV64_FUNCT3_OR: /* ORI */
        return emit_op_imm_hreg(w, regs, rd, rs1, HOST_GROUP1_OR, imm);
    case RV64_FUNCT3_AND: /* ANDI */
        return emit_op_imm_hreg(w, regs, rd, rs1, HOST_GROUP1_AND, imm);
    case RV64_FUNCT3_SLL: /* SLLI; funct6 must be 000000 for RV64 base shifts. */
        if (rv64_instr_funct6(instr) != RV64_FUNCT6_SHIFT_LOGICAL)
        {
            return false;
        }

        return emit_shift_imm_hreg(w, regs, rd, rs1, HOST_SHIFT_GROUP_SAL,
                                   (uint8_t)rv64_instr_shamt6(instr));
    case RV64_FUNCT3_SRL_SRA: /* SRLI/SRAI; funct6 selects logical versus arithmetic right shift. */
        if (rv64_instr_funct6(instr) == RV64_FUNCT6_SHIFT_LOGICAL)
        {
            return emit_shift_imm_hreg(w, regs, rd, rs1, HOST_SHIFT_GROUP_SHR,
                                       (uint8_t)rv64_instr_shamt6(instr));
        }

        if (rv64_instr_funct6(instr) == RV64_FUNCT6_SHIFT_ARITH)
        {
            return emit_shift_imm_hreg(w, regs, rd, rs1, HOST_SHIFT_GROUP_SAR,
                                       (uint8_t)rv64_instr_shamt6(instr));
        }

        return false;
    default:
        return false;
    }
}

/* Emit an RV64 OP-IMM-32 instruction and sign-extend the 32-bit result. */
static bool emit_op_imm32(rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs,
                          uint32_t instr)
{
    const uint32_t rd = rv64_instr_rd(instr);
    const uint32_t funct3 = rv64_instr_funct3(instr);
    const uint32_t rs1 = rv64_instr_rs1(instr);
    const int32_t imm = (int32_t)imm_i(instr);

    switch (funct3)
    {
    case RV64_FUNCT3_ADD_SUB: /* ADDIW; EAX addition naturally drops to 32 bits, then CDQE. */
        return jit_reg_read_rax(w, regs, rs1) &&
               emit_u8(w, 0x05) && emit_u32(w, (uint32_t)imm) &&
               emit_u8(w, 0x48) && emit_u8(w, 0x98) &&
               jit_reg_write_rax(w, regs, rd);
    case RV64_FUNCT3_SLL: /* SLLIW; funct7 must be zero and shamt is five bits. */
        if (rv64_instr_funct7(instr) != RV64_FUNCT7_BASE)
        {
            return false;
        }

        return jit_reg_read_rax(w, regs, rs1) &&
               emit_shift_eax_imm_sext(
                   w, HOST_SHIFT_SAL, (uint8_t)rv64_instr_shamt5(instr)) &&
               jit_reg_write_rax(w, regs, rd);
    case RV64_FUNCT3_SRL_SRA: /* SRLIW/SRAIW; funct7 distinguishes logical from arithmetic. */
        if (rv64_instr_funct7(instr) == RV64_FUNCT7_BASE)
        {
            return jit_reg_read_rax(w, regs, rs1) &&
                   emit_shift_eax_imm_sext(
                       w, HOST_SHIFT_SHR,
                       (uint8_t)rv64_instr_shamt5(instr)) &&
                   jit_reg_write_rax(w, regs, rd);
        }

        if (rv64_instr_funct7(instr) == RV64_FUNCT7_SUB_SRA)
        {
            return jit_reg_read_rax(w, regs, rs1) &&
                   emit_shift_eax_imm_sext(
                       w, HOST_SHIFT_SAR,
                       (uint8_t)rv64_instr_shamt5(instr)) &&
                   jit_reg_write_rax(w, regs, rd);
        }

        return false;
    default:
        return false;
    }
}

/* Lower one valid full-width RV64M instruction without entering the C ABI. */
static bool emit_rv64m_op(rv64_jit_writer_t *w,
                          rv64_jit_reg_cache_t *regs,
                          uint32_t instr)
{
    const uint32_t rd = rv64_instr_rd(instr);
    const uint32_t funct3 = rv64_instr_funct3(instr);
    const uint32_t rs1 = rv64_instr_rs1(instr);
    const uint32_t rs2 = rv64_instr_rs2(instr);
    rv64_jit_m_op_t op;

    switch (funct3)
    {
    case RV64_FUNCT3_ADD_SUB:
        op = RV64_JIT_M_OP_MUL;
        break;
    case RV64_FUNCT3_SLL:
        op = RV64_JIT_M_OP_MULH;
        break;
    case RV64_FUNCT3_SLT:
        op = RV64_JIT_M_OP_MULHSU;
        break;
    case RV64_FUNCT3_SLTU:
        op = RV64_JIT_M_OP_MULHU;
        break;
    case RV64_FUNCT3_XOR:
        op = RV64_JIT_M_OP_DIV;
        break;
    case RV64_FUNCT3_SRL_SRA:
        op = RV64_JIT_M_OP_DIVU;
        break;
    case RV64_FUNCT3_OR:
        op = RV64_JIT_M_OP_REM;
        break;
    case RV64_FUNCT3_AND:
        op = RV64_JIT_M_OP_REMU;
        break;
    default:
        return false;
    }

    /*
     * Every RV64M operation is pure. Once the encoding is known valid, an x0
     * destination has no observable result and cannot justify a host divide.
     */
    if (rd == RV64_GPR_ZERO)
    {
        return emit_finish_native_m(w, op);
    }

    if (op <= RV64_JIT_M_OP_MULHU &&
        (rs1 == RV64_GPR_ZERO || rs2 == RV64_GPR_ZERO))
    {
        return jit_reg_write_imm(w, regs, rd, 0) &&
               emit_finish_native_m(w, op);
    }

    if (rs2 == RV64_GPR_ZERO)
    {
        if (op == RV64_JIT_M_OP_DIV || op == RV64_JIT_M_OP_DIVU)
        {
            return jit_reg_write_imm(w, regs, rd, UINT64_MAX) &&
                   emit_finish_native_m(w, op);
        }

        if (op == RV64_JIT_M_OP_REM || op == RV64_JIT_M_OP_REMU)
        {
            return jit_reg_copy(w, regs, rd, rs1) &&
                   emit_finish_native_m(w, op);
        }
    }

    if (!jit_reg_read_rax(w, regs, rs1) ||
        !jit_reg_read_rcx(w, regs, rs2))
    {
        return false;
    }

    switch (op)
    {
    case RV64_JIT_M_OP_MUL:
        return emit_u8(w, 0x48) &&
               emit_u8(w, 0x0f) &&
               emit_u8(w, 0xaf) &&
               emit_u8(w, 0xc1) &&
               jit_reg_write_rax(w, regs, rd) &&
               emit_finish_native_m(w, op);
    case RV64_JIT_M_OP_MULH:
    case RV64_JIT_M_OP_MULHSU:
    case RV64_JIT_M_OP_MULHU:
        return emit_rv64_mul_high(w, regs, rd, op);
    case RV64_JIT_M_OP_DIV:
        return emit_rv64_divrem(w, regs, rd, true, false, op);
    case RV64_JIT_M_OP_DIVU:
        return emit_rv64_divrem(w, regs, rd, false, false, op);
    case RV64_JIT_M_OP_REM:
        return emit_rv64_divrem(w, regs, rd, true, true, op);
    case RV64_JIT_M_OP_REMU:
        return emit_rv64_divrem(w, regs, rd, false, true, op);
    default:
        return false;
    }
}

/* Lower the five valid RV64M OP-32 instructions, rejecting reserved funct3. */
static bool emit_rv64m_op32(rv64_jit_writer_t *w,
                            rv64_jit_reg_cache_t *regs,
                            uint32_t instr)
{
    const uint32_t rd = rv64_instr_rd(instr);
    const uint32_t funct3 = rv64_instr_funct3(instr);
    const uint32_t rs1 = rv64_instr_rs1(instr);
    const uint32_t rs2 = rv64_instr_rs2(instr);
    rv64_jit_m_op_t op;

    switch (funct3)
    {
    case RV64_FUNCT3_ADD_SUB:
        op = RV64_JIT_M_OP_MULW;
        break;
    case RV64_FUNCT3_XOR:
        op = RV64_JIT_M_OP_DIVW;
        break;
    case RV64_FUNCT3_SRL_SRA:
        op = RV64_JIT_M_OP_DIVUW;
        break;
    case RV64_FUNCT3_OR:
        op = RV64_JIT_M_OP_REMW;
        break;
    case RV64_FUNCT3_AND:
        op = RV64_JIT_M_OP_REMUW;
        break;
    default:
        return false;
    }

    if (rd == RV64_GPR_ZERO)
    {
        return emit_finish_native_m(w, op);
    }

    if (op == RV64_JIT_M_OP_MULW &&
        (rs1 == RV64_GPR_ZERO || rs2 == RV64_GPR_ZERO))
    {
        return jit_reg_write_imm(w, regs, rd, 0) &&
               emit_finish_native_m(w, op);
    }

    if (rs2 == RV64_GPR_ZERO)
    {
        if (op == RV64_JIT_M_OP_DIVW || op == RV64_JIT_M_OP_DIVUW)
        {
            return jit_reg_write_imm(w, regs, rd, UINT64_MAX) &&
                   emit_finish_native_m(w, op);
        }

        /*
         * Both W remainders return the low dividend word on division by zero,
         * then sign-extend bit 31 even though REMUW performs unsigned modulo.
         */
        return jit_reg_read_rax(w, regs, rs1) &&
               emit_cdqe(w) &&
               jit_reg_write_rax(w, regs, rd) &&
               emit_finish_native_m(w, op);
    }

    if (op == RV64_JIT_M_OP_MULW)
    {
        return emit_op32_hreg_commutative(
                   w, regs, rd, rs1, rs2, HOST_GROUP1_ADD, true) &&
               emit_finish_native_m(w, op);
    }

    if (!jit_reg_read_rax(w, regs, rs1) ||
        !jit_reg_read_rcx(w, regs, rs2))
    {
        return false;
    }

    switch (op)
    {
    case RV64_JIT_M_OP_DIVW:
        return emit_rv64_divrem_word(w, regs, rd, true, false, op);
    case RV64_JIT_M_OP_DIVUW:
        return emit_rv64_divrem_word(w, regs, rd, false, false, op);
    case RV64_JIT_M_OP_REMW:
        return emit_rv64_divrem_word(w, regs, rd, true, true, op);
    case RV64_JIT_M_OP_REMUW:
        return emit_rv64_divrem_word(w, regs, rd, false, true, op);
    default:
        return false;
    }
}

/* Emit a 64-bit RV64 OP instruction for the integer ALU subset. */
static bool emit_op(rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs,
                    uint32_t instr)
{
    const uint32_t rd = rv64_instr_rd(instr);
    const uint32_t funct3 = rv64_instr_funct3(instr);
    const uint32_t rs1 = rv64_instr_rs1(instr);
    const uint32_t rs2 = rv64_instr_rs2(instr);
    const uint32_t key = RV64_OP_KEY(rv64_instr_funct7(instr), funct3);

    if (rv64_instr_funct7(instr) == RV64_FUNCT7_MULDIV)
    {
        return emit_rv64m_op(w, regs, instr);
    }

    switch (key)
    {
    case RV64_OP_KEY(RV64_FUNCT7_BASE, RV64_FUNCT3_ADD_SUB): /* ADD */
        return emit_op_hreg(w, regs, rd, rs1, rs2, HOST_ALU_ADD, true);
    case RV64_OP_KEY(RV64_FUNCT7_SUB_SRA, RV64_FUNCT3_ADD_SUB): /* SUB */
        if (rd != rs2)
        {
            return emit_op_hreg(w, regs, rd, rs1, rs2, HOST_ALU_SUB, false);
        }
        break;
    case RV64_OP_KEY(RV64_FUNCT7_BASE, RV64_FUNCT3_XOR): /* XOR */
        return emit_op_hreg(w, regs, rd, rs1, rs2, HOST_ALU_XOR, true);
    case RV64_OP_KEY(RV64_FUNCT7_BASE, RV64_FUNCT3_OR): /* OR */
        return emit_op_hreg(w, regs, rd, rs1, rs2, HOST_ALU_OR, true);
    case RV64_OP_KEY(RV64_FUNCT7_BASE, RV64_FUNCT3_AND): /* AND */
        return emit_op_hreg(w, regs, rd, rs1, rs2, HOST_ALU_AND, true);
    default:
        break;
    }

    if (key == RV64_OP_KEY(RV64_FUNCT7_BASE, RV64_FUNCT3_SLT) ||
        key == RV64_OP_KEY(RV64_FUNCT7_BASE, RV64_FUNCT3_SLTU))
    {
        const uint8_t setcc_opcode =
            key == RV64_OP_KEY(RV64_FUNCT7_BASE, RV64_FUNCT3_SLT)
                ? HOST_SETCC_L
                : HOST_SETCC_B;
        bool handled = false;

        if (!jit_reg_try_emit_cached_compare(
                w, regs, rd, rs1, rs2, setcc_opcode, &handled))
        {
            return false;
        }

        if (handled)
        {
            return true;
        }
    }

    if (!jit_reg_read_rax(w, regs, rs1) || !jit_reg_read_rcx(w, regs, rs2))
    {
        return false;
    }

    switch (key)
    {
    case RV64_OP_KEY(RV64_FUNCT7_BASE, RV64_FUNCT3_ADD_SUB): /* ADD */
        return emit_rax_rcx_alu64(w, HOST_ALU_ADD) && jit_reg_write_rax(w, regs, rd);
    case RV64_OP_KEY(RV64_FUNCT7_SUB_SRA, RV64_FUNCT3_ADD_SUB): /* SUB */
        return emit_rax_rcx_alu64(w, HOST_ALU_SUB) && jit_reg_write_rax(w, regs, rd);
    case RV64_OP_KEY(RV64_FUNCT7_BASE, RV64_FUNCT3_SLL): /* SLL */
        return emit_shift_rax_cl(w, HOST_SHIFT_SAL) && jit_reg_write_rax(w, regs, rd);
    case RV64_OP_KEY(RV64_FUNCT7_BASE, RV64_FUNCT3_SLT): /* SLT, signed compare. */
        return emit_cmp_rax_rcx(w) &&
               emit_setcc_rax(w, HOST_SETCC_L) &&
               jit_reg_write_rax(w, regs, rd);
    case RV64_OP_KEY(RV64_FUNCT7_BASE, RV64_FUNCT3_SLTU): /* SLTU, unsigned compare. */
        return emit_cmp_rax_rcx(w) &&
               emit_setcc_rax(w, HOST_SETCC_B) &&
               jit_reg_write_rax(w, regs, rd);
    case RV64_OP_KEY(RV64_FUNCT7_BASE, RV64_FUNCT3_XOR): /* XOR */
        return emit_rax_rcx_alu64(w, HOST_ALU_XOR) && jit_reg_write_rax(w, regs, rd);
    case RV64_OP_KEY(RV64_FUNCT7_BASE, RV64_FUNCT3_SRL_SRA): /* SRL */
        return emit_shift_rax_cl(w, HOST_SHIFT_SHR) && jit_reg_write_rax(w, regs, rd);
    case RV64_OP_KEY(RV64_FUNCT7_SUB_SRA, RV64_FUNCT3_SRL_SRA): /* SRA */
        return emit_shift_rax_cl(w, HOST_SHIFT_SAR) && jit_reg_write_rax(w, regs, rd);
    case RV64_OP_KEY(RV64_FUNCT7_BASE, RV64_FUNCT3_OR): /* OR */
        return emit_rax_rcx_alu64(w, HOST_ALU_OR) && jit_reg_write_rax(w, regs, rd);
    case RV64_OP_KEY(RV64_FUNCT7_BASE, RV64_FUNCT3_AND): /* AND */
        return emit_rax_rcx_alu64(w, HOST_ALU_AND) && jit_reg_write_rax(w, regs, rd);
    default:
        return false;
    }
}

/* Emit an RV64 OP-32 instruction and sign-extend the 32-bit result. */
static bool emit_op32(rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs,
                      uint32_t instr)
{
    const uint32_t rd = rv64_instr_rd(instr);
    const uint32_t funct3 = rv64_instr_funct3(instr);
    const uint32_t rs1 = rv64_instr_rs1(instr);
    const uint32_t rs2 = rv64_instr_rs2(instr);
    const uint32_t key = RV64_OP_KEY(rv64_instr_funct7(instr), funct3);

    if (rv64_instr_funct7(instr) == RV64_FUNCT7_MULDIV)
    {
        return emit_rv64m_op32(w, regs, instr);
    }

    switch (key)
    {
    case RV64_OP_KEY(RV64_FUNCT7_BASE, RV64_FUNCT3_ADD_SUB): /* ADDW */
        if (rs1 != RV64_GPR_ZERO && rs2 != RV64_GPR_ZERO)
        {
            return emit_op32_hreg_commutative(w, regs, rd, rs1, rs2,
                                              HOST_ALU_ADD, false);
        }
        break;
    default:
        break;
    }

    if (!jit_reg_read_rax(w, regs, rs1) || !jit_reg_read_rcx(w, regs, rs2))
    {
        return false;
    }

    switch (key)
    {
    case RV64_OP_KEY(RV64_FUNCT7_BASE, RV64_FUNCT3_ADD_SUB): /* ADDW */
        return emit_eax_ecx_alu32_sext(w, HOST_ALU_ADD) && jit_reg_write_rax(w, regs, rd);
    case RV64_OP_KEY(RV64_FUNCT7_SUB_SRA, RV64_FUNCT3_ADD_SUB): /* SUBW */
        return emit_eax_ecx_alu32_sext(w, HOST_ALU_SUB) && jit_reg_write_rax(w, regs, rd);
    case RV64_OP_KEY(RV64_FUNCT7_BASE, RV64_FUNCT3_SLL): /* SLLW */
        return emit_shift_eax_cl_sext(w, HOST_SHIFT_SAL) && jit_reg_write_rax(w, regs, rd);
    case RV64_OP_KEY(RV64_FUNCT7_BASE, RV64_FUNCT3_SRL_SRA): /* SRLW */
        return emit_shift_eax_cl_sext(w, HOST_SHIFT_SHR) && jit_reg_write_rax(w, regs, rd);
    case RV64_OP_KEY(RV64_FUNCT7_SUB_SRA, RV64_FUNCT3_SRL_SRA): /* SRAW */
        return emit_shift_eax_cl_sext(w, HOST_SHIFT_SAR) && jit_reg_write_rax(w, regs, rd);
    default:
        return false;
    }
}

/*
 * Branch chaining.
 *
 * Simple counted loops are allowed to stay inside one native function when the
 * taken target is the current block head and every instruction in the body can
 * be re-entered safely.  The generated backedge accumulates retired
 * instructions in `rv64_jit_loop_extra` and checks one more full lap against the
 * cpu_exec() budget before jumping.  If the next lap would exceed the budget,
 * the block returns to C with `cpu.pc` at the branch target, preserving bounded
 * device polling and interrupt checks.
 */
/* Emit the taken side of a branch that can jump back to the native loop head. */
static bool emit_branch_chain_backedge(rv64_jit_writer_t *w,
                                       rv64_jit_reg_cache_t *regs, vaddr_t target,
                                       uint32_t exit_count,
                                       const uint8_t *target_native,
                                       bool stable_register_mapping)
{
    uint8_t *over_budget_disp = NULL;
    uint8_t *loop_disp = NULL;

    /*
     * The current lap has completed `exit_count` guest instructions at the
     * branch. Stable-register loops keep the completed count in R9D and the
     * budget remaining after the current lap in ESI. Subtracting one further
     * lap from ESI both reserves that work and sets the carry flag: JAE
     * re-enters the loop only when the reservation succeeded. Ordinary loops
     * retain the memory-backed ABI. Returning the current completed count keeps
     * cpu_exec() accounting exact.
     */
    if (stable_register_mapping)
    {
        if (!emit_add_r9d_imm32(w, exit_count) ||
            !emit_sub_esi_imm32(w, exit_count) ||
            !emit_jcc_rel32_placeholder(
                w, HOST_JCC_AE, &loop_disp))
        {
            return false;
        }

        patch_rel32(loop_disp, target_native);

        if (!emit_mov_eax_r9d(w))
        {
            return false;
        }
    }
    else
    {
        if (!emit_movabs_rdx(
                w, (uint64_t)(uintptr_t)&rv64_jit_loop_extra) ||
            !emit_mov_eax_m32_rdx(w) ||
            !emit_add_eax_imm32(w, exit_count) ||
            !emit_mov_ecx_eax(w) ||
            !emit_add_ecx_imm32(w, exit_count) ||
            !emit_movabs_rdx(
                w, (uint64_t)(uintptr_t)&rv64_jit_entry_budget) ||
            !emit_cmp_ecx_m32_rdx(w) ||
            !emit_jcc_rel32_placeholder(
                w, HOST_JCC_A, &over_budget_disp) ||
            !jit_reg_emit_flush_all_dirty(w, regs) ||
            !emit_movabs_rdx(
                w, (uint64_t)(uintptr_t)&rv64_jit_loop_extra) ||
            !emit_mov_m32_rdx_eax(w) ||
            !emit_jmp_rel32_placeholder(w, &loop_disp))
        {
            return false;
        }

        patch_rel32(loop_disp, target_native);
        patch_rel32(over_budget_disp, w->cur);
    }

    /*
     * EAX contains the completed count, but emit_store_pc_imm() uses RAX as its
     * immediate scratch register. Preserve the count in ECX across the PC store
     * and restore EAX before the native function returns.
     */
    return emit_mov_ecx_eax(w) &&
           jit_reg_emit_flush_all_dirty(w, regs) &&
           emit_store_pc_imm(w, target) &&
           emit_inc_jit_stat_counter(w,
                                     &rv64_jit_stats.side_exit_by_reason[RV64_JIT_SIDE_EXIT_CHAINED_OVER_BUDGET]) &&
           emit_mov_eax_ecx(w) &&
           emit_return_eax(w);
}

/* Emit one conditional branch with a taken side exit and fall-through fast path. */
bool rv64_jit_emit_branch(rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs,
                          uint32_t instr, vaddr_t pc,
                          vaddr_t block_start_pc,
                          const uint8_t *native_body_entry,
                          bool can_chain_self_backedge,
                          bool stable_self_backedge,
                          bool *emitted_native_backedge,
                          uint32_t retired_including_current,
                          bool current_block_uses_data_translation_state)
{
    const uint32_t funct3 = rv64_instr_funct3(instr);
    const uint32_t rs1 = rv64_instr_rs1(instr);
    const uint32_t rs2 = rv64_instr_rs2(instr);
    const vaddr_t target = pc + imm_b(instr);
    uint8_t inverse_jcc = 0;
    uint8_t *fallthrough_disp = NULL;

    /*
     * A conditional branch traps for a misaligned target only when taken.  A
     * statically misaligned target therefore makes the whole instruction fall
     * back to the interpreter, which can evaluate the condition before deciding
     * whether to raise the instruction-address-misaligned exception.
     */
    if ((target & RV64_IALIGN_MASK) != 0)
    {
        return false;
    }

    switch (funct3)
    {
    case RV64_FUNCT3_BEQ: /* BEQ: inverse JNE falls through when not equal. */
        inverse_jcc = HOST_JCC_NE;
        break;
    case RV64_FUNCT3_BNE: /* BNE: inverse JE falls through when equal. */
        inverse_jcc = HOST_JCC_E;
        break;
    case RV64_FUNCT3_BLT: /* BLT: inverse JGE falls through for signed greater/equal. */
        inverse_jcc = HOST_JCC_GE;
        break;
    case RV64_FUNCT3_BGE: /* BGE: inverse JL falls through for signed less-than. */
        inverse_jcc = HOST_JCC_L;
        break;
    case RV64_FUNCT3_BLTU: /* BLTU: inverse JAE falls through for unsigned above/equal. */
        inverse_jcc = HOST_JCC_AE;
        break;
    case RV64_FUNCT3_BGEU: /* BGEU: inverse JB falls through for unsigned below. */
        inverse_jcc = HOST_JCC_B;
        break;
    default:
        return false;
    }

    const bool equality_against_zero =
        (funct3 == RV64_FUNCT3_BEQ || funct3 == RV64_FUNCT3_BNE) &&
        ((rs1 == RV64_GPR_ZERO) != (rs2 == RV64_GPR_ZERO));

    if (equality_against_zero)
    {
        const uint32_t nonzero_reg =
            rs1 == RV64_GPR_ZERO ? rs2 : rs1;
        rv64_jit_reg_slot_t *slot =
            jit_reg_loaded_slot(w, regs, nonzero_reg);

        if (slot == NULL || !emit_test_hreg_hreg(w, slot->hreg))
        {
            return false;
        }
    }
    else if (!jit_reg_read_rax(w, regs, rs1) ||
             !jit_reg_read_rcx(w, regs, rs2) ||
             !emit_cmp_rax_rcx(w))
    {
        return false;
    }

    if (!emit_jcc_rel32_placeholder(
            w, inverse_jcc, &fallthrough_disp))
    {
        return false;
    }

    if (can_chain_self_backedge && target == block_start_pc)
    {
        if (!emit_branch_chain_backedge(w, regs, target,
                                        retired_including_current,
                                        native_body_entry,
                                        stable_self_backedge))
        {
            return false;
        }

        *emitted_native_backedge = true;
    }
    else if (!rv64_jit_direct_link_enabled())
    {
        if (!rv64_jit_emit_plain_block_exit(w, regs, target,
                                            retired_including_current))
        {
            return false;
        }
    }
    else if (!rv64_jit_emit_direct_link_exit(
                 w, regs, target, retired_including_current,
                 current_block_uses_data_translation_state,
                 &rv64_jit_stats.direct_branch_link_taken_count))
    {
        return false;
    }

    patch_rel32(fallthrough_disp, w->cur);

    /*
     * A not-taken final lap bypasses the backedge counter update. Publish the
     * earlier completed-lap count once so the ordinary direct-link or return
     * path can add this block's final instruction count exactly as before.
     */
    if (stable_self_backedge && can_chain_self_backedge &&
        target == block_start_pc)
    {
        return emit_movabs_rdx(
                   w, (uint64_t)(uintptr_t)&rv64_jit_loop_extra) &&
               emit_mov_m32_rdx_r9d(w);
    }

    return true;
}

/* Emit JAL or JALR, both of which end the current native block. */
bool rv64_jit_emit_jump_instr(rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs,
                              uint32_t instr, vaddr_t pc,
                              uint32_t completed_count,
                              bool source_uses_data_state)
{
    const uint32_t opcode = instr & RV64_OPCODE_MASK;
    const uint32_t rd = rv64_instr_rd(instr);
    const vaddr_t link = pc + RV64_INSN_SIZE;
    uint8_t *misaligned_disp = NULL;
    rv64_jit_reg_cache_t side_exit_regs;

    if (opcode == RV64_OPCODE_JAL)
    {
        const vaddr_t target = pc + imm_j(instr);

        if ((target & RV64_IALIGN_MASK) != 0)
        {
            return false;
        }

        const bool emitted =
            emit_movabs_rax(w, link) &&
            jit_reg_write_rax(w, regs, rd) &&
            (rv64_jit_direct_link_enabled()
                 ? rv64_jit_emit_direct_link_exit(
                       w, regs, target, completed_count + 1u,
                       source_uses_data_state, NULL)
                 : rv64_jit_emit_plain_block_exit(
                       w, regs, target, completed_count + 1u));
        if (emitted)
        {
            JIT_STAT_INC(emitted_sites.native_jumps);
        }

        return emitted;
    }

    if (opcode != RV64_OPCODE_JALR ||
        rv64_instr_funct3(instr) != RV64_FUNCT3_JALR)
    {
        return false;
    }

    const uint32_t rs1 = rv64_instr_rs1(instr);
    const int64_t immediate = imm_i(instr);
    const bool canonical_return =
        rd == RV64_GPR_ZERO &&
        rs1 == RV64_GPR_LINK &&
        immediate == 0;
    const bool return_stack_hint =
        rd == RV64_GPR_ZERO &&
        (rs1 == RV64_GPR_LINK ||
         rs1 == RISCV_GPR_ALTERNATE_LINK);
    const bool pic_eligible =
        rd != RV64_GPR_ZERO || return_stack_hint;
    const bool can_link_indirect =
        canonical_return
            ? rv64_jit_return_link_enabled()
            : rv64_jit_direct_link_enabled();

    /*
     * JALR computes `(rs1 + imm) & ~1`, then checks instruction alignment after
     * clearing bit zero.  The misaligned case returns before JALR executes so
     * the interpreter raises the same trap and does not write the link register.
     */
    if (!jit_reg_read_rax(w, regs, rs1) ||
        !emit_add_rax_imm32(w, (int32_t)immediate) ||
        !emit_and_rax_imm32(w, (int32_t)~RV64_JALR_TARGET_LSB_MASK) ||
        !emit_test_al_imm8(w, RV64_IALIGN_MASK) ||
        !emit_jcc_rel32_placeholder(w, HOST_JCC_NE, &misaligned_disp))
    {
        return false;
    }

    side_exit_regs = *regs;

    /*
     * Preserve canonical RET's narrower runtime switch and disjoint counters.
     * Every other JALR follows the ordinary direct-link switch. The ISA's RAS
     * operand hints recognise both x1 and x5 as link registers, irrespective
     * of the immediate, so their non-linking returns remain PIC-eligible even
     * though only the strict x1+0 form contributes to canonical RET counters.
     */
    if (can_link_indirect)
    {
        uint64_t *subset_taken_counter =
            canonical_return
                ? &rv64_jit_stats.direct_return_link_taken_count
                : &rv64_jit_stats.direct_jalr_link_taken_count;
        uint64_t *subset_miss_counter =
            canonical_return
                ? &rv64_jit_stats.direct_return_link_miss_count
                : &rv64_jit_stats.direct_jalr_link_miss_count;

        if (!emit_indirect_link_exit(
                w, regs, rd, link, completed_count + 1u,
                source_uses_data_state, pic_eligible,
                subset_taken_counter, subset_miss_counter,
                canonical_return
                    ? RV64_JIT_INDIRECT_PIC_RETURN
                    : RV64_JIT_INDIRECT_PIC_JALR))
        {
            return false;
        }
    }
    else if (!emit_mov_rcx_rax(w) ||
             !emit_movabs_rax(w, link) ||
             !jit_reg_write_rax(w, regs, rd) ||
             !jit_reg_flush_all_dirty(w, regs) ||
             !emit_mov_rax_rcx(w) ||
             !emit_store_rax_pc(w) ||
             !emit_return_total_retired(w, completed_count + 1u))
    {
        return false;
    }

    patch_rel32(misaligned_disp, w->cur);

    if (!emit_interpreter_side_exit(w, &side_exit_regs, pc, completed_count,
                                    RV64_JIT_SIDE_EXIT_JALR_MISALIGNED))
    {
        return false;
    }

    JIT_STAT_INC(emitted_sites.native_jumps);
    return true;
}

/* Dispatch one supported non-branch RISC-V instruction to the native emitter. */
bool rv64_jit_emit_instr(rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs,
                         uint32_t instr, vaddr_t pc,
                         uint32_t exit_count)
{
    const uint32_t opcode = instr & RV64_OPCODE_MASK;
    const uint32_t rd = rv64_instr_rd(instr);

    switch (opcode)
    {
    case RV64_OPCODE_OP_IMM: /* ADDI/SLTI/SLTIU/XORI/ORI/ANDI/SLLI/SRLI/SRAI. */
        return emit_op_imm(w, regs, instr);
    case RV64_OPCODE_OP_IMM_32: /* ADDIW/SLLIW/SRLIW/SRAIW. */
        return emit_op_imm32(w, regs, instr);
    case RV64_OPCODE_OP: /* 64-bit register-register integer ALU subset. */
        return emit_op(w, regs, instr);
    case RV64_OPCODE_OP_32: /* W-form register-register integer ALU subset. */
        return emit_op32(w, regs, instr);
    case RV64_OPCODE_LUI: /* LUI materialises the sign-extended U immediate. */
        return jit_reg_write_imm(w, regs, rd, (uint64_t)imm_u_sext(instr));
    case RV64_OPCODE_AUIPC: /* AUIPC adds the sign-extended U immediate to PC. */
        return jit_reg_write_imm(w, regs, rd, (uint64_t)(pc + imm_u_sext(instr)));
    default:
        (void)exit_count;
        return false;
    }
}

#endif /* CONFIG_RV64 */
