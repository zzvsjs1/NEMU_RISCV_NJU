#ifndef __ISA_RISCV32_H__
#define __ISA_RISCV32_H__

#include <common.h>

typedef struct
{
    struct
    {
        rtlreg_t _32;
    } gpr[MUXDEF(CONFIG_RVE, 16, 32)];
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

typedef struct
{
    uint32_t inst;
} riscv32_ISADecodeInfo;

#endif
