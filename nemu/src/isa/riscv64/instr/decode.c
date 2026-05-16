#include "../local-include/reg.h"
#include <cpu/ifetch.h>
#include <isa-all-instr.h>

def_all_THelper();

static uint32_t get_instr(Decode *s)
{
    return s->isa.instr.val;
}

static inline uint32_t rv64_opcode(Decode *s)
{
    return BITS(get_instr(s), 6, 0);
}

static inline uint32_t rv64_funct3(Decode *s)
{
    return BITS(get_instr(s), 14, 12);
}

static inline uint32_t rv64_funct7(Decode *s)
{
    return BITS(get_instr(s), 31, 25);
}

static inline uint32_t rv64_funct6(Decode *s)
{
    return BITS(get_instr(s), 31, 26);
}

static inline uint32_t rv64_funct7_funct3(Decode *s)
{
    return (rv64_funct7(s) << 3) | rv64_funct3(s);
}

static inline uint32_t rv64_rd(Decode *s)
{
    return BITS(get_instr(s), 11, 7);
}

static inline uint32_t rv64_rs1(Decode *s)
{
    return BITS(get_instr(s), 19, 15);
}

static inline uint32_t rv64_rs2(Decode *s)
{
    return BITS(get_instr(s), 24, 20);
}

static inline word_t rv64_imm_i(Decode *s)
{
    return (word_t)SEXT(BITS(get_instr(s), 31, 20), 12);
}

static inline word_t rv64_imm_s(Decode *s)
{
    const uint32_t imm = (BITS(get_instr(s), 31, 25) << 5) | rv64_rd(s);
    return (word_t)SEXT(imm, 12);
}

static inline word_t rv64_imm_b_raw(Decode *s)
{
    return (BITS(get_instr(s), 11, 8) << 1) |
           (BITS(get_instr(s), 30, 25) << 5) |
           (BITS(get_instr(s), 7, 7) << 11) |
           (BITS(get_instr(s), 31, 31) << 12);
}

static inline word_t rv64_imm_u(Decode *s)
{
    return get_instr(s) & 0xfffff000u;
}

static inline word_t rv64_imm_j_raw(Decode *s)
{
    return (BITS(get_instr(s), 31, 31) << 20) |
           (BITS(get_instr(s), 19, 12) << 12) |
           (BITS(get_instr(s), 20, 20) << 11) |
           (BITS(get_instr(s), 30, 21) << 1);
}

static inline uint32_t rv64_csr(Decode *s)
{
    return BITS(get_instr(s), 31, 20);
}

#define def_DopHelper(name) \
    void concat(decode_op_, name)(Decode * s, Operand * op, word_t val, bool flag)

static def_DopHelper(i)
{
    op->imm = val;
}

static def_DopHelper(r)
{
    bool is_write = flag;
    static word_t zero_null = 0;
    op->preg = (is_write && val == 0) ? &zero_null : &gpr(val);
}

static def_DHelper(R)
{
    decode_op_r(s, id_src1, rv64_rs1(s), false);
    decode_op_r(s, id_src2, rv64_rs2(s), false);
    decode_op_r(s, id_dest, rv64_rd(s), true);
}

static def_DHelper(I)
{
    decode_op_r(s, id_src1, rv64_rs1(s), false);
    decode_op_i(s, id_src2, rv64_imm_i(s), false);
    decode_op_r(s, id_dest, rv64_rd(s), true);
}

static def_DHelper(S)
{
    decode_op_r(s, id_src1, rv64_rs1(s), false);
    decode_op_i(s, id_src2, rv64_imm_s(s), false);
    decode_op_r(s, id_dest, rv64_rs2(s), false);
}

static def_DHelper(B)
{
    decode_op_r(s, id_src1, rv64_rs1(s), false);
    decode_op_r(s, id_src2, rv64_rs2(s), false);
    rtl_li(s, s0, rv64_imm_b_raw(s));
}

static def_DHelper(U)
{
    decode_op_i(s, id_src1, rv64_imm_u(s), true);
    decode_op_r(s, id_dest, rv64_rd(s), true);
}

static def_DHelper(J)
{
    decode_op_i(s, id_src1, rv64_imm_j_raw(s), false);
    decode_op_r(s, id_dest, rv64_rd(s), true);
}

static def_DHelper(CSR)
{
    decode_op_r(s, id_src1, rv64_rs1(s), false);
    decode_op_i(s, id_src2, rv64_csr(s), false);
    decode_op_r(s, id_dest, rv64_rd(s), true);
}

def_THelper(load)
{
    switch (rv64_funct3(s))
    {
    case 0x0:
        return EXEC_ID_lb;
    case 0x1:
        return EXEC_ID_lh;
    case 0x2:
        return EXEC_ID_lw;
    case 0x3:
        return EXEC_ID_ld;
    case 0x4:
        return EXEC_ID_lbu;
    case 0x5:
        return EXEC_ID_lhu;
    case 0x6:
        return EXEC_ID_lwu;
    default:
        return EXEC_ID_inv;
    }
}

def_THelper(store)
{
    switch (rv64_funct3(s))
    {
    case 0x0:
        return EXEC_ID_sb;
    case 0x1:
        return EXEC_ID_sh;
    case 0x2:
        return EXEC_ID_sw;
    case 0x3:
        return EXEC_ID_sd;
    default:
        return EXEC_ID_inv;
    }
}

def_THelper(OP_IMM)
{
    switch (rv64_funct3(s))
    {
    case 0x0:
        return EXEC_ID_addi;
    case 0x2:
        return EXEC_ID_slti;
    case 0x3:
        return EXEC_ID_sltiu;
    case 0x4:
        return EXEC_ID_xori;
    case 0x6:
        return EXEC_ID_ori;
    case 0x7:
        return EXEC_ID_andi;
    case 0x1:
        return rv64_funct6(s) == 0x00 ? EXEC_ID_slli : EXEC_ID_inv;
    case 0x5:
        if (rv64_funct6(s) == 0x00)
            return EXEC_ID_srli;
        if (rv64_funct6(s) == 0x10)
            return EXEC_ID_srai;
        return EXEC_ID_inv;
    default:
        return EXEC_ID_inv;
    }
}

def_THelper(OP_IMM_32)
{
    switch (rv64_funct3(s))
    {
    case 0x0:
        return EXEC_ID_addiw;
    case 0x1:
        return rv64_funct7(s) == 0x00 ? EXEC_ID_slliw : EXEC_ID_inv;
    case 0x5:
        if (rv64_funct7(s) == 0x00)
            return EXEC_ID_srliw;
        if (rv64_funct7(s) == 0x20)
            return EXEC_ID_sraiw;
        return EXEC_ID_inv;
    default:
        return EXEC_ID_inv;
    }
}

def_THelper(OP)
{
    switch (rv64_funct7_funct3(s))
    {
    case 0x000:
        return EXEC_ID_add;
    case 0x100:
        return EXEC_ID_sub;
    case 0x001:
        return EXEC_ID_sll;
    case 0x002:
        return EXEC_ID_slt;
    case 0x003:
        return EXEC_ID_sltu;
    case 0x004:
        return EXEC_ID_xor;
    case 0x005:
        return EXEC_ID_srl;
    case 0x105:
        return EXEC_ID_sra;
    case 0x006:
        return EXEC_ID_or;
    case 0x007:
        return EXEC_ID_and;
    case 0x008:
        return EXEC_ID_mul;
    case 0x009:
        return EXEC_ID_mulh;
    case 0x00a:
        return EXEC_ID_mulhsu;
    case 0x00b:
        return EXEC_ID_mulhu;
    case 0x00c:
        return EXEC_ID_div;
    case 0x00d:
        return EXEC_ID_divu;
    case 0x00e:
        return EXEC_ID_rem;
    case 0x00f:
        return EXEC_ID_remu;
    default:
        return EXEC_ID_inv;
    }
}

def_THelper(OP_32)
{
    switch (rv64_funct7_funct3(s))
    {
    case 0x000:
        return EXEC_ID_addw;
    case 0x100:
        return EXEC_ID_subw;
    case 0x001:
        return EXEC_ID_sllw;
    case 0x005:
        return EXEC_ID_srlw;
    case 0x105:
        return EXEC_ID_sraw;
    case 0x008:
        return EXEC_ID_mulw;
    case 0x00c:
        return EXEC_ID_divw;
    case 0x00d:
        return EXEC_ID_divuw;
    case 0x00e:
        return EXEC_ID_remw;
    case 0x00f:
        return EXEC_ID_remuw;
    default:
        return EXEC_ID_inv;
    }
}

def_THelper(BRANCH)
{
    switch (rv64_funct3(s))
    {
    case 0x0:
        return EXEC_ID_beq;
    case 0x1:
        return EXEC_ID_bne;
    case 0x4:
        return EXEC_ID_blt;
    case 0x5:
        return EXEC_ID_bge;
    case 0x6:
        return EXEC_ID_bltu;
    case 0x7:
        return EXEC_ID_bgeu;
    default:
        return EXEC_ID_inv;
    }
}

def_THelper(JALR)
{
    return rv64_funct3(s) == 0x0 ? EXEC_ID_jalr : EXEC_ID_inv;
}

def_THelper(SYSTEM)
{
    if (rv64_funct3(s) == 0x0)
    {
        switch (get_instr(s))
        {
        case 0x00000073u:
            return EXEC_ID_ecall;
        case 0x00100073u:
            return EXEC_ID_ebreak;
        case 0x30200073u:
            return EXEC_ID_mret;
        case 0x10500073u:
            return EXEC_ID_wfi;
        default:
            return EXEC_ID_inv;
        }
    }

    switch (rv64_funct3(s))
    {
    case 0x1:
        return EXEC_ID_csrrw;
    case 0x2:
        return EXEC_ID_csrrs;
    case 0x3:
        return EXEC_ID_csrrc;
    case 0x5:
        return EXEC_ID_csrrwi;
    case 0x6:
        return EXEC_ID_csrrsi;
    case 0x7:
        return EXEC_ID_csrrci;
    default:
        return EXEC_ID_inv;
    }
}

def_THelper(MISC_MEM)
{
    switch (rv64_funct3(s))
    {
    case 0x0:
        return EXEC_ID_fence;
    case 0x1:
        return EXEC_ID_fence_i;
    default:
        return EXEC_ID_inv;
    }
}

def_THelper(main)
{
    switch (rv64_opcode(s))
    {
    case 0x03:
        decode_I(s, 0);
        return table_load(s);
    case 0x23:
        decode_S(s, 0);
        return table_store(s);
    case 0x13:
        decode_I(s, 0);
        return table_OP_IMM(s);
    case 0x1b:
        decode_I(s, 0);
        return table_OP_IMM_32(s);
    case 0x37:
        decode_U(s, 0);
        return EXEC_ID_lui;
    case 0x17:
        decode_U(s, 0);
        return EXEC_ID_auipc;
    case 0x33:
        decode_R(s, 0);
        return table_OP(s);
    case 0x3b:
        decode_R(s, 0);
        return table_OP_32(s);
    case 0x63:
        decode_B(s, 0);
        return table_BRANCH(s);
    case 0x6f:
        decode_J(s, 0);
        return EXEC_ID_jal;
    case 0x67:
        decode_I(s, 0);
        return table_JALR(s);
    case 0x0f:
        return table_MISC_MEM(s);
    case 0x73:
        decode_CSR(s, 0);
        return table_SYSTEM(s);
    case 0x6b:
        return EXEC_ID_nemu_trap;
    default:
        return EXEC_ID_inv;
    }
}

int isa_fetch_decode(Decode *s)
{
    s->isa.instr.val = instr_fetch(&s->snpc, 4);
    return table_main(s);
}
