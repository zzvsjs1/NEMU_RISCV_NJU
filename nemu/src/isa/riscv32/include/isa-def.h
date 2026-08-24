#ifndef __ISA_RISCV_H__
#define __ISA_RISCV_H__

#include <common.h>

#define RISCV_XLEN MUXDEF(CONFIG_RV64, 64, 32)
#ifndef RISCV_GPR_NUM
#define RISCV_GPR_NUM MUXDEF(CONFIG_RVE, 16, 32)
#endif

/*
 * Every instruction supported by this target is a fixed-width, four-byte base
 * instruction.  The compressed C extension is not implemented, so IALIGN is 32
 * bits and a control-transfer target must have both low address bits clear.
 *
 * The opcode is always instruction[6:0].  Keeping its width and mask beside the
 * instruction size prevents the interpreter and both JITs from inventing local
 * spellings for the same base-format fields.
 */
enum
{
    RISCV_BASE_INSN_BYTES = 4,
    RISCV_IALIGN_32_MASK = RISCV_BASE_INSN_BYTES - 1,
    RISCV_OPCODE_WIDTH = 7,
    RISCV_OPCODE_MASK = (1u << RISCV_OPCODE_WIDTH) - 1u,
    RISCV_GPR_ZERO = 0,
    RISCV_GPR_LINK = 1,
    RISCV_GPR_ALTERNATE_LINK = 5,
    RISCV_JALR_FUNCT3 = 0,
    RISCV_JALR_TARGET_LSB_MASK = 1,
    RISCV32_SHAMT_WIDTH = 5,
    RISCV64_SHAMT_WIDTH = 6,
    RISCV32_WORD_SHAMT_MASK = (1u << RISCV32_SHAMT_WIDTH) - 1u,
};

#define RISCV_XLEN_SHAMT_MASK MUXDEF(CONFIG_RV64, ((1u << RISCV64_SHAMT_WIDTH) - 1u), RISCV32_WORD_SHAMT_MASK)

/*
 * Complete seven-bit major opcodes used outside INSTPAT declarations.  The
 * binary pattern strings in the direct interpreter deliberately remain
 * literal: there the visual field layout is more informative than a symbolic
 * comparison.  These names are for C switches in the RV32/RV64 JITs and their
 * diagnostic statistics, where a bare hexadecimal value carries no context.
 */
enum
{
    RISCV_OPCODE_LOAD = 0x03,
    RISCV_OPCODE_MISC_MEM = 0x0f,
    RISCV_OPCODE_OP_IMM = 0x13,
    RISCV_OPCODE_AUIPC = 0x17,
    RISCV_OPCODE_OP_IMM_32 = 0x1b,
    RISCV_OPCODE_STORE = 0x23,
    RISCV_OPCODE_AMO = 0x2f,
    RISCV_OPCODE_OP = 0x33,
    RISCV_OPCODE_LUI = 0x37,
    RISCV_OPCODE_OP_32 = 0x3b,
    RISCV_OPCODE_BRANCH = 0x63,
    RISCV_OPCODE_JALR = 0x67,
    RISCV_OPCODE_JAL = 0x6f,
    RISCV_OPCODE_SYSTEM = 0x73,
    RISCV_OPCODE_OP_V = 0x57,
    RISCV_OPCODE_OP_VE = 0x77,
};

/*
 * A standard CSR address is twelve bits.  Address bits [11:10] encode its
 * access class, with 0b11 denoting read-only, while bits [9:8] encode the
 * lowest privilege allowed to access it.  The addresses below are the CSRs
 * backed by this simplified NEMU CPU state, plus misa when a floating-point
 * target exposes its fixed extension set; they are not a complete CSR catalogue.
 */
enum
{
    RISCV_CSR_SATP = 0x180,
    RISCV_CSR_MSTATUS = 0x300,
    RISCV_CSR_MISA = 0x301,
    RISCV_CSR_MTVEC = 0x305,
    RISCV_CSR_MSCRATCH = 0x340,
    RISCV_CSR_MEPC = 0x341,
    RISCV_CSR_MCAUSE = 0x342,
    RISCV_CSR_MTVAL = 0x343,

    RISCV_CSR_PRIV_SHIFT = 8,
    RISCV_CSR_PRIV_WIDTH = 2,
    RISCV_CSR_PRIV_MASK = (1u << RISCV_CSR_PRIV_WIDTH) - 1u,
    RISCV_CSR_ACCESS_SHIFT = 10,
    RISCV_CSR_ACCESS_WIDTH = 2,
    RISCV_CSR_ACCESS_MASK = (1u << RISCV_CSR_ACCESS_WIDTH) - 1u,
    RISCV_CSR_ACCESS_READ_ONLY = RISCV_CSR_ACCESS_MASK,
};

/*
 * mcause uses the most-significant XLEN bit to distinguish an interrupt from a
 * synchronous exception.  The remaining bits contain the cause number; cause
 * 7 is a machine timer interrupt in both RV32 and RV64.
 */
#define RISCV_XCAUSE_INTERRUPT_MASK MUXDEF(CONFIG_RV64, ((word_t)1ull << 63), ((word_t)1u << 31))
#define RISCV_INTERRUPT_BIT RISCV_XCAUSE_INTERRUPT_MASK

enum
{
    RISCV_MACHINE_TIMER_INTERRUPT_CODE = 7,
};

#define RISCV_MCAUSE_MACHINE_TIMER_INTERRUPT (RISCV_XCAUSE_INTERRUPT_MASK | (word_t)RISCV_MACHINE_TIMER_INTERRUPT_CODE)

/*
 * mtvec[1:0] is MODE, leaving a minimum four-byte-aligned BASE in the remaining
 * bits.  Direct mode sends every trap to BASE.  Vectored mode changes only
 * asynchronous interrupt targets, selecting BASE + 4 * cause; four bytes is the
 * size of one base instruction and therefore one vector-table entry.
 */
enum
{
    RISCV_MTVEC_MODE_WIDTH = 2,
    RISCV_MTVEC_MODE_MASK = (1u << RISCV_MTVEC_MODE_WIDTH) - 1u,
    RISCV_MTVEC_MODE_DIRECT = 0,
    RISCV_MTVEC_MODE_VECTORED = 1,
    RISCV_MTVEC_VECTOR_ENTRY_BYTES = RISCV_BASE_INSN_BYTES,
};

/*
 * Complete seven-bit major opcodes for the standard floating-point instruction
 * families.  These values are the whole instruction[6:0] field, rather than a
 * shortened opcode fragment, so both the interpreter and diagnostic code can
 * compare a decoded major opcode directly with one descriptive name.
 *
 * The four fused multiply-add families use the R4-type layout.  Each family
 * needs its own major opcode because the instruction must encode three source
 * registers as well as whether the product and addend are negated; there is no
 * spare funct7 field in R4-type instructions in which to put those distinctions.
 */
enum
{
    RISCV_FP_OPCODE_LOAD = 0x07,
    RISCV_FP_OPCODE_STORE = 0x27,
    RISCV_FP_OPCODE_FMADD = 0x43,
    RISCV_FP_OPCODE_FMSUB = 0x47,
    RISCV_FP_OPCODE_FNMSUB = 0x4b,
    RISCV_FP_OPCODE_FNMADD = 0x4f,
    RISCV_FP_OPCODE_OP = 0x53,
};

#ifdef CONFIG_RISCV_FPU
#define RISCV_FPR_NUM 32

/*
 * RV32F has FLEN=32, while RV32D and RV64D have FLEN=64.  Select raw register
 * storage from FLEN rather than XLEN: RV32D is the important case where those
 * widths differ, and its single-precision values require 64-bit NaN boxes.
 */
#ifdef CONFIG_RISCV_D
typedef uint64_t riscv_fpr_t;
#else
typedef uint32_t riscv_fpr_t;
#endif

/*
 * RISC-V assigns three consecutive unprivileged CSR addresses to floating-point
 * control.  fflags exposes the accrued exception flags, frm holds the dynamic
 * rounding mode selected by an instruction with rm=DYN, and fcsr is the
 * combined view containing both fields.
 *
 * Within fcsr, fflags occupies bits [4:0].  frm begins immediately above it at
 * bit 5 and occupies bits [7:5].  Naming the widths and deriving the shift keeps
 * that relationship visible instead of relying on the hexadecimal values 0x1f,
 * 0xe0, and 0xff being recognised by the reader.
 */
enum
{
    RISCV_CSR_FFLAGS = 0x001,
    RISCV_CSR_FRM = 0x002,
    RISCV_CSR_FCSR = 0x003,
    RISCV_FFLAGS_WIDTH = 5,
    RISCV_FRM_SHIFT = RISCV_FFLAGS_WIDTH,
    RISCV_FRM_WIDTH = 3,
};

/*
 * Shifting one by a field width and subtracting one produces that many low one
 * bits: the five-bit fflags mask is therefore 0b1_1111, while the unshifted
 * three-bit frm value mask is 0b111.  Moving the latter left by
 * RISCV_FRM_SHIFT places it over fcsr[7:5], and ORing both field masks forms the
 * complete implemented fcsr mask for bits [7:0].
 *
 * The architectural fcsr is 32 bits even though CSR instructions transfer
 * values through an XLEN-bit integer register.  This implementation models no
 * fields in reserved fcsr bits [31:8].  Masking every combined read and write
 * with RISCV_FCSR_MASK therefore makes those reserved bits read as zero and
 * prevents a guest write from retaining values in them.
 */
#define RISCV_FFLAGS_MASK ((UINT32_C(1) << RISCV_FFLAGS_WIDTH) - 1)
#define RISCV_FRM_VALUE_MASK ((UINT32_C(1) << RISCV_FRM_WIDTH) - 1)
#define RISCV_FRM_MASK (RISCV_FRM_VALUE_MASK << RISCV_FRM_SHIFT)
#define RISCV_FCSR_MASK (RISCV_FFLAGS_MASK | RISCV_FRM_MASK)

/*
 * misa places its two-bit MXL field at the top of XLEN.  RV32 uses encoding 1
 * in bits [31:30], while RV64 uses encoding 2 in bits [63:62].  The remaining
 * low bits are the alphabetically indexed extension bitmap.
 */
enum
{
    RISCV32_MISA_MXL_SHIFT = 30,
    RISCV32_MISA_MXL_ENCODING = 1,
    RISCV64_MISA_MXL_SHIFT = 62,
    RISCV64_MISA_MXL_ENCODING = 2,
};

#define RISCV_MISA_MXL_SHIFT MUXDEF(CONFIG_RV64, RISCV64_MISA_MXL_SHIFT, RISCV32_MISA_MXL_SHIFT)
#define RISCV_MISA_MXL_ENCODING MUXDEF(CONFIG_RV64, RISCV64_MISA_MXL_ENCODING, RISCV32_MISA_MXL_ENCODING)
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

#ifdef CONFIG_RISCV_FPU
    /*
     * One raw FLEN-wide slot represents each architectural FPR.  Keeping fcsr
     * as one value makes fflags and frm true aliases instead of three
     * independent storage locations that could drift apart.
     *
     * These fields are conditional and placed at the end so RV32E and
     * non-floating-point state layouts remain byte-for-byte unchanged.
     */
    riscv_fpr_t fpr[RISCV_FPR_NUM];
    uint32_t fcsr;
#endif
} riscv_CPU_state;

typedef riscv_CPU_state riscv32_CPU_state;
typedef riscv_CPU_state riscv64_CPU_state;

enum
{
    RISCV_PRIV_U = 0,
    RISCV_PRIV_S = 1,
    RISCV_PRIV_RESERVED = 2,
    RISCV_PRIV_M = 3,
};

/*
 * mstatus implements a small stack for machine-mode trap entry and return.
 * MIE is the current global machine-interrupt enable, MPIE saves its previous
 * value, and MPP records the interrupted privilege.  MPRV makes data accesses
 * use MPP's privilege while executing in M-mode; SUM and MXR then modify the
 * page permissions used by an effective S-mode access.  TVM makes S-mode
 * virtual-memory-management instructions trap to M-mode.
 *
 * A named shift is retained as well as each mask because trap code extracts and
 * inserts MPP, whereas permission checks only need to test one complete mask.
 */
enum
{
    RISCV_MSTATUS_MIE_BIT = 3,
    RISCV_MSTATUS_MPIE_BIT = 7,
    RISCV_MSTATUS_MPP_SHIFT = 11,
    RISCV_MSTATUS_MPP_WIDTH = 2,
    RISCV_MSTATUS_MPRV_BIT = 17,
    RISCV_MSTATUS_SUM_BIT = 18,
    RISCV_MSTATUS_MXR_BIT = 19,
    RISCV_MSTATUS_TVM_BIT = 20,
};

#define RISCV_MSTATUS_MIE ((word_t)1u << RISCV_MSTATUS_MIE_BIT)
#define RISCV_MSTATUS_MPIE ((word_t)1u << RISCV_MSTATUS_MPIE_BIT)
#define RISCV_MSTATUS_MPP_VALUE_MASK ((word_t)((1u << RISCV_MSTATUS_MPP_WIDTH) - 1u))
#define RISCV_MSTATUS_MPP_MASK (RISCV_MSTATUS_MPP_VALUE_MASK << RISCV_MSTATUS_MPP_SHIFT)
#define RISCV_MSTATUS_MPRV ((word_t)1u << RISCV_MSTATUS_MPRV_BIT)
#define RISCV_MSTATUS_SUM ((word_t)1u << RISCV_MSTATUS_SUM_BIT)
#define RISCV_MSTATUS_MXR ((word_t)1u << RISCV_MSTATUS_MXR_BIT)
#define RISCV_MSTATUS_TVM ((word_t)1u << RISCV_MSTATUS_TVM_BIT)

#ifdef CONFIG_RV64
/*
 * UXL and SXL are two-bit WARL fields in RV64 mstatus.  Encoding 2 selects a
 * 64-bit lower-privilege execution environment.  NEMU implements only that
 * width, so every CSR write clears both complete fields before inserting 2.
 * Clearing first matters: merely ORing in bit 1 would turn incoming encoding 1
 * into reserved encoding 3.
 *
 * GVA is cleared on trap entry because this target does not implement the
 * hypervisor extension and therefore never reports a guest virtual address.
 */
enum
{
    RISCV64_MSTATUS_XLEN_WIDTH = 2,
    RISCV64_MSTATUS_XLEN_ENCODING_64 = 2,
    RISCV64_MSTATUS_UXL_SHIFT = 32,
    RISCV64_MSTATUS_SXL_SHIFT = 34,
    RISCV64_MSTATUS_GVA_BIT = 38,
};

#define RISCV64_MSTATUS_XLEN_VALUE_MASK ((UINT64_C(1) << RISCV64_MSTATUS_XLEN_WIDTH) - UINT64_C(1))
#define RISCV64_MSTATUS_UXL_MASK (RISCV64_MSTATUS_XLEN_VALUE_MASK << RISCV64_MSTATUS_UXL_SHIFT)
#define RISCV64_MSTATUS_SXL_MASK (RISCV64_MSTATUS_XLEN_VALUE_MASK << RISCV64_MSTATUS_SXL_SHIFT)
#define RISCV64_MSTATUS_UXL_SXL_MASK (RISCV64_MSTATUS_UXL_MASK | RISCV64_MSTATUS_SXL_MASK)
#define RISCV64_MSTATUS_UXL_VALUE ((uint64_t)RISCV64_MSTATUS_XLEN_ENCODING_64 << RISCV64_MSTATUS_UXL_SHIFT)
#define RISCV64_MSTATUS_SXL_VALUE ((uint64_t)RISCV64_MSTATUS_XLEN_ENCODING_64 << RISCV64_MSTATUS_SXL_SHIFT)
#define RISCV64_MSTATUS_UXL_SXL_VALUE (RISCV64_MSTATUS_UXL_VALUE | RISCV64_MSTATUS_SXL_VALUE)
#define RISCV64_MSTATUS_GVA (UINT64_C(1) << RISCV64_MSTATUS_GVA_BIT)
#endif

/*
 * A page-table entry devotes bits [9:0] to flags and software-reserved state,
 * so its physical page number starts at bit 10.  V identifies a valid entry;
 * R/W/X distinguish a leaf and grant accesses; U controls user accessibility;
 * A and D record whether a page has been accessed or written.  The RV64 Sv39
 * walker uses a software-managed A/D policy: missing A, or missing D on a write,
 * causes a page fault instead of NEMU updating the guest PTE.  The legacy RV32
 * Sv32 walker retains its earlier, simpler permission checks.
 */
enum
{
    RISCV_PTE_V_BIT = 0,
    RISCV_PTE_R_BIT = 1,
    RISCV_PTE_W_BIT = 2,
    RISCV_PTE_X_BIT = 3,
    RISCV_PTE_U_BIT = 4,
    RISCV_PTE_A_BIT = 6,
    RISCV_PTE_D_BIT = 7,
    RISCV_PTE_PPN_SHIFT = 10,
};

#define RISCV_PTE_V ((word_t)1u << RISCV_PTE_V_BIT)
#define RISCV_PTE_R ((word_t)1u << RISCV_PTE_R_BIT)
#define RISCV_PTE_W ((word_t)1u << RISCV_PTE_W_BIT)
#define RISCV_PTE_X ((word_t)1u << RISCV_PTE_X_BIT)
#define RISCV_PTE_U ((word_t)1u << RISCV_PTE_U_BIT)
#define RISCV_PTE_A ((word_t)1u << RISCV_PTE_A_BIT)
#define RISCV_PTE_D ((word_t)1u << RISCV_PTE_D_BIT)
#define RISCV_PTE_RWX (RISCV_PTE_R | RISCV_PTE_W | RISCV_PTE_X)
#define RISCV_PTE_NON_LEAF_RESERVED (RISCV_PTE_U | RISCV_PTE_A | RISCV_PTE_D)

/*
 * satp combines the translation mode with the root page-table PPN.  RV32 uses
 * one MODE bit and a 22-bit PPN for Sv32; RV64 uses a four-bit MODE field and a
 * 44-bit PPN for Sv39.  ASID occupies the bits between MODE and PPN but is not
 * currently used by NEMU's translation caches.
 */
enum
{
    RISCV_SATP_MODE_BARE = 0,
    RISCV32_SATP_MODE_SHIFT = 31,
    RISCV32_SATP_MODE_WIDTH = 1,
    RISCV32_SATP_MODE_SV32 = 1,
    RISCV32_SATP_PPN_WIDTH = 22,
    RISCV64_SATP_MODE_SHIFT = 60,
    RISCV64_SATP_MODE_WIDTH = 4,
    RISCV64_SATP_MODE_SV39 = 8,
    RISCV64_SATP_PPN_WIDTH = 44,
};

#define RISCV32_SATP_MODE_MASK (UINT32_C(1) << RISCV32_SATP_MODE_SHIFT)
#define RISCV32_SATP_PPN_MASK ((UINT32_C(1) << RISCV32_SATP_PPN_WIDTH) - UINT32_C(1))
#define RISCV64_SATP_MODE_VALUE_MASK ((UINT64_C(1) << RISCV64_SATP_MODE_WIDTH) - UINT64_C(1))
#define RISCV64_SATP_MODE_MASK (RISCV64_SATP_MODE_VALUE_MASK << RISCV64_SATP_MODE_SHIFT)
#define RISCV64_SATP_PPN_MASK ((UINT64_C(1) << RISCV64_SATP_PPN_WIDTH) - UINT64_C(1))

/*
 * Sv32 walks two levels of ten-bit VPNs with four-byte PTEs.  Sv39 walks three
 * levels of nine-bit VPNs with eight-byte PTEs and accepts only canonical
 * 39-bit virtual addresses: address bits [63:39] must all copy bit 38.  The low
 * physical PPN masks below validate that a higher-level Sv39 leaf is aligned
 * for its 2 MiB or 1 GiB superpage.
 */
enum
{
    RISCV32_SV32_LEVELS = 2,
    RISCV32_SV32_ROOT_LEVEL = RISCV32_SV32_LEVELS - 1,
    RISCV32_SV32_VPN_BITS = 10,
    RISCV32_SV32_PTE_BYTES = 4,
    RISCV64_SV39_LEVELS = 3,
    RISCV64_SV39_PAGE_LEVEL = 0,
    RISCV64_SV39_MEGAPAGE_LEVEL = 1,
    RISCV64_SV39_GIGAPAGE_LEVEL = 2,
    RISCV64_SV39_ROOT_LEVEL = RISCV64_SV39_GIGAPAGE_LEVEL,
    RISCV64_SV39_VPN_BITS = 9,
    RISCV64_SV39_PTE_BYTES = 8,
    RISCV64_SV39_CANONICAL_SIGN_BIT = 38,
    RISCV64_SV39_CANONICAL_HIGH_SHIFT = 39,
    RISCV64_SV39_CANONICAL_HIGH_BITS = 25,
};

#define RISCV32_SV32_VPN_MASK ((word_t)((1u << RISCV32_SV32_VPN_BITS) - 1u))
#define RISCV64_SV39_VPN_MASK ((word_t)((1u << RISCV64_SV39_VPN_BITS) - 1u))
#define RISCV64_SV39_LEVEL1_LOW_PPN_MASK RISCV64_SV39_VPN_MASK
#define RISCV64_SV39_LEVEL2_LOW_PPN_MASK ((word_t)((1u << (2u * RISCV64_SV39_VPN_BITS)) - 1u))
#define RISCV64_PTE_PPN_MASK RISCV64_SATP_PPN_MASK
/*
 * Svnapot uses bit 63 and Svpbmt uses bits [62:61].  Those extensions are not
 * implemented, and the remaining bits [60:54] are reserved, so any set bit in
 * this complete high region faults rather than entering a translation cache.
 */
#define RISCV64_PTE_UNSUPPORTED_HIGH_MASK (UINT64_C(0x3ff) << 54)

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

#ifdef CONFIG_RISCV_FPU
/*
 * VS, FS, and XS use the same two-bit state encoding: Off=0, Initial=1,
 * Clean=2, and Dirty=3.  This target implements floating-point state only, so
 * VS and XS are forced to Off and SD is derived solely from FS=Dirty.  SD is
 * the most-significant mstatus bit: bit 31 for RV32 and bit 63 for RV64.
 */
enum
{
    RISCV_MSTATUS_EXT_STATE_WIDTH = 2,
    RISCV_MSTATUS_EXT_STATE_OFF = 0,
    RISCV_MSTATUS_EXT_STATE_INITIAL = 1,
    RISCV_MSTATUS_EXT_STATE_CLEAN = 2,
    RISCV_MSTATUS_EXT_STATE_DIRTY = 3,
    RISCV_MSTATUS_VS_SHIFT = 9,
    RISCV_MSTATUS_FS_SHIFT = 13,
    RISCV_MSTATUS_XS_SHIFT = 15,
    RISCV32_MSTATUS_SD_BIT = 31,
    RISCV64_MSTATUS_SD_BIT = 63,
};

#define RISCV_MSTATUS_EXT_STATE_VALUE_MASK ((word_t)((1u << RISCV_MSTATUS_EXT_STATE_WIDTH) - 1u))
#define RISCV_MSTATUS_VS_MASK (RISCV_MSTATUS_EXT_STATE_VALUE_MASK << RISCV_MSTATUS_VS_SHIFT)
#define RISCV_MSTATUS_FS_MASK (RISCV_MSTATUS_EXT_STATE_VALUE_MASK << RISCV_MSTATUS_FS_SHIFT)
#define RISCV_MSTATUS_FS_OFF ((word_t)RISCV_MSTATUS_EXT_STATE_OFF << RISCV_MSTATUS_FS_SHIFT)
#define RISCV_MSTATUS_FS_INITIAL ((word_t)RISCV_MSTATUS_EXT_STATE_INITIAL << RISCV_MSTATUS_FS_SHIFT)
#define RISCV_MSTATUS_FS_CLEAN ((word_t)RISCV_MSTATUS_EXT_STATE_CLEAN << RISCV_MSTATUS_FS_SHIFT)
#define RISCV_MSTATUS_FS_DIRTY ((word_t)RISCV_MSTATUS_EXT_STATE_DIRTY << RISCV_MSTATUS_FS_SHIFT)
#define RISCV_MSTATUS_XS_MASK (RISCV_MSTATUS_EXT_STATE_VALUE_MASK << RISCV_MSTATUS_XS_SHIFT)
#define RISCV32_MSTATUS_SD ((word_t)1u << RISCV32_MSTATUS_SD_BIT)
#define RISCV64_MSTATUS_SD ((word_t)1ull << RISCV64_MSTATUS_SD_BIT)
#define RISCV_MSTATUS_SD MUXDEF(CONFIG_RV64, RISCV64_MSTATUS_SD, RISCV32_MSTATUS_SD)
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
    if (((value & RISCV_MSTATUS_MPP_MASK) >> RISCV_MSTATUS_MPP_SHIFT) == RISCV_PRIV_RESERVED)
    {
        value &= ~RISCV_MSTATUS_MPP_MASK;
    }

#ifdef CONFIG_RISCV_FPU
    /*
     * SD is a derived, read-only summary bit.  This model has no vector or
     * custom extension state, so VS and XS are read-only zero and only
     * FS=Dirty contributes to the summary at the XLEN-specific SD position.
     */
    value &= ~(RISCV_MSTATUS_SD | RISCV_MSTATUS_VS_MASK | RISCV_MSTATUS_XS_MASK);

    if ((value & RISCV_MSTATUS_FS_MASK) == RISCV_MSTATUS_FS_DIRTY)
    {
        value |= RISCV_MSTATUS_SD;
    }
#endif

#ifdef CONFIG_RV64
    /*
     * UXL and SXL are WARL rather than ordinary writable bits.  Clear both
     * fields before inserting encoding 2 so guest values 1 and 3 cannot leave
     * the reserved encoding 3 behind.
     */
    value &= ~(word_t)RISCV64_MSTATUS_UXL_SXL_MASK;
    return value | (word_t)RISCV64_MSTATUS_UXL_SXL_VALUE;
#else
    return value;
#endif
}

#define riscv64_mstatus_normalise(value) riscv_mstatus_normalise(value)

#ifdef CONFIG_RISCV_FPU
/* FP instructions and FP CSR accesses are illegal only while FS is Off. */
static inline bool riscv_mstatus_fp_enabled(word_t mstatus)
{
    return (mstatus & RISCV_MSTATUS_FS_MASK) != RISCV_MSTATUS_FS_OFF;
}

/* Return mstatus with the floating-point state marked Dirty and SD derived. */
static inline word_t riscv_mstatus_mark_fp_dirty(word_t mstatus)
{
    mstatus = (mstatus & ~RISCV_MSTATUS_FS_MASK) | RISCV_MSTATUS_FS_DIRTY;
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
