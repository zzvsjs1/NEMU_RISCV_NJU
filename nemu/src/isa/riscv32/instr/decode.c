#include "../local-include/reg.h"
#include <cpu/ifetch.h>
#include <isa-all-instr.h>

def_all_THelper();

static uint32_t get_instr(Decode *s)
{
    return s->isa.instr.val;
}

/* Start Helper */

static inline uint32_t rv32_opcode(Decode *s)
{
    return BITS(get_instr(s), 6, 0);
}

static inline uint32_t rv32_funct3(Decode *s)
{
    return BITS(get_instr(s), 14, 12);
}

static inline uint32_t rv32_funct7(Decode *s)
{
    return BITS(get_instr(s), 31, 25);
}

static inline uint32_t rv32_funct7_funct3(Decode *s)
{
    return (rv32_funct7(s) << 3) | rv32_funct3(s);
}

static inline uint32_t rv32_rd(Decode *s)
{
    return BITS(get_instr(s), 11, 7);
}

static inline uint32_t rv32_rs1(Decode *s)
{
    return BITS(get_instr(s), 19, 15);
}

static inline uint32_t rv32_rs2(Decode *s)
{
    return BITS(get_instr(s), 24, 20);
}

static inline word_t rv32_imm_i(Decode *s)
{
    return (word_t)SEXT(BITS(get_instr(s), 31, 20), 12);
}

static inline word_t rv32_imm_s(Decode *s)
{
    const uint32_t imm = (BITS(get_instr(s), 31, 25) << 5) | rv32_rd(s);
    return (word_t)SEXT(imm, 12);
}

static inline word_t rv32_imm_b_raw(Decode *s)
{
    return (BITS(get_instr(s), 11, 8) << 1) | (BITS(get_instr(s), 30, 25) << 5) | (BITS(get_instr(s), 7, 7) << 11) | (BITS(get_instr(s), 31, 31) << 12);
}

static inline word_t rv32_imm_u(Decode *s)
{
    return get_instr(s) & 0xfffff000u;
}

static inline word_t rv32_imm_j_raw(Decode *s)
{
    return (BITS(get_instr(s), 31, 31) << 20) | (BITS(get_instr(s), 19, 12) << 12) | (BITS(get_instr(s), 20, 20) << 11) | (BITS(get_instr(s), 30, 21) << 1);
}

static inline uint32_t rv32_csr(Decode *s)
{
    return BITS(get_instr(s), 31, 20);
}

/* End Helper */

/* Start decode operand helper */

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
    /*
     * x0 is hardwired to zero.  Decode writes to x0 target a private sink so
     * instruction helpers can write through ddest uniformly without checking
     * the destination register every time.
     */
    op->preg = (is_write && val == 0) ? &zero_null : &gpr(val);
}

/* End decode operand helper */

/* Start Decode helper functions */

static def_DHelper(R)
{
    decode_op_r(s, id_src1, rv32_rs1(s), false);
    decode_op_r(s, id_src2, rv32_rs2(s), false);
    decode_op_r(s, id_dest, rv32_rd(s), true);
}

static def_DHelper(I)
{
    decode_op_r(s, id_src1, rv32_rs1(s), false);
    decode_op_i(s, id_src2, rv32_imm_i(s), false);
    decode_op_r(s, id_dest, rv32_rd(s), true);
}

static def_DHelper(S)
{
    decode_op_r(s, id_src1, rv32_rs1(s), false);
    decode_op_i(s, id_src2, rv32_imm_s(s), false);
    decode_op_r(s, id_dest, rv32_rs2(s), false);
}

static def_DHelper(B)
{
    /*
     * Branch immediates are kept raw in s0 until the execution helper.  That
     * lets every branch helper share the same sign-extension and pc-relative
     * target calculation next to its comparison operation.
     */
    // rs1
    decode_op_r(s, id_src1, rv32_rs1(s), false);

    // rs2
    decode_op_r(s, id_src2, rv32_rs2(s), false);

    // imm
    rtl_li(s, s0, rv32_imm_b_raw(s));
}

static def_DHelper(U)
{
    // imm[31:12]
    decode_op_i(s, id_src1, rv32_imm_u(s), true);
    // rd[11:7]
    decode_op_r(s, id_dest, rv32_rd(s), true);
}

static def_DHelper(J)
{
    // imm -> src1
    // rd -> id_dest.
    decode_op_i(s, id_src1, rv32_imm_j_raw(s), false);
    decode_op_r(s, id_dest, rv32_rd(s), true);
}

static def_DHelper(CSR)
{
    // rs1 to src1
    decode_op_r(s, id_src1, rv32_rs1(s), false);

    // CSR address -> src2.
    decode_op_i(s, id_src2, rv32_csr(s), false);

    // rd to dest.
    decode_op_r(s, id_dest, rv32_rd(s), true);
}

/* End with Decode Helper functions */

/* Start table helper functions. The macro define in nemu/include/cpu/decode.h */

def_THelper(load)
{
    switch (rv32_funct3(s))
    {
    case 0x0:
        return EXEC_ID_lb;
    case 0x1:
        return EXEC_ID_lh;
    case 0x2:
        return EXEC_ID_lw;
    case 0x4:
        return EXEC_ID_lbu;
    case 0x5:
        return EXEC_ID_lhu;
    default:
        return EXEC_ID_inv;
    }
}

def_THelper(store)
{
    switch (rv32_funct3(s))
    {
    case 0x0:
        return EXEC_ID_sb;
    case 0x1:
        return EXEC_ID_sh;
    case 0x2:
        return EXEC_ID_sw;
    default:
        return EXEC_ID_inv;
    }
}

def_THelper(OP_IMM)
{
    switch (rv32_funct3(s))
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
        return rv32_funct7(s) == 0x00 ? EXEC_ID_slli : EXEC_ID_inv;
    case 0x5:
        if (rv32_funct7(s) == 0x00)
            return EXEC_ID_srli;

        if (rv32_funct7(s) == 0x20)
            return EXEC_ID_srai;
        return EXEC_ID_inv;
    default:
        return EXEC_ID_inv;
    }
}

def_THelper(JALR)
{
    return rv32_funct3(s) == 0x0 ? EXEC_ID_jalr : EXEC_ID_inv;
}

def_THelper(OP)
{
    switch (rv32_funct7_funct3(s))
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

def_THelper(BRANCH)
{
    switch (rv32_funct3(s))
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

def_THelper(SYSTEM)
{
    /*
     * SYSTEM uses funct3 == 0 for exact instruction encodings such as ecall and
     * mret.  CSR instructions share the same opcode but are selected by funct3,
     * so the exact-match cases must be handled before the CSR table below.
     */

    if (rv32_funct3(s) == 0x0)
    {
        switch (get_instr(s))
        {
        case 0x00000073u:
            return EXEC_ID_ecall;
        case 0x00100073u:
            return EXEC_ID_ebreak;
        case 0x30200073u:
            return EXEC_ID_mret;
        default:
            return EXEC_ID_inv;
        }
    }

    switch (rv32_funct3(s))
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

/* End table helper functions. */

/* Decode start from here. */

def_THelper(main)
{
    switch (rv32_opcode(s))
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
    case 0x37:
        decode_U(s, 0);
        return EXEC_ID_lui;
    case 0x17:
        decode_U(s, 0);
        return EXEC_ID_auipc;
    case 0x33:
        decode_R(s, 0);
        return table_OP(s);
    case 0x63:
        decode_B(s, 0);
        return table_BRANCH(s);
    case 0x6f:
        decode_J(s, 0);
        return EXEC_ID_jal;
    case 0x67:
        decode_I(s, 0);
        return table_JALR(s);
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
    /*
     * RISC-V32 base instructions are fixed at 32 bits in this decoder.  The
     * fetch helper advances snpc before table_main() fills operands, while
     * control-transfer helpers later overwrite dnpc when a jump or branch wins.
     */
    s->isa.instr.val = instr_fetch(&s->snpc, (int)sizeof(word_t));
    int idx = table_main(s);
    return idx;
}
