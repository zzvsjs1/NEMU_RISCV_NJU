#include "local-include/reg.h"
#ifdef CONFIG_RV64_FPU
#include "local-include/fpu.h"
#endif
#include <cpu/cpu.h>
#include <cpu/decode.h>
#include <cpu/difftest.h>
#include <cpu/ifetch.h>
#if defined(CONFIG_RV32_JIT) || defined(CONFIG_RV64_JIT)
#include <isa-jit.h>
#endif
#include <memory/vaddr.h>
#include <utils.h>

#define R(i) gpr(i)
#define Mr vaddr_read
#define Mw vaddr_write

/*
 * RISC-V direct-interpreter design.
 *
 * This file is shared by RV32, RV32E, and RV64.  It deliberately keeps decode,
 * operand extraction, architectural checks, and execution in one translation
 * unit so an instruction's full behaviour is easy to audit.
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
    /*
     * Operand formats used by the direct interpreter.  They mirror the common
     * RISC-V instruction layouts, plus the two CSR forms where the 12-bit CSR
     * address lives in the I-immediate field.
     */
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

/*
 * Shift amounts are masked by XLEN: RV32 uses five low bits and RV64 uses six.
 * The original immediate encodings are still matched by INSTPAT, so this mask
 * only protects the C shift operation from seeing an oversized host shift.
 */
#define RISCV_SHIFT_MASK MUXDEF(CONFIG_RV64, 0x3f, 0x1f)

enum
{
    /*
     * Branch relation ids.  Keeping them local avoids coupling the direct
     * interpreter to the older RTL relation enum used by table-interpreter ISAs.
     */
    RISCV_RELOP_EQ,
    RISCV_RELOP_NE,
    RISCV_RELOP_LT,
    RISCV_RELOP_GE,
    RISCV_RELOP_LTU,
    RISCV_RELOP_GEU,
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
 * Decode the U-format immediate payload.  RV64 sign-extension is applied later
 * through `riscv_u_imm_value()` because the raw field is also useful when
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

/* Sign-extend a 32-bit W-form result to the current XLEN. */
static inline word_t riscv_sext32(uint32_t value)
{
    return (word_t)(int64_t)(int32_t)value;
}

/* Apply RV64's sign-extension rule for LUI/AUIPC U immediates. */
static inline word_t riscv_u_imm_value(word_t imm)
{
    return MUXDEF(CONFIG_RV64, riscv_sext32((uint32_t)imm), imm);
}

/*
 * Raise a guest-visible trap and redirect the current instruction's next PC.
 * The caller must stop normal execution after this helper, because the trap is
 * the architectural effect of the instruction and no later writeback may
 * partially commit.
 */
static inline void riscv_raise_trap(Decode *s, word_t cause, word_t tval)
{
    /*
     * Direct helpers own trap delivery. They redirect dnpc and return before
     * performing the trapping instruction's normal writeback or memory effect.
     */
    s->dnpc = isa_raise_intr_tval(cause, s->pc, tval);
    difftest_skip_ref();
}

/* Validate a decoded GPR index before it can read or write CPU_state. */
static inline bool riscv_reg_ok(Decode *s, uint32_t idx)
{
    if (idx >= RISCV_GPR_NUM)
    {
        riscv_raise_trap(s, RISCV_CAUSE_ILLEGAL_INST, 0);
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
        /*
         * Register-register arithmetic, logical, multiply/divide, and W-form
         * operations all read rs1/rs2 and may write rd.
         */
        if (!riscv_reg_ok(s, *rd) ||
            !riscv_reg_ok(s, *rs1) ||
            !riscv_reg_ok(s, *rs2))
            return false;
        *src1 = R(*rs1);
        *src2 = R(*rs2);
        break;
    case TYPE_I:
        /*
         * I-format instructions use rs1 plus a sign-extended 12-bit immediate:
         * loads, ALU-immediate operations, JALR, and some SYSTEM encodings.
         */
        if (!riscv_reg_ok(s, *rd) ||
            !riscv_reg_ok(s, *rs1))
            return false;
        *src1 = R(*rs1);
        *imm = imm_i(inst);
        break;
    case TYPE_U:
        /*
         * U-format instructions only need rd and the upper 20-bit immediate;
         * the low 12 bits are architecturally zero.
         */
        if (!riscv_reg_ok(s, *rd))
            return false;
        *imm = imm_u(inst);
        break;
    case TYPE_S:
        /*
         * Stores read a base register and a data register.  They have no rd, so
         * a trapping store can return before any memory byte is changed.
         */
        if (!riscv_reg_ok(s, *rs1) ||
            !riscv_reg_ok(s, *rs2))
            return false;
        *src1 = R(*rs1);
        *src2 = R(*rs2);
        *imm = imm_s(inst);
        break;
    case TYPE_B:
        /*
         * Branches compare two registers and use the scattered B-immediate.
         * They never write rd; the only visible effect is a possible dnpc change.
         */
        if (!riscv_reg_ok(s, *rs1) ||
            !riscv_reg_ok(s, *rs2))
            return false;
        *src1 = R(*rs1);
        *src2 = R(*rs2);
        *imm = imm_b(inst);
        break;
    case TYPE_J:
        /*
         * JAL writes rd with the link address and computes its target from the
         * J-immediate.  No source register is read.
         */
        if (!riscv_reg_ok(s, *rd))
            return false;
        *imm = imm_j(inst);
        break;
    case TYPE_CSR:
        /*
         * Register CSR instructions read rs1 as the write/set/clear operand and
         * use rd to receive the old CSR value when rd is not x0.
         */
        if (!riscv_reg_ok(s, *rd) ||
            !riscv_reg_ok(s, *rs1))
            return false;
        *src1 = R(*rs1);
        *imm = csr_addr(inst);
        break;
    case TYPE_CSI:
        /*
         * Immediate CSR instructions treat the rs1 field as zimm[4:0].  Do not
         * read R(rs1), because the field is data, not a register index.
         */
        if (!riscv_reg_ok(s, *rd))
            return false;
        *src1 = *rs1;
        *imm = csr_addr(inst);
        break;
    case TYPE_N:
        /* Encodings such as ECALL, MRET, and FENCE carry no decoded operands. */
        break;
    default:
        panic("unsupported type = %d", type);
    }

    return true;
}

/* Return whether an address satisfies the natural alignment required by its width. */
static inline bool riscv_is_naturally_aligned(word_t addr, int len)
{
    return (addr & (word_t)(len - 1)) == 0;
}

/* Check load alignment before any destination register can be modified. */
static inline bool riscv_check_load_alignment(Decode *s, word_t addr, int len)
{
    if (len > 1 && !riscv_is_naturally_aligned(addr, len))
    {
        riscv_raise_trap(s, RISCV_CAUSE_LOAD_ADDR_MISALIGNED, addr);
        return false;
    }

    return true;
}

/* Check store alignment before any memory byte can be modified. */
static inline bool riscv_check_store_alignment(Decode *s, word_t addr, int len)
{
    if (len > 1 && !riscv_is_naturally_aligned(addr, len))
    {
        riscv_raise_trap(s, RISCV_CAUSE_STORE_ADDR_MISALIGNED, addr);
        return false;
    }

    return true;
}

/* Check JAL/JALR/branch targets while this RISC-V path has no compressed-instruction mode. */
static inline bool riscv_check_jump_alignment(Decode *s, word_t target)
{
    if ((target & 0x3u) != 0)
    {
        riscv_raise_trap(s, RISCV_CAUSE_INST_ADDR_MISALIGNED, target);
        return false;
    }

    return true;
}

/*
 * Validate one CSR access or raise an illegal-instruction trap.  Value-based
 * access is required because fflags, frm, and fcsr are overlapping views of one
 * architectural storage value and therefore cannot each have a truthful
 * backing pointer.
 */
static inline bool riscv_csr_access_ok(Decode *s, word_t addr, bool will_write)
{
    const word_t required_priv = (addr >> 8) & 0x3u;

    if (!isCSRImplemented(addr) ||
        cpu.prvi < required_priv ||
        (will_write && !isCSRWriteable(addr)))
    {
        riscv_raise_trap(s, RISCV_CAUSE_ILLEGAL_INST, 0);
        return false;
    }

#ifdef CONFIG_RV64_FPU
    /*
     * FP control CSR addresses are unprivileged, but the privileged ISA adds a
     * separate FS gate: any read or write is illegal while mstatus.FS is Off.
     */
    if (addr >= 0x001 && addr <= 0x003 &&
        !riscv_mstatus_fp_enabled(cpu.csr.mstatus))
    {
        riscv_raise_trap(s, RISCV_CAUSE_ILLEGAL_INST, 0);
        return false;
    }
#endif

    return true;
}

/* Evaluate one branch comparison using signedness selected by the decoded funct3. */
static inline bool riscv_branch_taken(int relop, word_t lhs, word_t rhs)
{
    switch (relop)
    {
    case RISCV_RELOP_EQ:
        return lhs == rhs;
    case RISCV_RELOP_NE:
        return lhs != rhs;
    case RISCV_RELOP_LT:
        return (sword_t)lhs < (sword_t)rhs;
    case RISCV_RELOP_GE:
        return (sword_t)lhs >= (sword_t)rhs;
    case RISCV_RELOP_LTU:
        return lhs < rhs;
    case RISCV_RELOP_GEU:
        return lhs >= rhs;
    default:
        return false;
    }
}

/* Redirect `dnpc` to a branch target only after condition and alignment both pass. */
static inline void riscv_branch(Decode *s, int relop, word_t src1, word_t src2, word_t imm)
{
    if (!riscv_branch_taken(relop, src1, src2))
    {
        return;
    }

    word_t target = s->pc + imm;
    if (riscv_check_jump_alignment(s, target))
    {
        s->dnpc = target;
    }
}

/* Return the high half of a signed-by-signed product for MULH. */
static inline word_t riscv_mulh(word_t src1, word_t src2)
{
#ifdef CONFIG_RV64
    __int128 lhs = (__int128)(sword_t)src1;
    __int128 rhs = (__int128)(sword_t)src2;
    return (word_t)((unsigned __int128)(lhs * rhs) >> 64);
#else
    int64_t lhs = (int64_t)(sword_t)src1;
    int64_t rhs = (int64_t)(sword_t)src2;
    return (word_t)((uint64_t)(lhs * rhs) >> 32);
#endif
}

/* Return the high half of a signed-by-unsigned product for MULHSU. */
static inline word_t riscv_mulhsu(word_t src1, word_t src2)
{
#ifdef CONFIG_RV64
    __int128 lhs = (__int128)(sword_t)src1;
    __int128 rhs = (__int128)(uint64_t)src2;
    return (word_t)((unsigned __int128)(lhs * rhs) >> 64);
#else
    int64_t lhs = (int32_t)src1;
    uint64_t rhs = (uint32_t)src2;
    int64_t product = lhs < 0
                          ? -(int64_t)((uint64_t)(-lhs) * rhs)
                          : (int64_t)((uint64_t)lhs * rhs);
    return (word_t)((uint64_t)product >> 32);
#endif
}

/* Return the high half of an unsigned-by-unsigned product for MULHU. */
static inline word_t riscv_mulhu(word_t src1, word_t src2)
{
#ifdef CONFIG_RV64
    unsigned __int128 lhs = (uint64_t)src1;
    unsigned __int128 rhs = (uint64_t)src2;
    return (word_t)((lhs * rhs) >> 64);
#else
    uint64_t lhs = (uint32_t)src1;
    uint64_t rhs = (uint32_t)src2;
    return (word_t)((lhs * rhs) >> 32);
#endif
}

/* Return the most negative signed XLEN value, used by division overflow checks. */
static inline sword_t riscv_signed_min(void)
{
    return (sword_t)((word_t)1 << (RISCV_XLEN - 1));
}

/* Implement RISC-V signed division, including divide-by-zero and overflow edge cases. */
static inline word_t riscv_div(word_t src1, word_t src2)
{
    sword_t dividend = (sword_t)src1;
    sword_t divisor = (sword_t)src2;

    if (divisor == 0)
        return (word_t)-1;
    if (dividend == riscv_signed_min() && divisor == -1)
        return src1;
    return (word_t)(dividend / divisor);
}

/* Implement RISC-V unsigned division, including the all-ones divide-by-zero result. */
static inline word_t riscv_divu(word_t src1, word_t src2)
{
    if (src2 == 0)
        return ~(word_t)0;
    return src1 / src2;
}

/* Implement RISC-V signed remainder, including divide-by-zero and overflow edge cases. */
static inline word_t riscv_rem(word_t src1, word_t src2)
{
    sword_t dividend = (sword_t)src1;
    sword_t divisor = (sword_t)src2;

    if (divisor == 0)
        return src1;
    if (dividend == riscv_signed_min() && divisor == -1)
        return 0;
    return (word_t)(dividend % divisor);
}

/* Implement RISC-V unsigned remainder, where divide-by-zero returns the dividend. */
static inline word_t riscv_remu(word_t src1, word_t src2)
{
    if (src2 == 0)
        return src1;
    return src1 % src2;
}

/* Implement RISC-V DIVW and sign-extend the 32-bit quotient to XLEN. */
static inline word_t riscv_divw(word_t src1, word_t src2)
{
    int32_t dividend = (int32_t)src1;
    int32_t divisor = (int32_t)src2;

    if (divisor == 0)
        return (word_t)-1;
    if (dividend == INT32_MIN && divisor == -1)
        return riscv_sext32((uint32_t)dividend);
    return riscv_sext32((uint32_t)(dividend / divisor));
}

/* Implement RISC-V DIVUW and sign-extend the low 32-bit quotient to XLEN. */
static inline word_t riscv_divuw(word_t src1, word_t src2)
{
    uint32_t dividend = (uint32_t)src1;
    uint32_t divisor = (uint32_t)src2;

    if (divisor == 0)
        return (word_t)-1;
    return riscv_sext32(dividend / divisor);
}

/* Implement RISC-V REMW and sign-extend the 32-bit remainder to XLEN. */
static inline word_t riscv_remw(word_t src1, word_t src2)
{
    int32_t dividend = (int32_t)src1;
    int32_t divisor = (int32_t)src2;

    if (divisor == 0)
        return riscv_sext32((uint32_t)dividend);
    if (dividend == INT32_MIN && divisor == -1)
        return 0;
    return riscv_sext32((uint32_t)(dividend % divisor));
}

/* Implement RISC-V REMUW and sign-extend the low 32-bit remainder to XLEN. */
static inline word_t riscv_remuw(word_t src1, word_t src2)
{
    uint32_t dividend = (uint32_t)src1;
    uint32_t divisor = (uint32_t)src2;

    if (divisor == 0)
        return riscv_sext32(dividend);
    return riscv_sext32(dividend % divisor);
}

/*
 * Execute MRET in architectural order: validate privilege and MPP, restore MIE
 * from MPIE, set MPIE, clear MPP, optionally clear MPRV, update privilege, and
 * finally redirect to MEPC.  The target PC is not alignment-checked here because
 * RISC-V defines the return target through the CSR value and the next fetch path
 * owns instruction-address faults.
 */
static inline void riscv_mret(Decode *s)
{
    if (cpu.prvi != RISCV_PRIV_M)
    {
        riscv_raise_trap(s, RISCV_CAUSE_ILLEGAL_INST, 0);
        return;
    }

    word_t mstatus = cpu.csr.mstatus;
    const word_t mpp = (mstatus >> 11) & 0x3u;
    const word_t mpie = (mstatus >> 7) & 0x1u;

    if (mpp == 0x2u)
    {
        riscv_raise_trap(s, RISCV_CAUSE_ILLEGAL_INST, 0);
        return;
    }

    mstatus &= ~((word_t)0x3u << 11);
    mstatus = (mstatus & ~((word_t)1u << 3)) | (mpie << 3);
    mstatus |= ((word_t)1u << 7);

    /* MPRV is cleared when returning to a mode below M-mode. */
    if (mpp != RISCV_PRIV_M)
    {
        mstatus &= ~((word_t)1u << 17);
    }

    cpu.csr.mstatus = riscv_mstatus_normalise(mstatus);
    cpu.prvi = mpp;
    s->dnpc = cpu.csr.mepc;
}

/*
 * Execute SFENCE.VMA for the modelled RISC-V state.  The interpreter does not
 * keep a software address-translation cache, but the RISC-V JIT has a data TLB;
 * therefore the instruction flushes that cache when JIT support is compiled in.
 */
static inline void riscv_sfence_vma(Decode *s)
{
    const word_t mstatus_tvm = (word_t)1u << 20;

    if (!riscv_reg_ok(s, rs1_idx(s->isa.inst)) ||
        !riscv_reg_ok(s, rs2_idx(s->isa.inst)))
    {
        return;
    }

    if (cpu.prvi == RISCV_PRIV_U ||
        (cpu.prvi == RISCV_PRIV_S && (cpu.csr.mstatus & mstatus_tvm) != 0))
    {
        riscv_raise_trap(s, RISCV_CAUSE_ILLEGAL_INST, 0);
        return;
    }

#if defined(CONFIG_RV32_JIT) || defined(CONFIG_RV64_JIT)
    isa_jit_flush_data_tlb();
#endif
}

/*
 * Match and execute one fetched RISC-V instruction.  Pattern order matters:
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
    /*
     * U-type immediates:
     * - LUI writes the upper immediate directly to rd.
     * - AUIPC adds that same immediate to the current instruction address.
     * RV64 sign-extends the 32-bit U result before it is written to the XLEN
     * register, which is why both instructions pass through riscv_u_imm_value().
     */
    INSTPAT("??????? ????? ????? ??? ????? 01101 11", lui, U, R(rd) = riscv_u_imm_value(imm));
    INSTPAT("??????? ????? ????? ??? ????? 00101 11", auipc, U, R(rd) = s->pc + riscv_u_imm_value(imm));

    /*
     * Loads:
     * - LB/LH/LW sign-extend the loaded byte/halfword/word.
     * - LBU/LHU/LWU zero-extend their loaded value.
     * - LH/LW/LD/LHU/LWU perform the natural-alignment check before rd is
     *   touched, so a trap cannot partially commit a destination register.
     */
    INSTPAT("??????? ????? ????? 000 ????? 00000 11", lb, I, R(rd) = SEXT(Mr(src1 + imm, 1), 8));
    INSTPAT("??????? ????? ????? 001 ????? 00000 11", lh, I,
            if (riscv_check_load_alignment(s, src1 + imm, 2))
                R(rd) = SEXT(Mr(src1 + imm, 2), 16));
    INSTPAT("??????? ????? ????? 010 ????? 00000 11", lw, I,
            if (riscv_check_load_alignment(s, src1 + imm, 4))
                R(rd) = SEXT(Mr(src1 + imm, 4), 32));
#ifdef CONFIG_RV64
    INSTPAT("??????? ????? ????? 011 ????? 00000 11", ld, I,
            if (riscv_check_load_alignment(s, src1 + imm, 8))
                R(rd) = Mr(src1 + imm, 8));
#endif
    INSTPAT("??????? ????? ????? 100 ????? 00000 11", lbu, I, R(rd) = Mr(src1 + imm, 1));
    INSTPAT("??????? ????? ????? 101 ????? 00000 11", lhu, I,
            if (riscv_check_load_alignment(s, src1 + imm, 2))
                R(rd) = Mr(src1 + imm, 2));
#ifdef CONFIG_RV64
    INSTPAT("??????? ????? ????? 110 ????? 00000 11", lwu, I,
            if (riscv_check_load_alignment(s, src1 + imm, 4))
                R(rd) = Mr(src1 + imm, 4));
#endif

    /*
     * Stores:
     * - SB/SH/SW/SD write the low 8/16/32/64 bits of rs2 to memory.
     * - Wider stores check alignment before vaddr_write() is called, so a
     *   misaligned store raises the architectural trap without changing memory.
     */
    INSTPAT("??????? ????? ????? 000 ????? 01000 11", sb, S, Mw(src1 + imm, 1, src2));
    INSTPAT("??????? ????? ????? 001 ????? 01000 11", sh, S,
            if (riscv_check_store_alignment(s, src1 + imm, 2))
                Mw(src1 + imm, 2, src2));
    INSTPAT("??????? ????? ????? 010 ????? 01000 11", sw, S,
            if (riscv_check_store_alignment(s, src1 + imm, 4))
                Mw(src1 + imm, 4, src2));
#ifdef CONFIG_RV64
    INSTPAT("??????? ????? ????? 011 ????? 01000 11", sd, S,
            if (riscv_check_store_alignment(s, src1 + imm, 8))
                Mw(src1 + imm, 8, src2));
#endif

#ifdef CONFIG_RV64_FPU
    /*
     * RV64 floating-point decoding is isolated in fpu.c.  These major-opcode
     * routes carry no integer decode type because FPR operand roles differ
     * from the base I/R/S formats.  The executor performs every finer encoding
     * check and raises illegal instruction for unsupported/reserved forms.
     */
    INSTPAT("??????? ????? ????? ??? ????? 00001 11", fp_load, N,
            riscv64_fpu_exec(s));
    INSTPAT("??????? ????? ????? ??? ????? 01001 11", fp_store, N,
            riscv64_fpu_exec(s));
    INSTPAT("??????? ????? ????? ??? ????? 10000 11", fp_fmadd, N,
            riscv64_fpu_exec(s));
    INSTPAT("??????? ????? ????? ??? ????? 10001 11", fp_fmsub, N,
            riscv64_fpu_exec(s));
    INSTPAT("??????? ????? ????? ??? ????? 10010 11", fp_fnmsub, N,
            riscv64_fpu_exec(s));
    INSTPAT("??????? ????? ????? ??? ????? 10011 11", fp_fnmadd, N,
            riscv64_fpu_exec(s));
    INSTPAT("??????? ????? ????? ??? ????? 10100 11", fp_op, N,
            riscv64_fpu_exec(s));
#endif

    /*
     * OP-IMM arithmetic and logic:
     * ADDI uses the sign-extended I-immediate as a normal XLEN value.  SLTI and
     * SLTIU differ only in signedness.  XORI/ORI/ANDI are bitwise operations
     * with the sign-extended immediate, exactly as RISC-V specifies.
     */
    INSTPAT("??????? ????? ????? 000 ????? 00100 11", addi, I, R(rd) = src1 + imm);
    /*
     * Immediate shifts use the shamt bits from the I-immediate.  The pattern
     * checks the legal funct7/funct6 prefix while RISCV_SHIFT_MASK selects the
     * five RV32 bits or six RV64 bits used by the actual C shift.
     */
#ifdef CONFIG_RV64
    INSTPAT("000000? ????? ????? 001 ????? 00100 11", slli, I, R(rd) = src1 << (imm & RISCV_SHIFT_MASK));
#else
    INSTPAT("0000000 ????? ????? 001 ????? 00100 11", slli, I, R(rd) = src1 << (imm & RISCV_SHIFT_MASK));
#endif
    INSTPAT("??????? ????? ????? 010 ????? 00100 11", slti, I, R(rd) = (sword_t)src1 < (sword_t)imm);
    INSTPAT("??????? ????? ????? 011 ????? 00100 11", sltiu, I, R(rd) = src1 < imm);
    INSTPAT("??????? ????? ????? 100 ????? 00100 11", xori, I, R(rd) = src1 ^ imm);
#ifdef CONFIG_RV64
    INSTPAT("000000? ????? ????? 101 ????? 00100 11", srli, I, R(rd) = src1 >> (imm & RISCV_SHIFT_MASK));
    INSTPAT("010000? ????? ????? 101 ????? 00100 11", srai, I, R(rd) = (word_t)((sword_t)src1 >> (imm & RISCV_SHIFT_MASK)));
#else
    INSTPAT("0000000 ????? ????? 101 ????? 00100 11", srli, I, R(rd) = src1 >> (imm & RISCV_SHIFT_MASK));
    INSTPAT("0100000 ????? ????? 101 ????? 00100 11", srai, I, R(rd) = (word_t)((sword_t)src1 >> (imm & RISCV_SHIFT_MASK)));
#endif
    INSTPAT("??????? ????? ????? 110 ????? 00100 11", ori, I, R(rd) = src1 | imm);
    INSTPAT("??????? ????? ????? 111 ????? 00100 11", andi, I, R(rd) = src1 & imm);

#ifdef CONFIG_RV64
    /*
     * RV64 OP-IMM-32 instructions operate on the low 32 bits and sign-extend
     * the 32-bit result.  Shift counts are always five bits for W-form shifts.
     */
    INSTPAT("??????? ????? ????? 000 ????? 00110 11", addiw, I, R(rd) = riscv_sext32((uint32_t)(src1 + imm)));
    INSTPAT("0000000 ????? ????? 001 ????? 00110 11", slliw, I, R(rd) = riscv_sext32((uint32_t)src1 << (imm & 0x1f)));
    INSTPAT("0000000 ????? ????? 101 ????? 00110 11", srliw, I, R(rd) = riscv_sext32((uint32_t)src1 >> (imm & 0x1f)));
    INSTPAT("0100000 ????? ????? 101 ????? 00110 11", sraiw, I, R(rd) = riscv_sext32((uint32_t)((int32_t)src1 >> (imm & 0x1f))));
#endif

    /*
     * Register-register base ALU instructions.  ADD/SUB and bitwise operations
     * use XLEN arithmetic.  SLT/SLTU write a boolean 0/1 result.  SLL/SRL/SRA
     * take the shift amount from rs2, again masked to the legal shamt width.
     */
    INSTPAT("0000000 ????? ????? 000 ????? 01100 11", add, R, R(rd) = src1 + src2);
    INSTPAT("0100000 ????? ????? 000 ????? 01100 11", sub, R, R(rd) = src1 - src2);
    INSTPAT("0000000 ????? ????? 001 ????? 01100 11", sll, R, R(rd) = src1 << (src2 & RISCV_SHIFT_MASK));
    INSTPAT("0000000 ????? ????? 010 ????? 01100 11", slt, R, R(rd) = (sword_t)src1 < (sword_t)src2);
    INSTPAT("0000000 ????? ????? 011 ????? 01100 11", sltu, R, R(rd) = src1 < src2);
    INSTPAT("0000000 ????? ????? 100 ????? 01100 11", xor, R, R(rd) = src1 ^ src2);
    INSTPAT("0000000 ????? ????? 101 ????? 01100 11", srl, R, R(rd) = src1 >> (src2 & RISCV_SHIFT_MASK));
    INSTPAT("0100000 ????? ????? 101 ????? 01100 11", sra, R, R(rd) = (word_t)((sword_t)src1 >> (src2 & RISCV_SHIFT_MASK)));
    INSTPAT("0000000 ????? ????? 110 ????? 01100 11", or, R, R(rd) = src1 | src2);
    INSTPAT("0000000 ????? ????? 111 ????? 01100 11", and, R, R(rd) = src1 & src2);

    /*
     * M extension multiply/divide instructions:
     * - MUL returns the low XLEN bits.
     * - MULH/MULHSU/MULHU return the high XLEN bits with signedness selected by
     *   the mnemonic.
     * - DIV/REM helpers implement RISC-V's defined divide-by-zero and signed
     *   overflow results instead of relying on host C undefined behaviour.
     */
    INSTPAT("0000001 ????? ????? 000 ????? 01100 11", mul, R, R(rd) = src1 * src2);
    INSTPAT("0000001 ????? ????? 001 ????? 01100 11", mulh, R, R(rd) = riscv_mulh(src1, src2));
    INSTPAT("0000001 ????? ????? 010 ????? 01100 11", mulhsu, R, R(rd) = riscv_mulhsu(src1, src2));
    INSTPAT("0000001 ????? ????? 011 ????? 01100 11", mulhu, R, R(rd) = riscv_mulhu(src1, src2));
    INSTPAT("0000001 ????? ????? 100 ????? 01100 11", div, R, R(rd) = riscv_div(src1, src2));
    INSTPAT("0000001 ????? ????? 101 ????? 01100 11", divu, R, R(rd) = riscv_divu(src1, src2));
    INSTPAT("0000001 ????? ????? 110 ????? 01100 11", rem, R, R(rd) = riscv_rem(src1, src2));
    INSTPAT("0000001 ????? ????? 111 ????? 01100 11", remu, R, R(rd) = riscv_remu(src1, src2));

#ifdef CONFIG_RV64
    /*
     * RV64 OP-32 register instructions.  Every result is formed in 32-bit
     * arithmetic, then sign-extended to XLEN before writeback.
     */
    INSTPAT("0000000 ????? ????? 000 ????? 01110 11", addw, R, R(rd) = riscv_sext32((uint32_t)(src1 + src2)));
    INSTPAT("0100000 ????? ????? 000 ????? 01110 11", subw, R, R(rd) = riscv_sext32((uint32_t)(src1 - src2)));
    INSTPAT("0000000 ????? ????? 001 ????? 01110 11", sllw, R, R(rd) = riscv_sext32((uint32_t)src1 << (src2 & 0x1f)));
    INSTPAT("0000000 ????? ????? 101 ????? 01110 11", srlw, R, R(rd) = riscv_sext32((uint32_t)src1 >> (src2 & 0x1f)));
    INSTPAT("0100000 ????? ????? 101 ????? 01110 11", sraw, R, R(rd) = riscv_sext32((uint32_t)((int32_t)src1 >> (src2 & 0x1f))));
    INSTPAT("0000001 ????? ????? 000 ????? 01110 11", mulw, R, R(rd) = riscv_sext32((uint32_t)((uint32_t)src1 * (uint32_t)src2)));
    INSTPAT("0000001 ????? ????? 100 ????? 01110 11", divw, R, R(rd) = riscv_divw(src1, src2));
    INSTPAT("0000001 ????? ????? 101 ????? 01110 11", divuw, R, R(rd) = riscv_divuw(src1, src2));
    INSTPAT("0000001 ????? ????? 110 ????? 01110 11", remw, R, R(rd) = riscv_remw(src1, src2));
    INSTPAT("0000001 ????? ????? 111 ????? 01110 11", remuw, R, R(rd) = riscv_remuw(src1, src2));
#endif

    /*
     * Conditional branches.  The helper first evaluates the relation; only a
     * taken branch computes and alignment-checks the target.  Not-taken branches
     * leave dnpc at snpc, the sequential PC chosen at function entry.
     */
    INSTPAT("??????? ????? ????? 000 ????? 11000 11", beq, B, riscv_branch(s, RISCV_RELOP_EQ, src1, src2, imm));
    INSTPAT("??????? ????? ????? 001 ????? 11000 11", bne, B, riscv_branch(s, RISCV_RELOP_NE, src1, src2, imm));
    INSTPAT("??????? ????? ????? 100 ????? 11000 11", blt, B, riscv_branch(s, RISCV_RELOP_LT, src1, src2, imm));
    INSTPAT("??????? ????? ????? 101 ????? 11000 11", bge, B, riscv_branch(s, RISCV_RELOP_GE, src1, src2, imm));
    INSTPAT("??????? ????? ????? 110 ????? 11000 11", bltu, B, riscv_branch(s, RISCV_RELOP_LTU, src1, src2, imm));
    INSTPAT("??????? ????? ????? 111 ????? 11000 11", bgeu, B, riscv_branch(s, RISCV_RELOP_GEU, src1, src2, imm));

    /*
     * JAL stores the return address (pc + 4) in rd and redirects to pc + imm.
     * Link-register writes are reported to ftrace before the architectural
     * writeback, and the writeback is skipped if target alignment traps.
     */
    INSTPAT("??????? ????? ????? ??? ????? 11011 11", jal, J,
            {
                word_t target = s->pc + imm;
                if (riscv_check_jump_alignment(s, target))
                {
                    if (rd == 1 || rd == 5)
                    {
                        ftrace_call(s->pc, target);
                    }
                    R(rd) = s->pc + 4;
                    s->dnpc = target;
                }
            });
    /*
     * JALR adds rs1 and the I-immediate, clears bit 0 as required by RISC-V,
     * then performs the same alignment and link-register handling as JAL.
     * A rd=x0, rs1=ra/t0, imm=0 form is treated as a return for ftrace.
     */
    INSTPAT("??????? ????? ????? 000 ????? 11001 11", jalr, I,
            {
                word_t target = (src1 + imm) & ~(word_t)1;
                if (riscv_check_jump_alignment(s, target))
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

    /*
     * FENCE and FENCE.I are ordering hints for this single-threaded interpreter.
     * There is no host-side reordering model to update here, so they retire as
     * no-ops while still consuming the instruction and advancing snpc.
     */
    INSTPAT("??????? ????? ????? 000 ????? 00011 11", fence, N, );
    INSTPAT("??????? ????? ????? 001 ????? 00011 11", fence_i, N, );

    /*
     * SYSTEM instructions with fixed encodings:
     * - ECALL raises the privilege-specific environment-call cause.
     * - EBREAK raises breakpoint.
     * - MRET restores privilege and interrupt-enable state.
     * - WFI is modelled as a DiffTest skip because it has no timing effect here.
     * - SFENCE.VMA validates privilege and flushes the JIT data TLB when present.
     */
    INSTPAT("0000000 00000 00000 000 00000 11100 11", ecall, N,
            riscv_raise_trap(s, riscv_ecall_cause_from_priv(cpu.prvi), 0));
    INSTPAT("0000000 00001 00000 000 00000 11100 11", ebreak, N,
            riscv_raise_trap(s, RISCV_CAUSE_BREAKPOINT, 0));
    INSTPAT("0011000 00010 00000 000 00000 11100 11", mret, N, riscv_mret(s));
    INSTPAT("0001000 00101 00000 000 00000 11100 11", wfi, N, difftest_skip_ref());
    INSTPAT("0001001 ????? ????? 000 00000 11100 11", sfence_vma, N, riscv_sfence_vma(s));

    /*
     * CSRRW atomically swaps rs1 into the CSR and optionally writes the old CSR
     * value to rd.  When rd is x0, the old value is not needed, but permission
     * checks still happen before the write.
     */
    INSTPAT("??????? ????? ????? 001 ????? 11100 11", csrrw, CSR,
            {
                if (riscv_csr_access_ok(s, imm, true))
                {
                    if (rd != 0)
                    {
                        word_t old = getCSRValue(imm);
                        setCSRValue(imm, src1);
                        R(rd) = old;
                    }
                    else
                    {
                        setCSRValue(imm, src1);
                    }
                }
            });
    /*
     * CSRRS reads the CSR into rd and sets bits selected by rs1.  rs1=x0 means
     * read-only behaviour, so read-only CSRs are legal in that case.
     */
    INSTPAT("??????? ????? ????? 010 ????? 11100 11", csrrs, CSR,
            {
                bool will_write = rs1 != 0;
                if (riscv_csr_access_ok(s, imm, will_write))
                {
                    word_t old = getCSRValue(imm);
                    R(rd) = old;
                    if (will_write)
                    {
                        setCSRValue(imm, old | src1);
                    }
                }
            });
    /*
     * CSRRC reads the CSR into rd and clears bits selected by rs1.  Like CSRRS,
     * rs1=x0 suppresses the write and therefore only needs read permission.
     */
    INSTPAT("??????? ????? ????? 011 ????? 11100 11", csrrc, CSR,
            {
                bool will_write = rs1 != 0;
                if (riscv_csr_access_ok(s, imm, will_write))
                {
                    word_t old = getCSRValue(imm);
                    R(rd) = old;
                    if (will_write)
                    {
                        setCSRValue(imm, old & ~src1);
                    }
                }
            });
    /*
     * CSRRWI is the immediate form of CSRRW.  The zimm value has already been
     * placed in src1 by decode_operand(), so no register read is involved.
     */
    INSTPAT("??????? ????? ????? 101 ????? 11100 11", csrrwi, CSI,
            {
                if (riscv_csr_access_ok(s, imm, true))
                {
                    if (rd != 0)
                    {
                        word_t old = getCSRValue(imm);
                        setCSRValue(imm, src1);
                        R(rd) = old;
                    }
                    else
                    {
                        setCSRValue(imm, src1);
                    }
                }
            });
    /*
     * CSRRSI sets CSR bits selected by zimm.  zimm=0 is a pure read and must not
     * trip the read-only CSR write check.
     */
    INSTPAT("??????? ????? ????? 110 ????? 11100 11", csrrsi, CSI,
            {
                bool will_write = rs1 != 0;
                if (riscv_csr_access_ok(s, imm, will_write))
                {
                    word_t old = getCSRValue(imm);
                    R(rd) = old;
                    if (will_write)
                    {
                        setCSRValue(imm, old | src1);
                    }
                }
            });
    /*
     * CSRRCI clears CSR bits selected by zimm.  The old CSR value is written to
     * rd before any modification, matching the atomic read-modify-write rule.
     */
    INSTPAT("??????? ????? ????? 111 ????? 11100 11", csrrci, CSI,
            {
                bool will_write = rs1 != 0;
                if (riscv_csr_access_ok(s, imm, will_write))
                {
                    word_t old = getCSRValue(imm);
                    R(rd) = old;
                    if (will_write)
                    {
                        setCSRValue(imm, old & ~src1);
                    }
                }
            });

    /*
     * NEMU's private trap instruction is how AM programs report good/bad traps;
     * a0 (x10) carries the return code.  The final catch-all turns any
     * otherwise unmatched encoding into a RISC-V illegal-instruction trap.
     */
    INSTPAT("??????? ????? ????? ??? ????? 11010 11", nemu_trap, N, NEMUTRAP(s->pc, R(10))); // R(10) is $a0
    INSTPAT("??????? ????? ????? ??? ????? ????? ??", inv, N,
            riscv_raise_trap(s, RISCV_CAUSE_ILLEGAL_INST, 0));
    INSTPAT_END();

    R(0) = 0; // reset $zero to 0

    return 0;
}

/* Fetch one 32-bit RISC-V instruction and execute it through the direct matcher. */
int isa_exec_once(Decode *s)
{
    s->isa.inst = inst_fetch(&s->snpc, 4);
    return decode_exec(s);
}
