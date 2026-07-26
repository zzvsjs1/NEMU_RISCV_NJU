#include <generated/autoconf.h>

#ifdef CONFIG_RV64

#include "jit-rv64-internal.h"

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
 * operations.  RAX, RCX, RDX and R8 are scratch unless a helper-specific comment
 * says otherwise.
 *
 * The extra 8-byte stack adjustment keeps the System V stack aligned before
 * helper calls.  Dirty cached guest registers are flushed before any helper
 * which can observe architectural state, and before every native/interpreter
 * exit.  The RV64M arithmetic helper is deliberately pure: it receives both
 * operands as arguments and therefore does not require a cache writeback.
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
    HOST_REG_R8 = 8,
    HOST_REG_R10 = 10,
    HOST_REG_R11 = 11,
    HOST_REG_R12 = 12,
    HOST_REG_R13 = 13,
    HOST_REG_R14 = 14,
    HOST_REG_R15 = 15,
} host_reg_t;

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

/* Return the x86 register number backing one callee-saved cache slot. */
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

/* Emit `movabs r10, imm64`, the fixed host PMEM base for direct loads. */
static bool emit_movabs_r10(rv64_jit_writer_t *w, uint64_t value)
{
    return emit_movabs_reg(w, HOST_REG_R10, value);
}

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

/* Emit `mov ecx, eax`, copying the loop count for the budget look-ahead. */
static bool emit_mov_ecx_eax(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x89) && emit_u8(w, 0xc1);
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

/* Emit `mov rsi, rdx`, preparing the second helper argument from a guest value. */
static bool emit_mov_rsi_rdx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x89) && emit_u8(w, 0xd6);
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

/* Compare a dword field in the R8-pointed DTLB entry with an immediate. */
static bool emit_cmp_r8d_field_imm32(rv64_jit_writer_t *w, uint32_t offset,
                                     uint32_t value)
{
    Assert(offset <= INT8_MAX, "jit: RV64 DTLB dword field offset is too large");
    return emit_u8(w, 0x41) && emit_u8(w, 0x81) &&
           emit_u8(w, 0x78) && emit_u8(w, (uint8_t)offset) && emit_u32(w, value);
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

/* Emit `mov edx, imm32`, preparing the third helper argument. */
static bool emit_mov_edx_imm32(rv64_jit_writer_t *w, uint32_t imm)
{
    return emit_u8(w, 0xba) && emit_u32(w, imm);
}

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

/* Copy one cached host-register value into RDX for helper arguments. */
static bool emit_mov_rdx_hreg(rv64_jit_writer_t *w, rv64_jit_hreg_t hreg)
{
    const uint8_t src = jit_hreg_x86_reg(hreg);

    /* RDX is r/m field 2 in `mov rdx, hreg`. */
    return emit_rex64(w, src, 2) &&
           emit_u8(w, 0x89) &&
           emit_u8(w, jit_modrm(3, src, 2));
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
    regs->next_age = 1;

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

/* Find the host-register slot currently assigned to one guest register. */
static rv64_jit_reg_slot_t *jit_reg_find(rv64_jit_reg_cache_t *regs,
                                         uint32_t reg)
{
    for (uint32_t i = 0; i < RV64_JIT_HREG_COUNT; i++)
    {
        rv64_jit_reg_slot_t *slot = &regs->slots[i];

        if (slot->valid && slot->guest_reg == reg)
        {
            return slot;
        }
    }

    return NULL;
}

/* Emit a store-back for one dirty cached slot without changing metadata. */
static bool jit_reg_emit_flush_slot(rv64_jit_writer_t *w,
                                    const rv64_jit_reg_slot_t *slot)
{
    if (!slot->valid || !slot->loaded || !slot->dirty || slot->guest_reg == 0)
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

/* Flush every dirty cached guest register before helper-visible exits. */
static bool jit_reg_flush_all_dirty(rv64_jit_writer_t *w,
                                    rv64_jit_reg_cache_t *regs)
{
    for (uint32_t i = 0; i < RV64_JIT_HREG_COUNT; i++)
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
    for (uint32_t i = 0; i < RV64_JIT_HREG_COUNT; i++)
    {
        if (!jit_reg_emit_flush_slot(w, &regs->slots[i]))
        {
            return false;
        }
    }

    return true;
}

/* Select a free slot or the least-recently-used slot when all are occupied. */
static rv64_jit_reg_slot_t *jit_reg_choose_slot(rv64_jit_reg_cache_t *regs)
{
    rv64_jit_reg_slot_t *oldest = &regs->slots[0];

    for (uint32_t i = 0; i < RV64_JIT_HREG_COUNT; i++)
    {
        rv64_jit_reg_slot_t *slot = &regs->slots[i];

        if (!slot->valid)
        {
            return slot;
        }

        if (slot->age < oldest->age)
        {
            oldest = slot;
        }
    }

    return oldest;
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
    const bool spill = slot->valid && slot->loaded && slot->dirty && slot->guest_reg != 0;

    if (!jit_reg_flush_slot(w, slot))
    {
        return NULL;
    }

    if (spill)
    {
        JIT_STAT_INC(reg_cache_spills);
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

/* Materialise a guest register in RAX, treating x0 as constant zero. */
static bool jit_reg_read_rax(rv64_jit_writer_t *w,
                             rv64_jit_reg_cache_t *regs, uint32_t reg)
{
    if (reg == 0)
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
    if (reg == 0)
    {
        return emit_u8(w, 0x31) && emit_u8(w, 0xc9);
    }

    rv64_jit_reg_slot_t *slot = jit_reg_loaded_slot(w, regs, reg);
    return slot != NULL && emit_mov_rcx_hreg(w, slot->hreg);
}

/* Materialise a guest register in RDX, treating x0 as constant zero. */
static bool jit_reg_read_rdx(rv64_jit_writer_t *w,
                             rv64_jit_reg_cache_t *regs, uint32_t reg)
{
    if (reg == 0)
    {
        return emit_u8(w, 0x31) && emit_u8(w, 0xd2);
    }

    rv64_jit_reg_slot_t *slot = jit_reg_loaded_slot(w, regs, reg);
    return slot != NULL && emit_mov_rdx_hreg(w, slot->hreg);
}

/* Write the current RAX result into one guest-register cache slot. */
static bool jit_reg_write_rax(rv64_jit_writer_t *w,
                              rv64_jit_reg_cache_t *regs, uint32_t reg)
{
    if (reg == 0)
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
    if (reg == 0)
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
    if (dst_reg == 0)
    {
        return true;
    }

    if (src_reg == 0)
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
    int64_t rel = target - (disp + 4);
    Assert(rel >= INT32_MIN && rel <= INT32_MAX, "jit: rel32 target is out of range");
    int32_t rel32 = (int32_t)rel;
    memcpy(disp, &rel32, sizeof(rel32));
}

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
    Assert(*miss_count < RV64_JIT_DIRECT_LINK_MISS_PATCHES,
           "jit: RV64 direct-link miss patch list overflow");
    return emit_jcc_rel32_placeholder(w, jcc_opcode, &miss_disps[(*miss_count)++]);
}

/* Emit the conservative block exit used when cross-block direct links are off. */
bool rv64_jit_emit_plain_block_exit(rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs,
                                  vaddr_t target_pc, uint32_t completed_count)
{
    return jit_reg_emit_flush_all_dirty(w, regs) &&
           emit_store_pc_imm(w, target_pc) &&
           emit_return_total_retired(w, completed_count);
}

/* Emit a guarded call to a known-next-PC native block, otherwise return to C. */
bool rv64_jit_emit_direct_link_exit(rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs,
                                  vaddr_t target_pc, uint32_t completed_count,
                                  bool source_uses_data_state,
                                  uint64_t *extra_taken_counter)
{
    const word_t satp = cpu.csr.satp;
    const uint32_t ifetch_state = rv64_jit_ifetch_state();
    rv64_jit_block_t *target =
        rv64_jit_cache_slot_context(target_pc, satp, ifetch_state);
    uint8_t *miss_disps[RV64_JIT_DIRECT_LINK_MISS_PATCHES];
    uint32_t miss_count = 0;

    const uint32_t valid_off = (uint32_t)offsetof(rv64_jit_block_t, valid);
    const uint32_t translated_off = (uint32_t)offsetof(rv64_jit_block_t, translated);
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
    const uint32_t insn_count_off =
        (uint32_t)offsetof(rv64_jit_block_t, insn_count);
    const uint32_t body_entry_off =
        (uint32_t)offsetof(rv64_jit_block_t, body_entry);
    const uint32_t data_state = rv64_jit_data_tlb_state(MEM_TYPE_READ);
    uint8_t *data_state_ok_disp = NULL;
    uint8_t *ifetch_generation_ok_disp = NULL;

    /*
     * The source block itself has already been matched by the C dispatcher.
     * Direct links duplicate only the cheap part of that validation.  If a
     * translated target may need source-page revalidation, the generation guard
     * misses back to C so rv64_jit_block_matches() owns the full page walk.
     */
    if (!jit_reg_emit_flush_all_dirty(w, regs) ||
        !emit_movabs_rdx(w, (uint64_t)(uintptr_t)target) ||
        !emit_cmp_rdxb_field_imm8(w, valid_off, 1) ||
        !emit_direct_link_miss_jcc(w, HOST_JCC_NE, miss_disps, &miss_count) ||
        !emit_movabs_rax(w, target_pc) ||
        !emit_cmp_rdxq_field_rax(w, pc_off) ||
        !emit_direct_link_miss_jcc(w, HOST_JCC_NE, miss_disps, &miss_count) ||
        !emit_movabs_rax(w, satp) ||
        !emit_cmp_rdxq_field_rax(w, satp_off) ||
        !emit_direct_link_miss_jcc(w, HOST_JCC_NE, miss_disps, &miss_count) ||
        !emit_cmp_rdxd_field_imm32(w, ifetch_state_off, ifetch_state) ||
        !emit_direct_link_miss_jcc(w, HOST_JCC_NE, miss_disps, &miss_count) ||
        !emit_cmp_rdxb_field_imm8(w, uses_data_state_off, 0))
    {
        return false;
    }

    if (source_uses_data_state)
    {
        if (!emit_jcc_rel32_placeholder(w, HOST_JCC_E, &data_state_ok_disp) ||
            !emit_cmp_rdxd_field_imm32(w, data_state_off, data_state) ||
            !emit_direct_link_miss_jcc(w, HOST_JCC_NE, miss_disps, &miss_count))
        {
            return false;
        }

        patch_rel32(data_state_ok_disp, w->cur);
    }
    else if (!emit_direct_link_miss_jcc(w, HOST_JCC_NE, miss_disps, &miss_count))
    {
        return false;
    }

    if (!emit_cmp_rdxb_field_imm8(w, translated_off, 0) ||
        !emit_jcc_rel32_placeholder(w, HOST_JCC_E, &ifetch_generation_ok_disp) ||
        !emit_movabs_rax(w, (uint64_t)(uintptr_t)&rv64_jit_ifetch_generation) ||
        !emit_mov_rax_m64_rax(w) ||
        !emit_cmp_rdxq_field_rax(w, ifetch_generation_off) ||
        !emit_direct_link_miss_jcc(w, HOST_JCC_NE, miss_disps, &miss_count))
    {
        return false;
    }

    patch_rel32(ifetch_generation_ok_disp, w->cur);

    if (!emit_cmp_rdxq_field_imm8(w, body_entry_off, 0) ||
        !emit_direct_link_miss_jcc(w, HOST_JCC_E, miss_disps, &miss_count) ||
        !emit_movabs_rdx(w, (uint64_t)(uintptr_t)&rv64_jit_loop_extra) ||
        !emit_mov_eax_m32_rdx(w) ||
        !emit_add_eax_imm32(w, completed_count) ||
        !emit_mov_ecx_eax(w) ||
        !emit_movabs_rdx(w, (uint64_t)(uintptr_t)target) ||
        !emit_add_ecx_rdxd_field(w, insn_count_off) ||
        !emit_movabs_rdx(w, (uint64_t)(uintptr_t)&rv64_jit_entry_budget) ||
        !emit_cmp_ecx_m32_rdx(w) ||
        !emit_direct_link_miss_jcc(w, HOST_JCC_A, miss_disps, &miss_count) ||
        !emit_movabs_rdx(w, (uint64_t)(uintptr_t)&rv64_jit_loop_extra) ||
        !emit_mov_m32_rdx_eax(w))
    {
        return false;
    }

#if RV64_JIT_STATS
    uint8_t *guarded_taken_disp = NULL;
    uint8_t *guarded_done_disp = NULL;

    if (!emit_movabs_rdx(w, (uint64_t)(uintptr_t)target) ||
        !emit_cmp_rdxb_field_imm8(w, translated_off, 0) ||
        !emit_jcc_rel32_placeholder(w, HOST_JCC_NE, &guarded_taken_disp) ||
        !emit_cmp_rdxb_field_imm8(w, uses_data_state_off, 0) ||
        !emit_jcc_rel32_placeholder(w, HOST_JCC_E, &guarded_done_disp))
    {
        return false;
    }

    patch_rel32(guarded_taken_disp, w->cur);

    if (!emit_inc_jit_stat_counter(w, &rv64_jit_stats.direct_guarded_link_taken_count))
    {
        return false;
    }

    patch_rel32(guarded_done_disp, w->cur);
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

    for (uint32_t i = 0; i < miss_count; i++)
    {
        patch_rel32(miss_disps[i], w->cur);
    }

    return emit_store_pc_imm(w, target_pc) &&
           emit_inc_jit_stat_counter(w, &rv64_jit_stats.direct_link_miss_count) &&
           emit_return_total_retired(w, completed_count);
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

    JIT_STAT_INC(native_loads);
    JIT_STAT_INC(native_paged_loads);
    JIT_STAT_INC(inline_paged_loads);
    return true;
}

/* Emit one guarded bare-mode RV64 load that falls back before unsafe accesses. */
bool rv64_jit_emit_load_instr(rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs,
                            uint32_t instr, vaddr_t pc,
                            uint32_t completed_count)
{
    const uint32_t rd = bits(instr, 11, 7);
    const uint32_t funct3 = bits(instr, 14, 12);
    const uint32_t rs1 = bits(instr, 19, 15);
    const int32_t imm = (int32_t)imm_i(instr);
    uint32_t len = 0;
    uintptr_t helper = 0;
    uint8_t *align_slow_disp = NULL;
    uint8_t *range_slow_disp = NULL;
    uint8_t *helper_done_disp = NULL;
    uint8_t *done_disp = NULL;
    rv64_jit_reg_cache_t side_exit_regs;

    switch (funct3)
    {
    case RV64_FUNCT3_LB:
        helper = (uintptr_t)rv64_jit_load_i8;
        len = 1;
        break;
    case RV64_FUNCT3_LBU:
        helper = (uintptr_t)rv64_jit_load_u8;
        len = 1;
        break;
    case RV64_FUNCT3_LH:
        helper = (uintptr_t)rv64_jit_load_i16;
        len = 2;
        break;
    case RV64_FUNCT3_LHU:
        helper = (uintptr_t)rv64_jit_load_u16;
        len = 2;
        break;
    case RV64_FUNCT3_LW:
        helper = (uintptr_t)rv64_jit_load_i32;
        len = 4;
        break;
    case RV64_FUNCT3_LWU:
        helper = (uintptr_t)rv64_jit_load_u32;
        len = 4;
        break;
    case RV64_FUNCT3_LD:
        helper = (uintptr_t)rv64_jit_load_u64;
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
    if ((cpu.csr.satp >> RV64_JIT_SATP_MODE_SHIFT) != 0)
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

    if (!emit_mov_rdx_rax(w) ||
        !emit_guard_bare_address_in_pmem(w, len, &range_slow_disp) ||
        !emit_direct_pmem_load_rax(w, funct3) ||
        !jit_reg_write_rax(w, regs, rd) ||
        !emit_jmp_rel32_placeholder(w, &done_disp))
    {
        return false;
    }

    patch_rel32(range_slow_disp, w->cur);

    /*
     * An aligned out-of-PMEM bare-mode load may be MMIO.  Call the architectural
     * helper and continue so device callbacks still run in order without forcing
     * every polling loop back through the interpreter.
     */
    if (!jit_reg_emit_flush_all_dirty(w, regs) ||
        !emit_mov_rdi_rax(w) ||
        !emit_store_pc_imm(w, pc) ||
        !emit_call_abs(w, helper) ||
        !emit_load_jit_bases(w) ||
        !jit_reg_write_rax(w, regs, rd) ||
        !emit_jmp_rel32_placeholder(w, &helper_done_disp))
    {
        return false;
    }

    if (align_slow_disp != NULL)
    {
        patch_rel32(align_slow_disp, w->cur);

        if (!emit_interpreter_side_exit(w, &side_exit_regs, pc, completed_count,
                                        RV64_JIT_SIDE_EXIT_LOAD_GUARD))
        {
            return false;
        }
    }

    patch_rel32(done_disp, w->cur);
    patch_rel32(helper_done_disp, w->cur);
    JIT_STAT_INC(native_loads);
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
     * Every miss or sensitive write uses the old helper-and-exit path, so stale
     * translations and self-modifying code are still observed before the next
     * native block lookup.
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
        !emit_call_abs(w, (uintptr_t)rv64_jit_store_vaddr) ||
        !emit_load_jit_bases(w) ||
        !emit_store_pc_imm(w, next_pc) ||
        !emit_inc_jit_stat_counter(w,
                                   &rv64_jit_stats.side_exit_by_reason[RV64_JIT_SIDE_EXIT_PAGED_STORE_HELPER]) ||
        !emit_return_total_retired(w, completed_count + 1u))
    {
        return false;
    }

    patch_rel32(fast_done_disp, w->cur);

    if (align_slow_disp != NULL)
    {
        /*
         * Only the pre-store alignment guard may enter this side exit.  A
         * successful inline store continues in native code, while a helper store
         * has already returned to the dispatcher after updating cpu.pc.
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

    JIT_STAT_INC(native_stores);
    JIT_STAT_INC(native_paged_stores);
    JIT_STAT_INC(inline_paged_stores);
    return true;
}

/* Emit one guarded bare-mode RV64 store that normally commits inline. */
bool rv64_jit_emit_store_instr(rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs,
                             uint32_t instr, vaddr_t pc,
                             vaddr_t next_pc, uint32_t completed_count)
{
    const uint32_t funct3 = bits(instr, 14, 12);
    const uint32_t rs1 = bits(instr, 19, 15);
    const uint32_t rs2 = bits(instr, 24, 20);
    const int32_t imm = (int32_t)imm_s(instr);
    uint32_t len = 0;
    uint8_t *align_slow_disp = NULL;
    uint8_t *range_slow_disp = NULL;
    uint8_t *cross_chunk_disp = NULL;
    uint8_t *source_chunk_disp = NULL;
    uint8_t *data_page_table_disp = NULL;
    uint8_t *ifetch_page_table_disp = NULL;
    uint8_t *exit_disp = NULL;
    uint8_t *direct_done_disp = NULL;
    uint8_t *continue_disp = NULL;
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

    if ((cpu.csr.satp >> RV64_JIT_SATP_MODE_SHIFT) != 0)
    {
        return emit_paged_store_instr(w, regs, rs1, rs2, imm, len, pc, next_pc,
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
    }

    patch_rel32(range_slow_disp, w->cur);

    if (!emit_interpreter_side_exit(w, &side_exit_regs, pc, completed_count,
                                    RV64_JIT_SIDE_EXIT_STORE_GUARD))
    {
        return false;
    }

    patch_rel32(direct_done_disp, w->cur);
    patch_rel32(continue_disp, w->cur);

    JIT_STAT_INC(native_stores);
    JIT_STAT_INC(native_store_continuations);
    return true;
}

/* Emit a helper-backed RV64M operation and keep compiling after the call. */
static bool emit_rv64m_via_pure_helper(rv64_jit_writer_t *w,
                                       rv64_jit_reg_cache_t *regs,
                                       uint32_t instr, uint32_t rd,
                                       uint32_t rs1, uint32_t rs2)
{
    /*
     * System V arguments are RDI, RSI, RDX. The helper returns the result in
     * RAX. Because a C call may clobber caller-saved R10/R11, reload both JIT
     * base registers before storing the result or emitting later PMEM accesses.
     */
    if (!jit_reg_read_rax(w, regs, rs1) ||
        !emit_mov_rdi_rax(w) ||
        !jit_reg_read_rdx(w, regs, rs2) ||
        !emit_mov_rsi_rdx(w) ||
        !emit_mov_edx_imm32(w, instr) ||
        !emit_call_abs(w, (uintptr_t)rv64_jit_m_result) ||
        !emit_load_jit_bases(w) ||
        !jit_reg_write_rax(w, regs, rd))
    {
        return false;
    }

    JIT_STAT_INC(native_m_ops);
    return true;
}

/* Emit a one-source integer op directly in the destination cache slot. */
static bool emit_op_imm_hreg(rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs,
                             uint32_t rd, uint32_t rs1, uint8_t subop,
                             int32_t imm)
{
    if (rd == 0)
    {
        return true;
    }

    if (rs1 == 0)
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
    if (rd == 0)
    {
        return true;
    }

    if (rs1 == 0)
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
    if (rd == 0)
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
    if (rd == 0)
    {
        return true;
    }

    if (rs1 == 0 || rs2 == 0)
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
    const uint32_t rd = bits(instr, 11, 7);
    const uint32_t funct3 = bits(instr, 14, 12);
    const uint32_t rs1 = bits(instr, 19, 15);
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
        if (bits(instr, 31, 26) != RV64_FUNCT6_SHIFT_LOGICAL)
        {
            return false;
        }

        return emit_shift_imm_hreg(w, regs, rd, rs1, HOST_SHIFT_GROUP_SAL,
                                   (uint8_t)bits(instr, 25, 20));
    case RV64_FUNCT3_SRL_SRA: /* SRLI/SRAI; funct6 selects logical versus arithmetic right shift. */
        if (bits(instr, 31, 26) == RV64_FUNCT6_SHIFT_LOGICAL)
        {
            return emit_shift_imm_hreg(w, regs, rd, rs1, HOST_SHIFT_GROUP_SHR,
                                       (uint8_t)bits(instr, 25, 20));
        }

        if (bits(instr, 31, 26) == RV64_FUNCT6_SHIFT_ARITH)
        {
            return emit_shift_imm_hreg(w, regs, rd, rs1, HOST_SHIFT_GROUP_SAR,
                                       (uint8_t)bits(instr, 25, 20));
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
    const uint32_t rd = bits(instr, 11, 7);
    const uint32_t funct3 = bits(instr, 14, 12);
    const uint32_t rs1 = bits(instr, 19, 15);
    const int32_t imm = (int32_t)imm_i(instr);

    switch (funct3)
    {
    case RV64_FUNCT3_ADD_SUB: /* ADDIW; EAX addition naturally drops to 32 bits, then CDQE. */
        return jit_reg_read_rax(w, regs, rs1) &&
               emit_u8(w, 0x05) && emit_u32(w, (uint32_t)imm) &&
               emit_u8(w, 0x48) && emit_u8(w, 0x98) &&
               jit_reg_write_rax(w, regs, rd);
    case RV64_FUNCT3_SLL: /* SLLIW; funct7 must be zero and shamt is five bits. */
        if (bits(instr, 31, 25) != RV64_FUNCT7_BASE)
        {
            return false;
        }

        return jit_reg_read_rax(w, regs, rs1) &&
               emit_shift_eax_imm_sext(
                   w, HOST_SHIFT_SAL, (uint8_t)bits(instr, 24, 20)) &&
               jit_reg_write_rax(w, regs, rd);
    case RV64_FUNCT3_SRL_SRA: /* SRLIW/SRAIW; funct7 distinguishes logical from arithmetic. */
        if (bits(instr, 31, 25) == RV64_FUNCT7_BASE)
        {
            return jit_reg_read_rax(w, regs, rs1) &&
                   emit_shift_eax_imm_sext(
                       w, HOST_SHIFT_SHR,
                       (uint8_t)bits(instr, 24, 20)) &&
                   jit_reg_write_rax(w, regs, rd);
        }

        if (bits(instr, 31, 25) == RV64_FUNCT7_SUB_SRA)
        {
            return jit_reg_read_rax(w, regs, rs1) &&
                   emit_shift_eax_imm_sext(
                       w, HOST_SHIFT_SAR,
                       (uint8_t)bits(instr, 24, 20)) &&
                   jit_reg_write_rax(w, regs, rd);
        }

        return false;
    default:
        return false;
    }
}

/* Emit a 64-bit RV64 OP instruction for the integer ALU subset. */
static bool emit_op(rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs,
                    uint32_t instr)
{
    const uint32_t rd = bits(instr, 11, 7);
    const uint32_t funct3 = bits(instr, 14, 12);
    const uint32_t rs1 = bits(instr, 19, 15);
    const uint32_t rs2 = bits(instr, 24, 20);
    const uint32_t key = RV64_OP_KEY(bits(instr, 31, 25), funct3);

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
    case RV64_OP_KEY(RV64_FUNCT7_MULDIV, RV64_FUNCT3_ADD_SUB):
    {
        /* MUL: the low 64 product bits match x86-64 IMUL RAX, RCX. */
        const bool emitted =
            emit_u8(w, 0x48) && emit_u8(w, 0x0f) &&
            emit_u8(w, 0xaf) && emit_u8(w, 0xc1) &&
            jit_reg_write_rax(w, regs, rd);
        if (emitted)
        {
            JIT_STAT_INC(native_m_ops);
        }

        return emitted;
    }
    case RV64_OP_KEY(RV64_FUNCT7_MULDIV, RV64_FUNCT3_SLL): /* MULH */
    case RV64_OP_KEY(RV64_FUNCT7_MULDIV, RV64_FUNCT3_SLT): /* MULHSU */
    case RV64_OP_KEY(RV64_FUNCT7_MULDIV, RV64_FUNCT3_SLTU): /* MULHU */
    case RV64_OP_KEY(RV64_FUNCT7_MULDIV, RV64_FUNCT3_XOR): /* DIV */
    case RV64_OP_KEY(RV64_FUNCT7_MULDIV, RV64_FUNCT3_SRL_SRA): /* DIVU */
    case RV64_OP_KEY(RV64_FUNCT7_MULDIV, RV64_FUNCT3_OR): /* REM */
    case RV64_OP_KEY(RV64_FUNCT7_MULDIV, RV64_FUNCT3_AND): /* REMU */
        return emit_rv64m_via_pure_helper(w, regs, instr, rd, rs1, rs2);
    default:
        return false;
    }
}

/* Emit an RV64 OP-32 instruction and sign-extend the 32-bit result. */
static bool emit_op32(rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs,
                      uint32_t instr)
{
    const uint32_t rd = bits(instr, 11, 7);
    const uint32_t funct3 = bits(instr, 14, 12);
    const uint32_t rs1 = bits(instr, 19, 15);
    const uint32_t rs2 = bits(instr, 24, 20);
    const uint32_t key = RV64_OP_KEY(bits(instr, 31, 25), funct3);

    switch (key)
    {
    case RV64_OP_KEY(RV64_FUNCT7_BASE, RV64_FUNCT3_ADD_SUB): /* ADDW */
        if (rs1 != 0 && rs2 != 0)
        {
            return emit_op32_hreg_commutative(w, regs, rd, rs1, rs2,
                                              HOST_ALU_ADD, false);
        }
        break;
    case RV64_OP_KEY(RV64_FUNCT7_MULDIV, RV64_FUNCT3_ADD_SUB): /* MULW */
        if (rs1 != 0 && rs2 != 0)
        {
            const bool emitted = emit_op32_hreg_commutative(
                w, regs, rd, rs1, rs2, HOST_GROUP1_ADD, true);
            if (emitted)
            {
                JIT_STAT_INC(native_m_ops);
            }

            return emitted;
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
    case RV64_OP_KEY(RV64_FUNCT7_MULDIV, RV64_FUNCT3_ADD_SUB):
    {
        /* MULW: IMUL keeps the low 32 bits; CDQE sign-extends the result. */
        const bool emitted =
            emit_u8(w, 0x0f) && emit_u8(w, 0xaf) && emit_u8(w, 0xc1) &&
            emit_u8(w, 0x48) && emit_u8(w, 0x98) &&
            jit_reg_write_rax(w, regs, rd);
        if (emitted)
        {
            JIT_STAT_INC(native_m_ops);
        }

        return emitted;
    }
    case RV64_OP_KEY(RV64_FUNCT7_MULDIV, RV64_FUNCT3_XOR): /* DIVW */
    case RV64_OP_KEY(RV64_FUNCT7_MULDIV, RV64_FUNCT3_SRL_SRA): /* DIVUW */
    case RV64_OP_KEY(RV64_FUNCT7_MULDIV, RV64_FUNCT3_OR): /* REMW */
    case RV64_OP_KEY(RV64_FUNCT7_MULDIV, RV64_FUNCT3_AND): /* REMUW */
        return emit_rv64m_via_pure_helper(w, regs, instr, rd, rs1, rs2);
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
                                       const uint8_t *target_native)
{
    uint8_t *over_budget_disp = NULL;
    uint8_t *loop_disp = NULL;

    /*
     * The current lap has completed `exit_count` guest instructions at the
     * branch.  EAX becomes the total completed count including this lap.  ECX
     * then looks one more full lap ahead; only if that still fits
     * `rv64_jit_entry_budget` do we store EAX in `rv64_jit_loop_extra` and jump back to
     * the native loop body.  Otherwise, returning EAX keeps cpu_exec() budget
     * accounting exact.
     */
    if (!emit_movabs_rdx(w, (uint64_t)(uintptr_t)&rv64_jit_loop_extra) ||
        !emit_mov_eax_m32_rdx(w) ||
        !emit_add_eax_imm32(w, exit_count) ||
        !emit_mov_ecx_eax(w) ||
        !emit_add_ecx_imm32(w, exit_count) ||
        !emit_movabs_rdx(w, (uint64_t)(uintptr_t)&rv64_jit_entry_budget) ||
        !emit_cmp_ecx_m32_rdx(w) ||
        !emit_jcc_rel32_placeholder(w, HOST_JCC_A, &over_budget_disp) ||
        !jit_reg_emit_flush_all_dirty(w, regs) ||
        !emit_movabs_rdx(w, (uint64_t)(uintptr_t)&rv64_jit_loop_extra) ||
        !emit_mov_m32_rdx_eax(w) ||
        !emit_jmp_rel32_placeholder(w, &loop_disp))
    {
        return false;
    }

    patch_rel32(loop_disp, target_native);
    patch_rel32(over_budget_disp, w->cur);
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
                          bool *emitted_native_backedge,
                          uint32_t retired_including_current,
                          bool current_block_uses_data_translation_state)
{
    const uint32_t funct3 = bits(instr, 14, 12);
    const uint32_t rs1 = bits(instr, 19, 15);
    const uint32_t rs2 = bits(instr, 24, 20);
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

    if (!jit_reg_read_rax(w, regs, rs1) ||
        !jit_reg_read_rcx(w, regs, rs2) ||
        !emit_cmp_rax_rcx(w) ||
        !emit_jcc_rel32_placeholder(w, inverse_jcc, &fallthrough_disp))
    {
        return false;
    }

    if (can_chain_self_backedge && target == block_start_pc)
    {
        if (!emit_branch_chain_backedge(w, regs, target,
                                        retired_including_current,
                                        native_body_entry))
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
    return true;
}

/* Emit JAL or JALR, both of which end the current native block. */
bool rv64_jit_emit_jump_instr(rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs,
                            uint32_t instr, vaddr_t pc,
                            uint32_t completed_count,
                            bool source_uses_data_state)
{
    const uint32_t opcode = instr & RV64_OPCODE_MASK;
    const uint32_t rd = bits(instr, 11, 7);
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
            JIT_STAT_INC(native_jumps);
        }

        return emitted;
    }

    if (opcode != RV64_OPCODE_JALR || bits(instr, 14, 12) != 0)
    {
        return false;
    }

    /*
     * JALR computes `(rs1 + imm) & ~1`, then checks instruction alignment after
     * clearing bit zero.  The misaligned case returns before JALR executes so
     * the interpreter raises the same trap and does not write the link register.
     */
    if (!jit_reg_read_rax(w, regs, bits(instr, 19, 15)) ||
        !emit_add_rax_imm32(w, (int32_t)imm_i(instr)) ||
        !emit_and_rax_imm32(w, -2) ||
        !emit_test_al_imm8(w, RV64_IALIGN_MASK) ||
        !emit_jcc_rel32_placeholder(w, HOST_JCC_NE, &misaligned_disp))
    {
        return false;
    }

    side_exit_regs = *regs;

    if (!emit_mov_rcx_rax(w) ||
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

    JIT_STAT_INC(native_jumps);
    return true;
}

/* Dispatch one supported non-branch RISC-V instruction to the native emitter. */
bool rv64_jit_emit_instr(rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs,
                       uint32_t instr, vaddr_t pc,
                       uint32_t exit_count)
{
    const uint32_t opcode = instr & RV64_OPCODE_MASK;
    const uint32_t rd = bits(instr, 11, 7);

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
