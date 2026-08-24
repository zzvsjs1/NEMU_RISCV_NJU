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

#include "local-include/reg.h"
#include <cpu/cpu.h>
#include <cpu/decode.h>
#include <cpu/difftest.h>
#include <cpu/ifetch.h>
#include <utils.h>

#define R(i) gpr(i)
#define Mr vaddr_read
#define Mw vaddr_write

enum
{
    TYPE_R,
    TYPE_I,
    TYPE_U,
    TYPE_B,
    TYPE_J,
    TYPE_N, // none
};

#define src1R() \
    do \
    { \
        *src1 = R(rs_idx); \
    } while (0)
#define src2R() \
    do \
    { \
        *src2 = R(rt_idx); \
    } while (0)
#define immI() \
    do \
    { \
        *imm = (word_t)(sword_t)(int16_t)BITS(inst, 15, 0); \
    } while (0)
#define immU() \
    do \
    { \
        *imm = BITS(inst, 15, 0); \
    } while (0)

/*
 * Decode operands separately from execution. Every match also receives the raw
 * register fields because COP0 and shift instructions attach architectural
 * meaning to fields that are not always ordinary operands.
 */
static void decode_operand(Decode *s, int *rd, int *rs, int *rt, int *sa, word_t *src1, word_t *src2, word_t *imm, int type)
{
    const uint32_t inst = s->isa.inst;
    const int rs_idx = BITS(inst, 25, 21);
    const int rt_idx = BITS(inst, 20, 16);

    *rs = rs_idx;
    *rt = rt_idx;
    *rd = BITS(inst, 15, 11);
    *sa = BITS(inst, 10, 6);

    switch (type)
    {
    case TYPE_R:
        src1R();
        src2R();
        break;
    case TYPE_I:
        *rd = rt_idx;
        src1R();
        immI();
        break;
    case TYPE_U:
        *rd = rt_idx;
        src1R();
        immU();
        break;
    case TYPE_B:
        src1R();
        src2R();
        immI();
        *imm = (word_t)((sword_t)*imm * 4);
        break;
    case TYPE_J:
        *imm = BITS(inst, 25, 0) << 2;
        break;
    case TYPE_N:
        break;
    default:
        panic("unsupported MIPS32 operand type = %d", type);
    }
}

/*
 * The supported MIPS32 guest binaries use -fno-delayed-branch, so every
 * architectural delay slot contains a NOP. NEMU omits that compiler-inserted
 * NOP and advances directly to the post-slot PC.
 */
static inline void mips32_finish_control_transfer(Decode *s, vaddr_t target)
{
    s->dnpc = target;
}

static inline void mips32_branch(Decode *s, bool taken, word_t displacement)
{
    const vaddr_t target = s->snpc + displacement;

    /* A not-taken branch also skips its compiler-inserted delay-slot NOP. */
    mips32_finish_control_transfer(s, taken ? target : s->snpc + 4);
}

static inline bool mips32_signed_add_overflows(word_t lhs, word_t rhs, word_t *result)
{
    const int64_t wide = (int64_t)(int32_t)lhs + (int64_t)(int32_t)rhs;
    *result = (word_t)wide;
    return wide > INT32_MAX || wide < INT32_MIN;
}

static inline bool mips32_signed_sub_overflows(word_t lhs, word_t rhs, word_t *result)
{
    const int64_t wide = (int64_t)(int32_t)lhs - (int64_t)(int32_t)rhs;
    *result = (word_t)wide;
    return wide > INT32_MAX || wide < INT32_MIN;
}

static inline void mips32_raise_trap_if(Decode *s, bool condition)
{
    if (condition)
    {
        s->dnpc = isa_raise_intr(MIPS32_EXC_TRAP, s->pc);
    }
}

static word_t *mips32_cp0_address(uint32_t rd, uint32_t select)
{
    if (select != 0)
    {
        return NULL;
    }

    switch (rd)
    {
    case MIPS32_CP0_INDEX:
        return &cpu.index;
    case MIPS32_CP0_ENTRYLO0:
        return &cpu.entrylo0;
    case MIPS32_CP0_ENTRYLO1:
        return &cpu.entrylo1;
    case MIPS32_CP0_BADVADDR:
        return &cpu.badvaddr;
    case MIPS32_CP0_ENTRYHI:
        return &cpu.entryhi;
    case MIPS32_CP0_STATUS:
        return &cpu.status;
    case MIPS32_CP0_CAUSE:
        return &cpu.cause;
    case MIPS32_CP0_EPC:
        return &cpu.epc;
    default:
        return NULL;
    }
}

static void mips32_move_from_cp0(Decode *s, uint32_t rt, uint32_t rd, uint32_t select)
{
    word_t *cp0 = mips32_cp0_address(rd, select);

    if (cp0 == NULL)
    {
        panic("unsupported MIPS32 CP0 register/select rd=%u select=%u pc=" FMT_WORD, rd, select, s->pc);
    }

    R(rt) = *cp0;
}

static void mips32_move_to_cp0(Decode *s, uint32_t rt, uint32_t rd, uint32_t select)
{
    word_t *cp0 = mips32_cp0_address(rd, select);
    const word_t value = R(rt);
    const word_t cause_writable_mask = (1u << 23) | (3u << 8);

    if (cp0 == NULL)
    {
        panic("unsupported MIPS32 CP0 register/select rd=%u select=%u pc=" FMT_WORD, rd, select, s->pc);
    }

    switch (rd)
    {
    case MIPS32_CP0_BADVADDR:
        /* BadVAddr is populated by hardware; MTC0 recognises but cannot alter it. */
        return;
    case MIPS32_CP0_STATUS:
        cpu.status = value;
        return;
    case MIPS32_CP0_CAUSE:
        /* Only IV and the two software interrupt-pending bits are writable. */
        cpu.cause = (cpu.cause & ~cause_writable_mask) | (value & cause_writable_mask);
        return;
    case MIPS32_CP0_EPC:
        cpu.epc = value;
        return;
    default:
        /* Index, EntryLo0, EntryLo1, and EntryHi are ordinary writable state. */
        *cp0 = value;
        return;
    }
}

static void mips32_divide_signed(word_t dividend, word_t divisor)
{
    const int32_t lhs = (int32_t)dividend;
    const int32_t rhs = (int32_t)divisor;

    if (rhs == 0)
    {
        /* Match the conventional pre-Release-6 result used by QEMU. */
        cpu.lo = lhs >= 0 ? UINT32_MAX : 1;
        cpu.hi = dividend;
    }
    else if (lhs == INT32_MIN && rhs == -1)
    {
        cpu.lo = (word_t)INT32_MIN;
        cpu.hi = 0;
    }
    else
    {
        cpu.lo = (word_t)(lhs / rhs);
        cpu.hi = (word_t)(lhs % rhs);
    }
}

static void mips32_divide_unsigned(word_t dividend, word_t divisor)
{
    if (divisor == 0)
    {
        cpu.lo = UINT32_MAX;
        cpu.hi = dividend;
    }
    else
    {
        cpu.lo = dividend / divisor;
        cpu.hi = dividend % divisor;
    }
}

/*
 * The MIPS32 target is little-endian.  Implement the four unaligned word
 * operations as an aligned four-byte read or read-modify-write.  This keeps
 * their unusual byte selection local to the ISA instead of extending NEMU's
 * architecture-independent memory interface with three-byte transactions.
 */
static word_t mips32_load_word_left(word_t old_value, vaddr_t address)
{
    const uint32_t offset = address & 3u;
    const uint32_t shift = (3u - offset) * 8u;
    const vaddr_t aligned = address & ~3u;
    const word_t memory_word = Mr(aligned, 4);

    if (shift == 0)
    {
        return memory_word;
    }

    return (memory_word << shift) | (old_value & (((word_t)1u << shift) - 1u));
}

static word_t mips32_load_word_right(word_t old_value, vaddr_t address)
{
    const uint32_t offset = address & 3u;
    const uint32_t shift = offset * 8u;
    const vaddr_t aligned = address & ~3u;
    const word_t memory_word = Mr(aligned, 4);

    if (shift == 0)
    {
        return memory_word;
    }

    return (old_value & (~(word_t)0u << (32u - shift))) | (memory_word >> shift);
}

static void mips32_store_word_left(vaddr_t address, word_t value)
{
    const uint32_t offset = address & 3u;
    const uint32_t shift = (3u - offset) * 8u;
    const vaddr_t aligned = address & ~3u;
    const word_t memory_word = Mr(aligned, 4);

    if (shift == 0)
    {
        Mw(aligned, 4, value);
        return;
    }

    const word_t mask = ((word_t)1u << (32u - shift)) - 1u;
    Mw(aligned, 4, (memory_word & ~mask) | ((value >> shift) & mask));
}

static void mips32_store_word_right(vaddr_t address, word_t value)
{
    const uint32_t offset = address & 3u;
    const uint32_t shift = offset * 8u;
    const vaddr_t aligned = address & ~3u;
    const word_t memory_word = Mr(aligned, 4);

    if (shift == 0)
    {
        Mw(aligned, 4, value);
        return;
    }

    const word_t mask = ~(word_t)0u << shift;
    Mw(aligned, 4, (memory_word & ~mask) | ((value << shift) & mask));
}

/*
 * Decode and execute one instruction with the same flat pattern table used by
 * the original NEMU MIPS32 scaffold.  Specific encodings precede the final
 * invalid catch-all because INSTPAT commits the first matching rule.
 */
static int decode_exec(Decode *s)
{
    s->dnpc = s->snpc;

#define INSTPAT_INST(s) ((s)->isa.inst)
#define INSTPAT_MATCH(s, name, type, ... /* execute body */) \
    { \
        int rd = 0, rs = 0, rt = 0, sa = 0; \
        word_t src1 = 0, src2 = 0, imm = 0; \
        decode_operand(s, &rd, &rs, &rt, &sa, &src1, &src2, &imm, concat(TYPE_, type)); \
        __VA_ARGS__; \
    }

    INSTPAT_START();

    /* SPECIAL: shifts, control flow, HI/LO, arithmetic, logic, and traps. */
    INSTPAT("000000 00000 ????? ????? ????? 000000", sll, R, R(rd) = src2 << sa);
    INSTPAT("000000 00000 ????? ????? ????? 000010", srl, R, R(rd) = src2 >> sa);
    INSTPAT("000000 00000 ????? ????? ????? 000011", sra, R, R(rd) = (word_t)((sword_t)src2 >> sa));
    INSTPAT("000000 ????? ????? ????? 00000 000100", sllv, R, R(rd) = src2 << (src1 & 0x1fu));
    INSTPAT("000000 ????? ????? ????? 00000 000110", srlv, R, R(rd) = src2 >> (src1 & 0x1fu));
    INSTPAT("000000 ????? ????? ????? 00000 000111", srav, R, R(rd) = (word_t)((sword_t)src2 >> (src1 & 0x1fu)));
    INSTPAT("000000 ????? 00000 00000 00000 001000", jr, R, {
        if (rs == 31)
            ftrace_ret(s->pc);

        mips32_finish_control_transfer(s, src1);
    });
    INSTPAT("000000 ????? 00000 ????? 00000 001001", jalr, R, {
        const vaddr_t target = src1;

        if (rd == 0 && rs == 31)
            ftrace_ret(s->pc);
        else if (rd != 0)
            ftrace_call(s->pc, target);

        R(rd) = s->pc + 8;
        mips32_finish_control_transfer(s, target);
    });
    /* Release-2 hazard-barrier forms have identical effects in this pipeline-free model. */
    INSTPAT("000000 ????? 00000 00000 10000 001000", jr_hb, R, {
        if (rs == 31)
            ftrace_ret(s->pc);

        mips32_finish_control_transfer(s, src1);
    });
    INSTPAT("000000 ????? 00000 ????? 10000 001001", jalr_hb, R, {
        const vaddr_t target = src1;

        if (rd == 0 && rs == 31)
            ftrace_ret(s->pc);
        else if (rd != 0)
            ftrace_call(s->pc, target);

        R(rd) = s->pc + 8;
        mips32_finish_control_transfer(s, target);
    });
    INSTPAT("000000 ????? ????? ????? 00000 001010", movz, R, if (src2 == 0) R(rd) = src1);
    INSTPAT("000000 ????? ????? ????? 00000 001011", movn, R, if (src2 != 0) R(rd) = src1);
    INSTPAT("000000 ????? ????? ????? ????? 001100", syscall, N, s->dnpc = isa_raise_intr(MIPS32_EXC_SYS, s->pc));
    INSTPAT("000000 00000 00000 00000 ????? 001111", sync, N, );
    INSTPAT("000000 00000 00000 ????? 00000 010000", mfhi, R, R(rd) = cpu.hi);
    INSTPAT("000000 ????? 00000 00000 00000 010001", mthi, R, cpu.hi = src1);
    INSTPAT("000000 00000 00000 ????? 00000 010010", mflo, R, R(rd) = cpu.lo);
    INSTPAT("000000 ????? 00000 00000 00000 010011", mtlo, R, cpu.lo = src1);
    INSTPAT("000000 ????? ????? 00000 00000 011000", mult, R, {
        const int64_t product = (int64_t)(int32_t)src1 * (int64_t)(int32_t)src2;
        cpu.lo = (word_t)product;
        cpu.hi = (word_t)((uint64_t)product >> 32);
    });
    INSTPAT("000000 ????? ????? 00000 00000 011001", multu, R, {
        const uint64_t product = (uint64_t)src1 * (uint64_t)src2;
        cpu.lo = (word_t)product;
        cpu.hi = (word_t)(product >> 32);
    });
    INSTPAT("000000 ????? ????? 00000 00000 011010", div, R, mips32_divide_signed(src1, src2));
    INSTPAT("000000 ????? ????? 00000 00000 011011", divu, R, mips32_divide_unsigned(src1, src2));
    INSTPAT("000000 ????? ????? ????? 00000 100000", add, R, {
        word_t result = 0;

        if (mips32_signed_add_overflows(src1, src2, &result))
            s->dnpc = isa_raise_intr(MIPS32_EXC_OV, s->pc);
        else
            R(rd) = result;
    });
    INSTPAT("000000 ????? ????? ????? 00000 100001", addu, R, R(rd) = src1 + src2);
    INSTPAT("000000 ????? ????? ????? 00000 100010", sub, R, {
        word_t result = 0;

        if (mips32_signed_sub_overflows(src1, src2, &result))
            s->dnpc = isa_raise_intr(MIPS32_EXC_OV, s->pc);
        else
            R(rd) = result;
    });
    INSTPAT("000000 ????? ????? ????? 00000 100011", subu, R, R(rd) = src1 - src2);
    INSTPAT("000000 ????? ????? ????? 00000 100100", and, R, R(rd) = src1 & src2);
    INSTPAT("000000 ????? ????? ????? 00000 100101", or, R, R(rd) = src1 | src2);
    INSTPAT("000000 ????? ????? ????? 00000 100110", xor, R, R(rd) = src1 ^ src2);
    INSTPAT("000000 ????? ????? ????? 00000 100111", nor, R, R(rd) = ~(src1 | src2));
    INSTPAT("000000 ????? ????? ????? 00000 101010", slt, R, R(rd) = (sword_t)src1 < (sword_t)src2);
    INSTPAT("000000 ????? ????? ????? 00000 101011", sltu, R, R(rd) = src1 < src2);
    INSTPAT("000000 ????? ????? ????? ????? 110000", tge, R, mips32_raise_trap_if(s, (sword_t)src1 >= (sword_t)src2));
    INSTPAT("000000 ????? ????? ????? ????? 110001", tgeu, R, mips32_raise_trap_if(s, src1 >= src2));
    INSTPAT("000000 ????? ????? ????? ????? 110010", tlt, R, mips32_raise_trap_if(s, (sword_t)src1 < (sword_t)src2));
    INSTPAT("000000 ????? ????? ????? ????? 110011", tltu, R, mips32_raise_trap_if(s, src1 < src2));
    INSTPAT("000000 ????? ????? ????? ????? 110100", teq, R, mips32_raise_trap_if(s, src1 == src2));
    INSTPAT("000000 ????? ????? ????? ????? 110110", tne, R, mips32_raise_trap_if(s, src1 != src2));

    /* REGIMM: the rt field selects the branch or immediate-trap operation. */
    INSTPAT("000001 ????? 00000 ????? ????? ??????", bltz, B, mips32_branch(s, (sword_t)src1 < 0, imm));
    INSTPAT("000001 ????? 00001 ????? ????? ??????", bgez, B, mips32_branch(s, (sword_t)src1 >= 0, imm));
    INSTPAT("000001 ????? 01000 ????? ????? ??????", tgei, I, mips32_raise_trap_if(s, (sword_t)src1 >= (sword_t)imm));
    INSTPAT("000001 ????? 01001 ????? ????? ??????", tgeiu, I, mips32_raise_trap_if(s, src1 >= imm));
    INSTPAT("000001 ????? 01010 ????? ????? ??????", tlti, I, mips32_raise_trap_if(s, (sword_t)src1 < (sword_t)imm));
    INSTPAT("000001 ????? 01011 ????? ????? ??????", tltiu, I, mips32_raise_trap_if(s, src1 < imm));
    INSTPAT("000001 ????? 01100 ????? ????? ??????", teqi, I, mips32_raise_trap_if(s, src1 == imm));
    INSTPAT("000001 ????? 01110 ????? ????? ??????", tnei, I, mips32_raise_trap_if(s, src1 != imm));
    INSTPAT("000001 ????? 10000 ????? ????? ??????", bltzal, B, {
        const bool taken = (sword_t)src1 < 0;

        if (taken)
        {
            const vaddr_t target = s->snpc + imm;
            R(31) = s->pc + 8;
            ftrace_call(s->pc, target);
        }

        mips32_branch(s, taken, imm);
    });
    INSTPAT("000001 ????? 10001 ????? ????? ??????", bgezal, B, {
        const bool taken = (sword_t)src1 >= 0;

        if (taken)
        {
            const vaddr_t target = s->snpc + imm;
            R(31) = s->pc + 8;
            ftrace_call(s->pc, target);
        }

        mips32_branch(s, taken, imm);
    });

    /* Primary opcodes: jumps, branches, immediate ALU operations, and LUI. */
    INSTPAT("000010 ????? ????? ????? ????? ??????", j, J, mips32_finish_control_transfer(s, (s->snpc & 0xf0000000u) | imm));
    INSTPAT("000011 ????? ????? ????? ????? ??????", jal, J, {
        const vaddr_t target = (s->snpc & 0xf0000000u) | imm;
        R(31) = s->pc + 8;
        ftrace_call(s->pc, target);
        mips32_finish_control_transfer(s, target);
    });
    INSTPAT("000100 ????? ????? ????? ????? ??????", beq, B, mips32_branch(s, src1 == src2, imm));
    INSTPAT("000101 ????? ????? ????? ????? ??????", bne, B, mips32_branch(s, src1 != src2, imm));
    INSTPAT("000110 ????? 00000 ????? ????? ??????", blez, B, mips32_branch(s, (sword_t)src1 <= 0, imm));
    INSTPAT("000111 ????? 00000 ????? ????? ??????", bgtz, B, mips32_branch(s, (sword_t)src1 > 0, imm));
    INSTPAT("001000 ????? ????? ????? ????? ??????", addi, I, {
        word_t result = 0;

        if (mips32_signed_add_overflows(src1, imm, &result))
            s->dnpc = isa_raise_intr(MIPS32_EXC_OV, s->pc);
        else
            R(rd) = result;
    });
    INSTPAT("001001 ????? ????? ????? ????? ??????", addiu, I, R(rd) = src1 + imm);
    INSTPAT("001010 ????? ????? ????? ????? ??????", slti, I, R(rd) = (sword_t)src1 < (sword_t)imm);
    INSTPAT("001011 ????? ????? ????? ????? ??????", sltiu, I, R(rd) = src1 < imm);
    INSTPAT("001100 ????? ????? ????? ????? ??????", andi, U, R(rd) = src1 & imm);
    INSTPAT("001101 ????? ????? ????? ????? ??????", ori, U, R(rd) = src1 | imm);
    INSTPAT("001110 ????? ????? ????? ????? ??????", xori, U, R(rd) = src1 ^ imm);
    INSTPAT("001111 00000 ????? ????? ????? ??????", lui, U, R(rd) = imm << 16);

    /*
     * The final three bits are CP0 select.  Only select zero is implemented;
     * the helpers report unsupported register/select pairs precisely.
     */
    INSTPAT("010000 00000 ????? ????? 00000 000???", mfc0, N, mips32_move_from_cp0(s, rt, rd, BITS(s->isa.inst, 2, 0)));
    INSTPAT("010000 00100 ????? ????? 00000 000???", mtc0, N, mips32_move_to_cp0(s, rt, rd, BITS(s->isa.inst, 2, 0)));
    INSTPAT("010000 10000 00000 00000 00000 001000", tlbp, N, mips32_tlbp());
    INSTPAT("010000 10000 00000 00000 00000 000010", tlbwi, N, mips32_tlbwi());
    INSTPAT("010000 10000 00000 00000 00000 000110", tlbwr, N, mips32_tlbwr());
    INSTPAT("010000 10000 00000 00000 00000 011000", eret, N, {
        s->dnpc = cpu.epc;
        cpu.status &= ~MIPS32_STATUS_EXL;
        etrace_eret(s->dnpc, cpu.status);
    });

    /* SPECIAL2 integer operations and the architectural SDBBP test trap. */
    INSTPAT("011100 ????? ????? ????? 00000 000010", mul, R, R(rd) = src1 * src2);

    /* Pre-Release-6 CLZ/CLO duplicate rd in rt; keep that encoded field matchable. */
    INSTPAT("011100 ????? ????? ????? 00000 100000", clz, R, R(rd) = src1 == 0 ? 32 : (word_t)__builtin_clz(src1));
    INSTPAT("011100 ????? ????? ????? 00000 100001", clo, R, R(rd) = src1 == UINT32_MAX ? 32 : (word_t)__builtin_clz(~src1));
    INSTPAT("011100 ????? ????? ????? ????? 111111", sdbbp, N, {
        difftest_skip_ref();
        NEMUTRAP(s->pc, R(2));
    });

    /* Loads and stores use the sign-extended I-format effective address. */
    INSTPAT("100000 ????? ????? ????? ????? ??????", lb, I, R(rd) = (word_t)(sword_t)(int8_t)Mr(src1 + imm, 1));
    INSTPAT("100001 ????? ????? ????? ????? ??????", lh, I, R(rd) = (word_t)(sword_t)(int16_t)Mr(src1 + imm, 2));
    INSTPAT("100010 ????? ????? ????? ????? ??????", lwl, I, R(rd) = mips32_load_word_left(R(rd), src1 + imm));
    INSTPAT("100011 ????? ????? ????? ????? ??????", lw, I, R(rd) = Mr(src1 + imm, 4));
    INSTPAT("100100 ????? ????? ????? ????? ??????", lbu, I, R(rd) = Mr(src1 + imm, 1));
    INSTPAT("100101 ????? ????? ????? ????? ??????", lhu, I, R(rd) = Mr(src1 + imm, 2));
    INSTPAT("100110 ????? ????? ????? ????? ??????", lwr, I, R(rd) = mips32_load_word_right(R(rd), src1 + imm));
    INSTPAT("101000 ????? ????? ????? ????? ??????", sb, I, Mw(src1 + imm, 1, R(rd)));
    INSTPAT("101001 ????? ????? ????? ????? ??????", sh, I, Mw(src1 + imm, 2, R(rd)));
    INSTPAT("101010 ????? ????? ????? ????? ??????", swl, I, mips32_store_word_left(src1 + imm, R(rd)));
    INSTPAT("101011 ????? ????? ????? ????? ??????", sw, I, Mw(src1 + imm, 4, R(rd)));
    INSTPAT("101110 ????? ????? ????? ????? ??????", swr, I, mips32_store_word_right(src1 + imm, R(rd)));

    /* AM's private exact encoding terminates a NEMU test image. */
    INSTPAT("111100 00000 00000 00000 00000 000000", nemu_trap, N, {
        difftest_skip_ref();
        NEMUTRAP(s->pc, R(2));
    });
    INSTPAT("?????? ????? ????? ????? ????? ??????", inv, N, INV(s->pc));
    INSTPAT_END();

    /* All writes to the architectural zero register are discarded. */
    R(0) = 0;
    return 0;
}

int isa_exec_once(Decode *s)
{
    s->isa.inst = inst_fetch(&s->snpc, 4);
    return decode_exec(s);
}
