#include <generated/autoconf.h>

#ifndef CONFIG_RV64

#include "jit-rv32-internal.h"

#ifdef CONFIG_RISCV_FPU
#include "local-include/fpu.h"
#endif

/*
 * RV32 JIT emitter layer.
 *
 * Reading map:
 *   1. x86-64 byte writing and generated-code ABI primitives;
 *   2. compile-time guest-register caching and RV32M helpers;
 *   3. Bare/Sv32 guarded memory operations and strict trap exits;
 *   4. branches, jumps, integer ALU operations, and F/D helper sites.
 *
 * Helpers named `emit_*` write x86-64 bytes. The exported `rv32_jit_emit_*`
 * functions compose those primitives into complete guest instructions.
 */

/*
 * Deliver a strict RISC-V trap from generated code.
 *
 * The native block calls this only after it has flushed earlier dirty guest
 * registers and before it performs the trapping instruction's destination
 * register write or memory write.  That preserves the same ordering as the
 * interpreter helpers: previous instructions are visible, the faulting memory
 * operation has no side effect, and mepc/mtval identify the instruction and
 * effective address that caused the trap.
 */
static void jit_raise_trap_tval(uint32_t cause, vaddr_t pc, vaddr_t tval)
{
    cpu.pc = isa_raise_intr_tval((word_t)cause, pc, (word_t)tval);
    difftest_skip_ref();
}

/*
 * Execute RV32M operations that are uncommon or awkward to emit inline.
 *
 * The helper decodes the already-fetched OP instruction, reads the architectural
 * registers from `cpu.gpr[]`, applies exact RISC-V divide/remainder edge cases,
 * writes rd when it is not x0, and returns the value for callers that also want
 * to seed the register cache from EAX after the helper call.
 */
static uint32_t jit_op_complex(uint32_t instr)
{
    JIT_STAT_INC(helper_complex_ops);

    const uint32_t rd = bits(instr, 11, 7);
    const uint32_t funct3 = bits(instr, 14, 12);
    const uint32_t rs1 = bits(instr, 19, 15);
    const uint32_t rs2 = bits(instr, 24, 20);
    const uint32_t funct7 = bits(instr, 31, 25);
    const uint32_t key = (funct7 << 3) | funct3;
    const uint32_t lhs = gpr(rs1);
    const uint32_t rhs = gpr(rs2);
    uint32_t out = 0;

    switch (key)
    {
    case 0x009:
        out = (uint32_t)(((int64_t)(int32_t)lhs * (int64_t)(int32_t)rhs) >> 32);
        break;
    case 0x00a:
        /*
         * MULHSU is signed(rs1) * unsigned(rs2). The product still fits in a
         * signed 64-bit value because both operands are 32-bit wide.
         */
        out = (uint32_t)(((int64_t)(int32_t)lhs * (int64_t)(uint64_t)rhs) >> 32);
        break;
    case 0x00b:
        out = (uint32_t)(((uint64_t)lhs * (uint64_t)rhs) >> 32);
        break;
    case 0x00c:
        out = (rhs == 0) ? UINT32_MAX : ((int32_t)lhs == INT32_MIN && (int32_t)rhs == -1 ? lhs : (uint32_t)((int32_t)lhs / (int32_t)rhs));
        break;
    case 0x00d:
        out = rhs == 0 ? UINT32_MAX : lhs / rhs;
        break;
    case 0x00e:
        out = (rhs == 0) ? lhs : ((int32_t)lhs == INT32_MIN && (int32_t)rhs == -1 ? 0 : (uint32_t)((int32_t)lhs % (int32_t)rhs));
        break;
    case 0x00f:
        out = rhs == 0 ? lhs : lhs % rhs;
        break;
    default:
        panic("jit: unsupported complex OP instruction 0x%08x", instr);
    }

    if (rd != RISCV_GPR_ZERO)
    {
        gpr(rd) = out;
    }

    return out;
}

/* Emit one raw x86-64 byte into the current writer. */
static bool emit_u8(rv32_jit_writer_t *w, uint8_t value)
{
    /*
     * All x86-64 emitters are written as boolean builders. A false result means
     * "do not publish this block"; callers either roll back to a known boundary or
     * abandon the translation before the cache entry becomes executable.
     */

    if (w->cur >= w->end)
    {
        return false;
    }

    *w->cur++ = value;
    return true;
}

/* Emit a 32-bit little-endian immediate or displacement. */
static bool emit_u32(rv32_jit_writer_t *w, uint32_t value)
{
    if ((size_t)(w->end - w->cur) < sizeof(value))
    {
        return false;
    }

    memcpy(w->cur, &value, sizeof(value));
    w->cur += sizeof(value);
    return true;
}

/* Emit a 64-bit little-endian immediate, mainly for movabs addresses. */
static bool emit_u64(rv32_jit_writer_t *w, uint64_t value)
{
    if ((size_t)(w->end - w->cur) < sizeof(value))
    {
        return false;
    }

    memcpy(w->cur, &value, sizeof(value));
    w->cur += sizeof(value);
    return true;
}

/* Emit `movabs r11, imm64`; r11 is this JIT's CPU-state base register. */
static bool emit_movabs_r11(rv32_jit_writer_t *w, uint64_t value)
{
    /* movabs r11, imm64 */
    return emit_u8(w, 0x49) && emit_u8(w, 0xbb) && emit_u64(w, value);
}

/* Load the address of global `cpu` into r11 for later `[r11 + offset]` access. */
static bool emit_load_cpu_base(rv32_jit_writer_t *w)
{
    /*
     * Generated blocks keep &cpu in r11 across straight-line code. Helper calls
     * use the host ABI and may clobber caller-saved registers, so call sites
     * reload r11 before continuing to access guest state.
     */
    return emit_movabs_r11(w, (uint64_t)(uintptr_t)&cpu);
}

/* Store a known immediate guest PC into `cpu.pc`. */
static bool emit_set_pc_imm(rv32_jit_writer_t *w, vaddr_t pc)
{
    const uint32_t off = (uint32_t)offsetof(CPU_state, pc);

    /* mov dword ptr [r11 + pc_off], imm32 */
    return emit_u8(w, 0x41) && emit_u8(w, 0xc7) && emit_u8(w, 0x83) && emit_u32(w, off) && emit_u32(w, pc);
}

/* Store EAX into `cpu.pc`, used after generated code computes a jump target. */
static bool emit_store_pc_eax(rv32_jit_writer_t *w)
{
    const uint32_t off = (uint32_t)offsetof(CPU_state, pc);

    /* mov dword ptr [r11 + pc_off], eax */
    return emit_u8(w, 0x41) && emit_u8(w, 0x89) && emit_u8(w, 0x83) && emit_u32(w, off);
}

/* Put a 32-bit immediate result into EAX, the normal temporary result register. */
static bool emit_mov_eax_imm(rv32_jit_writer_t *w, uint32_t value)
{
    return emit_u8(w, 0xb8) && emit_u32(w, value);
}

/* Add an RV32 immediate or address offset to EAX, using the short form when safe. */
static bool emit_add_eax_imm(rv32_jit_writer_t *w, uint32_t value)
{
    const int32_t signed_value = (int32_t)value;

    if (signed_value == 0)
    {
        return true;
    }

    if (signed_value >= INT8_MIN && signed_value <= INT8_MAX)
    {
        /* add eax, imm8; x86 sign-extends imm8, which matches RV32 immediates. */
        return emit_u8(w, 0x83) && emit_u8(w, 0xc0) && emit_u8(w, (uint8_t)signed_value);
    }

    return emit_u8(w, 0x05) && emit_u32(w, value);
}

/* Add an immediate to ECX, used by generated loop-budget checks. */
static bool emit_add_ecx_imm(rv32_jit_writer_t *w, uint32_t value)
{
    const int32_t signed_value = (int32_t)value;

    if (signed_value == 0)
    {
        return true;
    }

    if (signed_value >= INT8_MIN && signed_value <= INT8_MAX)
    {
        return emit_u8(w, 0x83) && emit_u8(w, 0xc1) && emit_u8(w, (uint8_t)signed_value);
    }

    return emit_u8(w, 0x81) && emit_u8(w, 0xc1) && emit_u32(w, value);
}

/* Compare EAX with an immediate so a following setcc/jcc can consume the flags. */
static bool emit_cmp_eax_imm(rv32_jit_writer_t *w, uint32_t value)
{
    return emit_u8(w, 0x3d) && emit_u32(w, value);
}

/* Compare EAX with ECX for register-register branches and SLT-style results. */
static bool emit_cmp_eax_ecx(rv32_jit_writer_t *w)
{
    return emit_u8(w, 0x39) && emit_u8(w, 0xc8);
}

/* Compare ECX with a sign-extended 8-bit immediate, used by RV32M guards. */
static bool emit_cmp_ecx_imm8(rv32_jit_writer_t *w, uint8_t value)
{
    return emit_u8(w, 0x83) && emit_u8(w, 0xf9) && emit_u8(w, value);
}

/* Test whether ECX is zero without modifying ECX. */
static bool emit_test_ecx_ecx(rv32_jit_writer_t *w)
{
    return emit_u8(w, 0x85) && emit_u8(w, 0xc9);
}

/* Test whether EAX is zero without modifying the helper return value. */
static bool emit_test_eax_eax(rv32_jit_writer_t *w)
{
    return emit_u8(w, 0x85) && emit_u8(w, 0xc0);
}

/* Test selected low address bits without modifying EAX. */
static bool emit_test_eax_imm(rv32_jit_writer_t *w, uint32_t value)
{
    /* test eax, imm32 */
    return emit_u8(w, 0xa9) && emit_u32(w, value);
}

/* Save a guest virtual address from EAX into ECX before inline guards clobber it. */
static bool emit_mov_ecx_eax(rv32_jit_writer_t *w)
{
    return emit_u8(w, 0x89) && emit_u8(w, 0xc1);
}

/* Load one 32-bit value through RDX into EAX. */
static bool emit_mov_eax_m32_rdx(rv32_jit_writer_t *w)
{
    return emit_u8(w, 0x8b) && emit_u8(w, 0x02);
}

/* Store EAX through RDX. */
static bool emit_mov_m32_rdx_eax(rv32_jit_writer_t *w)
{
    return emit_u8(w, 0x89) && emit_u8(w, 0x02);
}

/* Compare ECX against one 32-bit value loaded through RDX. */
static bool emit_cmp_ecx_m32_rdx(rv32_jit_writer_t *w)
{
    return emit_u8(w, 0x3b) && emit_u8(w, 0x0a);
}

/* Restore a saved guest virtual address from ECX into EAX for helper fallback. */
static bool emit_mov_eax_ecx(rv32_jit_writer_t *w)
{
    return emit_u8(w, 0x89) && emit_u8(w, 0xc8);
}

/* Save a store guest virtual address from EAX into EDI for helper fallback. */
static bool emit_mov_edi_eax(rv32_jit_writer_t *w)
{
    return emit_u8(w, 0x89) && emit_u8(w, 0xc7);
}

/* Load an immediate into EDI, the first System V integer argument register. */
static bool emit_mov_edi_imm(rv32_jit_writer_t *w, uint32_t value)
{
    return emit_u8(w, 0xbf) && emit_u32(w, value);
}

/* Load an immediate into ESI, the second System V integer argument register. */
static bool emit_mov_esi_imm(rv32_jit_writer_t *w, uint32_t value)
{
    return emit_u8(w, 0xbe) && emit_u32(w, value);
}

/* Copy EAX into EDX for address arithmetic. */
static bool emit_mov_edx_eax(rv32_jit_writer_t *w)
{
    return emit_u8(w, 0x89) && emit_u8(w, 0xc2);
}

/* Shift EDX right by an immediate count. */
static bool emit_shr_edx_imm(rv32_jit_writer_t *w, uint8_t value)
{
    return emit_u8(w, 0xc1) && emit_u8(w, 0xea) && emit_u8(w, value);
}

/* Mask EAX with an immediate, used to keep the 4 KiB page offset. */
static bool emit_and_eax_imm(rv32_jit_writer_t *w, uint32_t value)
{
    return emit_u8(w, 0x25) && emit_u32(w, value);
}

/* OR EAX into EDX, combining a translated page base with the page offset. */
static bool emit_or_edx_eax(rv32_jit_writer_t *w)
{
    return emit_u8(w, 0x09) && emit_u8(w, 0xc2);
}

/* Subtract an immediate from EDX, normally CONFIG_MBASE from a physical address. */
static bool emit_sub_edx_imm(rv32_jit_writer_t *w, uint32_t value)
{
    return emit_u8(w, 0x81) && emit_u8(w, 0xea) && emit_u32(w, value);
}

/* Shift R8 left by an immediate count; R8 holds a JIT TLB entry offset. */
static bool emit_shl_r8_imm(rv32_jit_writer_t *w, uint8_t value)
{
    return emit_u8(w, 0x49) && emit_u8(w, 0xc1) && emit_u8(w, 0xe0) && emit_u8(w, value);
}

/* Emit `movabs rdx, imm64` for global table addresses. */
static bool emit_movabs_rdx(rv32_jit_writer_t *w, uint64_t value)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0xba) && emit_u64(w, value);
}

/* Emit `movabs rax, imm64` when a guard needs an untracked table base. */
static bool emit_movabs_rax(rv32_jit_writer_t *w, uint64_t value)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0xb8) && emit_u64(w, value);
}

/* Add RDX to R8, producing a pointer into the JIT TLB. */
static bool emit_add_r8_rdx(rv32_jit_writer_t *w)
{
    return emit_u8(w, 0x49) && emit_u8(w, 0x01) && emit_u8(w, 0xd0);
}

/* Clear EDX before unsigned x86 DIV, which consumes EDX:EAX as the dividend. */
static bool emit_xor_edx_edx(rv32_jit_writer_t *w)
{
    return emit_u8(w, 0x31) && emit_u8(w, 0xd2);
}

/* Sign-extend EAX into EDX:EAX before signed x86 IDIV. */
static bool emit_cdq(rv32_jit_writer_t *w)
{
    return emit_u8(w, 0x99);
}

/* Copy EDX into EAX, used for high multiply halves and remainders. */
static bool emit_mov_eax_edx(rv32_jit_writer_t *w)
{
    return emit_u8(w, 0x89) && emit_u8(w, 0xd0);
}

/* Emit unsigned multiply of EAX by ECX, producing EDX:EAX. */
static bool emit_mul_ecx(rv32_jit_writer_t *w)
{
    /* Unsigned edx:eax = eax * ecx. */
    return emit_u8(w, 0xf7) && emit_u8(w, 0xe1);
}

/* Emit signed multiply of EAX by ECX, producing EDX:EAX. */
static bool emit_imul_ecx(rv32_jit_writer_t *w)
{
    /* Signed edx:eax = eax * ecx. */
    return emit_u8(w, 0xf7) && emit_u8(w, 0xe9);
}

/* Emit unsigned divide of EDX:EAX by ECX after caller has guarded ECX != 0. */
static bool emit_div_ecx(rv32_jit_writer_t *w)
{
    /* Unsigned edx:eax / ecx, quotient in eax, remainder in edx. */
    return emit_u8(w, 0xf7) && emit_u8(w, 0xf1);
}

/* Emit signed divide of EDX:EAX by ECX after caller has guarded x86 trap cases. */
static bool emit_idiv_ecx(rv32_jit_writer_t *w)
{
    /* Signed edx:eax / ecx, quotient in eax, remainder in edx. */
    return emit_u8(w, 0xf7) && emit_u8(w, 0xf9);
}

/* Convert a condition-code result into RV32 boolean 0/1 in EAX. */
static bool emit_setcc_eax(rv32_jit_writer_t *w, uint8_t setcc_opcode)
{
    /* setcc al; movzx eax, al */
    return emit_u8(w, 0x0f) && emit_u8(w, setcc_opcode) && emit_u8(w, 0xc0) && emit_u8(w, 0x0f) && emit_u8(w, 0xb6) && emit_u8(w, 0xc0);
}

/* Emit a conditional rel32 branch and return the displacement byte location. */
static bool emit_jcc_rel32_placeholder(rv32_jit_writer_t *w, uint8_t jcc_opcode, uint8_t **disp)
{
    if (!emit_u8(w, 0x0f) || !emit_u8(w, jcc_opcode))
    {
        return false;
    }

    *disp = w->cur;
    return emit_u32(w, 0);
}

/* Emit an unconditional rel32 jump and return the displacement byte location. */
static bool emit_jmp_rel32_placeholder(rv32_jit_writer_t *w, uint8_t **disp)
{
    if (!emit_u8(w, 0xe9))
    {
        return false;
    }

    *disp = w->cur;
    return emit_u32(w, 0);
}

/* Patch a previously emitted rel32 displacement to jump to `target`. */
static void patch_rel32(uint8_t *disp, const uint8_t *target)
{
    /*
     * x86 relative branches are measured from the byte after the displacement.
     * The code arena is small enough that rel32 should always be sufficient; the
     * assertion catches accidental jumps outside the emitted block.
     */
    const int64_t rel = target - (disp + 4);
    Assert(rel >= INT32_MIN && rel <= INT32_MAX, "jit: x86 branch displacement out of range");
    const int32_t rel32 = (int32_t)rel;
    memcpy(disp, &rel32, sizeof(rel32));
}

/* Emit an absolute call through RAX, suitable for C helper function addresses. */
static bool emit_call_abs(rv32_jit_writer_t *w, uintptr_t func)
{
    /* movabs rax, func; call rax */
    return emit_u8(w, 0x48) && emit_u8(w, 0xb8) && emit_u64(w, (uint64_t)func) && emit_u8(w, 0xff) && emit_u8(w, 0xd0);
}

#if defined(CONFIG_RISCV_FPU) && RV32_JIT_STATS
/* Emit a native-side increment for one run-time FP edge counter. */
static bool emit_inc_fp_stat_counter(rv32_jit_writer_t *w, uint64_t *counter)
{
    /*
     * `48 ff 00` is `inc qword ptr [rax]`. The increment is deliberately
     * non-atomic because NEMU executes this CPU and its generated code on one
     * execution thread. RAX is safe scratch here because the FP outcome branch
     * has already consumed the helper's EAX result.
     */
    return emit_movabs_rax(w, (uint64_t)(uintptr_t)counter) && emit_u8(w, 0x48) && emit_u8(w, 0xff) && emit_u8(w, 0x00);
}
#endif

/* Map one JIT host-register enum value to the x86 register number encoding. */
static uint8_t jit_hreg_x86_reg(rv32_jit_hreg_t hreg)
{
    switch (hreg)
    {
    case RV32_JIT_HREG_RBX:
        return 3;
    case RV32_JIT_HREG_R12:
        return 12;
    case RV32_JIT_HREG_R13:
        return 13;
    case RV32_JIT_HREG_R14:
        return 14;
    case RV32_JIT_HREG_R15:
        return 15;
    default:
        Assert(0, "jit: invalid host register slot %d", hreg);
    }

    return 3;
}

/* Build an x86 ModRM byte from its three logical fields. */
static uint8_t jit_modrm(uint8_t mod, uint8_t reg, uint8_t rm)
{
    return (uint8_t)((mod << 6) | ((reg & 7u) << 3) | (rm & 7u));
}

/* Emit a REX prefix only when a 32-bit instruction references r8-r15. */
static bool emit_rex32_if_needed(rv32_jit_writer_t *w, uint8_t reg, uint8_t rm)
{
    uint8_t rex = 0x40;

    if ((reg & 8u) != 0)
    {
        rex |= 0x04;
    }

    if ((rm & 8u) != 0)
    {
        rex |= 0x01;
    }

    return rex == 0x40 || emit_u8(w, rex);
}

/* Save all callee-saved host registers that this JIT uses as cache slots. */
static bool emit_push_saved_hregs(rv32_jit_writer_t *w)
{
    return emit_u8(w, 0x53) && emit_u8(w, 0x41) && emit_u8(w, 0x54) && emit_u8(w, 0x41) && emit_u8(w, 0x55) && emit_u8(w, 0x41) && emit_u8(w, 0x56) &&
           emit_u8(w, 0x41) && emit_u8(w, 0x57);
}

/* Restore host registers in the opposite order of emit_push_saved_hregs(). */
static bool emit_pop_saved_hregs(rv32_jit_writer_t *w)
{
    return emit_u8(w, 0x41) && emit_u8(w, 0x5f) && emit_u8(w, 0x41) && emit_u8(w, 0x5e) && emit_u8(w, 0x41) && emit_u8(w, 0x5d) && emit_u8(w, 0x41) &&
           emit_u8(w, 0x5c) && emit_u8(w, 0x5b);
}

/* Load `cpu.gpr[reg]` into one cached host register. */
static bool emit_load_gpr_hreg(rv32_jit_writer_t *w, rv32_jit_hreg_t hreg, uint32_t reg)
{
    const uint32_t off = (uint32_t)offsetof(CPU_state, gpr) + reg * sizeof(cpu.gpr[0]);
    const uint8_t dst = jit_hreg_x86_reg(hreg);
    const uint8_t base = 11;

    /* mov hreg32, dword ptr [r11 + off] */
    return emit_rex32_if_needed(w, dst, base) && emit_u8(w, 0x8b) && emit_u8(w, jit_modrm(2, dst, base)) && emit_u32(w, off);
}

/* Store one cached host register back into `cpu.gpr[reg]`. */
static bool emit_store_gpr_hreg(rv32_jit_writer_t *w, uint32_t reg, rv32_jit_hreg_t hreg)
{
    const uint32_t off = (uint32_t)offsetof(CPU_state, gpr) + reg * sizeof(cpu.gpr[0]);
    const uint8_t src = jit_hreg_x86_reg(hreg);
    const uint8_t base = 11;

    /* mov dword ptr [r11 + off], hreg32 */
    return emit_rex32_if_needed(w, src, base) && emit_u8(w, 0x89) && emit_u8(w, jit_modrm(2, src, base)) && emit_u32(w, off);
}

/* Copy a cached host-register value into EAX for generic emitters. */
static bool emit_mov_eax_hreg(rv32_jit_writer_t *w, rv32_jit_hreg_t hreg)
{
    const uint8_t src = jit_hreg_x86_reg(hreg);

    /* mov eax, hreg32 */
    return emit_rex32_if_needed(w, src, 0) && emit_u8(w, 0x89) && emit_u8(w, jit_modrm(3, src, 0));
}

/* Copy a cached host-register value into ECX, often the second ALU operand. */
static bool emit_mov_ecx_hreg(rv32_jit_writer_t *w, rv32_jit_hreg_t hreg)
{
    const uint8_t src = jit_hreg_x86_reg(hreg);

    /* mov ecx, hreg32 */
    return emit_rex32_if_needed(w, src, 1) && emit_u8(w, 0x89) && emit_u8(w, jit_modrm(3, src, 1));
}

/* Copy the EAX temporary result into a cached host register. */
static bool emit_mov_hreg_eax(rv32_jit_writer_t *w, rv32_jit_hreg_t hreg)
{
    const uint8_t dst = jit_hreg_x86_reg(hreg);

    /* mov hreg32, eax */
    return emit_rex32_if_needed(w, 0, dst) && emit_u8(w, 0x89) && emit_u8(w, jit_modrm(3, 0, dst));
}

/* Copy one cached host register to another when rd and rs are different. */
static bool emit_mov_hreg_hreg(rv32_jit_writer_t *w, rv32_jit_hreg_t dst, rv32_jit_hreg_t src)
{
    const uint8_t dst_reg = jit_hreg_x86_reg(dst);
    const uint8_t src_reg = jit_hreg_x86_reg(src);

    if (dst == src)
    {
        return true;
    }

    /* mov dst32, src32 */
    return emit_rex32_if_needed(w, src_reg, dst_reg) && emit_u8(w, 0x89) && emit_u8(w, jit_modrm(3, src_reg, dst_reg));
}

/* Load a constant guest-register value into a cached host register. */
static bool emit_mov_hreg_imm(rv32_jit_writer_t *w, rv32_jit_hreg_t hreg, uint32_t value)
{
    const uint8_t dst = jit_hreg_x86_reg(hreg);

    /* mov hreg32, imm32 */
    return emit_rex32_if_needed(w, 0, dst) && emit_u8(w, 0xc7) && emit_u8(w, jit_modrm(3, 0, dst)) && emit_u32(w, value);
}

/* Copy ECX into a cached host register; retained for future ECX-result emitters. */
static bool __attribute__((unused)) emit_mov_hreg_ecx(rv32_jit_writer_t *w, rv32_jit_hreg_t hreg)
{
    const uint8_t dst = jit_hreg_x86_reg(hreg);

    /* mov hreg32, ecx */
    return emit_rex32_if_needed(w, 1, dst) && emit_u8(w, 0x89) && emit_u8(w, jit_modrm(3, 1, dst));
}

/* Initialise the per-block guest-register cache before emitting instructions. */
void rv32_jit_reg_cache_init(rv32_jit_reg_cache_t *regs)
{
    regs->next_age = 1;
    regs->source_refs_loaded = false;

    for (uint32_t i = 0; i < RV32_JIT_HREG_COUNT; i++)
    {
        regs->slots[i] = (rv32_jit_reg_slot_t){
            .valid = false,
            .loaded = false,
            .dirty = false,
            .guest_reg = 0,
            .age = 0,
            .hreg = (rv32_jit_hreg_t)i,
        };
    }
}

/* Roll back compile-time register-cache metadata after a failed emitter. */
void rv32_jit_reg_cache_restore(rv32_jit_reg_cache_t *regs, const rv32_jit_reg_cache_t *snapshot)
{
    *regs = *snapshot;
}

/* Find the host-register cache slot currently assigned to one guest register. */
static rv32_jit_reg_slot_t *jit_reg_find(rv32_jit_reg_cache_t *regs, uint32_t reg)
{
    for (uint32_t i = 0; i < RV32_JIT_HREG_COUNT; i++)
    {
        rv32_jit_reg_slot_t *slot = &regs->slots[i];

        if (slot->valid && slot->guest_reg == reg)
        {
            return slot;
        }
    }

    return NULL;
}

/* Emit a store-back for one dirty slot without mutating compile-time metadata. */
static bool jit_reg_emit_flush_slot(rv32_jit_writer_t *w, const rv32_jit_reg_slot_t *slot)
{
    if (!slot->valid || !slot->loaded || !slot->dirty || slot->guest_reg == RISCV_GPR_ZERO)
    {
        return true;
    }

    return emit_store_gpr_hreg(w, slot->guest_reg, slot->hreg);
}

/* Flush one slot and mark it clean once the store-back bytes are emitted. */
static bool jit_reg_flush_slot(rv32_jit_writer_t *w, rv32_jit_reg_slot_t *slot)
{
    if (!jit_reg_emit_flush_slot(w, slot))
    {
        return false;
    }

    slot->dirty = false;
    return true;
}

/* Emit store-backs for all dirty slots while leaving their dirty bits unchanged. */
static bool __attribute__((unused)) jit_reg_emit_flush_all_dirty(rv32_jit_writer_t *w, const rv32_jit_reg_cache_t *regs)
{
    for (uint32_t i = 0; i < RV32_JIT_HREG_COUNT; i++)
    {
        if (!jit_reg_emit_flush_slot(w, &regs->slots[i]))
        {
            return false;
        }
    }

    return true;
}

/* Flush all dirty guest-register cache slots before helper calls or block exit. */
static bool jit_reg_flush_all_dirty(rv32_jit_writer_t *w, rv32_jit_reg_cache_t *regs)
{
    for (uint32_t i = 0; i < RV32_JIT_HREG_COUNT; i++)
    {
        if (!jit_reg_flush_slot(w, &regs->slots[i]))
        {
            return false;
        }
    }

    return true;
}

/* Forget all cached guest-register mappings after a helper may have changed CPU state. */
static void jit_reg_invalidate_all(rv32_jit_reg_cache_t *regs)
{
    for (uint32_t i = 0; i < RV32_JIT_HREG_COUNT; i++)
    {
        regs->slots[i].valid = false;
        regs->slots[i].loaded = false;
        regs->slots[i].dirty = false;
        regs->slots[i].guest_reg = 0;
        regs->slots[i].age = 0;
    }
}

/* Select a free slot, or the least-recently-used slot when all are occupied. */
static rv32_jit_reg_slot_t *jit_reg_choose_slot(rv32_jit_reg_cache_t *regs)
{
    rv32_jit_reg_slot_t *oldest = &regs->slots[0];

    for (uint32_t i = 0; i < RV32_JIT_HREG_COUNT; i++)
    {
        rv32_jit_reg_slot_t *slot = &regs->slots[i];

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

/* Reserve a host-register cache slot for a guest register, flushing if replaced. */
static rv32_jit_reg_slot_t *jit_reg_alloc(rv32_jit_writer_t *w, rv32_jit_reg_cache_t *regs, uint32_t reg)
{
    rv32_jit_reg_slot_t *slot = jit_reg_find(regs, reg);

    if (slot != NULL)
    {
        slot->age = regs->next_age++;
        return slot;
    }

    slot = jit_reg_choose_slot(regs);

    if (!jit_reg_flush_slot(w, slot))
    {
        return NULL;
    }

    slot->valid = true;
    slot->loaded = false;
    slot->dirty = false;
    slot->guest_reg = reg;
    slot->age = regs->next_age++;
    return slot;
}

/* Materialise a guest register in EAX, loading it into the cache if needed. */
static bool jit_reg_read_eax(rv32_jit_writer_t *w, rv32_jit_reg_cache_t *regs, uint32_t reg)
{
    if (reg == RISCV_GPR_ZERO)
    {
        return emit_mov_eax_imm(w, 0);
    }

    rv32_jit_reg_slot_t *slot = jit_reg_alloc(w, regs, reg);

    if (slot == NULL)
    {
        return false;
    }

    if (!slot->loaded)
    {
        if (!emit_load_gpr_hreg(w, slot->hreg, reg))
        {
            return false;
        }

        slot->loaded = true;
    }

    slot->age = regs->next_age++;
    return emit_mov_eax_hreg(w, slot->hreg);
}

/* Materialise a guest register in ECX, loading it into the cache if needed. */
static bool jit_reg_read_ecx(rv32_jit_writer_t *w, rv32_jit_reg_cache_t *regs, uint32_t reg)
{
    if (reg == RISCV_GPR_ZERO)
    {
        return emit_u8(w, 0x31) && emit_u8(w, 0xc9);
    }

    rv32_jit_reg_slot_t *slot = jit_reg_alloc(w, regs, reg);

    if (slot == NULL)
    {
        return false;
    }

    if (!slot->loaded)
    {
        if (!emit_load_gpr_hreg(w, slot->hreg, reg))
        {
            return false;
        }

        slot->loaded = true;
    }

    slot->age = regs->next_age++;
    return emit_mov_ecx_hreg(w, slot->hreg);
}

/* Write the current EAX result to a guest register cache slot. */
static bool jit_reg_write_eax(rv32_jit_writer_t *w, rv32_jit_reg_cache_t *regs, uint32_t reg)
{
    if (reg == RISCV_GPR_ZERO)
    {
        return true;
    }

    rv32_jit_reg_slot_t *slot = jit_reg_alloc(w, regs, reg);

    if (slot == NULL)
    {
        return false;
    }

    if (!emit_mov_hreg_eax(w, slot->hreg))
    {
        return false;
    }

    slot->loaded = true;
    slot->dirty = true;
    slot->age = regs->next_age++;
    return true;
}

/* Write a compile-time constant value to a guest register cache slot. */
static bool jit_reg_write_imm(rv32_jit_writer_t *w, rv32_jit_reg_cache_t *regs, uint32_t reg, uint32_t value)
{
    if (reg == RISCV_GPR_ZERO)
    {
        return true;
    }

    rv32_jit_reg_slot_t *slot = jit_reg_alloc(w, regs, reg);

    if (slot == NULL)
    {
        return false;
    }

    if (!emit_mov_hreg_imm(w, slot->hreg, value))
    {
        return false;
    }

    slot->loaded = true;
    slot->dirty = true;
    slot->age = regs->next_age++;
    return true;
}

/* Return a cache slot whose host register definitely contains the guest value. */
static rv32_jit_reg_slot_t *jit_reg_loaded_slot(rv32_jit_writer_t *w, rv32_jit_reg_cache_t *regs, uint32_t reg)
{
    rv32_jit_reg_slot_t *slot = jit_reg_alloc(w, regs, reg);

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

/* Mark a slot as containing a new value that must eventually be written back. */
static void jit_reg_mark_hreg_dirty(rv32_jit_reg_cache_t *regs, rv32_jit_reg_slot_t *slot)
{
    slot->loaded = true;
    slot->dirty = true;
    slot->age = regs->next_age++;
}

/* Emit a two-register x86 ALU operation directly between cached host registers. */
static bool emit_hreg_binop_hreg(rv32_jit_writer_t *w, uint8_t opcode, rv32_jit_hreg_t dst, rv32_jit_hreg_t src)
{
    const uint8_t dst_reg = jit_hreg_x86_reg(dst);
    const uint8_t src_reg = jit_hreg_x86_reg(src);

    /* opcode dst, src */
    return emit_rex32_if_needed(w, src_reg, dst_reg) && emit_u8(w, opcode) && emit_u8(w, jit_modrm(3, src_reg, dst_reg));
}

/* Emit an x86 ALU immediate operation against a cached host register. */
static bool emit_hreg_alu_imm(rv32_jit_writer_t *w, rv32_jit_hreg_t hreg, uint8_t subop, uint32_t imm)
{
    const uint8_t dst = jit_hreg_x86_reg(hreg);
    const int32_t simm = (int32_t)imm;

    if (simm >= INT8_MIN && simm <= INT8_MAX)
    {
        /* 83 /subop ib sign-extends the immediate, matching these RV32 values. */
        return emit_rex32_if_needed(w, subop, dst) && emit_u8(w, 0x83) && emit_u8(w, jit_modrm(3, subop, dst)) && emit_u8(w, (uint8_t)simm);
    }

    /* 81 /subop id against the cached host register. */
    return emit_rex32_if_needed(w, subop, dst) && emit_u8(w, 0x81) && emit_u8(w, jit_modrm(3, subop, dst)) && emit_u32(w, imm);
}

/* Emit an x86 shift by immediate against a cached host register. */
static bool emit_hreg_shift_imm(rv32_jit_writer_t *w, rv32_jit_hreg_t hreg, uint8_t subop, uint8_t amount)
{
    const uint8_t dst = jit_hreg_x86_reg(hreg);
    return emit_rex32_if_needed(w, subop, dst) && emit_u8(w, 0xc1) && emit_u8(w, jit_modrm(3, subop, dst)) && emit_u8(w, amount);
}

/* Emit an x86 shift by CL against a cached host register. */
static bool emit_hreg_shift_cl(rv32_jit_writer_t *w, rv32_jit_hreg_t hreg, uint8_t subop)
{
    const uint8_t dst = jit_hreg_x86_reg(hreg);
    return emit_rex32_if_needed(w, subop, dst) && emit_u8(w, 0xd3) && emit_u8(w, jit_modrm(3, subop, dst));
}

/* Apply an immediate ALU operation in place to a cached guest register. */
static bool jit_reg_apply_imm(rv32_jit_writer_t *w, rv32_jit_reg_cache_t *regs, uint32_t reg, uint8_t subop, uint32_t imm)
{
    rv32_jit_reg_slot_t *slot = jit_reg_loaded_slot(w, regs, reg);

    if (slot == NULL)
    {
        return false;
    }

    if (!emit_hreg_alu_imm(w, slot->hreg, subop, imm))
    {
        return false;
    }

    jit_reg_mark_hreg_dirty(regs, slot);
    return true;
}

/* Apply an immediate shift in place to a cached guest register. */
static bool jit_reg_apply_shift_imm(rv32_jit_writer_t *w, rv32_jit_reg_cache_t *regs, uint32_t reg, uint8_t subop, uint8_t amount)
{
    rv32_jit_reg_slot_t *slot = jit_reg_loaded_slot(w, regs, reg);

    if (slot == NULL)
    {
        return false;
    }

    if (!emit_hreg_shift_imm(w, slot->hreg, subop, amount))
    {
        return false;
    }

    jit_reg_mark_hreg_dirty(regs, slot);
    return true;
}

/* Apply a register-register ALU operation in place to a cached destination. */
static bool jit_reg_apply_reg(rv32_jit_writer_t *w, rv32_jit_reg_cache_t *regs, uint32_t dst_reg, uint32_t src_reg, uint8_t opcode)
{
    rv32_jit_reg_slot_t *dst = jit_reg_loaded_slot(w, regs, dst_reg);

    if (dst == NULL)
    {
        return false;
    }

    rv32_jit_reg_slot_t *src = jit_reg_loaded_slot(w, regs, src_reg);

    if (src == NULL)
    {
        return false;
    }

    if (!emit_hreg_binop_hreg(w, opcode, dst->hreg, src->hreg))
    {
        return false;
    }

    jit_reg_mark_hreg_dirty(regs, dst);
    return true;
}

/* Copy one guest register value to another using the host-register cache. */
static bool jit_reg_copy(rv32_jit_writer_t *w, rv32_jit_reg_cache_t *regs, uint32_t dst_reg, uint32_t src_reg)
{
    if (dst_reg == RISCV_GPR_ZERO)
    {
        return true;
    }

    if (src_reg == RISCV_GPR_ZERO)
    {
        return jit_reg_write_imm(w, regs, dst_reg, 0);
    }

    rv32_jit_reg_slot_t *src = jit_reg_loaded_slot(w, regs, src_reg);

    if (src == NULL)
    {
        return false;
    }

    if (dst_reg == src_reg)
    {
        return true;
    }

    rv32_jit_reg_slot_t *dst = jit_reg_alloc(w, regs, dst_reg);

    if (dst == NULL)
    {
        return false;
    }

    if (!emit_mov_hreg_hreg(w, dst->hreg, src->hreg))
    {
        return false;
    }

    jit_reg_mark_hreg_dirty(regs, dst);
    return true;
}

/* Apply a register-count shift in place, using ECX for the x86 CL count. */
static bool jit_reg_apply_shift_reg(rv32_jit_writer_t *w, rv32_jit_reg_cache_t *regs, uint32_t dst_reg, uint32_t src_reg, uint8_t subop)
{
    rv32_jit_reg_slot_t *dst = jit_reg_loaded_slot(w, regs, dst_reg);

    if (dst == NULL || !jit_reg_read_ecx(w, regs, src_reg))
    {
        return false;
    }

    if (!emit_hreg_shift_cl(w, dst->hreg, subop))
    {
        return false;
    }

    jit_reg_mark_hreg_dirty(regs, dst);
    return true;
}

/* Emit MULH or MULHU by taking the high 32 bits from x86 EDX. */
static bool emit_rv32_mul_high(rv32_jit_writer_t *w, rv32_jit_reg_cache_t *regs, uint32_t rd, bool is_signed)
{
    return (is_signed ? emit_imul_ecx(w) : emit_mul_ecx(w)) && emit_mov_eax_edx(w) && jit_reg_write_eax(w, regs, rd);
}

/* Emit RV32 DIVU, including the defined divide-by-zero all-ones result. */
static bool emit_rv32_divu(rv32_jit_writer_t *w, rv32_jit_reg_cache_t *regs, uint32_t rd)
{
    uint8_t *zero_disp = NULL;
    uint8_t *done_disp = NULL;

    /*
     * RISC-V division by zero is not a trap: DIVU returns all ones. x86 DIV would
     * fault, so emit an explicit zero-divisor side exit around the native divide.
     */

    if (!emit_test_ecx_ecx(w) || !emit_jcc_rel32_placeholder(w, 0x84, &zero_disp) || !emit_xor_edx_edx(w) || !emit_div_ecx(w) ||
        !emit_jmp_rel32_placeholder(w, &done_disp))
    {
        return false;
    }

    patch_rel32(zero_disp, w->cur);

    if (!emit_mov_eax_imm(w, UINT32_MAX))
    {
        return false;
    }

    patch_rel32(done_disp, w->cur);
    return jit_reg_write_eax(w, regs, rd);
}

/* Emit RV32 REMU, including the defined divide-by-zero dividend result. */
static bool emit_rv32_remu(rv32_jit_writer_t *w, rv32_jit_reg_cache_t *regs, uint32_t rd)
{
    uint8_t *done_disp = NULL;

    /*
     * REMU by zero returns the original dividend. EAX already contains rs1, so
     * the zero-divisor branch can skip the native divide and keep EAX unchanged.
     */

    if (!emit_test_ecx_ecx(w) || !emit_jcc_rel32_placeholder(w, 0x84, &done_disp) || !emit_xor_edx_edx(w) || !emit_div_ecx(w) || !emit_mov_eax_edx(w))
    {
        return false;
    }

    patch_rel32(done_disp, w->cur);
    return jit_reg_write_eax(w, regs, rd);
}

/* Emit RV32 DIV, guarding both x86 signed-divide trap cases first. */
static bool emit_rv32_div(rv32_jit_writer_t *w, rv32_jit_reg_cache_t *regs, uint32_t rd)
{
    uint8_t *zero_disp = NULL;
    uint8_t *normal_disp = NULL;
    uint8_t *overflow_disp = NULL;
    uint8_t *normal_done_disp = NULL;
    uint8_t *zero_done_disp = NULL;

    /*
     * x86 IDIV traps on zero divisors and on INT_MIN / -1. RISC-V defines both
     * cases, so guard them before using the native signed divide.
     */

    if (!emit_test_ecx_ecx(w) || !emit_jcc_rel32_placeholder(w, 0x84, &zero_disp) || !emit_cmp_eax_imm(w, (uint32_t)INT32_MIN) ||
        !emit_jcc_rel32_placeholder(w, 0x85, &normal_disp) || !emit_cmp_ecx_imm8(w, 0xff) || !emit_jcc_rel32_placeholder(w, 0x84, &overflow_disp))
    {
        return false;
    }

    patch_rel32(normal_disp, w->cur);

    if (!emit_cdq(w) || !emit_idiv_ecx(w) || !emit_jmp_rel32_placeholder(w, &normal_done_disp))
    {
        return false;
    }

    patch_rel32(zero_disp, w->cur);

    if (!emit_mov_eax_imm(w, UINT32_MAX) || !emit_jmp_rel32_placeholder(w, &zero_done_disp))
    {
        return false;
    }

    patch_rel32(overflow_disp, w->cur);

    if (!emit_mov_eax_imm(w, (uint32_t)INT32_MIN))
    {
        return false;
    }

    patch_rel32(normal_done_disp, w->cur);
    patch_rel32(zero_done_disp, w->cur);
    return jit_reg_write_eax(w, regs, rd);
}

/* Emit RV32 REM, including zero-divisor and INT_MIN / -1 edge cases. */
static bool emit_rv32_rem(rv32_jit_writer_t *w, rv32_jit_reg_cache_t *regs, uint32_t rd)
{
    uint8_t *zero_disp = NULL;
    uint8_t *normal_disp = NULL;
    uint8_t *overflow_disp = NULL;
    uint8_t *normal_done_disp = NULL;
    uint8_t *zero_done_disp = NULL;

    if (!emit_test_ecx_ecx(w) || !emit_jcc_rel32_placeholder(w, 0x84, &zero_disp) || !emit_cmp_eax_imm(w, (uint32_t)INT32_MIN) ||
        !emit_jcc_rel32_placeholder(w, 0x85, &normal_disp) || !emit_cmp_ecx_imm8(w, 0xff) || !emit_jcc_rel32_placeholder(w, 0x84, &overflow_disp))
    {
        return false;
    }

    patch_rel32(normal_disp, w->cur);

    if (!emit_cdq(w) || !emit_idiv_ecx(w) || !emit_mov_eax_edx(w) || !emit_jmp_rel32_placeholder(w, &normal_done_disp))
    {
        return false;
    }

    patch_rel32(zero_disp, w->cur);

    if (!emit_jmp_rel32_placeholder(w, &zero_done_disp))
    {
        return false;
    }

    patch_rel32(overflow_disp, w->cur);

    if (!emit_mov_eax_imm(w, 0))
    {
        return false;
    }

    patch_rel32(normal_done_disp, w->cur);
    patch_rel32(zero_done_disp, w->cur);
    return jit_reg_write_eax(w, regs, rd);
}

/* Emit `movabs r9, imm64`; r9 holds the source-chunk refcount table base. */
static bool emit_movabs_r9(rv32_jit_writer_t *w, uint64_t value)
{
    /* movabs r9, imm64 */
    return emit_u8(w, 0x49) && emit_u8(w, 0xb9) && emit_u64(w, value);
}

/* Emit `movabs r10, imm64`; r10 holds the host PMEM base pointer. */
static bool emit_movabs_r10(rv32_jit_writer_t *w, uint64_t value)
{
    /* movabs r10, imm64 */
    return emit_u8(w, 0x49) && emit_u8(w, 0xba) && emit_u64(w, value);
}

/* Load r10 with the host pointer corresponding to guest physical CONFIG_MBASE. */
static bool emit_load_pmem_base(rv32_jit_writer_t *w)
{
    /*
     * Direct-PMEM fast paths are common enough that loading this once per native
     * block is cheaper than repeating a movabs before every translated load or
     * store. r10 is caller-saved, so helper calls that rejoin the block reload it.
     */
    return emit_movabs_r10(w, (uint64_t)(uintptr_t)guest_to_host(CONFIG_MBASE));
}

/* Load r9 with the source-chunk refcount table base for store guards. */
static bool emit_load_source_refs_base(rv32_jit_writer_t *w)
{
    /*
     * r9 holds the source-chunk reference table for direct stores.  It is loaded
     * lazily because blocks with no stores do not need it, but once a store guard
     * has needed the table, later stores in the same straight-line block can reuse
     * the base instead of paying another movabs.
     */
    return emit_movabs_r9(w, (uint64_t)(uintptr_t)rv32_jit_source_chunk_refs);
}

/* Lazily load r9 once when the first direct-store source guard needs it. */
static bool jit_reg_ensure_source_refs_base(rv32_jit_writer_t *w, rv32_jit_reg_cache_t *regs)
{
    if (regs->source_refs_loaded)
    {
        return true;
    }

    regs->source_refs_loaded = true;
    return emit_load_source_refs_base(w);
}

/* Emit LEA that computes a PMEM offset in EDX from the guest address in EAX. */
static bool emit_lea_edx_eax_imm(rv32_jit_writer_t *w, uint32_t value)
{
    /*
     * lea edx, [rax + disp32] computes the low RV32 address bits in one
     * instruction. With disp32 = -CONFIG_MBASE it replaces mov edx,eax; sub edx,
     * CONFIG_MBASE in the direct-PMEM guard.
     */
    return emit_u8(w, 0x8d) && emit_u8(w, 0x90) && emit_u32(w, value);
}

/* Copy the PMEM offset from EDX to R8D for source-chunk calculations. */
static bool emit_mov_r8d_edx(rv32_jit_writer_t *w)
{
    return emit_u8(w, 0x41) && emit_u8(w, 0x89) && emit_u8(w, 0xd0);
}

/* Compare the computed PMEM offset in EDX with an immediate bound. */
static bool emit_cmp_edx_imm(rv32_jit_writer_t *w, uint32_t value)
{
    return emit_u8(w, 0x81) && emit_u8(w, 0xfa) && emit_u32(w, value);
}

/* Mask R8D with an immediate, used to test offset within a source chunk. */
static bool emit_and_r8d_imm(rv32_jit_writer_t *w, uint32_t value)
{
    return emit_u8(w, 0x41) && emit_u8(w, 0x81) && emit_u8(w, 0xe0) && emit_u32(w, value);
}

/* Compare R8D with an immediate during store source-chunk checks. */
static bool emit_cmp_r8d_imm(rv32_jit_writer_t *w, uint32_t value)
{
    return emit_u8(w, 0x41) && emit_u8(w, 0x81) && emit_u8(w, 0xf8) && emit_u32(w, value);
}

/* Shift R8D right to convert a PMEM byte offset into a chunk index. */
static bool emit_shr_r8d_imm(rv32_jit_writer_t *w, uint8_t value)
{
    return emit_u8(w, 0x41) && emit_u8(w, 0xc1) && emit_u8(w, 0xe8) && emit_u8(w, value);
}

/* Compare a byte field in the R8-pointed JIT TLB entry with an immediate. */
static bool emit_cmp_r8b_field_imm8(rv32_jit_writer_t *w, uint32_t offset, uint8_t value)
{
    Assert(offset <= INT8_MAX, "jit: TLB byte field offset is too large");
    return emit_u8(w, 0x41) && emit_u8(w, 0x80) && emit_u8(w, 0x78) && emit_u8(w, (uint8_t)offset) && emit_u8(w, value);
}

/* Compare a dword field in the R8-pointed JIT TLB entry with an immediate. */
static bool emit_cmp_r8d_field_imm32(rv32_jit_writer_t *w, uint32_t offset, uint32_t value)
{
    Assert(offset <= INT8_MAX, "jit: TLB dword field offset is too large");
    return emit_u8(w, 0x41) && emit_u8(w, 0x81) && emit_u8(w, 0x78) && emit_u8(w, (uint8_t)offset) && emit_u32(w, value);
}

/* Compare a dword field in the R8-pointed JIT TLB entry with EDX. */
static bool emit_cmp_r8d_field_edx(rv32_jit_writer_t *w, uint32_t offset)
{
    Assert(offset <= INT8_MAX, "jit: TLB dword field offset is too large");
    return emit_u8(w, 0x41) && emit_u8(w, 0x39) && emit_u8(w, 0x50) && emit_u8(w, (uint8_t)offset);
}

/* Test permission bits in a dword field in the R8-pointed JIT TLB entry. */
static bool emit_test_r8d_field_imm32(rv32_jit_writer_t *w, uint32_t offset, uint32_t value)
{
    Assert(offset <= INT8_MAX, "jit: TLB dword field offset is too large");
    return emit_u8(w, 0x41) && emit_u8(w, 0xf7) && emit_u8(w, 0x40) && emit_u8(w, (uint8_t)offset) && emit_u32(w, value);
}

/* Load a dword field from the R8-pointed JIT TLB entry into EDX. */
static bool emit_mov_edx_r8d_field(rv32_jit_writer_t *w, uint32_t offset)
{
    Assert(offset <= INT8_MAX, "jit: TLB dword field offset is too large");
    return emit_u8(w, 0x41) && emit_u8(w, 0x8b) && emit_u8(w, 0x50) && emit_u8(w, (uint8_t)offset);
}

/* Compare `rv32_jit_source_chunk_refs[r8d]` with zero inside generated code. */
static bool emit_cmp_source_chunk_ref_zero(rv32_jit_writer_t *w)
{
    /* cmp word ptr [r9 + r8 * 2], 0 */
    return emit_u8(w, 0x66) && emit_u8(w, 0x43) && emit_u8(w, 0x83) && emit_u8(w, 0x3c) && emit_u8(w, 0x41) && emit_u8(w, 0x00);
}

/* Compare `rv32_jit_tlb_pt_page_refs[r8d]` with zero inside generated code. */
static bool emit_cmp_pt_page_ref_zero(rv32_jit_writer_t *w)
{
    /*
     * Use RAX as an untracked table base so this guard does not disturb the lazy
     * R9 source-ref base used by store source-chunk checks elsewhere in the block.
     */
    return emit_movabs_rax(w, (uint64_t)(uintptr_t)rv32_jit_tlb_pt_page_refs)
           /* cmp word ptr [rax + r8 * 2], 0 */
           && emit_u8(w, 0x66) && emit_u8(w, 0x42) && emit_u8(w, 0x83) && emit_u8(w, 0x3c) && emit_u8(w, 0x40) && emit_u8(w, 0x00);
}

/*
 * Emit the generated-code guard for an inline PMEM access.
 *
 * Input: EAX contains the guest virtual address. Output on the fast path: EDX
 * contains the byte offset from CONFIG_MBASE. Slow-path branch placeholders are
 * recorded in `patch` so the caller can patch them after emitting the helper
 * path.
 */
static bool emit_direct_pmem_guard(rv32_jit_writer_t *w, uint32_t len, rv32_jit_pmem_guard_patch_t *patch)
{
    Assert(len >= 1 && len <= 4, "jit: unsupported direct PMEM width %u", len);

    /*
     * Keep the guard stricter than paddr_read(): it only accepts a complete
     * in-PMEM byte range. Any boundary, MMIO, paging, or wraparound case falls
     * back to the existing helper path.
     *
     * Blocks are tagged by satp and `rv32_jit_block_matches()` rejects a cached block
     * if satp changes. A block compiled in Bare mode can therefore omit the
     * runtime satp reload on every memory access; translated-mode blocks still
     * jump straight to the helper path.
     */

    if ((cpu.csr.satp & RV32_JIT_SATP_MODE_MASK) != RISCV_SATP_MODE_BARE)
    {
        if (!emit_jmp_rel32_placeholder(w, &patch->satp_slow_disp))
        {
            return false;
        }
    }

    return emit_lea_edx_eax_imm(w, 0u - (uint32_t)CONFIG_MBASE) && emit_cmp_edx_imm(w, (uint32_t)CONFIG_MSIZE - len) &&
           emit_jcc_rel32_placeholder(w, 0x87, &patch->range_slow_disp);
}

/* Patch every slow-path branch emitted by emit_direct_pmem_guard(). */
static void patch_direct_pmem_guard(const rv32_jit_pmem_guard_patch_t *patch, const uint8_t *slow_path)
{
    if (patch->satp_slow_disp != NULL)
    {
        patch_rel32(patch->satp_slow_disp, slow_path);
    }

    patch_rel32(patch->range_slow_disp, slow_path);
}

/* Emit the inline PMEM load variant selected by the RV32 load funct3 field. */
static bool emit_direct_pmem_load_eax(rv32_jit_writer_t *w, uint32_t funct3)
{
    /*
     * EDX is the PMEM offset produced by emit_direct_pmem_guard().  The native
     * loads below mirror the RV32 load family exactly: byte/halfword signedness is
     * encoded in the x86 instruction, while LW naturally writes a 32-bit result.
     */
    switch (funct3)
    {
    case 0x0:
        /* movsx eax, byte ptr [r10 + rdx] */
        return emit_u8(w, 0x41) && emit_u8(w, 0x0f) && emit_u8(w, 0xbe) && emit_u8(w, 0x04) && emit_u8(w, 0x12);
    case 0x1:
        /* movsx eax, word ptr [r10 + rdx] */
        return emit_u8(w, 0x41) && emit_u8(w, 0x0f) && emit_u8(w, 0xbf) && emit_u8(w, 0x04) && emit_u8(w, 0x12);
    case 0x2:
        /* mov eax, dword ptr [r10 + rdx] */
        return emit_u8(w, 0x41) && emit_u8(w, 0x8b) && emit_u8(w, 0x04) && emit_u8(w, 0x12);
    case 0x4:
        /* movzx eax, byte ptr [r10 + rdx] */
        return emit_u8(w, 0x41) && emit_u8(w, 0x0f) && emit_u8(w, 0xb6) && emit_u8(w, 0x04) && emit_u8(w, 0x12);
    case 0x5:
        /* movzx eax, word ptr [r10 + rdx] */
        return emit_u8(w, 0x41) && emit_u8(w, 0x0f) && emit_u8(w, 0xb7) && emit_u8(w, 0x04) && emit_u8(w, 0x12);
    default:
        return false;
    }
}

/* Emit one conservative fallback branch for the inline Sv32 load guard. */
static bool emit_tlb_load_slow_jcc(rv32_jit_writer_t *w, rv32_jit_tlb_load_patch_t *patch, uint8_t jcc_opcode)
{
    Assert(patch->count < sizeof(patch->slow_disps) / sizeof(patch->slow_disps[0]), "jit: too many paged-load slow-path branches");
    return emit_jcc_rel32_placeholder(w, jcc_opcode, &patch->slow_disps[patch->count++]);
}

/* Patch every fallback branch emitted by emit_paged_tlb_load_eax(). */
static void patch_tlb_load_guard(const rv32_jit_tlb_load_patch_t *patch, const uint8_t *slow_path)
{
    for (uint32_t i = 0; i < patch->count; i++)
    {
        patch_rel32(patch->slow_disps[i], slow_path);
    }
}

/*
 * Emit an inline Sv32 TLB-hit load.
 *
 * Input: EAX contains the guest virtual address.  On success, EAX contains the
 * loaded RV32 value.  ECX preserves the original guest address for every slow
 * branch, so the helper fallback can receive the exact same argument it did
 * before this fast path existed.
 */
static bool emit_paged_tlb_load_eax(rv32_jit_writer_t *w, uint32_t funct3, uint32_t len, rv32_jit_tlb_load_patch_t *patch)
{
    Assert(len >= 1 && len <= 4, "jit: unsupported paged load width %u", len);

    const uint32_t satp = cpu.csr.satp;
    const uint32_t valid_off = (uint32_t)offsetof(rv32_jit_tlb_entry_t, valid);
    const uint32_t satp_off = (uint32_t)offsetof(rv32_jit_tlb_entry_t, satp);
    const uint32_t vpn_off = (uint32_t)offsetof(rv32_jit_tlb_entry_t, vpn);
    const uint32_t perm_off = (uint32_t)offsetof(rv32_jit_tlb_entry_t, perm);
    const uint32_t pg_paddr_off = (uint32_t)offsetof(rv32_jit_tlb_entry_t, pg_paddr);

    /*
     * The index calculation is:
     *   vpn = vaddr >> 12
     *   entry = &rv32_jit_tlb[vpn & (RV32_JIT_TLB_SIZE - 1)]
     * The 32-byte entry size lets the generated code use a shift rather than a
     * host multiply.  If the C struct layout changes, the typedef assertion near
     * rv32_jit_tlb_entry_t fails at build time.
     */

    if (!emit_mov_ecx_eax(w) || !emit_mov_edx_eax(w) || !emit_shr_edx_imm(w, PAGE_SHIFT) || !emit_mov_r8d_edx(w) ||
        !emit_and_r8d_imm(w, RV32_JIT_TLB_SIZE - 1u) || !emit_shl_r8_imm(w, 5) || !emit_movabs_rdx(w, (uint64_t)(uintptr_t)rv32_jit_tlb) ||
        !emit_add_r8_rdx(w))
    {
        return false;
    }

    /*
     * Recompute VPN after loading the table base into RDX.  The generated block is
     * already tagged by satp, but checking the entry's satp as well protects the
     * direct-mapped JIT TLB from stale entries after address-space reuse.
     */
    return emit_mov_edx_eax(w) && emit_shr_edx_imm(w, PAGE_SHIFT) && emit_cmp_r8b_field_imm8(w, valid_off, 0) &&
           emit_tlb_load_slow_jcc(w, patch, 0x84) && emit_cmp_r8d_field_imm32(w, satp_off, satp) && emit_tlb_load_slow_jcc(w, patch, 0x85) &&
           emit_cmp_r8d_field_edx(w, vpn_off) && emit_tlb_load_slow_jcc(w, patch, 0x85) && emit_test_r8d_field_imm32(w, perm_off, RV32_JIT_PTE_R) &&
           emit_tlb_load_slow_jcc(w, patch, 0x84) && emit_and_eax_imm(w, PAGE_MASK) && emit_cmp_eax_imm(w, PAGE_SIZE - len) &&
           emit_tlb_load_slow_jcc(w, patch, 0x87) && emit_mov_edx_r8d_field(w, pg_paddr_off) && emit_or_edx_eax(w) &&
           emit_sub_edx_imm(w, (uint32_t)CONFIG_MBASE) && emit_direct_pmem_load_eax(w, funct3);
}

/*
 * Emit an inline Sv32 TLB-hit store address translation.
 *
 * Input: EAX contains the guest virtual address and ECX contains the store
 * value.  On success, EDX contains the PMEM byte offset for `[r10 + rdx]`, and
 * ECX is still the store value.  EDI keeps the original guest address for the
 * helper fallback, because EAX is free for guard table bases after translation.
 */
static bool emit_paged_tlb_store_offset_edx(rv32_jit_writer_t *w, uint32_t len, rv32_jit_tlb_load_patch_t *patch)
{
    Assert(len >= 1 && len <= 4, "jit: unsupported paged store width %u", len);

    const uint32_t satp = cpu.csr.satp;
    const uint32_t valid_off = (uint32_t)offsetof(rv32_jit_tlb_entry_t, valid);
    const uint32_t satp_off = (uint32_t)offsetof(rv32_jit_tlb_entry_t, satp);
    const uint32_t vpn_off = (uint32_t)offsetof(rv32_jit_tlb_entry_t, vpn);
    const uint32_t perm_off = (uint32_t)offsetof(rv32_jit_tlb_entry_t, perm);
    const uint32_t pg_paddr_off = (uint32_t)offsetof(rv32_jit_tlb_entry_t, pg_paddr);

    if (!emit_mov_edi_eax(w) || !emit_mov_edx_eax(w) || !emit_shr_edx_imm(w, PAGE_SHIFT) || !emit_mov_r8d_edx(w) ||
        !emit_and_r8d_imm(w, RV32_JIT_TLB_SIZE - 1u) || !emit_shl_r8_imm(w, 5) || !emit_movabs_rdx(w, (uint64_t)(uintptr_t)rv32_jit_tlb) ||
        !emit_add_r8_rdx(w))
    {
        return false;
    }

    return emit_mov_edx_eax(w) && emit_shr_edx_imm(w, PAGE_SHIFT) && emit_cmp_r8b_field_imm8(w, valid_off, 0) &&
           emit_tlb_load_slow_jcc(w, patch, 0x84) && emit_cmp_r8d_field_imm32(w, satp_off, satp) && emit_tlb_load_slow_jcc(w, patch, 0x85) &&
           emit_cmp_r8d_field_edx(w, vpn_off) && emit_tlb_load_slow_jcc(w, patch, 0x85) && emit_test_r8d_field_imm32(w, perm_off, RV32_JIT_PTE_W) &&
           emit_tlb_load_slow_jcc(w, patch, 0x84) && emit_and_eax_imm(w, PAGE_MASK) && emit_cmp_eax_imm(w, PAGE_SIZE - len) &&
           emit_tlb_load_slow_jcc(w, patch, 0x87) && emit_mov_edx_r8d_field(w, pg_paddr_off) && emit_or_edx_eax(w) &&
           emit_sub_edx_imm(w, (uint32_t)CONFIG_MBASE);
}

/* Emit an inline PMEM store from ECX using the selected byte width. */
static bool emit_direct_pmem_store_from_ecx(rv32_jit_writer_t *w, uint32_t len)
{
    /*
     * Stores use the low part of ECX so SB/SH truncate in the same way host_write()
     * does. The caller has already proved the final address is an in-PMEM byte
     * offset and checked source-code/page-table refs before taking this
     * continuation path.  That proof can come from Bare mode or from an Sv32 JIT
     * TLB hit.
     */
    switch (len)
    {
    case 1:
        /* mov byte ptr [r10 + rdx], cl */
        return emit_u8(w, 0x41) && emit_u8(w, 0x88) && emit_u8(w, 0x0c) && emit_u8(w, 0x12);
    case 2:
        /* mov word ptr [r10 + rdx], cx */
        return emit_u8(w, 0x66) && emit_u8(w, 0x41) && emit_u8(w, 0x89) && emit_u8(w, 0x0c) && emit_u8(w, 0x12);
    case 4:
        /* mov dword ptr [r10 + rdx], ecx */
        return emit_u8(w, 0x41) && emit_u8(w, 0x89) && emit_u8(w, 0x0c) && emit_u8(w, 0x12);
    default:
        return false;
    }
}

/*
 * Emit guards that decide whether an inline PMEM store can continue in-block.
 *
 * A direct store is safe to continue only when it stays within one source chunk
 * and that chunk has no compiled-code references. Otherwise the store must go
 * through the helper so exact invalidation happens before the next fetch.
 */
static bool emit_store_source_chunk_guard(rv32_jit_writer_t *w, rv32_jit_reg_cache_t *regs, uint32_t len, uint8_t **cross_chunk_disp,
                                          uint8_t **source_chunk_disp)
{
    Assert(len >= 1 && len <= 4, "jit: unsupported direct store width %u", len);

    /*
     * Direct continuing stores only handle one source-tracking chunk. Crossing a
     * chunk boundary is rare for byte/halfword/word stores, and the helper path
     * remains the conservative choice because it can perform exact invalidation
     * and return to cpu_exec() before the next guest fetch.
     */
    return emit_mov_r8d_edx(w) && emit_and_r8d_imm(w, RV32_JIT_SOURCE_CHUNK_MASK) && emit_cmp_r8d_imm(w, RV32_JIT_SOURCE_CHUNK_SIZE - len) &&
           emit_jcc_rel32_placeholder(w, 0x87, cross_chunk_disp) && emit_mov_r8d_edx(w) && emit_shr_r8d_imm(w, RV32_JIT_SOURCE_CHUNK_SHIFT) &&
           jit_reg_ensure_source_refs_base(w, regs) && emit_cmp_source_chunk_ref_zero(w) && emit_jcc_rel32_placeholder(w, 0x85, source_chunk_disp);
}

/* Emit a guard that keeps inline stores away from cached page-table pages. */
static bool emit_store_page_table_guard(rv32_jit_writer_t *w, uint8_t **page_table_disp)
{
    /*
     * EDX is a byte offset from CONFIG_MBASE.  Dividing by 4096 gives the PMEM
     * page index used by rv32_jit_tlb_pt_page_refs[].  A non-zero refcount means a
     * store could stale a JIT TLB entry, so the helper must perform the write,
     * flush the JIT TLB, and leave the native block.
     */
    return emit_mov_r8d_edx(w) && emit_shr_r8d_imm(w, PAGE_SHIFT) && emit_cmp_pt_page_ref_zero(w) &&
           emit_jcc_rel32_placeholder(w, 0x85, page_table_disp);
}

/* Emit the common native-block prologue and load long-lived base registers. */
bool rv32_jit_emit_prologue(rv32_jit_writer_t *w)
{
    /*
     * System V enters generated code with rsp % 16 == 8. Five callee-saved
     * pushes align the stack before helper calls and provide the guest register
     * cache slots.
     */
    return emit_push_saved_hregs(w) && emit_load_cpu_base(w) && emit_load_pmem_base(w);
}

/* Emit the common native-block epilogue and return completed guest insn count. */
static bool emit_epilogue_return_count(rv32_jit_writer_t *w, uint32_t count)
{
    /* mov eax, count; pop saved cache registers; ret */
    return emit_u8(w, 0xb8) && emit_u32(w, count) && emit_pop_saved_hregs(w) && emit_u8(w, 0xc3);
}

/* Emit the common epilogue when EAX already holds the dynamic return count. */
static bool emit_epilogue_return_eax(rv32_jit_writer_t *w)
{
    return emit_pop_saved_hregs(w) && emit_u8(w, 0xc3);
}

/* Return `rv32_jit_loop_extra + count` for exits from blocks with chained laps. */
static bool emit_epilogue_return_loop_count(rv32_jit_writer_t *w, uint32_t count)
{
    return emit_movabs_rdx(w, (uint64_t)(uintptr_t)&rv32_jit_loop_extra) && emit_mov_eax_m32_rdx(w) && emit_add_eax_imm(w, count) &&
           emit_epilogue_return_eax(w);
}

/*
 * Emit a native side exit for a strict trap whose mtval is already in EAX.
 * Earlier dirty guest-register cache slots are flushed before the helper call,
 * but the trapping instruction's own destination write is emitted only on the
 * normal path after its guard has succeeded.
 */
static bool emit_trap_side_exit_from_eax(rv32_jit_writer_t *w, rv32_jit_reg_cache_t *regs, uint32_t cause, vaddr_t cur_pc, uint32_t exit_count,
                                         bool loop_count_needed)
{
    return jit_reg_emit_flush_all_dirty(w, regs) && emit_mov_edx_eax(w) && emit_mov_edi_imm(w, cause) && emit_mov_esi_imm(w, cur_pc) &&
           emit_call_abs(w, (uintptr_t)jit_raise_trap_tval) &&
           (loop_count_needed ? emit_epilogue_return_loop_count(w, exit_count) : emit_epilogue_return_count(w, exit_count));
}

/*
 * Emit the strict alignment check shared by native JIT loads and stores.
 *
 * EAX contains the guest effective address.  Byte accesses are always naturally
 * aligned, so they need no guard.  Halfword and word accesses branch over an
 * out-of-line trap side exit when the low address bits are zero.  The side exit
 * flushes guest-register cache state produced by earlier instructions in this
 * block, passes the original effective address as mtval, and returns to
 * cpu_exec() after reporting that the trapping instruction retired.
 */
static bool emit_memory_alignment_guard(rv32_jit_writer_t *w, rv32_jit_reg_cache_t *regs, uint32_t len, uint32_t cause, vaddr_t cur_pc,
                                        uint32_t exit_count, bool loop_count_needed)
{
    if (len <= 1)
    {
        return true;
    }

    uint8_t *aligned_disp = NULL;

    if (!emit_test_eax_imm(w, len - 1u) || !emit_jcc_rel32_placeholder(w, 0x84, &aligned_disp) ||
        !emit_trap_side_exit_from_eax(w, regs, cause, cur_pc, exit_count, loop_count_needed))
    {
        return false;
    }

    patch_rel32(aligned_disp, w->cur);
    return true;
}

/*
 * Translate one RV32 load instruction.
 *
 * The fast path performs direct PMEM loads inside the native block when Bare
 * mode or a simple Sv32 JIT TLB hit proves the final physical range.  The slow
 * path flushes dirty registers, sets cpu.pc to the load instruction, calls the
 * typed load helper, and reloads base registers that helper calls may clobber.
 */
static bool emit_load_instr(rv32_jit_writer_t *w, rv32_jit_reg_cache_t *regs, uint32_t instr, vaddr_t cur_pc, uint32_t exit_count,
                            bool loop_count_needed)
{
    const uint32_t rd = bits(instr, 11, 7);
    const uint32_t funct3 = bits(instr, 14, 12);
    const uint32_t rs1 = bits(instr, 19, 15);

    uintptr_t helper = 0;
    uint32_t len = 0;

    switch (funct3)
    {
    case 0x0:
        helper = (uintptr_t)rv32_jit_load_i8;
        len = 1;
        break;
    case 0x1:
        helper = (uintptr_t)rv32_jit_load_i16;
        len = 2;
        break;
    case 0x2:
        helper = (uintptr_t)rv32_jit_load_u32;
        len = 4;
        break;
    case 0x4:
        helper = (uintptr_t)rv32_jit_load_u8;
        len = 1;
        break;
    case 0x5:
        helper = (uintptr_t)rv32_jit_load_u16;
        len = 2;
        break;
    default:
        return false;
    }

    if ((cpu.csr.satp & RV32_JIT_SATP_MODE_MASK) != RISCV_SATP_MODE_BARE)
    {
        rv32_jit_tlb_load_patch_t tlb_guard = {0};
        uint8_t *done_disp = NULL;

        if (!jit_reg_read_eax(w, regs, rs1) || !emit_add_eax_imm(w, (uint32_t)imm_i(instr)) ||
            !emit_memory_alignment_guard(w, regs, len, RISCV32_CAUSE_LOAD_ADDR_MISALIGNED, cur_pc, exit_count, loop_count_needed) ||
            !emit_paged_tlb_load_eax(w, funct3, len, &tlb_guard) || !emit_jmp_rel32_placeholder(w, &done_disp))
        {
            return false;
        }

        const uint8_t *slow_path = w->cur;
        patch_tlb_load_guard(&tlb_guard, slow_path);
        /*
         * The inline guard saves the full guest virtual address in ECX before it
         * masks EAX down to a page offset. Restore EAX so the old helper path keeps
         * the same argument and fault/MMIO behaviour as before.
         */

        if (!emit_mov_eax_ecx(w) || !jit_reg_emit_flush_all_dirty(w, regs) || !emit_set_pc_imm(w, cur_pc) || !emit_u8(w, 0x89) || !emit_u8(w, 0xc7) ||
            !emit_call_abs(w, helper) || !emit_load_cpu_base(w) || !emit_load_pmem_base(w) ||
            (regs->source_refs_loaded && !emit_load_source_refs_base(w)))
        {
            return false;
        }

        patch_rel32(done_disp, w->cur);
        return jit_reg_write_eax(w, regs, rd);
    }

    rv32_jit_pmem_guard_patch_t guard = {0};
    uint8_t *done_disp = NULL;

    if (!jit_reg_read_eax(w, regs, rs1) || !emit_add_eax_imm(w, (uint32_t)imm_i(instr)) ||
        !emit_memory_alignment_guard(w, regs, len, RISCV32_CAUSE_LOAD_ADDR_MISALIGNED, cur_pc, exit_count, loop_count_needed) ||
        !emit_direct_pmem_guard(w, len, &guard) || !emit_direct_pmem_load_eax(w, funct3) || !emit_jmp_rel32_placeholder(w, &done_disp))
    {
        return false;
    }

    const uint8_t *slow_path = w->cur;
    patch_direct_pmem_guard(&guard, slow_path);
    /*
     * The slow helper may enter the normal vaddr path, which can report MMIO,
     * translation, or bounds failures using cpu.pc. EAX still holds the guest
     * address here, so writing cpu.pc first does not disturb the helper argument.
     */

    if (!jit_reg_emit_flush_all_dirty(w, regs) || !emit_set_pc_imm(w, cur_pc) || !emit_u8(w, 0x89) || !emit_u8(w, 0xc7) ||
        !emit_call_abs(w, helper) || !emit_load_cpu_base(w) || !emit_load_pmem_base(w) ||
        (regs->source_refs_loaded && !emit_load_source_refs_base(w)))
    {
        return false;
    }

    patch_rel32(done_disp, w->cur);
    return jit_reg_write_eax(w, regs, rd);
}

/*
 * Translate one RV32 store instruction.
 *
 * Plain PMEM data stores can continue in the native block. Stores that may hit
 * MMIO, source bytes, or page-table pages call the store helper and then leave
 * the block, so the dispatcher observes any invalidation before the next block.
 */
static bool emit_store_instr(rv32_jit_writer_t *w, rv32_jit_reg_cache_t *regs, uint32_t instr, vaddr_t cur_pc, vaddr_t next_pc, uint32_t exit_count,
                             bool loop_count_needed)
{
    const uint32_t funct3 = bits(instr, 14, 12);
    const uint32_t rs1 = bits(instr, 19, 15);
    const uint32_t rs2 = bits(instr, 24, 20);

    uintptr_t helper = 0;
    uintptr_t continue_helper = 0;
    uint32_t len = 0;

    switch (funct3)
    {
    case 0x0:
        helper = (uintptr_t)rv32_jit_store_u8;
        continue_helper = (uintptr_t)rv32_jit_store_u8_continue;
        len = 1;
        break;
    case 0x1:
        helper = (uintptr_t)rv32_jit_store_u16;
        continue_helper = (uintptr_t)rv32_jit_store_u16_continue;
        len = 2;
        break;
    case 0x2:
        helper = (uintptr_t)rv32_jit_store_u32;
        continue_helper = (uintptr_t)rv32_jit_store_u32_continue;
        len = 4;
        break;
    default:
        return false;
    }

    if ((cpu.csr.satp & RV32_JIT_SATP_MODE_MASK) != RISCV_SATP_MODE_BARE)
    {
        rv32_jit_tlb_load_patch_t tlb_guard = {0};
        uint8_t *cross_chunk_disp = NULL;
        uint8_t *source_chunk_disp = NULL;
        uint8_t *page_table_disp = NULL;
        uint8_t *exit_disp = NULL;
        uint8_t *fast_done_disp = NULL;
        uint8_t *helper_done_disp = NULL;
        /*
         * Paged-mode stores first try the same translated-PMEM TLB hit that the C
         * helper would use.  Inline continuation is allowed only for ordinary data
         * pages: source-code writes and page-table writes still go through the
         * helper and then exit so invalidation is observed before the next fetch.
         */

        if (!jit_reg_read_eax(w, regs, rs1) || !emit_add_eax_imm(w, (uint32_t)imm_s(instr)) ||
            !emit_memory_alignment_guard(w, regs, len, RISCV32_CAUSE_STORE_ADDR_MISALIGNED, cur_pc, exit_count, loop_count_needed) ||
            !jit_reg_read_ecx(w, regs, rs2) || !emit_paged_tlb_store_offset_edx(w, len, &tlb_guard) ||
            !emit_store_source_chunk_guard(w, regs, len, &cross_chunk_disp, &source_chunk_disp) ||
            !emit_store_page_table_guard(w, &page_table_disp) || !emit_direct_pmem_store_from_ecx(w, len) ||
            !emit_jmp_rel32_placeholder(w, &fast_done_disp))
        {
            return false;
        }

        const uint8_t *slow_path = w->cur;
        patch_tlb_load_guard(&tlb_guard, slow_path);
        patch_rel32(cross_chunk_disp, slow_path);
        patch_rel32(source_chunk_disp, slow_path);
        patch_rel32(page_table_disp, slow_path);

        if (!jit_reg_emit_flush_all_dirty(w, regs) || !emit_set_pc_imm(w, cur_pc) || !emit_u8(w, 0x89) || !emit_u8(w, 0xce) ||
            !emit_call_abs(w, continue_helper) || !emit_load_cpu_base(w) || !emit_test_eax_eax(w) ||
            !emit_jcc_rel32_placeholder(w, 0x84, &exit_disp) || !emit_load_pmem_base(w) ||
            (regs->source_refs_loaded && !emit_load_source_refs_base(w)) || !emit_jmp_rel32_placeholder(w, &helper_done_disp))
        {
            return false;
        }

        patch_rel32(exit_disp, w->cur);

        if (!emit_set_pc_imm(w, next_pc) ||
            !(loop_count_needed ? emit_epilogue_return_loop_count(w, exit_count) : emit_epilogue_return_count(w, exit_count)))
        {
            return false;
        }

        patch_rel32(fast_done_disp, w->cur);
        patch_rel32(helper_done_disp, w->cur);
        return true;
    }

    rv32_jit_pmem_guard_patch_t guard = {0};
    uint8_t *cross_chunk_disp = NULL;
    uint8_t *source_chunk_disp = NULL;
    uint8_t *page_table_disp = NULL;
    uint8_t *done_disp = NULL;
    /*
     * Stores have two native continuations. Plain PMEM data stores commit inline
     * and continue in the same block; stores that might touch translated source
     * bytes divert to the helper, which invalidates by physical address and exits
     * before the dispatcher performs the next block lookup.
     */

    if (!jit_reg_read_eax(w, regs, rs1) || !emit_add_eax_imm(w, (uint32_t)imm_s(instr)) ||
        !emit_memory_alignment_guard(w, regs, len, RISCV32_CAUSE_STORE_ADDR_MISALIGNED, cur_pc, exit_count, loop_count_needed) ||
        !jit_reg_read_ecx(w, regs, rs2) || !emit_direct_pmem_guard(w, len, &guard) ||
        !emit_store_source_chunk_guard(w, regs, len, &cross_chunk_disp, &source_chunk_disp) || !emit_store_page_table_guard(w, &page_table_disp) ||
        !emit_direct_pmem_store_from_ecx(w, len) || !emit_jmp_rel32_placeholder(w, &done_disp))
    {
        return false;
    }

    const uint8_t *slow_path = w->cur;
    patch_direct_pmem_guard(&guard, slow_path);
    patch_rel32(cross_chunk_disp, slow_path);
    patch_rel32(source_chunk_disp, slow_path);
    patch_rel32(page_table_disp, slow_path);

    /*
     * The helper path handles MMIO, paging, cross-chunk direct stores, and source
     * code invalidation. Set cpu.pc to the store itself before the call so faults
     * and MMIO diagnostics identify the correct guest instruction. After a
     * successful helper return, advance cpu.pc and leave the native block; the JIT
     * dispatcher may run another block, but it will start from the post-store PC.
     */

    if (!jit_reg_emit_flush_all_dirty(w, regs) || !emit_set_pc_imm(w, cur_pc) || !emit_u8(w, 0x89) || !emit_u8(w, 0xc7) || !emit_u8(w, 0x89) ||
        !emit_u8(w, 0xce) || !emit_call_abs(w, helper) || !emit_load_cpu_base(w) || !emit_set_pc_imm(w, next_pc) ||
        !(loop_count_needed ? emit_epilogue_return_loop_count(w, exit_count) : emit_epilogue_return_count(w, exit_count)))
    {
        return false;
    }

    patch_rel32(done_disp, w->cur);
    return true;
}

/* Dispatch a decoded LOAD or STORE opcode to its specialised emitter. */
bool rv32_jit_emit_load_store(rv32_jit_writer_t *w, rv32_jit_reg_cache_t *regs, uint32_t instr, vaddr_t cur_pc, uint32_t exit_count,
                              bool loop_count_needed)
{
    const uint32_t opcode = instr & RISCV_OPCODE_MASK;

    if (opcode == RISCV_OPCODE_LOAD)
    {
        return emit_load_instr(w, regs, instr, cur_pc, exit_count, loop_count_needed);
    }

    if (opcode == RISCV_OPCODE_STORE)
    {
        return emit_store_instr(w, regs, instr, cur_pc, cur_pc + RISCV_BASE_INSN_BYTES, exit_count, loop_count_needed);
    }

    return false;
}

/* Return true for instructions that can stay inside a chained loop body. */
bool rv32_jit_instr_can_chain_body(uint32_t instr)
{
    const uint32_t opcode = instr & RISCV_OPCODE_MASK;

    switch (opcode)
    {
    case RISCV_OPCODE_OP_IMM:
    case RISCV_OPCODE_LOAD:
    case RISCV_OPCODE_STORE:
    case RISCV_OPCODE_AUIPC:
    case RISCV_OPCODE_OP:
    case RISCV_OPCODE_LUI:
    case RISCV_OPCODE_BRANCH:
        return true;
    default:
        return false;
    }
}

/* Translate one conditional branch, keeping fall-through in the same block. */
static bool emit_branch_chain_backedge(rv32_jit_writer_t *w, rv32_jit_reg_cache_t *regs, vaddr_t target, uint32_t exit_count,
                                       const uint8_t *target_native)
{
    uint8_t *over_budget_disp = NULL;
    uint8_t *loop_disp = NULL;

    /*
     * The taken branch has already completed `exit_count` guest instructions from
     * the native loop head. Chain only when another full lap fits the current
     * cpu_exec() budget; otherwise return to the dispatcher at the branch target.
     */
    if (!emit_movabs_rdx(w, (uint64_t)(uintptr_t)&rv32_jit_loop_extra) || !emit_mov_eax_m32_rdx(w) || !emit_add_eax_imm(w, exit_count) ||
        !emit_mov_ecx_eax(w) || !emit_add_ecx_imm(w, exit_count) || !emit_movabs_rdx(w, (uint64_t)(uintptr_t)&rv32_jit_entry_budget) ||
        !emit_cmp_ecx_m32_rdx(w) || !emit_jcc_rel32_placeholder(w, 0x87, &over_budget_disp) ||
        !emit_movabs_rdx(w, (uint64_t)(uintptr_t)&rv32_jit_loop_extra) || !emit_mov_m32_rdx_eax(w) || !jit_reg_emit_flush_all_dirty(w, regs) ||
        !emit_jmp_rel32_placeholder(w, &loop_disp))
    {
        return false;
    }

    patch_rel32(loop_disp, target_native);
    patch_rel32(over_budget_disp, w->cur);

    return jit_reg_emit_flush_all_dirty(w, regs) && emit_set_pc_imm(w, target) && emit_epilogue_return_eax(w);
}

bool rv32_jit_emit_branch(rv32_jit_writer_t *w, rv32_jit_reg_cache_t *regs, uint32_t instr, vaddr_t pc, vaddr_t block_start_pc,
                          const uint8_t *block_start_native, bool loop_count_needed, bool chain_safe, bool *branch_chained, uint32_t exit_count)
{
    const uint32_t funct3 = bits(instr, 14, 12);
    const uint32_t rs1 = bits(instr, 19, 15);
    const uint32_t rs2 = bits(instr, 24, 20);
    uint8_t jcc = 0;

    switch (funct3)
    {
    case 0x0:
        jcc = 0x84;
        break; /* JE  */
    case 0x1:
        jcc = 0x85;
        break; /* JNE */
    case 0x4:
        jcc = 0x8c;
        break; /* JL, signed */
    case 0x5:
        jcc = 0x8d;
        break; /* JGE, signed */
    case 0x6:
        jcc = 0x82;
        break; /* JB, unsigned */
    case 0x7:
        jcc = 0x83;
        break; /* JAE, unsigned */
    default:
        return false;
    }

    uint8_t *fallthrough_disp = NULL;
    const vaddr_t target = pc + imm_b(instr);

    if ((target & RISCV_IALIGN_32_MASK) != 0)
    {
        return false;
    }

    /*
     * Conditional branches are the first control-flow case that can keep useful
     * cached registers alive. The untaken path stays in this native block, while
     * the taken path materialises the same register state into cpu.gpr[] and
     * returns to the dispatcher at the branch target.
     */

    if (!jit_reg_read_eax(w, regs, rs1) || !jit_reg_read_ecx(w, regs, rs2) || !emit_cmp_eax_ecx(w) ||
        !emit_jcc_rel32_placeholder(w, (uint8_t)(jcc ^ 1u), &fallthrough_disp))
    {
        return false;
    }

    if (chain_safe && target == block_start_pc)
    {
        if (!emit_branch_chain_backedge(w, regs, target, exit_count, block_start_native))
        {
            return false;
        }

        *branch_chained = true;
    }
    else if (!jit_reg_emit_flush_all_dirty(w, regs) || !emit_set_pc_imm(w, target) ||
             !(loop_count_needed ? emit_epilogue_return_loop_count(w, exit_count) : emit_epilogue_return_count(w, exit_count)))
    {
        return false;
    }

    patch_rel32(fallthrough_disp, w->cur);
    return true;
}

/* Translate JAL and JALR control flow instructions that always end the block. */
bool rv32_jit_emit_control_flow(rv32_jit_writer_t *w, rv32_jit_reg_cache_t *regs, uint32_t instr, vaddr_t pc, uint32_t exit_count)
{
    const uint32_t opcode = instr & RISCV_OPCODE_MASK;
    const uint32_t rd = bits(instr, 11, 7);
    const uint32_t funct3 = bits(instr, 14, 12);
    const uint32_t rs1 = bits(instr, 19, 15);

    if (opcode == RISCV_OPCODE_BRANCH)
    {
        return false;
    }

    if (opcode == RISCV_OPCODE_JAL)
    {
        const vaddr_t target = pc + imm_j(instr);

        if ((target & RISCV_IALIGN_32_MASK) != 0)
        {
            return false;
        }

        return emit_mov_eax_imm(w, pc + RISCV_BASE_INSN_BYTES) && jit_reg_write_eax(w, regs, rd) && jit_reg_emit_flush_all_dirty(w, regs) &&
               emit_set_pc_imm(w, target);
    }

    if (opcode == RISCV_OPCODE_JALR && funct3 == RISCV_JALR_FUNCT3)
    {
        /*
         * JALR computes and aligns the target before writing the link register.
         * The misaligned-target side exit must therefore run before rd is
         * changed, otherwise rd == rs1 cases would observe a link write that
         * should not happen for the trapping instruction.
         */
        uint8_t *aligned_disp = NULL;

        if (!jit_reg_read_eax(w, regs, rs1) || !emit_add_eax_imm(w, (uint32_t)imm_i(instr)) ||
            !emit_and_eax_imm(w, ~(uint32_t)RISCV_JALR_TARGET_LSB_MASK) || !emit_test_eax_imm(w, RISCV_IALIGN_32_MASK) ||
            !emit_jcc_rel32_placeholder(w, 0x84, &aligned_disp) ||
            !emit_trap_side_exit_from_eax(w, regs, RISCV32_CAUSE_INST_ADDR_MISALIGNED, pc, exit_count, false))
        {
            return false;
        }

        patch_rel32(aligned_disp, w->cur);

        return emit_store_pc_eax(w) && emit_mov_eax_imm(w, pc + RISCV_BASE_INSN_BYTES) && jit_reg_write_eax(w, regs, rd) &&
               jit_reg_emit_flush_all_dirty(w, regs);
    }

    return false;
}

/*
 * Translate RV32 integer ALU instructions.
 *
 * This emitter handles LUI, AUIPC, OP-IMM, OP, and common RV32M operations. It
 * first tries cache-friendly forms that update guest-register slots directly,
 * then falls back to EAX/ECX temporary sequences for less convenient cases.
 */
bool rv32_jit_emit_alu(rv32_jit_writer_t *w, rv32_jit_reg_cache_t *regs, uint32_t instr, vaddr_t cur_pc)
{
    const uint32_t opcode = instr & RISCV_OPCODE_MASK;
    const uint32_t rd = bits(instr, 11, 7);
    const uint32_t funct3 = bits(instr, 14, 12);
    const uint32_t rs1 = bits(instr, 19, 15);
    const uint32_t rs2 = bits(instr, 24, 20);
    const uint32_t funct7 = bits(instr, 31, 25);

    if (opcode == RISCV_OPCODE_LUI)
    {
        /* LUI places the U-immediate directly in rd. */
        return jit_reg_write_imm(w, regs, rd, imm_u(instr));
    }

    if (opcode == RISCV_OPCODE_AUIPC)
    {
        return jit_reg_write_imm(w, regs, rd, cur_pc + imm_u(instr));
    }

    if (opcode == RISCV_OPCODE_OP_IMM)
    {
        const uint32_t imm = (uint32_t)imm_i(instr);

        if (rs1 == RISCV_GPR_ZERO)
        {
            switch (funct3)
            {
            case 0x0:
                return jit_reg_write_imm(w, regs, rd, imm);
            case 0x1:
                if (bits(instr, 31, 25) != 0x00)
                {
                    return false;
                }

                return jit_reg_write_imm(w, regs, rd, 0);
            case 0x2:
                return jit_reg_write_imm(w, regs, rd, (int32_t)0 < imm_i(instr));
            case 0x3:
                return jit_reg_write_imm(w, regs, rd, imm != 0);
            case 0x4:
                return jit_reg_write_imm(w, regs, rd, imm);
            case 0x5:
                if (bits(instr, 31, 25) == 0x00 || bits(instr, 31, 25) == 0x20)
                {
                    return jit_reg_write_imm(w, regs, rd, 0);
                }

                return false;
            case 0x6:
                return jit_reg_write_imm(w, regs, rd, imm);
            case 0x7:
                return jit_reg_write_imm(w, regs, rd, 0);
            default:
                return false;
            }
        }

        if (rd != RISCV_GPR_ZERO && rd == rs1)
        {
            switch (funct3)
            {
            case 0x0:
                return imm == 0 ? true : jit_reg_apply_imm(w, regs, rd, 0, imm);
            case 0x1:
                if (bits(instr, 31, 25) != 0x00)
                {
                    return false;
                }

                return jit_reg_apply_shift_imm(w, regs, rd, 4, (uint8_t)bits(instr, 24, 20));
            case 0x4:
                return imm == 0 ? true : jit_reg_apply_imm(w, regs, rd, 6, imm);
            case 0x5:
                if (bits(instr, 31, 25) == 0x00)
                {
                    return jit_reg_apply_shift_imm(w, regs, rd, 5, (uint8_t)bits(instr, 24, 20));
                }

                if (bits(instr, 31, 25) == 0x20)
                {
                    return jit_reg_apply_shift_imm(w, regs, rd, 7, (uint8_t)bits(instr, 24, 20));
                }

                return false;
            case 0x6:
                return imm == 0 ? true : jit_reg_apply_imm(w, regs, rd, 1, imm);
            case 0x7:
                return jit_reg_apply_imm(w, regs, rd, 4, imm);
            default:
                break;
            }
        }

        if (rd != RISCV_GPR_ZERO)
        {
            const uint8_t shamt = (uint8_t)bits(instr, 24, 20);

            /*
             * The compiler emits many OP-IMM instructions as copies or as a simple
             * transformation of one live value into a different destination register.
             * Keep those inside the guest-register cache instead of bouncing through
             * eax and then copying back to a cache slot.
             */
            switch (funct3)
            {
            case 0x0:
                return jit_reg_copy(w, regs, rd, rs1) && (imm == 0 || jit_reg_apply_imm(w, regs, rd, 0, imm));
            case 0x1:
                if (bits(instr, 31, 25) != 0x00)
                {
                    return false;
                }

                return jit_reg_copy(w, regs, rd, rs1) && (shamt == 0 || jit_reg_apply_shift_imm(w, regs, rd, 4, shamt));
            case 0x4:
                return jit_reg_copy(w, regs, rd, rs1) && (imm == 0 || jit_reg_apply_imm(w, regs, rd, 6, imm));
            case 0x5:
                if (bits(instr, 31, 25) == 0x00)
                {
                    return jit_reg_copy(w, regs, rd, rs1) && (shamt == 0 || jit_reg_apply_shift_imm(w, regs, rd, 5, shamt));
                }

                if (bits(instr, 31, 25) == 0x20)
                {
                    return jit_reg_copy(w, regs, rd, rs1) && (shamt == 0 || jit_reg_apply_shift_imm(w, regs, rd, 7, shamt));
                }

                return false;
            case 0x6:
                return jit_reg_copy(w, regs, rd, rs1) && (imm == 0 || jit_reg_apply_imm(w, regs, rd, 1, imm));
            case 0x7:
                if (imm == 0)
                {
                    return jit_reg_write_imm(w, regs, rd, 0);
                }

                return jit_reg_copy(w, regs, rd, rs1) && (imm == UINT32_MAX || jit_reg_apply_imm(w, regs, rd, 4, imm));
            default:
                break;
            }
        }

        if (!jit_reg_read_eax(w, regs, rs1))
        {
            return false;
        }

        switch (funct3)
        {
        case 0x0:
            return emit_add_eax_imm(w, imm) && jit_reg_write_eax(w, regs, rd);
        case 0x1:
            if (bits(instr, 31, 25) != 0x00)
            {
                return false;
            }

            return emit_u8(w, 0xc1) && emit_u8(w, 0xe0) && emit_u8(w, bits(instr, 24, 20)) && jit_reg_write_eax(w, regs, rd);
        case 0x2:
            return emit_cmp_eax_imm(w, imm) && emit_setcc_eax(w, 0x9c) && jit_reg_write_eax(w, regs, rd);
        case 0x3:
            return emit_cmp_eax_imm(w, imm) && emit_setcc_eax(w, 0x92) && jit_reg_write_eax(w, regs, rd);
        case 0x4:
            return emit_u8(w, 0x35) && emit_u32(w, imm) && jit_reg_write_eax(w, regs, rd);
        case 0x5:
            if (bits(instr, 31, 25) == 0x00)
            {
                return emit_u8(w, 0xc1) && emit_u8(w, 0xe8) && emit_u8(w, bits(instr, 24, 20)) && jit_reg_write_eax(w, regs, rd);
            }

            if (bits(instr, 31, 25) == 0x20)
            {
                return emit_u8(w, 0xc1) && emit_u8(w, 0xf8) && emit_u8(w, bits(instr, 24, 20)) && jit_reg_write_eax(w, regs, rd);
            }

            return false;
        case 0x6:
            return emit_u8(w, 0x0d) && emit_u32(w, imm) && jit_reg_write_eax(w, regs, rd);
        case 0x7:
            return emit_u8(w, 0x25) && emit_u32(w, imm) && jit_reg_write_eax(w, regs, rd);
        default:
            return false;
        }
    }

    if (opcode == RISCV_OPCODE_OP)
    {
        const uint32_t key = (funct7 << 3) | funct3;

        if (rd != RISCV_GPR_ZERO)
        {
            switch (key)
            {
            case 0x000:
                if (rd == rs1 && rs2 != RISCV_GPR_ZERO)
                {
                    return jit_reg_apply_reg(w, regs, rd, rs2, 0x01);
                }

                if (rd == rs2 && rs1 != RISCV_GPR_ZERO)
                {
                    return jit_reg_apply_reg(w, regs, rd, rs1, 0x01);
                }
                break;
            case 0x100:
                if (rd == rs1 && rs2 != RISCV_GPR_ZERO)
                {
                    return jit_reg_apply_reg(w, regs, rd, rs2, 0x29);
                }
                break;
            case 0x001:
                if (rd == rs1)
                {
                    return jit_reg_apply_shift_reg(w, regs, rd, rs2, 4);
                }
                break;
            case 0x004:
                if (rd == rs1 && rs2 != RISCV_GPR_ZERO)
                {
                    return jit_reg_apply_reg(w, regs, rd, rs2, 0x31);
                }

                if (rd == rs2 && rs1 != RISCV_GPR_ZERO)
                {
                    return jit_reg_apply_reg(w, regs, rd, rs1, 0x31);
                }
                break;
            case 0x005:
                if (rd == rs1)
                {
                    return jit_reg_apply_shift_reg(w, regs, rd, rs2, 5);
                }
                break;
            case 0x105:
                if (rd == rs1)
                {
                    return jit_reg_apply_shift_reg(w, regs, rd, rs2, 7);
                }
                break;
            case 0x006:
                if (rd == rs1 && rs2 != RISCV_GPR_ZERO)
                {
                    return jit_reg_apply_reg(w, regs, rd, rs2, 0x09);
                }

                if (rd == rs2 && rs1 != RISCV_GPR_ZERO)
                {
                    return jit_reg_apply_reg(w, regs, rd, rs1, 0x09);
                }
                break;
            case 0x007:
                if (rd == rs1 && rs2 != RISCV_GPR_ZERO)
                {
                    return jit_reg_apply_reg(w, regs, rd, rs2, 0x21);
                }

                if (rd == rs2 && rs1 != RISCV_GPR_ZERO)
                {
                    return jit_reg_apply_reg(w, regs, rd, rs1, 0x21);
                }
                break;
            default:
                break;
            }

            /*
             * If rd is a third guest register, start by copying rs1 into rd and then
             * apply the second operand in place. This emits one cached-register move
             * plus the ALU operation, avoiding the old sequence
             *   cached rs1 -> eax -> ALU -> cached rd.
             * The rd != rs2 condition is important for shifts and subtraction because
             * overwriting rd would otherwise destroy the still-needed source value.
             */
            switch (key)
            {
            case 0x000:
                if (rs1 == RISCV_GPR_ZERO)
                {
                    return jit_reg_copy(w, regs, rd, rs2);
                }

                if (rs2 == RISCV_GPR_ZERO)
                {
                    return jit_reg_copy(w, regs, rd, rs1);
                }

                if (rd != rs1 && rd != rs2)
                {
                    return jit_reg_copy(w, regs, rd, rs1) && jit_reg_apply_reg(w, regs, rd, rs2, 0x01);
                }
                break;
            case 0x100:
                if (rs1 == rs2 || rs2 == RISCV_GPR_ZERO)
                {
                    return rs1 == rs2 ? jit_reg_write_imm(w, regs, rd, 0) : jit_reg_copy(w, regs, rd, rs1);
                }

                if (rd != rs1 && rd != rs2)
                {
                    return jit_reg_copy(w, regs, rd, rs1) && jit_reg_apply_reg(w, regs, rd, rs2, 0x29);
                }
                break;
            case 0x001:
                if (rs1 == RISCV_GPR_ZERO)
                {
                    return jit_reg_write_imm(w, regs, rd, 0);
                }

                if (rs2 == RISCV_GPR_ZERO)
                {
                    return jit_reg_copy(w, regs, rd, rs1);
                }

                if (rd != rs1 && rd != rs2)
                {
                    return jit_reg_copy(w, regs, rd, rs1) && jit_reg_apply_shift_reg(w, regs, rd, rs2, 4);
                }
                break;
            case 0x002:
            case 0x003:
                if (rs1 == rs2)
                {
                    return jit_reg_write_imm(w, regs, rd, 0);
                }
                break;
            case 0x004:
                if (rs1 == rs2)
                {
                    return jit_reg_write_imm(w, regs, rd, 0);
                }

                if (rs1 == RISCV_GPR_ZERO)
                {
                    return jit_reg_copy(w, regs, rd, rs2);
                }

                if (rs2 == RISCV_GPR_ZERO)
                {
                    return jit_reg_copy(w, regs, rd, rs1);
                }

                if (rd != rs1 && rd != rs2)
                {
                    return jit_reg_copy(w, regs, rd, rs1) && jit_reg_apply_reg(w, regs, rd, rs2, 0x31);
                }
                break;
            case 0x005:
            case 0x105:
                if (rs1 == RISCV_GPR_ZERO)
                {
                    return jit_reg_write_imm(w, regs, rd, 0);
                }

                if (rs2 == RISCV_GPR_ZERO)
                {
                    return jit_reg_copy(w, regs, rd, rs1);
                }

                if (rd != rs1 && rd != rs2)
                {
                    return jit_reg_copy(w, regs, rd, rs1) && jit_reg_apply_shift_reg(w, regs, rd, rs2, key == 0x005 ? 5 : 7);
                }
                break;
            case 0x006:
                if (rs1 == RISCV_GPR_ZERO)
                {
                    return jit_reg_copy(w, regs, rd, rs2);
                }

                if (rs2 == RISCV_GPR_ZERO)
                {
                    return jit_reg_copy(w, regs, rd, rs1);
                }

                if (rd != rs1 && rd != rs2)
                {
                    return jit_reg_copy(w, regs, rd, rs1) && jit_reg_apply_reg(w, regs, rd, rs2, 0x09);
                }
                break;
            case 0x007:
                if (rs1 == RISCV_GPR_ZERO || rs2 == RISCV_GPR_ZERO)
                {
                    return jit_reg_write_imm(w, regs, rd, 0);
                }

                if (rd != rs1 && rd != rs2)
                {
                    return jit_reg_copy(w, regs, rd, rs1) && jit_reg_apply_reg(w, regs, rd, rs2, 0x21);
                }
                break;
            default:
                break;
            }
        }

        if (!jit_reg_read_eax(w, regs, rs1) || !jit_reg_read_ecx(w, regs, rs2))
        {
            return false;
        }

        switch (key)
        {
        case 0x000:
            return emit_u8(w, 0x01) && emit_u8(w, 0xc8) && jit_reg_write_eax(w, regs, rd);
        case 0x100:
            return emit_u8(w, 0x29) && emit_u8(w, 0xc8) && jit_reg_write_eax(w, regs, rd);
        case 0x001:
            return emit_u8(w, 0xd3) && emit_u8(w, 0xe0) && jit_reg_write_eax(w, regs, rd);
        case 0x002:
            return emit_cmp_eax_ecx(w) && emit_setcc_eax(w, 0x9c) && jit_reg_write_eax(w, regs, rd);
        case 0x003:
            return emit_cmp_eax_ecx(w) && emit_setcc_eax(w, 0x92) && jit_reg_write_eax(w, regs, rd);
        case 0x004:
            return emit_u8(w, 0x31) && emit_u8(w, 0xc8) && jit_reg_write_eax(w, regs, rd);
        case 0x005:
            return emit_u8(w, 0xd3) && emit_u8(w, 0xe8) && jit_reg_write_eax(w, regs, rd);
        case 0x105:
            return emit_u8(w, 0xd3) && emit_u8(w, 0xf8) && jit_reg_write_eax(w, regs, rd);
        case 0x006:
            return emit_u8(w, 0x09) && emit_u8(w, 0xc8) && jit_reg_write_eax(w, regs, rd);
        case 0x007:
            return emit_u8(w, 0x21) && emit_u8(w, 0xc8) && jit_reg_write_eax(w, regs, rd);
        case 0x008:
            return emit_u8(w, 0x0f) && emit_u8(w, 0xaf) && emit_u8(w, 0xc1) && jit_reg_write_eax(w, regs, rd);
        case 0x009:
            return emit_rv32_mul_high(w, regs, rd, true);
        case 0x00b:
            return emit_rv32_mul_high(w, regs, rd, false);
        case 0x00c:
            return emit_rv32_div(w, regs, rd);
        case 0x00d:
            return emit_rv32_divu(w, regs, rd);
        case 0x00e:
            return emit_rv32_rem(w, regs, rd);
        case 0x00f:
            return emit_rv32_remu(w, regs, rd);
        case 0x00a:
            return jit_reg_flush_all_dirty(w, regs) && emit_u8(w, 0xbf) && emit_u32(w, instr) && emit_call_abs(w, (uintptr_t)jit_op_complex) &&
                   emit_load_cpu_base(w) && emit_load_pmem_base(w) && (!regs->source_refs_loaded || emit_load_source_refs_base(w)) &&
                   (jit_reg_invalidate_all(regs), true) && jit_reg_write_eax(w, regs, rd);
        default:
            return false;
        }
    }

    return false;
}

#ifdef CONFIG_RISCV_FPU
/* FP memory accesses leave the block after the shared helper has completed. */
static bool jit_fp_opcode_is_memory(uint32_t opcode)
{
    return opcode == RISCV_FP_OPCODE_LOAD || opcode == RISCV_FP_OPCODE_STORE;
}

/*
 * Record helper entry and architectural traps around the shared FP executor.
 *
 * Successful outcomes are counted by generated code on the native edge which
 * actually resumes or terminates the block. This keeps those counters tied to
 * emitted control flow rather than a classification made inside this wrapper.
 */
static uint32_t jit_exec_fpu(uint32_t instr, vaddr_t pc)
{
    JIT_STAT_INC(fp_helper_calls);

    const uint32_t completed = riscv_fpu_jit_exec(instr, pc);

    if (completed == 0)
    {
        JIT_STAT_INC(fp_helper_trap_exits);
    }

    return completed;
}

/*
 * Lower one configured FP instruction to the shared SoftFloat executor.
 *
 * The helper can read or write integer registers, so every dirty cached GPR is
 * materialised first. Successful non-memory instructions may resume generated
 * code after caller-saved base registers and cache metadata have been repaired.
 * FP memory instructions always finish the block: their shared memory path may
 * perform MMIO, raise a fault, update page-table state, or invalidate translated
 * source bytes.
 */
bool rv32_jit_emit_fpu(rv32_jit_writer_t *w, rv32_jit_reg_cache_t *regs, uint32_t instr, vaddr_t pc, uint32_t completed_count, bool *ends_block)
{
    const bool memory = jit_fp_opcode_is_memory(instr & RISCV_OPCODE_MASK);
    const bool reload_source_refs = regs->source_refs_loaded;

    *ends_block = false;

    if (!jit_reg_flush_all_dirty(w, regs) || !emit_mov_edi_imm(w, instr) || !emit_mov_esi_imm(w, pc) || !emit_call_abs(w, (uintptr_t)jit_exec_fpu))
    {
        return false;
    }

    if (memory)
    {
#if RV32_JIT_STATS
        uint8_t *skip_memory_exit_stat_disp = NULL;

        /*
         * A zero helper result is the trap edge. Only normal FP memory
         * completion reaches the native increment before both outcomes join
         * the common terminal epilogue.
         */
        if (!emit_test_eax_eax(w) || !emit_jcc_rel32_placeholder(w, 0x84, &skip_memory_exit_stat_disp) ||
            !emit_inc_fp_stat_counter(w, &rv32_jit_stats.fp_helper_memory_exits))
        {
            return false;
        }

        patch_rel32(skip_memory_exit_stat_disp, w->cur);
#endif

        /*
         * riscv_fpu_jit_exec() publishes cpu.pc for both normal completion and
         * traps. The common block epilogue therefore only has to return the
         * completed instruction count.
         */
        *ends_block = true;
        return true;
    }

    uint8_t *continue_disp = NULL;

    /*
     * EAX is zero only after an architectural trap. The helper already
     * published the trap target in cpu.pc, so this side exit must not infer the
     * outcome from the target address or overwrite it with sequential pc.
     */
    if (!emit_test_eax_eax(w) || !emit_jcc_rel32_placeholder(w, 0x85, &continue_disp) || !emit_epilogue_return_count(w, completed_count))
    {
        return false;
    }

    patch_rel32(continue_disp, w->cur);

#if RV32_JIT_STATS
    if (!emit_inc_fp_stat_counter(w, &rv32_jit_stats.fp_helper_continuations))
    {
        return false;
    }
#endif

    if (!emit_load_cpu_base(w) || !emit_load_pmem_base(w) || (reload_source_refs && !emit_load_source_refs_base(w)))
    {
        return false;
    }

    /*
     * The helper may have changed any integer register. Forget every mapping so
     * the next native instruction reloads architectural state instead of using
     * a stale callee-saved value.
     */
    jit_reg_invalidate_all(regs);
    return true;
}
#endif

/*
 * Finish one compiled block using the exact finalisation order from the former
 * monolith. Only chained loops read the dynamic loop-lap counter; ordinary
 * blocks return the fixed completed-instruction count.
 */
bool rv32_jit_emit_block_exit(rv32_jit_writer_t *writer, rv32_jit_reg_cache_t *regs, vaddr_t next_pc, uint32_t completed_count, bool pc_already_set,
                              bool return_loop_count)
{
    if (!pc_already_set && (!jit_reg_flush_all_dirty(writer, regs) || !emit_set_pc_imm(writer, next_pc)))
    {
        return false;
    }

    return return_loop_count ? emit_epilogue_return_loop_count(writer, completed_count) : emit_epilogue_return_count(writer, completed_count);
}

#endif /* !CONFIG_RV64 */
