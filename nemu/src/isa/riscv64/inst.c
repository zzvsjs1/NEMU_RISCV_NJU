#include "local-include/reg.h"
#include <cpu/cpu.h>
#include <cpu/decode.h>
#include <cpu/difftest.h>
#include <cpu/ifetch.h>
#ifdef CONFIG_RV64_JIT
#include <isa-jit.h>
#endif
#include <memory/vaddr.h>
#include <utils.h>

#define R(i) gpr(i)
#define Mr vaddr_read
#define Mw vaddr_write

/*
 * RV64 direct-interpreter design.
 *
 * This file is the RV64 counterpart of the newer RV32 direct interpreter.  It
 * deliberately keeps decode, operand extraction, architectural checks, and
 * execution in one translation unit so an instruction's full behaviour is easy
 * to audit.  The old table-interpreter split spread the same logic across
 * decode helpers, instruction fragments, and RTL-style temporary operations;
 * that made it hard to prove trap ordering when RV64 started sharing the
 * stricter RISC-V exception model used by RV32.
 *
 * The execution contract is:
 *   1. `isa_exec_once()` fetches exactly one 32-bit base instruction and stores
 *      it in `Decode::isa.inst`.  Compressed instructions are not decoded here.
 *   2. `decode_exec()` sets the default next PC to `snpc`, then the pattern
 *      table either commits one instruction, redirects `dnpc`, or raises a
 *      trap.  Each instruction body is written in architectural order.
 *   3. Operand helpers decode raw register indexes and immediates from the
 *      instruction word.  `decode_operand()` validates the register indexes
 *      before reading guest state, so a future wider register encoding cannot
 *      silently index outside `cpu.gpr[]`.
 *   4. Trap helpers update `dnpc` through `isa_raise_intr_tval()` and call
 *      `difftest_skip_ref()`.  A trapping instruction must return before
 *      writing its destination register or changing memory.
 *   5. Memory and control-flow helpers check natural alignment before the
 *      visible load/store/jump effect.  This gives visible RISC-V traps for
 *      misaligned scalar accesses and misaligned jump targets.
 *   6. The last action in `decode_exec()` restores x0 to zero.  Instruction
 *      bodies may write `R(0)` naturally; the architectural zero register is
 *      enforced once at the common exit.
 *
 * Keep comments here explicit.  Most helpers encode one small rule from the
 * unprivileged or privileged RISC-V specification, and their purpose is easier
 * to verify when the comment names both the bit layout and the commit-order
 * constraint that the helper protects.
 */

enum
{
    TYPE_R,
    TYPE_I,
    TYPE_U,
    TYPE_S,
    TYPE_B,
    TYPE_J,
    TYPE_CSR,
    TYPE_CSI,
    TYPE_N, // none
};

enum
{
    RV64_RELOP_EQ,
    RV64_RELOP_NE,
    RV64_RELOP_LT,
    RV64_RELOP_GE,
    RV64_RELOP_LTU,
    RV64_RELOP_GEU,
};

/* Extract the 12-bit CSR address from bits [31:20] of SYSTEM instructions. */
static inline uint32_t csr_addr(uint32_t inst)
{
    return BITS(inst, 31, 20);
}

/* Extract the destination GPR index from the standard rd field, bits [11:7]. */
static inline uint32_t rd_idx(uint32_t inst)
{
    return BITS(inst, 11, 7);
}

/* Extract the first source GPR index from the standard rs1 field, bits [19:15]. */
static inline uint32_t rs1_idx(uint32_t inst)
{
    return BITS(inst, 19, 15);
}

/* Extract the second source GPR index from the standard rs2 field, bits [24:20]. */
static inline uint32_t rs2_idx(uint32_t inst)
{
    return BITS(inst, 24, 20);
}

/* Decode and sign-extend the contiguous I-format immediate field. */
static inline word_t imm_i(uint32_t inst)
{
    return (word_t)SEXT(BITS(inst, 31, 20), 12);
}

/*
 * Decode the U-format immediate payload.  LUI/AUIPC sign-extension is applied
 * later through `rv64_u_imm_value()` because the raw field is also useful when
 * reading the bit layout.
 */
static inline word_t imm_u(uint32_t inst)
{
    return (word_t)(inst & 0xfffff000u);
}

/* Decode and sign-extend the S-format immediate split across bits [31:25] and [11:7]. */
static inline word_t imm_s(uint32_t inst)
{
    return (word_t)SEXT((BITS(inst, 31, 25) << 5) | BITS(inst, 11, 7), 12);
}

/* Decode and sign-extend the B-format branch offset, including its implicit low zero bit. */
static inline word_t imm_b(uint32_t inst)
{
    uint32_t raw = (BITS(inst, 31, 31) << 12) |
                   (BITS(inst, 7, 7) << 11) |
                   (BITS(inst, 30, 25) << 5) |
                   (BITS(inst, 11, 8) << 1);
    return (word_t)SEXT(raw, 13);
}

/* Decode and sign-extend the J-format jump offset, including its implicit low zero bit. */
static inline word_t imm_j(uint32_t inst)
{
    uint32_t raw = (BITS(inst, 31, 31) << 20) |
                   (BITS(inst, 19, 12) << 12) |
                   (BITS(inst, 20, 20) << 11) |
                   (BITS(inst, 30, 21) << 1);
    return (word_t)SEXT(raw, 21);
}

/* Sign-extend a 32-bit W-form result to RV64 XLEN. */
static inline word_t rv64_sext32(uint32_t value)
{
    return (word_t)(int64_t)(int32_t)value;
}

/* Apply RV64's sign-extension rule for LUI/AUIPC U immediates. */
static inline word_t rv64_u_imm_value(word_t imm)
{
    return rv64_sext32((uint32_t)imm);
}

/*
 * Raise a guest-visible trap and redirect the current instruction's next PC.
 * The caller must stop normal execution after this helper, because the trap is
 * the architectural effect of the instruction and no later writeback may
 * partially commit.
 */
static inline void riscv64_raise_trap(Decode *s, word_t cause, word_t tval)
{
    /*
     * Direct helpers own trap delivery. They redirect dnpc and return before
     * performing the trapping instruction's normal writeback or memory effect.
     */
    s->dnpc = isa_raise_intr_tval(cause, s->pc, tval);
    difftest_skip_ref();
}

/* Validate a decoded GPR index before it can read or write CPU_state. */
static inline bool riscv64_reg_ok(Decode *s, uint32_t idx)
{
    if (idx >= 32)
    {
        riscv64_raise_trap(s, RISCV64_CAUSE_ILLEGAL_INST, 0);
        return false;
    }

    return true;
}

/*
 * Decode operands for one instruction pattern and read source registers only
 * after validating the required indexes.  CSR immediate forms use the rs1 field
 * as a five-bit unsigned immediate, so TYPE_CSI intentionally stores `rs1`
 * itself in `src1` rather than reading a GPR.
 */
static bool decode_operand(Decode *s, int *rd, int *rs1, int *rs2,
                           word_t *src1, word_t *src2, word_t *imm, int type)
{
    uint32_t inst = s->isa.inst;
    *rd = rd_idx(inst);
    *rs1 = rs1_idx(inst);
    *rs2 = rs2_idx(inst);
    *src1 = 0;
    *src2 = 0;
    *imm = 0;

    switch (type)
    {
    case TYPE_R:
        if (!riscv64_reg_ok(s, *rd) ||
            !riscv64_reg_ok(s, *rs1) ||
            !riscv64_reg_ok(s, *rs2))
            return false;
        *src1 = R(*rs1);
        *src2 = R(*rs2);
        break;
    case TYPE_I:
        if (!riscv64_reg_ok(s, *rd) ||
            !riscv64_reg_ok(s, *rs1))
            return false;
        *src1 = R(*rs1);
        *imm = imm_i(inst);
        break;
    case TYPE_U:
        if (!riscv64_reg_ok(s, *rd))
            return false;
        *imm = imm_u(inst);
        break;
    case TYPE_S:
        if (!riscv64_reg_ok(s, *rs1) ||
            !riscv64_reg_ok(s, *rs2))
            return false;
        *src1 = R(*rs1);
        *src2 = R(*rs2);
        *imm = imm_s(inst);
        break;
    case TYPE_B:
        if (!riscv64_reg_ok(s, *rs1) ||
            !riscv64_reg_ok(s, *rs2))
            return false;
        *src1 = R(*rs1);
        *src2 = R(*rs2);
        *imm = imm_b(inst);
        break;
    case TYPE_J:
        if (!riscv64_reg_ok(s, *rd))
            return false;
        *imm = imm_j(inst);
        break;
    case TYPE_CSR:
        if (!riscv64_reg_ok(s, *rd) ||
            !riscv64_reg_ok(s, *rs1))
            return false;
        *src1 = R(*rs1);
        *imm = csr_addr(inst);
        break;
    case TYPE_CSI:
        if (!riscv64_reg_ok(s, *rd))
            return false;
        *src1 = *rs1;
        *imm = csr_addr(inst);
        break;
    case TYPE_N:
        break;
    default:
        panic("unsupported type = %d", type);
    }

    return true;
}

/* Return whether an address satisfies the natural alignment required by its width. */
static inline bool riscv64_is_naturally_aligned(word_t addr, int len)
{
    return (addr & (word_t)(len - 1)) == 0;
}

/* Check load alignment before any destination register can be modified. */
static inline bool riscv64_check_load_alignment(Decode *s, word_t addr, int len)
{
    if (len > 1 && !riscv64_is_naturally_aligned(addr, len))
    {
        riscv64_raise_trap(s, RISCV64_CAUSE_LOAD_ADDR_MISALIGNED, addr);
        return false;
    }

    return true;
}

/* Check store alignment before any memory byte can be modified. */
static inline bool riscv64_check_store_alignment(Decode *s, word_t addr, int len)
{
    if (len > 1 && !riscv64_is_naturally_aligned(addr, len))
    {
        riscv64_raise_trap(s, RISCV64_CAUSE_STORE_ADDR_MISALIGNED, addr);
        return false;
    }

    return true;
}

/* Check JAL/JALR/branch targets while this RV64 path has no compressed-instruction mode. */
static inline bool riscv64_check_jump_alignment(Decode *s, word_t target)
{
    if ((target & 0x3u) != 0)
    {
        riscv64_raise_trap(s, RISCV64_CAUSE_INST_ADDR_MISALIGNED, target);
        return false;
    }

    return true;
}

/*
 * Return an implemented CSR pointer or raise an illegal-instruction trap.
 * The helper enforces implemented-address, current-privilege, and read-only
 * checks before the instruction reads or writes CSR state.
 */
static inline rtlreg_t *riscv64_get_csr_or_trap(Decode *s, word_t addr, bool will_write)
{
    const word_t required_priv = (addr >> 8) & 0x3u;

    if (!isCSRImplemented(addr) ||
        cpu.prvi < required_priv ||
        (will_write && !isCSRWriteable(addr)))
    {
        riscv64_raise_trap(s, RISCV64_CAUSE_ILLEGAL_INST, 0);
        return NULL;
    }

    return getCSRAddress(addr);
}

/* Write a CSR, applying the local WARL normalisation that RV64 currently models. */
static inline void riscv64_write_csr(word_t addr, rtlreg_t *csr, word_t value)
{
    *csr = addr == 0x300 ? riscv64_mstatus_normalise(value) : value;
}

/* Evaluate one branch comparison using signedness selected by the decoded funct3. */
static inline bool riscv64_branch_taken(int relop, word_t lhs, word_t rhs)
{
    switch (relop)
    {
    case RV64_RELOP_EQ:
        return lhs == rhs;
    case RV64_RELOP_NE:
        return lhs != rhs;
    case RV64_RELOP_LT:
        return (sword_t)lhs < (sword_t)rhs;
    case RV64_RELOP_GE:
        return (sword_t)lhs >= (sword_t)rhs;
    case RV64_RELOP_LTU:
        return lhs < rhs;
    case RV64_RELOP_GEU:
        return lhs >= rhs;
    default:
        return false;
    }
}

/* Redirect `dnpc` to a branch target only after condition and alignment both pass. */
static inline void riscv64_branch(Decode *s, int relop, word_t src1, word_t src2, word_t imm)
{
    if (!riscv64_branch_taken(relop, src1, src2))
    {
        return;
    }

    word_t target = s->pc + imm;
    if (riscv64_check_jump_alignment(s, target))
    {
        s->dnpc = target;
    }
}

/* Return the high 64 bits of a signed-by-signed 128-bit product for MULH. */
static inline word_t riscv64_mulh(word_t src1, word_t src2)
{
    __int128 lhs = (__int128)(sword_t)src1;
    __int128 rhs = (__int128)(sword_t)src2;
    return (word_t)((unsigned __int128)(lhs * rhs) >> 64);
}

/* Return the high 64 bits of a signed-by-unsigned 128-bit product for MULHSU. */
static inline word_t riscv64_mulhsu(word_t src1, word_t src2)
{
    __int128 lhs = (__int128)(sword_t)src1;
    __int128 rhs = (__int128)(uint64_t)src2;
    return (word_t)((unsigned __int128)(lhs * rhs) >> 64);
}

/* Return the high 64 bits of an unsigned-by-unsigned 128-bit product for MULHU. */
static inline word_t riscv64_mulhu(word_t src1, word_t src2)
{
    unsigned __int128 lhs = (uint64_t)src1;
    unsigned __int128 rhs = (uint64_t)src2;
    return (word_t)((lhs * rhs) >> 64);
}

/* Implement RV64 signed division, including divide-by-zero and overflow edge cases. */
static inline word_t riscv64_div(word_t src1, word_t src2)
{
    sword_t dividend = (sword_t)src1;
    sword_t divisor = (sword_t)src2;

    if (divisor == 0)
        return (word_t)-1;
    if (dividend == INT64_MIN && divisor == -1)
        return src1;
    return (word_t)(dividend / divisor);
}

/* Implement RV64 unsigned division, including the all-ones divide-by-zero result. */
static inline word_t riscv64_divu(word_t src1, word_t src2)
{
    if (src2 == 0)
        return ~(word_t)0;
    return src1 / src2;
}

/* Implement RV64 signed remainder, including divide-by-zero and overflow edge cases. */
static inline word_t riscv64_rem(word_t src1, word_t src2)
{
    sword_t dividend = (sword_t)src1;
    sword_t divisor = (sword_t)src2;

    if (divisor == 0)
        return src1;
    if (dividend == INT64_MIN && divisor == -1)
        return 0;
    return (word_t)(dividend % divisor);
}

/* Implement RV64 unsigned remainder, where divide-by-zero returns the dividend. */
static inline word_t riscv64_remu(word_t src1, word_t src2)
{
    if (src2 == 0)
        return src1;
    return src1 % src2;
}

/* Implement RV64 DIVW and sign-extend the 32-bit quotient to XLEN. */
static inline word_t riscv64_divw(word_t src1, word_t src2)
{
    int32_t dividend = (int32_t)src1;
    int32_t divisor = (int32_t)src2;

    if (divisor == 0)
        return (word_t)-1;
    if (dividend == INT32_MIN && divisor == -1)
        return rv64_sext32((uint32_t)dividend);
    return rv64_sext32((uint32_t)(dividend / divisor));
}

/* Implement RV64 DIVUW and sign-extend the low 32-bit quotient to XLEN. */
static inline word_t riscv64_divuw(word_t src1, word_t src2)
{
    uint32_t dividend = (uint32_t)src1;
    uint32_t divisor = (uint32_t)src2;

    if (divisor == 0)
        return (word_t)-1;
    return rv64_sext32(dividend / divisor);
}

/* Implement RV64 REMW and sign-extend the 32-bit remainder to XLEN. */
static inline word_t riscv64_remw(word_t src1, word_t src2)
{
    int32_t dividend = (int32_t)src1;
    int32_t divisor = (int32_t)src2;

    if (divisor == 0)
        return rv64_sext32((uint32_t)dividend);
    if (dividend == INT32_MIN && divisor == -1)
        return 0;
    return rv64_sext32((uint32_t)(dividend % divisor));
}

/* Implement RV64 REMUW and sign-extend the low 32-bit remainder to XLEN. */
static inline word_t riscv64_remuw(word_t src1, word_t src2)
{
    uint32_t dividend = (uint32_t)src1;
    uint32_t divisor = (uint32_t)src2;

    if (divisor == 0)
        return rv64_sext32(dividend);
    return rv64_sext32(dividend % divisor);
}

/*
 * Execute MRET in architectural order: validate privilege and MPP, restore MIE
 * from MPIE, set MPIE, clear MPP, optionally clear MPRV, update privilege, and
 * finally redirect to MEPC.  The target PC is not alignment-checked here because
 * RISC-V defines the return target through the CSR value and the next fetch path
 * owns instruction-address faults.
 */
static inline void riscv64_mret(Decode *s)
{
    if (cpu.prvi != RISCV64_PRIV_M)
    {
        riscv64_raise_trap(s, RISCV64_CAUSE_ILLEGAL_INST, 0);
        return;
    }

    word_t mstatus = cpu.csr.mstatus;
    const word_t mpp = (mstatus >> 11) & 0x3u;
    const word_t mpie = (mstatus >> 7) & 0x1u;

    if (mpp == 0x2u)
    {
        riscv64_raise_trap(s, RISCV64_CAUSE_ILLEGAL_INST, 0);
        return;
    }

    mstatus &= ~((word_t)0x3u << 11);
    mstatus = (mstatus & ~((word_t)1u << 3)) | (mpie << 3);
    mstatus |= ((word_t)1u << 7);

    /* MPRV is cleared when returning to a mode below M-mode. */
    if (mpp != RISCV64_PRIV_M)
    {
        mstatus &= ~((word_t)1u << 17);
    }

    cpu.csr.mstatus = riscv64_mstatus_normalise(mstatus);
    cpu.prvi = mpp;
    s->dnpc = cpu.csr.mepc;
}

/*
 * Execute SFENCE.VMA for the modelled RV64 state.  The interpreter does not
 * keep a software address-translation cache, but the RV64 JIT has a data TLB;
 * therefore the instruction flushes that cache when JIT support is compiled in.
 */
static inline void riscv64_sfence_vma(Decode *s)
{
    const word_t mstatus_tvm = (word_t)1u << 20;

    if (!riscv64_reg_ok(s, rs1_idx(s->isa.inst)) ||
        !riscv64_reg_ok(s, rs2_idx(s->isa.inst)))
    {
        return;
    }

    if (cpu.prvi == RISCV64_PRIV_U ||
        (cpu.prvi == RISCV64_PRIV_S && (cpu.csr.mstatus & mstatus_tvm) != 0))
    {
        riscv64_raise_trap(s, RISCV64_CAUSE_ILLEGAL_INST, 0);
        return;
    }

#ifdef CONFIG_RV64_JIT
    isa_jit_flush_data_tlb();
#endif
}

/*
 * Match and execute one fetched RV64 instruction.  Pattern order matters:
 * specific SYSTEM encodings appear before the final invalid-instruction catch
 * all, and each instruction body is responsible for preserving the no-partial
 * commit rule when it can trap.
 */
static int decode_exec(Decode *s)
{
    s->dnpc = s->snpc;

#define INSTPAT_INST(s) ((s)->isa.inst)
#define INSTPAT_MATCH(s, name, type, ... /* execute body */) \
    { \
        int rd = 0, rs1 = 0, rs2 = 0; \
        word_t src1 = 0, src2 = 0, imm = 0; \
        if (decode_operand(s, &rd, &rs1, &rs2, &src1, &src2, &imm, concat(TYPE_, type))) \
        { \
            __VA_ARGS__; \
        } \
    }

    INSTPAT_START();
    INSTPAT("??????? ????? ????? ??? ????? 01101 11", lui, U, R(rd) = rv64_u_imm_value(imm));
    INSTPAT("??????? ????? ????? ??? ????? 00101 11", auipc, U, R(rd) = s->pc + rv64_u_imm_value(imm));

    INSTPAT("??????? ????? ????? 000 ????? 00000 11", lb, I, R(rd) = SEXT(Mr(src1 + imm, 1), 8));
    INSTPAT("??????? ????? ????? 001 ????? 00000 11", lh, I,
            if (riscv64_check_load_alignment(s, src1 + imm, 2))
                R(rd) = SEXT(Mr(src1 + imm, 2), 16));
    INSTPAT("??????? ????? ????? 010 ????? 00000 11", lw, I,
            if (riscv64_check_load_alignment(s, src1 + imm, 4))
                R(rd) = SEXT(Mr(src1 + imm, 4), 32));
    INSTPAT("??????? ????? ????? 011 ????? 00000 11", ld, I,
            if (riscv64_check_load_alignment(s, src1 + imm, 8))
                R(rd) = Mr(src1 + imm, 8));
    INSTPAT("??????? ????? ????? 100 ????? 00000 11", lbu, I, R(rd) = Mr(src1 + imm, 1));
    INSTPAT("??????? ????? ????? 101 ????? 00000 11", lhu, I,
            if (riscv64_check_load_alignment(s, src1 + imm, 2))
                R(rd) = Mr(src1 + imm, 2));
    INSTPAT("??????? ????? ????? 110 ????? 00000 11", lwu, I,
            if (riscv64_check_load_alignment(s, src1 + imm, 4))
                R(rd) = Mr(src1 + imm, 4));

    INSTPAT("??????? ????? ????? 000 ????? 01000 11", sb, S, Mw(src1 + imm, 1, src2));
    INSTPAT("??????? ????? ????? 001 ????? 01000 11", sh, S,
            if (riscv64_check_store_alignment(s, src1 + imm, 2))
                Mw(src1 + imm, 2, src2));
    INSTPAT("??????? ????? ????? 010 ????? 01000 11", sw, S,
            if (riscv64_check_store_alignment(s, src1 + imm, 4))
                Mw(src1 + imm, 4, src2));
    INSTPAT("??????? ????? ????? 011 ????? 01000 11", sd, S,
            if (riscv64_check_store_alignment(s, src1 + imm, 8))
                Mw(src1 + imm, 8, src2));

    INSTPAT("??????? ????? ????? 000 ????? 00100 11", addi, I, R(rd) = src1 + imm);
    INSTPAT("000000? ????? ????? 001 ????? 00100 11", slli, I, R(rd) = src1 << (imm & 0x3f));
    INSTPAT("??????? ????? ????? 010 ????? 00100 11", slti, I, R(rd) = (sword_t)src1 < (sword_t)imm);
    INSTPAT("??????? ????? ????? 011 ????? 00100 11", sltiu, I, R(rd) = src1 < imm);
    INSTPAT("??????? ????? ????? 100 ????? 00100 11", xori, I, R(rd) = src1 ^ imm);
    INSTPAT("000000? ????? ????? 101 ????? 00100 11", srli, I, R(rd) = src1 >> (imm & 0x3f));
    INSTPAT("010000? ????? ????? 101 ????? 00100 11", srai, I, R(rd) = (word_t)((sword_t)src1 >> (imm & 0x3f)));
    INSTPAT("??????? ????? ????? 110 ????? 00100 11", ori, I, R(rd) = src1 | imm);
    INSTPAT("??????? ????? ????? 111 ????? 00100 11", andi, I, R(rd) = src1 & imm);

    INSTPAT("??????? ????? ????? 000 ????? 00110 11", addiw, I, R(rd) = rv64_sext32((uint32_t)(src1 + imm)));
    INSTPAT("0000000 ????? ????? 001 ????? 00110 11", slliw, I, R(rd) = rv64_sext32((uint32_t)src1 << (imm & 0x1f)));
    INSTPAT("0000000 ????? ????? 101 ????? 00110 11", srliw, I, R(rd) = rv64_sext32((uint32_t)src1 >> (imm & 0x1f)));
    INSTPAT("0100000 ????? ????? 101 ????? 00110 11", sraiw, I, R(rd) = rv64_sext32((uint32_t)((int32_t)src1 >> (imm & 0x1f))));

    INSTPAT("0000000 ????? ????? 000 ????? 01100 11", add, R, R(rd) = src1 + src2);
    INSTPAT("0100000 ????? ????? 000 ????? 01100 11", sub, R, R(rd) = src1 - src2);
    INSTPAT("0000000 ????? ????? 001 ????? 01100 11", sll, R, R(rd) = src1 << (src2 & 0x3f));
    INSTPAT("0000000 ????? ????? 010 ????? 01100 11", slt, R, R(rd) = (sword_t)src1 < (sword_t)src2);
    INSTPAT("0000000 ????? ????? 011 ????? 01100 11", sltu, R, R(rd) = src1 < src2);
    INSTPAT("0000000 ????? ????? 100 ????? 01100 11", xor, R, R(rd) = src1 ^ src2);
    INSTPAT("0000000 ????? ????? 101 ????? 01100 11", srl, R, R(rd) = src1 >> (src2 & 0x3f));
    INSTPAT("0100000 ????? ????? 101 ????? 01100 11", sra, R, R(rd) = (word_t)((sword_t)src1 >> (src2 & 0x3f)));
    INSTPAT("0000000 ????? ????? 110 ????? 01100 11", or, R, R(rd) = src1 | src2);
    INSTPAT("0000000 ????? ????? 111 ????? 01100 11", and, R, R(rd) = src1 & src2);

    INSTPAT("0000001 ????? ????? 000 ????? 01100 11", mul, R, R(rd) = src1 * src2);
    INSTPAT("0000001 ????? ????? 001 ????? 01100 11", mulh, R, R(rd) = riscv64_mulh(src1, src2));
    INSTPAT("0000001 ????? ????? 010 ????? 01100 11", mulhsu, R, R(rd) = riscv64_mulhsu(src1, src2));
    INSTPAT("0000001 ????? ????? 011 ????? 01100 11", mulhu, R, R(rd) = riscv64_mulhu(src1, src2));
    INSTPAT("0000001 ????? ????? 100 ????? 01100 11", div, R, R(rd) = riscv64_div(src1, src2));
    INSTPAT("0000001 ????? ????? 101 ????? 01100 11", divu, R, R(rd) = riscv64_divu(src1, src2));
    INSTPAT("0000001 ????? ????? 110 ????? 01100 11", rem, R, R(rd) = riscv64_rem(src1, src2));
    INSTPAT("0000001 ????? ????? 111 ????? 01100 11", remu, R, R(rd) = riscv64_remu(src1, src2));

    INSTPAT("0000000 ????? ????? 000 ????? 01110 11", addw, R, R(rd) = rv64_sext32((uint32_t)(src1 + src2)));
    INSTPAT("0100000 ????? ????? 000 ????? 01110 11", subw, R, R(rd) = rv64_sext32((uint32_t)(src1 - src2)));
    INSTPAT("0000000 ????? ????? 001 ????? 01110 11", sllw, R, R(rd) = rv64_sext32((uint32_t)src1 << (src2 & 0x1f)));
    INSTPAT("0000000 ????? ????? 101 ????? 01110 11", srlw, R, R(rd) = rv64_sext32((uint32_t)src1 >> (src2 & 0x1f)));
    INSTPAT("0100000 ????? ????? 101 ????? 01110 11", sraw, R, R(rd) = rv64_sext32((uint32_t)((int32_t)src1 >> (src2 & 0x1f))));
    INSTPAT("0000001 ????? ????? 000 ????? 01110 11", mulw, R, R(rd) = rv64_sext32((uint32_t)((uint32_t)src1 * (uint32_t)src2)));
    INSTPAT("0000001 ????? ????? 100 ????? 01110 11", divw, R, R(rd) = riscv64_divw(src1, src2));
    INSTPAT("0000001 ????? ????? 101 ????? 01110 11", divuw, R, R(rd) = riscv64_divuw(src1, src2));
    INSTPAT("0000001 ????? ????? 110 ????? 01110 11", remw, R, R(rd) = riscv64_remw(src1, src2));
    INSTPAT("0000001 ????? ????? 111 ????? 01110 11", remuw, R, R(rd) = riscv64_remuw(src1, src2));

    INSTPAT("??????? ????? ????? 000 ????? 11000 11", beq, B, riscv64_branch(s, RV64_RELOP_EQ, src1, src2, imm));
    INSTPAT("??????? ????? ????? 001 ????? 11000 11", bne, B, riscv64_branch(s, RV64_RELOP_NE, src1, src2, imm));
    INSTPAT("??????? ????? ????? 100 ????? 11000 11", blt, B, riscv64_branch(s, RV64_RELOP_LT, src1, src2, imm));
    INSTPAT("??????? ????? ????? 101 ????? 11000 11", bge, B, riscv64_branch(s, RV64_RELOP_GE, src1, src2, imm));
    INSTPAT("??????? ????? ????? 110 ????? 11000 11", bltu, B, riscv64_branch(s, RV64_RELOP_LTU, src1, src2, imm));
    INSTPAT("??????? ????? ????? 111 ????? 11000 11", bgeu, B, riscv64_branch(s, RV64_RELOP_GEU, src1, src2, imm));

    INSTPAT("??????? ????? ????? ??? ????? 11011 11", jal, J,
            {
                word_t target = s->pc + imm;
                if (riscv64_check_jump_alignment(s, target))
                {
                    if (rd == 1 || rd == 5)
                    {
                        ftrace_call(s->pc, target);
                    }
                    R(rd) = s->pc + 4;
                    s->dnpc = target;
                }
            });
    INSTPAT("??????? ????? ????? 000 ????? 11001 11", jalr, I,
            {
                word_t target = (src1 + imm) & ~(word_t)1;
                if (riscv64_check_jump_alignment(s, target))
                {
                    if (rd == 0 && (rs1 == 1 || rs1 == 5) && imm == 0)
                    {
                        ftrace_ret(s->pc);
                    }
                    else if (rd == 1 || rd == 5)
                    {
                        ftrace_call(s->pc, target);
                    }
                    R(rd) = s->pc + 4;
                    s->dnpc = target;
                }
            });

    INSTPAT("??????? ????? ????? 000 ????? 00011 11", fence, N, );
    INSTPAT("??????? ????? ????? 001 ????? 00011 11", fence_i, N, );

    INSTPAT("0000000 00000 00000 000 00000 11100 11", ecall, N,
            riscv64_raise_trap(s, riscv64_ecall_cause_from_priv(cpu.prvi), 0));
    INSTPAT("0000000 00001 00000 000 00000 11100 11", ebreak, N,
            riscv64_raise_trap(s, RISCV64_CAUSE_BREAKPOINT, 0));
    INSTPAT("0011000 00010 00000 000 00000 11100 11", mret, N, riscv64_mret(s));
    INSTPAT("0001000 00101 00000 000 00000 11100 11", wfi, N, difftest_skip_ref());
    INSTPAT("0001001 ????? ????? 000 00000 11100 11", sfence_vma, N, riscv64_sfence_vma(s));

    INSTPAT("??????? ????? ????? 001 ????? 11100 11", csrrw, CSR,
            {
                rtlreg_t *csr = riscv64_get_csr_or_trap(s, imm, true);
                if (csr != NULL)
                {
                    if (rd != 0)
                    {
                        rtlreg_t old = *csr;
                        riscv64_write_csr(imm, csr, src1);
                        R(rd) = old;
                    }
                    else
                    {
                        riscv64_write_csr(imm, csr, src1);
                    }
                }
            });
    INSTPAT("??????? ????? ????? 010 ????? 11100 11", csrrs, CSR,
            {
                bool will_write = rs1 != 0;
                rtlreg_t *csr = riscv64_get_csr_or_trap(s, imm, will_write);
                if (csr != NULL)
                {
                    R(rd) = *csr;
                    if (will_write)
                    {
                        riscv64_write_csr(imm, csr, *csr | src1);
                    }
                }
            });
    INSTPAT("??????? ????? ????? 011 ????? 11100 11", csrrc, CSR,
            {
                bool will_write = rs1 != 0;
                rtlreg_t *csr = riscv64_get_csr_or_trap(s, imm, will_write);
                if (csr != NULL)
                {
                    R(rd) = *csr;
                    if (will_write)
                    {
                        riscv64_write_csr(imm, csr, *csr & ~src1);
                    }
                }
            });
    INSTPAT("??????? ????? ????? 101 ????? 11100 11", csrrwi, CSI,
            {
                rtlreg_t *csr = riscv64_get_csr_or_trap(s, imm, true);
                if (csr != NULL)
                {
                    if (rd != 0)
                    {
                        rtlreg_t old = *csr;
                        riscv64_write_csr(imm, csr, src1);
                        R(rd) = old;
                    }
                    else
                    {
                        riscv64_write_csr(imm, csr, src1);
                    }
                }
            });
    INSTPAT("??????? ????? ????? 110 ????? 11100 11", csrrsi, CSI,
            {
                bool will_write = rs1 != 0;
                rtlreg_t *csr = riscv64_get_csr_or_trap(s, imm, will_write);
                if (csr != NULL)
                {
                    R(rd) = *csr;
                    if (will_write)
                    {
                        riscv64_write_csr(imm, csr, *csr | src1);
                    }
                }
            });
    INSTPAT("??????? ????? ????? 111 ????? 11100 11", csrrci, CSI,
            {
                bool will_write = rs1 != 0;
                rtlreg_t *csr = riscv64_get_csr_or_trap(s, imm, will_write);
                if (csr != NULL)
                {
                    R(rd) = *csr;
                    if (will_write)
                    {
                        riscv64_write_csr(imm, csr, *csr & ~src1);
                    }
                }
            });

    INSTPAT("??????? ????? ????? ??? ????? 11010 11", nemu_trap, N, NEMUTRAP(s->pc, R(10))); // R(10) is $a0
    INSTPAT("??????? ????? ????? ??? ????? ????? ??", inv, N,
            riscv64_raise_trap(s, RISCV64_CAUSE_ILLEGAL_INST, 0));
    INSTPAT_END();

    R(0) = 0; // reset $zero to 0

    return 0;
}

/* Fetch one 32-bit RV64 instruction and execute it through the direct matcher. */
int isa_exec_once(Decode *s)
{
    s->isa.inst = inst_fetch(&s->snpc, 4);
    return decode_exec(s);
}
