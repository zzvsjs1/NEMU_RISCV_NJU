#ifndef __ISA_RISCV32_H__
#define __ISA_RISCV32_H__

#include <common.h>

typedef struct
{
    struct
    {
        rtlreg_t _32;
    } gpr[32];
    vaddr_t pc;

    // Current, only use - Table 7. Currently allocated RISC-V machine-level CSR addresses
    struct
    {
        // 0x180, Supervisor Address Translation and Protection
        rtlreg_t satp;

        // Machine status register.
        // 0x300
        rtlreg_t mstatus;

        // Machine trap-handler base address.
        // 0x305
        rtlreg_t mtvec;

        // Machine scratch register.
        // 0x340
        rtlreg_t mscratch;

        // Machine exception program counter.
        // 0x341
        rtlreg_t mepc;

        // Machine trap cause
        // 0x342
        rtlreg_t mcause;

        // Machine trap value.
        // 0x343
        rtlreg_t mtval;
    } csr;

    rtlreg_t prvi;
    bool INTR;
} riscv32_CPU_state;

enum
{
    RISCV32_PRIV_U = 0,
    RISCV32_PRIV_S = 1,
    RISCV32_PRIV_M = 3,
};

enum
{
    RISCV32_CAUSE_INST_ADDR_MISALIGNED = 0,
    RISCV32_CAUSE_ILLEGAL_INST = 2,
    RISCV32_CAUSE_BREAKPOINT = 3,
    RISCV32_CAUSE_LOAD_ADDR_MISALIGNED = 4,
    RISCV32_CAUSE_STORE_ADDR_MISALIGNED = 6,
    RISCV32_CAUSE_ECALL_U = 8,
    RISCV32_CAUSE_ECALL_S = 9,
    RISCV32_CAUSE_ECALL_M = 11,
    RISCV32_CAUSE_INST_PAGE_FAULT = 12,
    RISCV32_CAUSE_LOAD_PAGE_FAULT = 13,
    RISCV32_CAUSE_STORE_PAGE_FAULT = 15,
};

static inline word_t riscv32_ecall_cause_from_priv(word_t priv)
{
    if (priv == RISCV32_PRIV_U)
    {
        return RISCV32_CAUSE_ECALL_U;
    }

    if (priv == RISCV32_PRIV_S)
    {
        return RISCV32_CAUSE_ECALL_S;
    }

    return RISCV32_CAUSE_ECALL_M;
}

// decode
typedef struct
{
    union
    {

        // Refer to Volume I: RISC-V Unprivileged ISA V20191213.
        // Page 16.

        /*
         * The only difference between the S and B formats is that 
         * the 12-bit immediate field is used to encode
         * branch offsets in multiples of 2 in the B format. 
         * Instead of shifting all bits in the instruction-encoded
         * immediate left by one in hardware as is conventionally done, 
         * the middle bits (imm[10:1]) and sign
         * bit stay in fixed positions, while the lowest bit in S format (inst[7]) 
         * encodes a high-order bit in B format.
        */

        // Sign extension always uses inst[31].

        // R-type
        struct
        {
            uint32_t opcode1_0 : 2;
            uint32_t opcode6_2 : 5;
            uint32_t rd : 5;
            uint32_t funct3 : 3;
            uint32_t rs1 : 5;
            uint32_t rs2 : 5;
            uint32_t funct7 : 7;
        } r;

        // I-type
        struct
        {
            uint32_t opcode : 7;
            uint32_t rd : 5;
            uint32_t funct3 : 3;
            uint32_t rs1 : 5;
            int32_t simm11_0 : 12;
        } i;

        // S-type
        struct
        {
            uint32_t opcode1_0 : 2;
            uint32_t opcode6_2 : 5;
            uint32_t imm4_0 : 5;
            uint32_t funct3 : 3;
            uint32_t rs1 : 5;
            uint32_t rs2 : 5;
            int32_t simm11_5 : 7;
        } s;

        // B-type
        struct
        {
            uint32_t opcode1_0 : 2;
            uint32_t opcode6_2 : 5;
            uint32_t imm11 : 1;
            uint32_t imm4_1 : 4;
            uint32_t funct3 : 3;
            uint32_t rs1 : 5;
            uint32_t rs2 : 5;
            uint32_t imm10_5 : 6;
            int32_t simm12 : 1;
        } b;

        // U-type
        struct
        {
            uint32_t opcode : 7;
            uint32_t rd : 5;
            uint32_t imm31_12 : 20;
        } u;

        // J-type
        struct
        {
            uint32_t opcode1_0 : 2;
            uint32_t opcode6_2 : 5;
            uint32_t rd : 5;
            uint32_t imm19_12 : 8;
            uint32_t imm11 : 1;
            uint32_t imm10_1 : 10;
            uint32_t imm20 : 1;
        } j;

        struct
        {
            uint32_t opcode : 7;
            uint32_t rd : 5;
            uint32_t funct3 : 3;
            uint32_t rs1 : 5;
            uint32_t csr : 12;
        } CSR;

        uint32_t val;
    } instr;
} riscv32_ISADecodeInfo;

#endif
