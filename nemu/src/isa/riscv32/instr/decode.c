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

/* End Helper */


/* Start decode operand helper */

#define def_DopHelper(name) \
    void concat(decode_op_, name) (Decode *s, Operand *op, word_t val, bool flag)

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

/* End decode operand helper */



/* Start Decode helper functions */

static def_DHelper(R)
{
    decode_op_r(s, id_src1, s->isa.instr.r.rs1, false);
    decode_op_r(s, id_src2, s->isa.instr.r.rs2, false);
    decode_op_r(s, id_dest, s->isa.instr.r.rd, true);
}

static def_DHelper(I)
{
    decode_op_r(s, id_src1, s->isa.instr.i.rs1, false);
    decode_op_i(s, id_src2, s->isa.instr.i.simm11_0, false);
    decode_op_r(s, id_dest, s->isa.instr.i.rd, true);
}

static def_DHelper(S)
{
    decode_op_r(s, id_src1, s->isa.instr.s.rs1, false);
    sword_t simm = (s->isa.instr.s.simm11_5 << 5) | s->isa.instr.s.imm4_0;
    decode_op_i(s, id_src2, simm, false);
    decode_op_r(s, id_dest, s->isa.instr.s.rs2, false);
}

static def_DHelper(B)
{
    // rs1
    decode_op_r(s, id_src1, s->isa.instr.b.rs1, false);
    
    // rs2
    decode_op_r(s, id_src2, s->isa.instr.b.rs2, false);

    word_t simm = (s->isa.instr.b.imm4_1 << 1)
                    | (s->isa.instr.b.imm10_5 << 5)
                    | (s->isa.instr.b.imm11 << 11)
                    | (s->isa.instr.b.simm12 << 12);

    // imm
    rtl_li(s, s0, simm);
}

static def_DHelper(U)
{
    // imm[31:12]
    decode_op_i(s, id_src1, s->isa.instr.u.imm31_12 << 12, true);
    // rd[11:7]
    decode_op_r(s, id_dest, s->isa.instr.u.rd, true);
}

static def_DHelper(J)
{
    word_t cur_imm = s->isa.instr.j.imm20 << 20;
    cur_imm |= s->isa.instr.j.imm19_12 << 12;
    cur_imm |= s->isa.instr.j.imm11 << 11;
    cur_imm |= s->isa.instr.j.imm10_1 << 1;

    // imm -> src1
    // rd -> id_dest.
    decode_op_i(s, id_src1, cur_imm, false);
    decode_op_r(s, id_dest, s->isa.instr.j.rd, true);
}

static def_DHelper(CSR)
{
    // rs1 to src1
    decode_op_r(s, id_src1, s->isa.instr.CSR.rs1, false);

    // CSR address -> src2.
    decode_op_i(s, id_src2, s->isa.instr.CSR.csr, false);

    // rd to dest.
    decode_op_r(s, id_dest, s->isa.instr.CSR.rd, true);
}

/* End with Decode Helper functions */



/* Start table helper functions. The macro define in nemu/include/cpu/decode.h */

def_THelper(load)
{
    switch (rv32_funct3(s))
    {
        case 0x0: return EXEC_ID_lb;
        case 0x1: return EXEC_ID_lh;
        case 0x2: return EXEC_ID_lw;
        case 0x4: return EXEC_ID_lbu;
        case 0x5: return EXEC_ID_lhu;
        default:  return EXEC_ID_inv;
    }
}

def_THelper(store)
{
    switch (rv32_funct3(s))
    {
        case 0x0: return EXEC_ID_sb;
        case 0x1: return EXEC_ID_sh;
        case 0x2: return EXEC_ID_sw;
        default:  return EXEC_ID_inv;
    }
}

def_THelper(OP_IMM)
{
    switch (rv32_funct3(s))
    {
        case 0x0: return EXEC_ID_addi;
        case 0x2: return EXEC_ID_slti;
        case 0x3: return EXEC_ID_sltiu;
        case 0x4: return EXEC_ID_xori;
        case 0x6: return EXEC_ID_ori;
        case 0x7: return EXEC_ID_andi;
        case 0x1: return rv32_funct7(s) == 0x00 ? EXEC_ID_slli : EXEC_ID_inv;
        case 0x5:
            if (rv32_funct7(s) == 0x00) return EXEC_ID_srli;
            if (rv32_funct7(s) == 0x20) return EXEC_ID_srai;
            return EXEC_ID_inv;
        default: return EXEC_ID_inv;
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
        case 0x000: return EXEC_ID_add;
        case 0x100: return EXEC_ID_sub;
        case 0x001: return EXEC_ID_sll;
        case 0x002: return EXEC_ID_slt;
        case 0x003: return EXEC_ID_sltu;
        case 0x004: return EXEC_ID_xor;
        case 0x005: return EXEC_ID_srl;
        case 0x105: return EXEC_ID_sra;
        case 0x006: return EXEC_ID_or;
        case 0x007: return EXEC_ID_and;
        case 0x008: return EXEC_ID_mul;
        case 0x009: return EXEC_ID_mulh;
        case 0x00a: return EXEC_ID_mulhsu;
        case 0x00b: return EXEC_ID_mulhu;
        case 0x00c: return EXEC_ID_div;
        case 0x00d: return EXEC_ID_divu;
        case 0x00e: return EXEC_ID_rem;
        case 0x00f: return EXEC_ID_remu;
        default:    return EXEC_ID_inv;
    }
}

def_THelper(BRANCH)
{
    switch (rv32_funct3(s))
    {
        case 0x0: return EXEC_ID_beq;
        case 0x1: return EXEC_ID_bne;
        case 0x4: return EXEC_ID_blt;
        case 0x5: return EXEC_ID_bge;
        case 0x6: return EXEC_ID_bltu;
        case 0x7: return EXEC_ID_bgeu;
        default:  return EXEC_ID_inv;
    }
}

def_THelper(SYSTEM)
{
    if (rv32_funct3(s) == 0x0)
    {
        switch (get_instr(s))
        {
            case 0x00000073u: return EXEC_ID_ecall;
            case 0x00100073u: return EXEC_ID_ebreak;
            case 0x30200073u: return EXEC_ID_mret;
            default:          return EXEC_ID_inv;
        }
    }

    switch (rv32_funct3(s))
    {
        case 0x1: return EXEC_ID_csrrw;
        case 0x2: return EXEC_ID_csrrs;
        case 0x3: return EXEC_ID_csrrc;
        case 0x5: return EXEC_ID_csrrwi;
        case 0x6: return EXEC_ID_csrrsi;
        case 0x7: return EXEC_ID_csrrci;
        default:  return EXEC_ID_inv;
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
    s->isa.instr.val = instr_fetch(&s->snpc, (int) sizeof(word_t));
    int idx = table_main(s);
    return idx;
}
