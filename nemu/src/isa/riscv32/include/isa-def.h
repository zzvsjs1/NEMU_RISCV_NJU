#ifndef __ISA_RISCV_H__
#define __ISA_RISCV_H__

#include <common.h>

#define RISCV_XLEN MUXDEF(CONFIG_RV64, 64, 32)
#ifndef RISCV_GPR_NUM
#define RISCV_GPR_NUM MUXDEF(CONFIG_RVE, 16, 32)
#endif
#define RISCV_INTERRUPT_BIT MUXDEF(CONFIG_RV64, ((word_t)1ull << 63), ((word_t)1u << 31))

#ifdef CONFIG_RV64_FPU
#define RISCV_FPR_NUM 32
#define RISCV_FFLAGS_MASK UINT32_C(0x1f)
#define RISCV_FRM_MASK UINT32_C(0xe0)
#define RISCV_FCSR_MASK UINT32_C(0xff)
#endif

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

#ifdef CONFIG_RV64_FPU
    /*
     * RV64D sets FLEN=64, so one raw 64-bit slot represents each architectural
     * FPR.  Keeping fcsr as one value makes fflags and frm true aliases instead
     * of three independent storage locations that could drift apart.
     *
     * These fields are conditional and placed at the end so RV32, RV32E, and
     * non-FPU RV64 state layouts remain byte-for-byte unchanged.
     */
    uint64_t fpr[RISCV_FPR_NUM];
    uint32_t fcsr;
#endif
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

#ifdef CONFIG_RV64_FPU
#define RISCV_MSTATUS_VS_MASK ((word_t)0x3u << 9)
#define RISCV_MSTATUS_FS_SHIFT 13
#define RISCV_MSTATUS_FS_MASK ((word_t)0x3u << RISCV_MSTATUS_FS_SHIFT)
#define RISCV_MSTATUS_FS_OFF ((word_t)0u << RISCV_MSTATUS_FS_SHIFT)
#define RISCV_MSTATUS_FS_INITIAL ((word_t)1u << RISCV_MSTATUS_FS_SHIFT)
#define RISCV_MSTATUS_FS_CLEAN ((word_t)2u << RISCV_MSTATUS_FS_SHIFT)
#define RISCV_MSTATUS_FS_DIRTY ((word_t)3u << RISCV_MSTATUS_FS_SHIFT)
#define RISCV_MSTATUS_XS_MASK ((word_t)0x3u << 15)
#define RISCV64_MSTATUS_SD ((word_t)1ull << 63)
#endif

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
#ifdef CONFIG_RV64_FPU
    /*
     * SD is a derived, read-only summary bit.  This RV64 model has no vector
     * or custom extension state, so VS and XS are read-only zero and only
     * FS=Dirty contributes to the summary.
     */
    value &= ~(RISCV64_MSTATUS_SD |
               RISCV_MSTATUS_VS_MASK |
               RISCV_MSTATUS_XS_MASK);
    if ((value & RISCV_MSTATUS_FS_MASK) == RISCV_MSTATUS_FS_DIRTY)
    {
        value |= RISCV64_MSTATUS_SD;
    }
#endif

    /* RV64 exposes SXL=2 and UXL=2 in mstatus. */
    return value | RISCV64_MSTATUS_UXL_SXL;
#else
    return value;
#endif
}

#define riscv64_mstatus_normalise(value) riscv_mstatus_normalise(value)

#ifdef CONFIG_RV64_FPU
/* FP instructions and FP CSR accesses are illegal only while FS is Off. */
static inline bool riscv_mstatus_fp_enabled(word_t mstatus)
{
    return (mstatus & RISCV_MSTATUS_FS_MASK) != RISCV_MSTATUS_FS_OFF;
}

/* Return mstatus with the floating-point state marked Dirty and SD derived. */
static inline word_t riscv_mstatus_mark_fp_dirty(word_t mstatus)
{
    mstatus = (mstatus & ~RISCV_MSTATUS_FS_MASK) |
              RISCV_MSTATUS_FS_DIRTY;
    return riscv_mstatus_normalise(mstatus);
}
#endif

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
