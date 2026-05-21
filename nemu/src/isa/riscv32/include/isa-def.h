#ifndef __ISA_RISCV_H__
#define __ISA_RISCV_H__

#include <common.h>

#define RISCV_XLEN MUXDEF(CONFIG_RV64, 64, 32)
#ifndef RISCV_GPR_NUM
#define RISCV_GPR_NUM MUXDEF(CONFIG_RVE, 16, 32)
#endif
#define RISCV_INTERRUPT_BIT MUXDEF(CONFIG_RV64, ((word_t)1ull << 63), ((word_t)1u << 31))

typedef struct riscv_CPU_state
{
    struct
    {
#ifdef CONFIG_RV64
        uint64_t _64;
#else
        rtlreg_t _32;
#endif
    } gpr[RISCV_GPR_NUM];

    vaddr_t pc;

    struct
    {
        rtlreg_t satp;
        rtlreg_t mstatus;
        rtlreg_t mtvec;
        rtlreg_t mscratch;
        rtlreg_t mepc;
        rtlreg_t mcause;
        rtlreg_t mtval;
    } csr;

    rtlreg_t prvi;
    bool INTR;
} riscv_CPU_state;

typedef riscv_CPU_state riscv32_CPU_state;
typedef riscv_CPU_state riscv64_CPU_state;

enum
{
    RISCV_PRIV_U = 0,
    RISCV_PRIV_S = 1,
    RISCV_PRIV_M = 3,
};

enum
{
    RISCV_CAUSE_INST_ADDR_MISALIGNED = 0,
    RISCV_CAUSE_ILLEGAL_INST = 2,
    RISCV_CAUSE_BREAKPOINT = 3,
    RISCV_CAUSE_LOAD_ADDR_MISALIGNED = 4,
    RISCV_CAUSE_STORE_ADDR_MISALIGNED = 6,
    RISCV_CAUSE_ECALL_U = 8,
    RISCV_CAUSE_ECALL_S = 9,
    RISCV_CAUSE_ECALL_M = 11,
    RISCV_CAUSE_INST_PAGE_FAULT = 12,
    RISCV_CAUSE_LOAD_PAGE_FAULT = 13,
    RISCV_CAUSE_STORE_PAGE_FAULT = 15,
};

#define RISCV32_PRIV_U RISCV_PRIV_U
#define RISCV32_PRIV_S RISCV_PRIV_S
#define RISCV32_PRIV_M RISCV_PRIV_M
#define RISCV64_PRIV_U RISCV_PRIV_U
#define RISCV64_PRIV_S RISCV_PRIV_S
#define RISCV64_PRIV_M RISCV_PRIV_M

#define RISCV32_CAUSE_INST_ADDR_MISALIGNED RISCV_CAUSE_INST_ADDR_MISALIGNED
#define RISCV32_CAUSE_ILLEGAL_INST RISCV_CAUSE_ILLEGAL_INST
#define RISCV32_CAUSE_BREAKPOINT RISCV_CAUSE_BREAKPOINT
#define RISCV32_CAUSE_LOAD_ADDR_MISALIGNED RISCV_CAUSE_LOAD_ADDR_MISALIGNED
#define RISCV32_CAUSE_STORE_ADDR_MISALIGNED RISCV_CAUSE_STORE_ADDR_MISALIGNED
#define RISCV32_CAUSE_ECALL_U RISCV_CAUSE_ECALL_U
#define RISCV32_CAUSE_ECALL_S RISCV_CAUSE_ECALL_S
#define RISCV32_CAUSE_ECALL_M RISCV_CAUSE_ECALL_M
#define RISCV32_CAUSE_INST_PAGE_FAULT RISCV_CAUSE_INST_PAGE_FAULT
#define RISCV32_CAUSE_LOAD_PAGE_FAULT RISCV_CAUSE_LOAD_PAGE_FAULT
#define RISCV32_CAUSE_STORE_PAGE_FAULT RISCV_CAUSE_STORE_PAGE_FAULT

#define RISCV64_CAUSE_INST_ADDR_MISALIGNED RISCV_CAUSE_INST_ADDR_MISALIGNED
#define RISCV64_CAUSE_ILLEGAL_INST RISCV_CAUSE_ILLEGAL_INST
#define RISCV64_CAUSE_BREAKPOINT RISCV_CAUSE_BREAKPOINT
#define RISCV64_CAUSE_LOAD_ADDR_MISALIGNED RISCV_CAUSE_LOAD_ADDR_MISALIGNED
#define RISCV64_CAUSE_STORE_ADDR_MISALIGNED RISCV_CAUSE_STORE_ADDR_MISALIGNED
#define RISCV64_CAUSE_ECALL_U RISCV_CAUSE_ECALL_U
#define RISCV64_CAUSE_ECALL_S RISCV_CAUSE_ECALL_S
#define RISCV64_CAUSE_ECALL_M RISCV_CAUSE_ECALL_M
#define RISCV64_CAUSE_INST_PAGE_FAULT RISCV_CAUSE_INST_PAGE_FAULT
#define RISCV64_CAUSE_LOAD_PAGE_FAULT RISCV_CAUSE_LOAD_PAGE_FAULT
#define RISCV64_CAUSE_STORE_PAGE_FAULT RISCV_CAUSE_STORE_PAGE_FAULT

#define RISCV64_MSTATUS_UXL_SXL (((word_t)2u << 32) | ((word_t)2u << 34))

/*
 * Apply the WARL parts of mstatus that this model currently implements.  WARL
 * means "write any, read legal": software may write a reserved value, but the
 * stored value must be normalised to one the architecture permits.
 */
static inline word_t riscv_mstatus_normalise(word_t value)
{
    /*
     * MPP=2 is reserved for both RV32 and RV64.  Keep the local WARL model
     * simple by clearing that value before the rest of the machine state sees it.
     */
    if (((value >> 11) & 0x3u) == 0x2u)
    {
        value &= ~((word_t)0x3u << 11);
    }

#ifdef CONFIG_RV64
    /* RV64 exposes SXL=2 and UXL=2 in mstatus. */
    return value | RISCV64_MSTATUS_UXL_SXL;
#else
    return value;
#endif
}

#define riscv64_mstatus_normalise(value) riscv_mstatus_normalise(value)

/*
 * ECALL has a different exception cause for each current privilege level.  The
 * direct interpreter calls this before trap delivery so mcause matches the mode
 * that executed ECALL, not the mode entered by the trap.
 */
static inline word_t riscv_ecall_cause_from_priv(word_t priv)
{
    if (priv == RISCV_PRIV_U)
    {
        return RISCV_CAUSE_ECALL_U;
    }

    if (priv == RISCV_PRIV_S)
    {
        return RISCV_CAUSE_ECALL_S;
    }

    return RISCV_CAUSE_ECALL_M;
}

#define riscv32_ecall_cause_from_priv(priv) riscv_ecall_cause_from_priv(priv)
#define riscv64_ecall_cause_from_priv(priv) riscv_ecall_cause_from_priv(priv)

typedef struct
{
    uint32_t inst;
} riscv_ISADecodeInfo;

typedef riscv_ISADecodeInfo riscv32_ISADecodeInfo;
typedef riscv_ISADecodeInfo riscv64_ISADecodeInfo;

#endif
