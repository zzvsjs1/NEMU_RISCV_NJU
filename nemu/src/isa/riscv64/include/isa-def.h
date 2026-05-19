#ifndef __ISA_RISCV64_H__
#define __ISA_RISCV64_H__

#include <common.h>

typedef struct
{
    union
    {
        uint64_t _64;
    } gpr[32];

    vaddr_t pc;

    struct
    {
        // Supervisor address translation and protection register.
        // CSR address 0x180.
        rtlreg_t satp;

        // Machine status register.
        // CSR address 0x300.
        rtlreg_t mstatus;

        // Machine trap-handler base address.
        // CSR address 0x305.
        rtlreg_t mtvec;

        // Machine scratch register.
        // CSR address 0x340.
        rtlreg_t mscratch;

        // Machine exception program counter.
        // CSR address 0x341.
        rtlreg_t mepc;

        // Machine trap cause.
        // CSR address 0x342.
        rtlreg_t mcause;

        // Machine trap value.
        // CSR address 0x343.
        rtlreg_t mtval;
    } csr;

    rtlreg_t prvi;
    bool INTR;
} riscv64_CPU_state;

enum
{
    RISCV64_PRIV_U = 0,
    RISCV64_PRIV_S = 1,
    RISCV64_PRIV_M = 3,
};

enum
{
    RISCV64_CAUSE_INST_ADDR_MISALIGNED = 0,
    RISCV64_CAUSE_ILLEGAL_INST = 2,
    RISCV64_CAUSE_BREAKPOINT = 3,
    RISCV64_CAUSE_LOAD_ADDR_MISALIGNED = 4,
    RISCV64_CAUSE_STORE_ADDR_MISALIGNED = 6,
    RISCV64_CAUSE_ECALL_U = 8,
    RISCV64_CAUSE_ECALL_S = 9,
    RISCV64_CAUSE_ECALL_M = 11,
    RISCV64_CAUSE_INST_PAGE_FAULT = 12,
    RISCV64_CAUSE_LOAD_PAGE_FAULT = 13,
    RISCV64_CAUSE_STORE_PAGE_FAULT = 15,
};

#define RISCV64_MSTATUS_UXL_SXL (((word_t)2u << 32) | ((word_t)2u << 34))

static inline word_t riscv64_mstatus_normalise(word_t value)
{
    /*
     * RV64 exposes SXL=2 and UXL=2 in mstatus when S/U modes are available.
     * The local CSR model does not otherwise use those high fields, but keeping
     * them set makes architectural CSR reads match Spike and preserves them
     * across ordinary mstatus writes.
     */
    if (((value >> 11) & 0x3u) == 0x2u)
    {
        value &= ~((word_t)0x3u << 11);
    }

    return value | RISCV64_MSTATUS_UXL_SXL;
}

static inline word_t riscv64_ecall_cause_from_priv(word_t priv)
{
    if (priv == RISCV64_PRIV_U)
    {
        return RISCV64_CAUSE_ECALL_U;
    }

    if (priv == RISCV64_PRIV_S)
    {
        return RISCV64_CAUSE_ECALL_S;
    }

    return RISCV64_CAUSE_ECALL_M;
}

typedef struct
{
    uint32_t inst;
} riscv64_ISADecodeInfo;

#endif
