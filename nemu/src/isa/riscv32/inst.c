#include "local-include/reg.h"
#include <cpu/cpu.h>
#include <cpu/decode.h>
#include <cpu/difftest.h>
#include <cpu/ifetch.h>
#ifdef CONFIG_RV32_JIT
#include <isa-jit.h>
#endif
#include <memory/vaddr.h>
#include <utils.h>

#define R(i) gpr(i)
#define Mr vaddr_read
#define Mw vaddr_write

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
    RV32_RELOP_EQ,
    RV32_RELOP_NE,
    RV32_RELOP_LT,
    RV32_RELOP_GE,
    RV32_RELOP_LTU,
    RV32_RELOP_GEU,
};

static inline uint32_t csr_addr(uint32_t inst)
{
    return BITS(inst, 31, 20);
}

static inline uint32_t rd_idx(uint32_t inst)
{
    return BITS(inst, 11, 7);
}

static inline uint32_t rs1_idx(uint32_t inst)
{
    return BITS(inst, 19, 15);
}

static inline uint32_t rs2_idx(uint32_t inst)
{
    return BITS(inst, 24, 20);
}

static inline word_t imm_i(uint32_t inst)
{
    return (word_t)SEXT(BITS(inst, 31, 20), 12);
}

static inline word_t imm_u(uint32_t inst)
{
    return (word_t)(inst & 0xfffff000u);
}

static inline word_t imm_s(uint32_t inst)
{
    return (word_t)SEXT((BITS(inst, 31, 25) << 5) | BITS(inst, 11, 7), 12);
}

static inline word_t imm_b(uint32_t inst)
{
    uint32_t raw = (BITS(inst, 31, 31) << 12) |
                   (BITS(inst, 7, 7) << 11) |
                   (BITS(inst, 30, 25) << 5) |
                   (BITS(inst, 11, 8) << 1);
    return (word_t)SEXT(raw, 13);
}

static inline word_t imm_j(uint32_t inst)
{
    uint32_t raw = (BITS(inst, 31, 31) << 20) |
                   (BITS(inst, 19, 12) << 12) |
                   (BITS(inst, 20, 20) << 11) |
                   (BITS(inst, 30, 21) << 1);
    return (word_t)SEXT(raw, 21);
}

static inline void riscv32_raise_trap(Decode *s, word_t cause, word_t tval)
{
    /*
     * The instruction helpers own architectural trap delivery. They set dnpc to
     * mtvec through the common trap helper and then return without performing
     * the trapping instruction's normal architectural writeback.
     */
    s->dnpc = isa_raise_intr_tval(cause, s->pc, tval);
    /*
     * The reference has already been moved to the trap entry. Copy DUT state on
     * the following DiffTest hook instead of letting Spike execute one handler
     * instruction ahead of NEMU.
     */
    difftest_skip_ref();
}

static inline bool riscv32_reg_ok(Decode *s, uint32_t idx)
{
    if (idx >= MUXDEF(CONFIG_RVE, 16, 32))
    {
        riscv32_raise_trap(s, RISCV32_CAUSE_ILLEGAL_INST, 0);
        return false;
    }

    return true;
}

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
        if (!riscv32_reg_ok(s, *rd) ||
            !riscv32_reg_ok(s, *rs1) ||
            !riscv32_reg_ok(s, *rs2))
            return false;
        *src1 = R(*rs1);
        *src2 = R(*rs2);
        break;
    case TYPE_I:
        if (!riscv32_reg_ok(s, *rd) ||
            !riscv32_reg_ok(s, *rs1))
            return false;
        *src1 = R(*rs1);
        *imm = imm_i(inst);
        break;
    case TYPE_U:
        if (!riscv32_reg_ok(s, *rd))
            return false;
        *imm = imm_u(inst);
        break;
    case TYPE_S:
        if (!riscv32_reg_ok(s, *rs1) ||
            !riscv32_reg_ok(s, *rs2))
            return false;
        *src1 = R(*rs1);
        *src2 = R(*rs2);
        *imm = imm_s(inst);
        break;
    case TYPE_B:
        if (!riscv32_reg_ok(s, *rs1) ||
            !riscv32_reg_ok(s, *rs2))
            return false;
        *src1 = R(*rs1);
        *src2 = R(*rs2);
        *imm = imm_b(inst);
        break;
    case TYPE_J:
        if (!riscv32_reg_ok(s, *rd))
            return false;
        *imm = imm_j(inst);
        break;
    case TYPE_CSR:
        if (!riscv32_reg_ok(s, *rd) ||
            !riscv32_reg_ok(s, *rs1))
            return false;
        *src1 = R(*rs1);
        *imm = csr_addr(inst);
        break;
    case TYPE_CSI:
        if (!riscv32_reg_ok(s, *rd))
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

static inline bool riscv32_is_naturally_aligned(word_t addr, int len)
{
    return (addr & (word_t)(len - 1)) == 0;
}

static inline bool riscv32_check_load_alignment(Decode *s, word_t addr, int len)
{
    if (len > 1 && !riscv32_is_naturally_aligned(addr, len))
    {
        riscv32_raise_trap(s, RISCV32_CAUSE_LOAD_ADDR_MISALIGNED, addr);
        return false;
    }

    return true;
}

static inline bool riscv32_check_store_alignment(Decode *s, word_t addr, int len)
{
    if (len > 1 && !riscv32_is_naturally_aligned(addr, len))
    {
        riscv32_raise_trap(s, RISCV32_CAUSE_STORE_ADDR_MISALIGNED, addr);
        return false;
    }

    return true;
}

static inline bool riscv32_check_jump_alignment(Decode *s, word_t target)
{
    if ((target & 0x3u) != 0)
    {
        riscv32_raise_trap(s, RISCV32_CAUSE_INST_ADDR_MISALIGNED, target);
        return false;
    }

    return true;
}

static inline rtlreg_t *riscv32_get_csr_or_trap(Decode *s, word_t addr, bool will_write)
{
    const word_t required_priv = (addr >> 8) & 0x3u;

    if (!isCSRImplemented(addr) ||
        cpu.prvi < required_priv ||
        (will_write && !isCSRWriteable(addr)))
    {
        riscv32_raise_trap(s, RISCV32_CAUSE_ILLEGAL_INST, 0);
        return NULL;
    }

    return getCSRAddress(addr);
}

static inline bool riscv32_branch_taken(int relop, word_t lhs, word_t rhs)
{
    switch (relop)
    {
    case RV32_RELOP_EQ:
        return lhs == rhs;
    case RV32_RELOP_NE:
        return lhs != rhs;
    case RV32_RELOP_LT:
        return (sword_t)lhs < (sword_t)rhs;
    case RV32_RELOP_GE:
        return (sword_t)lhs >= (sword_t)rhs;
    case RV32_RELOP_LTU:
        return lhs < rhs;
    case RV32_RELOP_GEU:
        return lhs >= rhs;
    default:
        return false;
    }
}

static inline void riscv32_branch(Decode *s, int relop, word_t src1, word_t src2, word_t imm)
{
    if (!riscv32_branch_taken(relop, src1, src2))
    {
        return;
    }

    word_t target = s->pc + imm;
    if (riscv32_check_jump_alignment(s, target))
    {
        s->dnpc = target;
    }
}

static inline word_t riscv32_div(word_t src1, word_t src2)
{
    sword_t dividend = (sword_t)src1;
    sword_t divisor = (sword_t)src2;
    sword_t int_min = (sword_t)((word_t)1 << (sizeof(word_t) * 8 - 1));

    if (divisor == 0)
        return (word_t)-1;
    if (dividend == int_min && divisor == -1)
        return src1;
    return (word_t)(dividend / divisor);
}

static inline word_t riscv32_divu(word_t src1, word_t src2)
{
    if (src2 == 0)
        return ~(word_t)0;
    return src1 / src2;
}

static inline word_t riscv32_rem(word_t src1, word_t src2)
{
    sword_t dividend = (sword_t)src1;
    sword_t divisor = (sword_t)src2;
    sword_t int_min = (sword_t)((word_t)1 << (sizeof(word_t) * 8 - 1));

    if (divisor == 0)
        return src1;
    if (dividend == int_min && divisor == -1)
        return 0;
    return (word_t)(dividend % divisor);
}

static inline word_t riscv32_remu(word_t src1, word_t src2)
{
    if (src2 == 0)
        return src1;
    return src1 % src2;
}

static inline word_t riscv32_mulh(word_t src1, word_t src2)
{
    int64_t lhs = (int64_t)(sword_t)src1;
    int64_t rhs = (int64_t)(sword_t)src2;
    return (word_t)((uint64_t)(lhs * rhs) >> 32);
}

static inline word_t riscv32_mulhsu(word_t src1, word_t src2)
{
    int64_t lhs = (int32_t)src1;
    uint64_t rhs = (uint32_t)src2;
    int64_t product = lhs < 0
                          ? -(int64_t)((uint64_t)(-lhs) * rhs)
                          : (int64_t)((uint64_t)lhs * rhs);
    return (word_t)((uint64_t)product >> 32);
}

static inline word_t riscv32_mulhu(word_t src1, word_t src2)
{
    uint64_t lhs = (uint32_t)src1;
    uint64_t rhs = (uint32_t)src2;
    return (word_t)((lhs * rhs) >> 32);
}

static inline void riscv32_mret(Decode *s)
{
    if (cpu.prvi != RISCV32_PRIV_M)
    {
        riscv32_raise_trap(s, RISCV32_CAUSE_ILLEGAL_INST, 0);
        return;
    }

    word_t mstatus = cpu.csr.mstatus;
    word_t mpp = (mstatus >> 11) & 0x3u;
    word_t mpie = (mstatus >> 7) & 0x1u;

    mstatus &= ~((word_t)0x3u << 11);
    mstatus = (mstatus & ~((word_t)1u << 3)) | (mpie << 3);
    mstatus |= ((word_t)1u << 7);

    /* MPRV is cleared when returning to a mode below M-mode. */
    if (mpp != RISCV32_PRIV_M)
    {
        mstatus &= ~((word_t)1u << 17);
    }

    cpu.csr.mstatus = mstatus;
    cpu.prvi = mpp;
    s->dnpc = cpu.csr.mepc;
}

static inline void riscv32_sfence_vma(Decode *s)
{
    const word_t mstatus_tvm = (word_t)1u << 20;

    if (!riscv32_reg_ok(s, rs1_idx(s->isa.inst)) ||
        !riscv32_reg_ok(s, rs2_idx(s->isa.inst)))
    {
        return;
    }

    if (cpu.prvi == RISCV32_PRIV_U ||
        (cpu.prvi == RISCV32_PRIV_S && (cpu.csr.mstatus & mstatus_tvm) != 0))
    {
        riscv32_raise_trap(s, RISCV32_CAUSE_ILLEGAL_INST, 0);
        return;
    }

#ifdef CONFIG_RV32_JIT
    isa_jit_flush_data_tlb();
#endif
}

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
    INSTPAT("??????? ????? ????? ??? ????? 01101 11", lui, U, R(rd) = imm);
    INSTPAT("??????? ????? ????? ??? ????? 00101 11", auipc, U, R(rd) = s->pc + imm);

    INSTPAT("??????? ????? ????? 000 ????? 00000 11", lb, I, R(rd) = SEXT(Mr(src1 + imm, 1), 8));
    INSTPAT("??????? ????? ????? 001 ????? 00000 11", lh, I,
            if (riscv32_check_load_alignment(s, src1 + imm, 2))
                R(rd) = SEXT(Mr(src1 + imm, 2), 16));
    INSTPAT("??????? ????? ????? 010 ????? 00000 11", lw, I,
            if (riscv32_check_load_alignment(s, src1 + imm, 4))
                R(rd) = Mr(src1 + imm, 4));
    INSTPAT("??????? ????? ????? 100 ????? 00000 11", lbu, I, R(rd) = Mr(src1 + imm, 1));
    INSTPAT("??????? ????? ????? 101 ????? 00000 11", lhu, I,
            if (riscv32_check_load_alignment(s, src1 + imm, 2))
                R(rd) = Mr(src1 + imm, 2));

    INSTPAT("??????? ????? ????? 000 ????? 01000 11", sb, S, Mw(src1 + imm, 1, src2));
    INSTPAT("??????? ????? ????? 001 ????? 01000 11", sh, S,
            if (riscv32_check_store_alignment(s, src1 + imm, 2))
                Mw(src1 + imm, 2, src2));
    INSTPAT("??????? ????? ????? 010 ????? 01000 11", sw, S,
            if (riscv32_check_store_alignment(s, src1 + imm, 4))
                Mw(src1 + imm, 4, src2));

    INSTPAT("??????? ????? ????? 000 ????? 00100 11", addi, I, R(rd) = src1 + imm);
    INSTPAT("0000000 ????? ????? 001 ????? 00100 11", slli, I, R(rd) = src1 << (imm & 0x1f));
    INSTPAT("??????? ????? ????? 010 ????? 00100 11", slti, I, R(rd) = (sword_t)src1 < (sword_t)imm);
    INSTPAT("??????? ????? ????? 011 ????? 00100 11", sltiu, I, R(rd) = src1 < imm);
    INSTPAT("??????? ????? ????? 100 ????? 00100 11", xori, I, R(rd) = src1 ^ imm);
    INSTPAT("0000000 ????? ????? 101 ????? 00100 11", srli, I, R(rd) = src1 >> (imm & 0x1f));
    INSTPAT("0100000 ????? ????? 101 ????? 00100 11", srai, I, R(rd) = (word_t)((sword_t)src1 >> (imm & 0x1f)));
    INSTPAT("??????? ????? ????? 110 ????? 00100 11", ori, I, R(rd) = src1 | imm);
    INSTPAT("??????? ????? ????? 111 ????? 00100 11", andi, I, R(rd) = src1 & imm);

    INSTPAT("0000000 ????? ????? 000 ????? 01100 11", add, R, R(rd) = src1 + src2);
    INSTPAT("0100000 ????? ????? 000 ????? 01100 11", sub, R, R(rd) = src1 - src2);
    INSTPAT("0000000 ????? ????? 001 ????? 01100 11", sll, R, R(rd) = src1 << (src2 & 0x1f));
    INSTPAT("0000000 ????? ????? 010 ????? 01100 11", slt, R, R(rd) = (sword_t)src1 < (sword_t)src2);
    INSTPAT("0000000 ????? ????? 011 ????? 01100 11", sltu, R, R(rd) = src1 < src2);
    INSTPAT("0000000 ????? ????? 100 ????? 01100 11", xor, R, R(rd) = src1 ^ src2);
    INSTPAT("0000000 ????? ????? 101 ????? 01100 11", srl, R, R(rd) = src1 >> (src2 & 0x1f));
    INSTPAT("0100000 ????? ????? 101 ????? 01100 11", sra, R, R(rd) = (word_t)((sword_t)src1 >> (src2 & 0x1f)));
    INSTPAT("0000000 ????? ????? 110 ????? 01100 11", or, R, R(rd) = src1 | src2);
    INSTPAT("0000000 ????? ????? 111 ????? 01100 11", and, R, R(rd) = src1 & src2);

    INSTPAT("0000001 ????? ????? 000 ????? 01100 11", mul, R, R(rd) = src1 * src2);
    INSTPAT("0000001 ????? ????? 001 ????? 01100 11", mulh, R, R(rd) = riscv32_mulh(src1, src2));
    INSTPAT("0000001 ????? ????? 010 ????? 01100 11", mulhsu, R, R(rd) = riscv32_mulhsu(src1, src2));
    INSTPAT("0000001 ????? ????? 011 ????? 01100 11", mulhu, R, R(rd) = riscv32_mulhu(src1, src2));
    INSTPAT("0000001 ????? ????? 100 ????? 01100 11", div, R, R(rd) = riscv32_div(src1, src2));
    INSTPAT("0000001 ????? ????? 101 ????? 01100 11", divu, R, R(rd) = riscv32_divu(src1, src2));
    INSTPAT("0000001 ????? ????? 110 ????? 01100 11", rem, R, R(rd) = riscv32_rem(src1, src2));
    INSTPAT("0000001 ????? ????? 111 ????? 01100 11", remu, R, R(rd) = riscv32_remu(src1, src2));

    INSTPAT("??????? ????? ????? 000 ????? 11000 11", beq, B, riscv32_branch(s, RV32_RELOP_EQ, src1, src2, imm));
    INSTPAT("??????? ????? ????? 001 ????? 11000 11", bne, B, riscv32_branch(s, RV32_RELOP_NE, src1, src2, imm));
    INSTPAT("??????? ????? ????? 100 ????? 11000 11", blt, B, riscv32_branch(s, RV32_RELOP_LT, src1, src2, imm));
    INSTPAT("??????? ????? ????? 101 ????? 11000 11", bge, B, riscv32_branch(s, RV32_RELOP_GE, src1, src2, imm));
    INSTPAT("??????? ????? ????? 110 ????? 11000 11", bltu, B, riscv32_branch(s, RV32_RELOP_LTU, src1, src2, imm));
    INSTPAT("??????? ????? ????? 111 ????? 11000 11", bgeu, B, riscv32_branch(s, RV32_RELOP_GEU, src1, src2, imm));

    INSTPAT("??????? ????? ????? ??? ????? 11011 11", jal, J,
            {
                word_t target = s->pc + imm;
                if (riscv32_check_jump_alignment(s, target))
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
                if (riscv32_check_jump_alignment(s, target))
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
            riscv32_raise_trap(s, riscv32_ecall_cause_from_priv(cpu.prvi), 0));
    INSTPAT("0000000 00001 00000 000 00000 11100 11", ebreak, N,
            riscv32_raise_trap(s, RISCV32_CAUSE_BREAKPOINT, 0));
    INSTPAT("0011000 00010 00000 000 00000 11100 11", mret, N, riscv32_mret(s));
    INSTPAT("0001000 00101 00000 000 00000 11100 11", wfi, N, difftest_skip_ref());
    INSTPAT("0001001 ????? ????? 000 00000 11100 11", sfence_vma, N, riscv32_sfence_vma(s));

    INSTPAT("??????? ????? ????? 001 ????? 11100 11", csrrw, CSR,
            {
                rtlreg_t *csr = riscv32_get_csr_or_trap(s, imm, true);
                if (csr != NULL)
                {
                    if (rd != 0)
                    {
                        rtlreg_t old = *csr;
                        *csr = src1;
                        R(rd) = old;
                    }
                    else
                    {
                        *csr = src1;
                    }
                }
            });
    INSTPAT("??????? ????? ????? 010 ????? 11100 11", csrrs, CSR,
            {
                bool will_write = rs1 != 0;
                rtlreg_t *csr = riscv32_get_csr_or_trap(s, imm, will_write);
                if (csr != NULL)
                {
                    R(rd) = *csr;
                    if (will_write)
                    {
                        *csr |= src1;
                    }
                }
            });
    INSTPAT("??????? ????? ????? 011 ????? 11100 11", csrrc, CSR,
            {
                bool will_write = rs1 != 0;
                rtlreg_t *csr = riscv32_get_csr_or_trap(s, imm, will_write);
                if (csr != NULL)
                {
                    R(rd) = *csr;
                    if (will_write)
                    {
                        *csr &= ~src1;
                    }
                }
            });
    INSTPAT("??????? ????? ????? 101 ????? 11100 11", csrrwi, CSI,
            {
                rtlreg_t *csr = riscv32_get_csr_or_trap(s, imm, true);
                if (csr != NULL)
                {
                    if (rd != 0)
                    {
                        rtlreg_t old = *csr;
                        *csr = src1;
                        R(rd) = old;
                    }
                    else
                    {
                        *csr = src1;
                    }
                }
            });
    INSTPAT("??????? ????? ????? 110 ????? 11100 11", csrrsi, CSI,
            {
                bool will_write = rs1 != 0;
                rtlreg_t *csr = riscv32_get_csr_or_trap(s, imm, will_write);
                if (csr != NULL)
                {
                    R(rd) = *csr;
                    if (will_write)
                    {
                        *csr |= src1;
                    }
                }
            });
    INSTPAT("??????? ????? ????? 111 ????? 11100 11", csrrci, CSI,
            {
                bool will_write = rs1 != 0;
                rtlreg_t *csr = riscv32_get_csr_or_trap(s, imm, will_write);
                if (csr != NULL)
                {
                    R(rd) = *csr;
                    if (will_write)
                    {
                        *csr &= ~src1;
                    }
                }
            });

    INSTPAT("??????? ????? ????? ??? ????? 11010 11", nemu_trap, N, NEMUTRAP(s->pc, R(10))); // R(10) is $a0
    INSTPAT("??????? ????? ????? ??? ????? ????? ??", inv, N,
            riscv32_raise_trap(s, RISCV32_CAUSE_ILLEGAL_INST, 0));
    INSTPAT_END();

    R(0) = 0; // reset $zero to 0

    return 0;
}

int isa_exec_once(Decode *s)
{
    s->isa.inst = inst_fetch(&s->snpc, 4);
    return decode_exec(s);
}
