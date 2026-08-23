#include "local-include/fpu.h"

#ifdef CONFIG_RISCV_FPU

#include "local-include/reg.h"
#include <cpu/difftest.h>
#include <memory/vaddr.h>
#include <softfloat.h>

/*
 * NEMU currently executes one hart on one host thread.  Keeping the current
 * instruction's trap result here lets the existing leaf executors retain their
 * simple void interfaces while giving generated code an unambiguous outcome.
 */
static bool fp_instruction_trapped;

/*
 * All supported instructions use one of the standard 32-bit floating-point
 * layouts from the unprivileged ISA:
 *
 *   ordinary OP-FP: [funct7][rs2][rs1][rm/funct3][rd][opcode]
 *   fused R4 form:  [rs3][fmt][rs2][rs1][rm][rd][opcode]
 *   load I-form:    [       imm[11:0]][rs1][width][rd][opcode]
 *   store S-form:   [imm[11:5]][rs2][rs1][width][imm[4:0]][opcode]
 *
 * Giving each boundary a name keeps the extraction helpers tied to those
 * diagrams.  It also makes the two deliberate field overloads visible:
 * bits [14:12] hold a rounding mode for arithmetic but a sub-operation for
 * several non-rounding OP-FP instructions, while bits [26:25] are both `fmt`
 * in the R4 form and the low two bits of an OP-FP `funct7`.
 */
enum
{
    RISCV_FP_INST_MSB = 31,
    RISCV_FP_INST_OPCODE_MASK = RISCV_OPCODE_MASK,
    RISCV_FP_INST_RD_HI = 11,
    RISCV_FP_INST_RD_LO = 7,
    RISCV_FP_INST_RM_HI = 14,
    RISCV_FP_INST_RM_LO = 12,
    RISCV_FP_INST_RS1_HI = 19,
    RISCV_FP_INST_RS1_LO = 15,
    RISCV_FP_INST_RS2_HI = 24,
    RISCV_FP_INST_RS2_LO = 20,
    RISCV_FP_INST_FUNCT7_LO = 25,
    RISCV_FP_INST_RS3_LO = 27,
    RISCV_FP_INST_FMT_HI = 26,
    RISCV_FP_INST_FMT_LO = 25,
    RISCV_FP_INST_I_IMM_LO = 20,
    RISCV_FP_INST_S_IMM_HIGH_LO = 25,
    RISCV_FP_INST_S_IMM_LOW_HI = 11,
    RISCV_FP_INST_S_IMM_LOW_LO = 7,
    RISCV_FP_INST_S_IMM_LOW_WIDTH = 5,
    RISCV_FP_INST_IMMEDIATE_WIDTH = 12,
};

/*
 * Table 26 encodes precision in two bits.  An F-only configuration accepts S,
 * while any D configuration accepts S and D independently of XLEN. Naming H
 * and Q documents the complete encoding and keeps the capability check out of
 * unexplained integer literals.
 */
enum riscv_fp_format
{
    RISCV_FP_FORMAT_SINGLE = 0,
    RISCV_FP_FORMAT_DOUBLE = 1,
    RISCV_FP_FORMAT_HALF = 2,
    RISCV_FP_FORMAT_QUAD = 3,
};

/*
 * The load/store `width` field follows the base memory-width encoding:
 * 010 selects a 32-bit word and 011 selects a 64-bit doubleword.  The byte
 * counts are kept separate because memory helpers accept bytes, not `funct3`.
 */
enum
{
    RISCV_FP_WIDTH_WORD = 2,
    RISCV_FP_WIDTH_DOUBLEWORD = 3,
    RISCV_FP32_BYTES = sizeof(uint32_t),
    RISCV_FP64_BYTES = sizeof(uint64_t),
};

/*
 * Table 24 assigns 111 to DYN in an instruction's `rm` field.  The same value
 * is reserved when stored in `frm`, which is why resolving a dynamic mode can
 * still fail after reading fcsr.
 */
enum
{
    RISCV_FP_RM_DYNAMIC = 7,
};

/*
 * These enums name the sub-operation values carried in the instruction field
 * that the generic decoder calls `rm`.  They are not rounding modes for these
 * instruction families.
 */
enum riscv_fp_sign_operation
{
    RISCV_FP_SIGN_COPY = 0,
    RISCV_FP_SIGN_NEGATE = 1,
    RISCV_FP_SIGN_XOR = 2,
};

enum riscv_fp_minmax_operation
{
    RISCV_FP_MINIMUM = 0,
    RISCV_FP_MAXIMUM = 1,
};

enum riscv_fp_compare_operation
{
    RISCV_FP_COMPARE_LESS_OR_EQUAL = 0,
    RISCV_FP_COMPARE_LESS_THAN = 1,
    RISCV_FP_COMPARE_EQUAL = 2,
};

enum riscv_fp_move_class_operation
{
    RISCV_FP_MOVE = 0,
    RISCV_FP_CLASSIFY = 1,
};

/*
 * Conversion instructions overload `rs2` with the integer type.  W and WU are
 * 32-bit signed and unsigned values; L and LU are their RV64-only 64-bit
 * counterparts.  Consequently LU is also the largest legal selector.
 */
enum riscv_fp_integer_type
{
    RISCV_FP_INT_TYPE_W = 0,
    RISCV_FP_INT_TYPE_WU = 1,
    RISCV_FP_INT_TYPE_L = 2,
    RISCV_FP_INT_TYPE_LU = 3,
};

/*
 * OP-FP `funct7` is the concatenation of a five-bit operation and the two-bit
 * `fmt` value.  That construction makes every S/D pair adjacent.  Full names
 * are retained here so dispatch code describes an instruction rather than a
 * hexadecimal value.
 */
enum riscv_fp_funct7
{
    RISCV_FP_FUNCT7_FADD_S = 0x00,
    RISCV_FP_FUNCT7_FADD_D = 0x01,
    RISCV_FP_FUNCT7_FSUB_S = 0x04,
    RISCV_FP_FUNCT7_FSUB_D = 0x05,
    RISCV_FP_FUNCT7_FMUL_S = 0x08,
    RISCV_FP_FUNCT7_FMUL_D = 0x09,
    RISCV_FP_FUNCT7_FDIV_S = 0x0c,
    RISCV_FP_FUNCT7_FDIV_D = 0x0d,
    RISCV_FP_FUNCT7_FSGNJ_S = 0x10,
    RISCV_FP_FUNCT7_FSGNJ_D = 0x11,
    RISCV_FP_FUNCT7_FMIN_MAX_S = 0x14,
    RISCV_FP_FUNCT7_FMIN_MAX_D = 0x15,
    RISCV_FP_FUNCT7_FCVT_S_D = 0x20,
    RISCV_FP_FUNCT7_FCVT_D_S = 0x21,
    RISCV_FP_FUNCT7_FSQRT_S = 0x2c,
    RISCV_FP_FUNCT7_FSQRT_D = 0x2d,
    RISCV_FP_FUNCT7_FCOMPARE_S = 0x50,
    RISCV_FP_FUNCT7_FCOMPARE_D = 0x51,
    RISCV_FP_FUNCT7_FCVT_INT_S = 0x60,
    RISCV_FP_FUNCT7_FCVT_INT_D = 0x61,
    RISCV_FP_FUNCT7_FCVT_S_INT = 0x68,
    RISCV_FP_FUNCT7_FCVT_D_INT = 0x69,
    RISCV_FP_FUNCT7_FMV_X_W_FCLASS_S = 0x70,
    RISCV_FP_FUNCT7_FMV_X_D_FCLASS_D = 0x71,
    RISCV_FP_FUNCT7_FMV_W_X = 0x78,
    RISCV_FP_FUNCT7_FMV_D_X = 0x79,
};

/*
 * Execution and JIT register-cache effects must agree on every recognised
 * OP-FP family.  Decode the architectural funct7 once into both properties so
 * adding an instruction cannot update one independent switch and miss the
 * other.  The all-zero descriptor is deliberately conservative for unknown
 * encodings.
 */
typedef enum
{
    FP_OP_INVALID = 0,
    FP_OP_ADD,
    FP_OP_SUB,
    FP_OP_MUL,
    FP_OP_DIV,
    FP_OP_SIGN,
    FP_OP_MIN_MAX,
    FP_OP_CROSS_PRECISION,
    FP_OP_SQRT,
    FP_OP_COMPARE,
    FP_OP_FLOAT_TO_INTEGER,
    FP_OP_INTEGER_TO_FLOAT,
    FP_OP_MOVE_OR_CLASS,
} fp_op_kind_t;

typedef enum
{
    FP_GPR_ACCESS_UNKNOWN = 0,
    FP_GPR_ACCESS_NONE,
    FP_GPR_ACCESS_READ_RS1,
    FP_GPR_ACCESS_WRITE_RD,
} fp_gpr_access_t;

typedef struct
{
    fp_op_kind_t kind;
    fp_gpr_access_t gpr_access;
    /* Cross-precision conversion uses this as its destination precision. */
    bool is_double;
} fp_op_decode_t;

/* Every decode row must state its operation, GPR access, and precision. */
#define FP_OP_DECODE(kind_value, access_value, double_value) \
    ((fp_op_decode_t){                                      \
        .kind = (kind_value),                               \
        .gpr_access = (access_value),                       \
        .is_double = (double_value),                        \
    })

/*
 * Table 28 defines a one-hot ten-bit FCLASS result.  Naming the destination
 * positions avoids a dense collection of conditional shift counts in the two
 * IEEE-format classifiers.
 */
enum riscv_fp_class_bit
{
    RISCV_FP_CLASS_NEGATIVE_INFINITY = 0,
    RISCV_FP_CLASS_NEGATIVE_NORMAL = 1,
    RISCV_FP_CLASS_NEGATIVE_SUBNORMAL = 2,
    RISCV_FP_CLASS_NEGATIVE_ZERO = 3,
    RISCV_FP_CLASS_POSITIVE_ZERO = 4,
    RISCV_FP_CLASS_POSITIVE_SUBNORMAL = 5,
    RISCV_FP_CLASS_POSITIVE_NORMAL = 6,
    RISCV_FP_CLASS_POSITIVE_INFINITY = 7,
    RISCV_FP_CLASS_SIGNALLING_NAN = 8,
    RISCV_FP_CLASS_QUIET_NAN = 9,
};

/*
 * IEEE-754 binary32 has one sign bit, eight exponent bits, and 23 fraction
 * bits.  An all-one exponent with a zero fraction is infinity; any non-zero
 * fraction is NaN, and the most-significant fraction bit distinguishes a quiet
 * NaN.  RISC-V's canonical NaN therefore combines the positive-infinity
 * encoding with that quiet bit.  In an FLEN=64 register, a valid binary32 value
 * additionally has every upper bit set by the D extension's NaN-boxing rule.
 */
#define RISCV_FP32_SIGN_MASK UINT32_C(0x80000000)
#define RISCV_FP32_MAGNITUDE_MASK UINT32_C(0x7fffffff)
#define RISCV_FP32_EXPONENT_MASK UINT32_C(0x7f800000)
#define RISCV_FP32_FRACTION_MASK UINT32_C(0x007fffff)
#define RISCV_FP32_QUIET_NAN_BIT UINT32_C(0x00400000)
#define RISCV_FP32_POSITIVE_INFINITY RISCV_FP32_EXPONENT_MASK
#define RISCV_FP32_CANONICAL_NAN \
    (RISCV_FP32_POSITIVE_INFINITY | RISCV_FP32_QUIET_NAN_BIT)
#define RISCV_FP32_BOX_MASK UINT64_C(0xffffffff00000000)

/*
 * IEEE-754 binary64 follows the same decision tree with an eleven-bit exponent
 * and a 52-bit fraction.  The quiet bit is fraction bit 51.  No boxing mask is
 * needed because a binary64 value already occupies the complete FLEN=64 FPR.
 */
#define RISCV_FP64_SIGN_MASK UINT64_C(0x8000000000000000)
#define RISCV_FP64_MAGNITUDE_MASK UINT64_C(0x7fffffffffffffff)
#define RISCV_FP64_EXPONENT_MASK UINT64_C(0x7ff0000000000000)
#define RISCV_FP64_FRACTION_MASK UINT64_C(0x000fffffffffffff)
#define RISCV_FP64_QUIET_NAN_BIT UINT64_C(0x0008000000000000)
#define RISCV_FP64_POSITIVE_INFINITY RISCV_FP64_EXPONENT_MASK
#define RISCV_FP64_CANONICAL_NAN \
    (RISCV_FP64_POSITIVE_INFINITY | RISCV_FP64_QUIET_NAN_BIT)

/* Several unary encodings require the otherwise unused `rs2` field to be 0. */
enum
{
    RISCV_FP_RS2_UNUSED = 0,
};

static inline uint32_t fp_opcode(uint32_t inst)
{
    return inst & RISCV_FP_INST_OPCODE_MASK;
}

static inline uint32_t fp_rd(uint32_t inst)
{
    return BITS(inst, RISCV_FP_INST_RD_HI, RISCV_FP_INST_RD_LO);
}

static inline uint32_t fp_rm(uint32_t inst)
{
    return BITS(inst, RISCV_FP_INST_RM_HI, RISCV_FP_INST_RM_LO);
}

static inline uint32_t fp_rs1(uint32_t inst)
{
    return BITS(inst, RISCV_FP_INST_RS1_HI, RISCV_FP_INST_RS1_LO);
}

static inline uint32_t fp_rs2(uint32_t inst)
{
    return BITS(inst, RISCV_FP_INST_RS2_HI, RISCV_FP_INST_RS2_LO);
}

static inline uint32_t fp_funct7(uint32_t inst)
{
    return BITS(inst, RISCV_FP_INST_MSB, RISCV_FP_INST_FUNCT7_LO);
}

static inline uint32_t fp_rs3(uint32_t inst)
{
    return BITS(inst, RISCV_FP_INST_MSB, RISCV_FP_INST_RS3_LO);
}

static inline uint32_t fp_fmt(uint32_t inst)
{
    return BITS(inst, RISCV_FP_INST_FMT_HI, RISCV_FP_INST_FMT_LO);
}

static inline word_t fp_imm_i(uint32_t inst)
{
    return (word_t)SEXT(
        BITS(inst, RISCV_FP_INST_MSB, RISCV_FP_INST_I_IMM_LO),
        RISCV_FP_INST_IMMEDIATE_WIDTH);
}

static inline word_t fp_imm_s(uint32_t inst)
{
    /*
     * An S-immediate is split around the register fields.  Instruction bits
     * [31:25] become immediate bits [11:5], so shifting that seven-bit slice by
     * the five low-immediate bits restores its place before bits [11:7] are
     * ORed in as immediate bits [4:0].  Sign-extending the reconstructed
     * twelve-bit value then produces the XLEN-wide byte offset.
     */
    const uint32_t raw =
        (BITS(inst, RISCV_FP_INST_MSB,
              RISCV_FP_INST_S_IMM_HIGH_LO)
         << RISCV_FP_INST_S_IMM_LOW_WIDTH) |
        BITS(inst, RISCV_FP_INST_S_IMM_LOW_HI,
             RISCV_FP_INST_S_IMM_LOW_LO);
    return (word_t)SEXT(raw, RISCV_FP_INST_IMMEDIATE_WIDTH);
}

/*
 * Floating-point traps must follow the same direct-interpreter contract as
 * integer traps: redirect before writeback and keep Spike DiffTest from
 * stepping a reference instruction after NEMU has already taken the trap.
 */
static void fp_raise_trap(Decode *s, word_t cause, word_t tval)
{
    fp_instruction_trapped = true;
    s->dnpc = isa_raise_intr_tval(cause, s->pc, tval);
    difftest_skip_ref();
}

static void fp_raise_illegal(Decode *s)
{
    fp_raise_trap(s, RISCV_CAUSE_ILLEGAL_INST, 0);
}

static bool fp_require_enabled(Decode *s)
{
    if (!riscv_mstatus_fp_enabled(cpu.csr.mstatus))
    {
        fp_raise_illegal(s);
        return false;
    }

    return true;
}

static inline bool fp_naturally_aligned(word_t address, int length)
{
    /*
     * Supported lengths are powers of two.  Subtracting one therefore creates
     * a mask for exactly the address bits that must be zero: three low bits for
     * an eight-byte FLD/FSD and two for a four-byte FLW/FSW.
     */
    return (address & (word_t)(length - 1)) == 0;
}

static bool fp_check_load_alignment(Decode *s, word_t address, int length)
{
    if (!fp_naturally_aligned(address, length))
    {
        fp_raise_trap(s, RISCV_CAUSE_LOAD_ADDR_MISALIGNED, address);
        return false;
    }

    return true;
}

static bool fp_check_store_alignment(Decode *s, word_t address, int length)
{
    if (!fp_naturally_aligned(address, length))
    {
        fp_raise_trap(s, RISCV_CAUSE_STORE_ADDR_MISALIGNED, address);
        return false;
    }

    return true;
}

#ifdef CONFIG_RISCV_D
/*
 * The generic virtual-memory interface carries one `word_t`.  A single
 * eight-byte transfer therefore preserves all bits only when XLEN is 64.
 * RV32D assembles the little-endian doubleword from two word transfers.  The
 * caller performs the architectural eight-byte alignment check first.
 *
 * Chapter 22.3 guarantees atomic FLD/FSD execution only when XLEN is at least
 * 64, so the two RV32 accesses are a permitted implementation choice.
 */
static uint64_t fp_load_doubleword(word_t address)
{
#ifdef CONFIG_RV64
    return (uint64_t)vaddr_read(address, RISCV_FP64_BYTES);
#else
    const uint64_t low =
        (uint32_t)vaddr_read(address, RISCV_FP32_BYTES);
    const uint64_t high =
        (uint32_t)vaddr_read(address + RISCV_FP32_BYTES,
                             RISCV_FP32_BYTES);

    return low | (high << 32);
#endif
}

static void fp_store_doubleword(word_t address, uint64_t value)
{
#ifdef CONFIG_RV64
    vaddr_write(address, RISCV_FP64_BYTES, value);
#else
    vaddr_write(address, RISCV_FP32_BYTES, (uint32_t)value);
    vaddr_write(address + RISCV_FP32_BYTES,
                RISCV_FP32_BYTES,
                (uint32_t)(value >> 32));
#endif
}
#endif

static inline void fp_mark_dirty(void)
{
    cpu.csr.mstatus = riscv_mstatus_mark_fp_dirty(cpu.csr.mstatus);
}

static inline riscv_fpr_t fp_box_s(uint32_t value)
{
#ifdef CONFIG_RISCV_D
    return RISCV_FP32_BOX_MASK | value;
#else
    /*
     * RV32F has FLEN=32, so the binary32 payload occupies the complete
     * architectural register and no NaN-boxing bits exist.
     */
    return value;
#endif
}

static inline uint32_t fp_unbox_s(riscv_fpr_t value)
{
#ifdef CONFIG_RISCV_D
    /*
     * Checking the upper-half mask directly is the D-extension rule: every bit
     * above the 32-bit payload must be one.  A malformed box is a computational
     * canonical NaN, but raw transfer instructions deliberately bypass this
     * helper and retain their payload bits.
     */
    return (value & RISCV_FP32_BOX_MASK) == RISCV_FP32_BOX_MASK
               ? (uint32_t)value
               : RISCV_FP32_CANONICAL_NAN;
#else
    return value;
#endif
}

static inline bool fp_is_nan_s(uint32_t value)
{
    return (value & RISCV_FP32_MAGNITUDE_MASK) >
           RISCV_FP32_POSITIVE_INFINITY;
}

static inline bool fp_is_nan_d(uint64_t value)
{
    return (value & RISCV_FP64_MAGNITUDE_MASK) >
           RISCV_FP64_POSITIVE_INFINITY;
}

static inline float32_t fp_read_s(uint32_t index)
{
    const float32_t value = {.v = fp_unbox_s(cpu.fpr[index])};
    return value;
}

static inline float64_t fp_read_d(uint32_t index)
{
    const float64_t value = {.v = cpu.fpr[index]};
    return value;
}

static inline void fp_write_s(uint32_t index, float32_t value)
{
    cpu.fpr[index] = fp_box_s(value.v);
    fp_mark_dirty();
}

static inline void fp_write_d(uint32_t index, float64_t value)
{
    cpu.fpr[index] = value.v;
    fp_mark_dirty();
}

/*
 * RISC-V and this SoftFloat specialisation deliberately use the same values
 * for RNE, RTZ, RDN, RUP, and RMM.  Dynamic rm=111 reads frm; every effective
 * value above RMM is rejected before arithmetic state can change.
 */
static bool fp_resolve_rounding_mode(Decode *s, uint32_t encoded,
                                     uint_fast8_t *resolved)
{
    uint32_t rm = encoded;

    if (rm == RISCV_FP_RM_DYNAMIC)
    {
        rm = (cpu.fcsr & RISCV_FRM_MASK) >> RISCV_FRM_SHIFT;
    }

    if (rm > softfloat_round_near_maxMag)
    {
        fp_raise_illegal(s);
        return false;
    }

    *resolved = (uint_fast8_t)rm;
    return true;
}

static void fp_begin_softfloat(uint_fast8_t rounding_mode)
{
    /*
     * SoftFloat keeps host-thread globals.  Each guest instruction owns a fresh
     * scratch flag set; architectural flags are accrued explicitly afterwards.
     */
    softfloat_exceptionFlags = 0;
    softfloat_roundingMode = rounding_mode;
}

static void fp_finish_softfloat(void)
{
    const uint32_t raised =
        (uint32_t)softfloat_exceptionFlags & RISCV_FFLAGS_MASK;

    softfloat_exceptionFlags = 0;

    if (raised != 0)
    {
        cpu.fcsr |= raised;
        fp_mark_dirty();
    }
}

static void fp_exec_load(Decode *s, uint32_t inst)
{
    const uint32_t funct3 = fp_rm(inst);
    const uint32_t rd = fp_rd(inst);
    const word_t address = gpr(fp_rs1(inst)) + fp_imm_i(inst);

    switch (funct3)
    {
    case RISCV_FP_WIDTH_WORD: /* FLW */
        if (fp_check_load_alignment(s, address, RISCV_FP32_BYTES))
        {
            const float32_t value = {
                .v = (uint32_t)vaddr_read(address, RISCV_FP32_BYTES),
            };
            fp_write_s(rd, value);
        }

        return;
#ifdef CONFIG_RISCV_D
    case RISCV_FP_WIDTH_DOUBLEWORD: /* FLD */
        if (fp_check_load_alignment(s, address, RISCV_FP64_BYTES))
        {
            const float64_t value = {
                .v = fp_load_doubleword(address),
            };
            fp_write_d(rd, value);
        }

        return;
#endif
    default:
        fp_raise_illegal(s);
        return;
    }
}

static void fp_exec_store(Decode *s, uint32_t inst)
{
    const uint32_t funct3 = fp_rm(inst);
    const word_t address = gpr(fp_rs1(inst)) + fp_imm_s(inst);
    const riscv_fpr_t value = cpu.fpr[fp_rs2(inst)];

    switch (funct3)
    {
    case RISCV_FP_WIDTH_WORD:
        /* FSW transfers the raw low word without an unboxing check. */
        if (fp_check_store_alignment(s, address, RISCV_FP32_BYTES))
        {
            vaddr_write(address, RISCV_FP32_BYTES, (uint32_t)value);
        }

        return;
#ifdef CONFIG_RISCV_D
    case RISCV_FP_WIDTH_DOUBLEWORD:
        /* FSD transfers every raw bit. */
        if (fp_check_store_alignment(s, address, RISCV_FP64_BYTES))
        {
            fp_store_doubleword(address, (uint64_t)value);
        }

        return;
#endif
    default:
        fp_raise_illegal(s);
        return;
    }
}

static void fp_exec_add(Decode *s, uint32_t inst, bool is_double)
{
    uint_fast8_t rm = 0;

    if (!fp_resolve_rounding_mode(s, fp_rm(inst), &rm))
    {
        return;
    }

    fp_begin_softfloat(rm);

    if (is_double)
    {
        const float64_t result =
            f64_add(fp_read_d(fp_rs1(inst)), fp_read_d(fp_rs2(inst)));
        fp_finish_softfloat();
        fp_write_d(fp_rd(inst), result);
    }
    else
    {
        const float32_t result =
            f32_add(fp_read_s(fp_rs1(inst)), fp_read_s(fp_rs2(inst)));
        fp_finish_softfloat();
        fp_write_s(fp_rd(inst), result);
    }
}

enum fp_binary_operation
{
    FP_BINARY_SUB,
    FP_BINARY_MUL,
    FP_BINARY_DIV,
};

static void fp_exec_binary_arithmetic(Decode *s, uint32_t inst,
                                      bool is_double,
                                      enum fp_binary_operation operation)
{
    uint_fast8_t rm = 0;

    if (!fp_resolve_rounding_mode(s, fp_rm(inst), &rm))
    {
        return;
    }

    fp_begin_softfloat(rm);

    if (is_double)
    {
        const float64_t lhs = fp_read_d(fp_rs1(inst));
        const float64_t rhs = fp_read_d(fp_rs2(inst));
        float64_t result = {.v = 0};

        switch (operation)
        {
        case FP_BINARY_SUB:
            result = f64_sub(lhs, rhs);
            break;
        case FP_BINARY_MUL:
            result = f64_mul(lhs, rhs);
            break;
        case FP_BINARY_DIV:
            result = f64_div(lhs, rhs);
            break;
        }

        fp_finish_softfloat();
        fp_write_d(fp_rd(inst), result);
    }
    else
    {
        const float32_t lhs = fp_read_s(fp_rs1(inst));
        const float32_t rhs = fp_read_s(fp_rs2(inst));
        float32_t result = {.v = 0};

        switch (operation)
        {
        case FP_BINARY_SUB:
            result = f32_sub(lhs, rhs);
            break;
        case FP_BINARY_MUL:
            result = f32_mul(lhs, rhs);
            break;
        case FP_BINARY_DIV:
            result = f32_div(lhs, rhs);
            break;
        }

        fp_finish_softfloat();
        fp_write_s(fp_rd(inst), result);
    }
}

static void fp_exec_sqrt(Decode *s, uint32_t inst, bool is_double)
{
    uint_fast8_t rm = 0;

    if (fp_rs2(inst) != RISCV_FP_RS2_UNUSED ||
        !fp_resolve_rounding_mode(s, fp_rm(inst), &rm))
    {
        if (fp_rs2(inst) != RISCV_FP_RS2_UNUSED)
        {
            fp_raise_illegal(s);
        }

        return;
    }

    fp_begin_softfloat(rm);

    if (is_double)
    {
        const float64_t result = f64_sqrt(fp_read_d(fp_rs1(inst)));
        fp_finish_softfloat();
        fp_write_d(fp_rd(inst), result);
    }
    else
    {
        const float32_t result = f32_sqrt(fp_read_s(fp_rs1(inst)));
        fp_finish_softfloat();
        fp_write_s(fp_rd(inst), result);
    }
}

static void fp_exec_sign_injection(Decode *s, uint32_t inst, bool is_double)
{
    const uint32_t mode = fp_rm(inst);

    if (mode > RISCV_FP_SIGN_XOR)
    {
        fp_raise_illegal(s);
        return;
    }

    if (is_double)
    {
        const uint64_t lhs = fp_read_d(fp_rs1(inst)).v;
        const uint64_t rhs = fp_read_d(fp_rs2(inst)).v;
        uint64_t sign = 0;

        switch (mode)
        {
        case RISCV_FP_SIGN_COPY: /* FSGNJ: copy rs2's sign. */
            sign = rhs & RISCV_FP64_SIGN_MASK;
            break;
        case RISCV_FP_SIGN_NEGATE:
            /* FSGNJN: copy the inverse of rs2's sign. */
            sign = (~rhs) & RISCV_FP64_SIGN_MASK;
            break;
        case RISCV_FP_SIGN_XOR: /* FSGNJX: XOR both source signs. */
            sign = (lhs ^ rhs) & RISCV_FP64_SIGN_MASK;
            break;
        }

        const float64_t result = {
            .v = (lhs & ~RISCV_FP64_SIGN_MASK) | sign,
        };
        fp_write_d(fp_rd(inst), result);
    }
    else
    {
        const uint32_t lhs = fp_read_s(fp_rs1(inst)).v;
        const uint32_t rhs = fp_read_s(fp_rs2(inst)).v;
        uint32_t sign = 0;

        switch (mode)
        {
        case RISCV_FP_SIGN_COPY:
            sign = rhs & RISCV_FP32_SIGN_MASK;
            break;
        case RISCV_FP_SIGN_NEGATE:
            sign = (~rhs) & RISCV_FP32_SIGN_MASK;
            break;
        case RISCV_FP_SIGN_XOR:
            sign = (lhs ^ rhs) & RISCV_FP32_SIGN_MASK;
            break;
        }

        const float32_t result = {
            .v = (lhs & ~RISCV_FP32_SIGN_MASK) | sign,
        };
        fp_write_s(fp_rd(inst), result);
    }
}

static void fp_exec_min_max(Decode *s, uint32_t inst, bool is_double)
{
    const uint32_t mode = fp_rm(inst);

    if (mode > RISCV_FP_MAXIMUM)
    {
        fp_raise_illegal(s);
        return;
    }

    fp_begin_softfloat(softfloat_round_near_even);

    if (is_double)
    {
        const float64_t lhs = fp_read_d(fp_rs1(inst));
        const float64_t rhs = fp_read_d(fp_rs2(inst));
        const bool lhs_nan = fp_is_nan_d(lhs.v);
        const bool rhs_nan = fp_is_nan_d(rhs.v);
        float64_t result = {.v = RISCV_FP64_CANONICAL_NAN};

        if (!(lhs_nan && rhs_nan))
        {
            if (mode == RISCV_FP_MINIMUM)
            {
                const bool less =
                    f64_lt_quiet(lhs, rhs) ||
                    (f64_eq(lhs, rhs) &&
                     (lhs.v & RISCV_FP64_SIGN_MASK));
                result = (less || rhs_nan) ? lhs : rhs;
            }
            else
            {
                const bool greater =
                    f64_lt_quiet(rhs, lhs) ||
                    (f64_eq(rhs, lhs) &&
                     (rhs.v & RISCV_FP64_SIGN_MASK));
                result = (greater || rhs_nan) ? lhs : rhs;
            }
        }
        else
        {
            /*
             * Run a quiet comparison even for two NaNs so a signalling NaN
             * contributes NV before the canonical result is written.
             */
            (void)f64_lt_quiet(lhs, rhs);
        }

        fp_finish_softfloat();
        fp_write_d(fp_rd(inst), result);
    }
    else
    {
        const float32_t lhs = fp_read_s(fp_rs1(inst));
        const float32_t rhs = fp_read_s(fp_rs2(inst));
        const bool lhs_nan = fp_is_nan_s(lhs.v);
        const bool rhs_nan = fp_is_nan_s(rhs.v);
        float32_t result = {.v = RISCV_FP32_CANONICAL_NAN};

        if (!(lhs_nan && rhs_nan))
        {
            if (mode == RISCV_FP_MINIMUM)
            {
                const bool less =
                    f32_lt_quiet(lhs, rhs) ||
                    (f32_eq(lhs, rhs) &&
                     (lhs.v & RISCV_FP32_SIGN_MASK));
                result = (less || rhs_nan) ? lhs : rhs;
            }
            else
            {
                const bool greater =
                    f32_lt_quiet(rhs, lhs) ||
                    (f32_eq(rhs, lhs) &&
                     (rhs.v & RISCV_FP32_SIGN_MASK));
                result = (greater || rhs_nan) ? lhs : rhs;
            }
        }
        else
        {
            (void)f32_lt_quiet(lhs, rhs);
        }

        fp_finish_softfloat();
        fp_write_s(fp_rd(inst), result);
    }
}

static void fp_exec_compare(Decode *s, uint32_t inst, bool is_double)
{
    const uint32_t relation = fp_rm(inst);
    bool result = false;

    if (relation > RISCV_FP_COMPARE_EQUAL)
    {
        fp_raise_illegal(s);
        return;
    }

    fp_begin_softfloat(softfloat_round_near_even);

    if (is_double)
    {
        const float64_t lhs = fp_read_d(fp_rs1(inst));
        const float64_t rhs = fp_read_d(fp_rs2(inst));

        switch (relation)
        {
        case RISCV_FP_COMPARE_LESS_OR_EQUAL:
            result = f64_le(lhs, rhs);
            break;
        case RISCV_FP_COMPARE_LESS_THAN:
            result = f64_lt(lhs, rhs);
            break;
        case RISCV_FP_COMPARE_EQUAL:
            result = f64_eq(lhs, rhs);
            break;
        }
    }
    else
    {
        const float32_t lhs = fp_read_s(fp_rs1(inst));
        const float32_t rhs = fp_read_s(fp_rs2(inst));

        switch (relation)
        {
        case RISCV_FP_COMPARE_LESS_OR_EQUAL:
            result = f32_le(lhs, rhs);
            break;
        case RISCV_FP_COMPARE_LESS_THAN:
            result = f32_lt(lhs, rhs);
            break;
        case RISCV_FP_COMPARE_EQUAL:
            result = f32_eq(lhs, rhs);
            break;
        }
    }

    fp_finish_softfloat();
    gpr(fp_rd(inst)) = result ? 1 : 0;
}

/*
 * FCLASS is an architectural bit inspection, not a SoftFloat operation.  Work
 * directly from the binary32 encoding so that every input selects exactly one
 * result bit and neither the SoftFloat scratch flags nor guest fflags change.
 */
static inline word_t fp_class_mask(enum riscv_fp_class_bit bit)
{
    return (word_t)1u << bit;
}

static word_t fp_classify_s(float32_t value)
{
    const uint32_t bits = value.v;
    const bool sign = (bits & RISCV_FP32_SIGN_MASK) != 0;
    const uint32_t exponent = bits & RISCV_FP32_EXPONENT_MASK;
    const uint32_t fraction = bits & RISCV_FP32_FRACTION_MASK;

    if (exponent == RISCV_FP32_EXPONENT_MASK)
    {
        if (fraction == 0)
        {
            return fp_class_mask(
                sign ? RISCV_FP_CLASS_NEGATIVE_INFINITY
                     : RISCV_FP_CLASS_POSITIVE_INFINITY);
        }

        return fp_class_mask(
            (fraction & RISCV_FP32_QUIET_NAN_BIT)
                ? RISCV_FP_CLASS_QUIET_NAN
                : RISCV_FP_CLASS_SIGNALLING_NAN);
    }

    if (exponent == 0)
    {
        if (fraction == 0)
        {
            return fp_class_mask(
                sign ? RISCV_FP_CLASS_NEGATIVE_ZERO
                     : RISCV_FP_CLASS_POSITIVE_ZERO);
        }

        return fp_class_mask(
            sign ? RISCV_FP_CLASS_NEGATIVE_SUBNORMAL
                 : RISCV_FP_CLASS_POSITIVE_SUBNORMAL);
    }

    return fp_class_mask(
        sign ? RISCV_FP_CLASS_NEGATIVE_NORMAL
             : RISCV_FP_CLASS_POSITIVE_NORMAL);
}

#ifdef CONFIG_RISCV_D
/*
 * The binary64 layout follows the same decision tree. Bit 51 is the NaN quiet
 * bit; a clear bit therefore selects signalling NaN without invoking a helper
 * that might accrue an invalid-operation exception.
 */
static word_t fp_classify_d(float64_t value)
{
    const uint64_t bits = value.v;
    const bool sign = (bits & RISCV_FP64_SIGN_MASK) != 0;
    const uint64_t exponent = bits & RISCV_FP64_EXPONENT_MASK;
    const uint64_t fraction = bits & RISCV_FP64_FRACTION_MASK;

    if (exponent == RISCV_FP64_EXPONENT_MASK)
    {
        if (fraction == 0)
        {
            return fp_class_mask(
                sign ? RISCV_FP_CLASS_NEGATIVE_INFINITY
                     : RISCV_FP_CLASS_POSITIVE_INFINITY);
        }

        return fp_class_mask(
            (fraction & RISCV_FP64_QUIET_NAN_BIT)
                ? RISCV_FP_CLASS_QUIET_NAN
                : RISCV_FP_CLASS_SIGNALLING_NAN);
    }

    if (exponent == 0)
    {
        if (fraction == 0)
        {
            return fp_class_mask(
                sign ? RISCV_FP_CLASS_NEGATIVE_ZERO
                     : RISCV_FP_CLASS_POSITIVE_ZERO);
        }

        return fp_class_mask(
            sign ? RISCV_FP_CLASS_NEGATIVE_SUBNORMAL
                 : RISCV_FP_CLASS_POSITIVE_SUBNORMAL);
    }

    return fp_class_mask(
        sign ? RISCV_FP_CLASS_NEGATIVE_NORMAL
             : RISCV_FP_CLASS_POSITIVE_NORMAL);
}
#endif

static void fp_exec_move_or_class(Decode *s, uint32_t inst)
{
    const uint32_t funct7 = fp_funct7(inst);
    const uint32_t funct3 = fp_rm(inst);
    const uint32_t rs2 = fp_rs2(inst);
    const uint32_t rd = fp_rd(inst);
    const uint32_t rs1 = fp_rs1(inst);

    if (rs2 != RISCV_FP_RS2_UNUSED)
    {
        fp_raise_illegal(s);
        return;
    }

    switch (funct7)
    {
    case RISCV_FP_FUNCT7_FMV_X_W_FCLASS_S:
        /*
         * FMV.X.W returns the complete raw RV32 word. RV64 observes the same
         * low bits sign-extended to XLEN, as required by the F extension.
         */
        if (funct3 == RISCV_FP_MOVE)
        {
            gpr(rd) = (word_t)(int64_t)(int32_t)cpu.fpr[rs1];
        }
        else if (funct3 == RISCV_FP_CLASSIFY)
        {
            /* FCLASS.S uses a computational unbox. */
            gpr(rd) = fp_classify_s(fp_read_s(rs1));
        }
        else
        {
            fp_raise_illegal(s);
        }

        return;
#ifdef CONFIG_RISCV_D
    case RISCV_FP_FUNCT7_FMV_X_D_FCLASS_D:
        if (funct3 == RISCV_FP_MOVE)
        {
#ifdef CONFIG_RV64
            /* A whole-double GPR move exists only when XLEN is at least 64. */
            gpr(rd) = cpu.fpr[rs1];
#else
            fp_raise_illegal(s);
#endif
        }
        else if (funct3 == RISCV_FP_CLASSIFY)
        {
            gpr(rd) = fp_classify_d(fp_read_d(rs1));
        }
        else
        {
            fp_raise_illegal(s);
        }

        return;
#endif
    case RISCV_FP_FUNCT7_FMV_W_X:
        /* FMV.W.X writes the raw low integer word, boxing only when FLEN=64. */
        {
            if (funct3 != RISCV_FP_MOVE)
            {
                fp_raise_illegal(s);
                return;
            }

            const float32_t value = {.v = (uint32_t)gpr(rs1)};
            fp_write_s(rd, value);
            return;
        }
#if defined(CONFIG_RISCV_D) && defined(CONFIG_RV64)
    case RISCV_FP_FUNCT7_FMV_D_X:
        /* FMV.D.X transfers all XLEN bits. */
        {
            if (funct3 != RISCV_FP_MOVE)
            {
                fp_raise_illegal(s);
                return;
            }

            const float64_t value = {.v = (uint64_t)gpr(rs1)};
            fp_write_d(rd, value);
            return;
        }
#endif
    default:
        fp_raise_illegal(s);
        return;
    }
}

static inline word_t fp_sign_extend_word(uint32_t value)
{
    return (word_t)(int64_t)(int32_t)value;
}

static void fp_exec_float_to_integer(Decode *s, uint32_t inst,
                                     bool is_double)
{
    uint_fast8_t rm = 0;
    word_t result = 0;
    const uint32_t destination_type = fp_rs2(inst);
    const uint32_t maximum_type =
        MUXDEF(CONFIG_RV64,
               RISCV_FP_INT_TYPE_LU,
               RISCV_FP_INT_TYPE_WU);

    if (destination_type > maximum_type ||
        !fp_resolve_rounding_mode(s, fp_rm(inst), &rm))
    {
        if (destination_type > maximum_type)
        {
            fp_raise_illegal(s);
        }

        return;
    }

    fp_begin_softfloat(rm);

    if (is_double)
    {
        const float64_t source = fp_read_d(fp_rs1(inst));

        switch (destination_type)
        {
        case RISCV_FP_INT_TYPE_W:
            result = fp_sign_extend_word(
                (uint32_t)f64_to_i32(source, rm, true));
            break;
        case RISCV_FP_INT_TYPE_WU:
            /*
             * RV64 sign-extends both W and WU conversion results from bit 31,
             * even though the WU operation itself produces an unsigned word.
             */
            result = fp_sign_extend_word(
                (uint32_t)f64_to_ui32(source, rm, true));
            break;
        case RISCV_FP_INT_TYPE_L:
            result = (word_t)f64_to_i64(source, rm, true);
            break;
        case RISCV_FP_INT_TYPE_LU:
            result = (word_t)f64_to_ui64(source, rm, true);
            break;
        }
    }
    else
    {
        const float32_t source = fp_read_s(fp_rs1(inst));

        switch (destination_type)
        {
        case RISCV_FP_INT_TYPE_W:
            result = fp_sign_extend_word(
                (uint32_t)f32_to_i32(source, rm, true));
            break;
        case RISCV_FP_INT_TYPE_WU:
            result = fp_sign_extend_word(
                (uint32_t)f32_to_ui32(source, rm, true));
            break;
        case RISCV_FP_INT_TYPE_L:
            result = (word_t)f32_to_i64(source, rm, true);
            break;
        case RISCV_FP_INT_TYPE_LU:
            result = (word_t)f32_to_ui64(source, rm, true);
            break;
        }
    }

    fp_finish_softfloat();
    gpr(fp_rd(inst)) = result;
}

static void fp_exec_integer_to_float(Decode *s, uint32_t inst,
                                     bool is_double)
{
    uint_fast8_t rm = 0;
    const uint32_t source_type = fp_rs2(inst);
    const word_t source = gpr(fp_rs1(inst));
    const uint32_t maximum_type =
        MUXDEF(CONFIG_RV64,
               RISCV_FP_INT_TYPE_LU,
               RISCV_FP_INT_TYPE_WU);

    if (source_type > maximum_type ||
        !fp_resolve_rounding_mode(s, fp_rm(inst), &rm))
    {
        if (source_type > maximum_type)
        {
            fp_raise_illegal(s);
        }

        return;
    }

    fp_begin_softfloat(rm);

    if (is_double)
    {
        float64_t result = {.v = 0};

        switch (source_type)
        {
        case RISCV_FP_INT_TYPE_W:
            result = i32_to_f64((int32_t)source);
            break;
        case RISCV_FP_INT_TYPE_WU:
            result = ui32_to_f64((uint32_t)source);
            break;
        case RISCV_FP_INT_TYPE_L:
            result = i64_to_f64((int64_t)source);
            break;
        case RISCV_FP_INT_TYPE_LU:
            result = ui64_to_f64((uint64_t)source);
            break;
        }

        fp_finish_softfloat();
        fp_write_d(fp_rd(inst), result);
    }
    else
    {
        float32_t result = {.v = 0};

        switch (source_type)
        {
        case RISCV_FP_INT_TYPE_W:
            result = i32_to_f32((int32_t)source);
            break;
        case RISCV_FP_INT_TYPE_WU:
            result = ui32_to_f32((uint32_t)source);
            break;
        case RISCV_FP_INT_TYPE_L:
            result = i64_to_f32((int64_t)source);
            break;
        case RISCV_FP_INT_TYPE_LU:
            result = ui64_to_f32((uint64_t)source);
            break;
        }

        fp_finish_softfloat();
        fp_write_s(fp_rd(inst), result);
    }
}

#ifdef CONFIG_RISCV_D
static void fp_exec_cross_precision(Decode *s, uint32_t inst,
                                    bool destination_double)
{
    uint_fast8_t rm = 0;
    /*
     * For FCVT.fmt.fmt, `fmt` names the destination precision and `rs2`
     * names the source precision.  The source must therefore be S when the
     * destination is D, and D when the destination is S.
     */
    const uint32_t required_rs2 =
        destination_double ? RISCV_FP_FORMAT_SINGLE
                           : RISCV_FP_FORMAT_DOUBLE;

    if (fp_rs2(inst) != required_rs2 ||
        !fp_resolve_rounding_mode(s, fp_rm(inst), &rm))
    {
        if (fp_rs2(inst) != required_rs2)
        {
            fp_raise_illegal(s);
        }

        return;
    }

    fp_begin_softfloat(rm);

    if (destination_double)
    {
        const float64_t result = f32_to_f64(fp_read_s(fp_rs1(inst)));
        fp_finish_softfloat();
        fp_write_d(fp_rd(inst), result);
    }
    else
    {
        const float32_t result = f64_to_f32(fp_read_d(fp_rs1(inst)));
        fp_finish_softfloat();
        fp_write_s(fp_rd(inst), result);
    }
}
#endif

static void fp_exec_fused(Decode *s, uint32_t inst)
{
    uint_fast8_t rm = 0;
    const uint32_t format = fp_fmt(inst);
    const uint32_t opcode = fp_opcode(inst);
    /*
     * SoftFloat exposes one fused `lhs * rhs + addend` primitive.  RISC-V's
     * other three mnemonics are obtained by flipping signs before that single
     * rounding step: FNMSUB/FNMADD negate the product, while FMSUB/FNMADD
     * negate the addend.  Changing signs as raw bits is exact and preserves the
     * fused operation; performing separate multiply/subtract calls would round
     * twice and give observably different edge-case results.
     */
    const bool negate_product =
        opcode == RISCV_FP_OPCODE_FNMSUB ||
        opcode == RISCV_FP_OPCODE_FNMADD;
    const bool negate_addend =
        opcode == RISCV_FP_OPCODE_FMSUB ||
        opcode == RISCV_FP_OPCODE_FNMADD;
    const uint32_t maximum_format =
        MUXDEF(CONFIG_RISCV_D,
               RISCV_FP_FORMAT_DOUBLE,
               RISCV_FP_FORMAT_SINGLE);

    if (format > maximum_format ||
        !fp_resolve_rounding_mode(s, fp_rm(inst), &rm))
    {
        if (format > maximum_format)
        {
            fp_raise_illegal(s);
        }

        return;
    }

    fp_begin_softfloat(rm);

    if (format == RISCV_FP_FORMAT_DOUBLE)
    {
        float64_t lhs = fp_read_d(fp_rs1(inst));
        const float64_t rhs = fp_read_d(fp_rs2(inst));
        float64_t addend = fp_read_d(fp_rs3(inst));

        if (negate_product)
        {
            lhs.v ^= RISCV_FP64_SIGN_MASK;
        }

        if (negate_addend)
        {
            addend.v ^= RISCV_FP64_SIGN_MASK;
        }

        const float64_t result = f64_mulAdd(lhs, rhs, addend);
        fp_finish_softfloat();
        fp_write_d(fp_rd(inst), result);
    }
    else
    {
        float32_t lhs = fp_read_s(fp_rs1(inst));
        const float32_t rhs = fp_read_s(fp_rs2(inst));
        float32_t addend = fp_read_s(fp_rs3(inst));

        if (negate_product)
        {
            lhs.v ^= RISCV_FP32_SIGN_MASK;
        }

        if (negate_addend)
        {
            addend.v ^= RISCV_FP32_SIGN_MASK;
        }

        const float32_t result = f32_mulAdd(lhs, rhs, addend);
        fp_finish_softfloat();
        fp_write_s(fp_rd(inst), result);
    }
}

static inline fp_op_decode_t fp_decode_op(uint32_t inst)
{
    switch (fp_funct7(inst))
    {
    case RISCV_FP_FUNCT7_FADD_S:
        return FP_OP_DECODE(FP_OP_ADD, FP_GPR_ACCESS_NONE, false);
#ifdef CONFIG_RISCV_D
    case RISCV_FP_FUNCT7_FADD_D:
        return FP_OP_DECODE(FP_OP_ADD, FP_GPR_ACCESS_NONE, true);
#endif
    case RISCV_FP_FUNCT7_FSUB_S:
        return FP_OP_DECODE(FP_OP_SUB, FP_GPR_ACCESS_NONE, false);
#ifdef CONFIG_RISCV_D
    case RISCV_FP_FUNCT7_FSUB_D:
        return FP_OP_DECODE(FP_OP_SUB, FP_GPR_ACCESS_NONE, true);
#endif
    case RISCV_FP_FUNCT7_FMUL_S:
        return FP_OP_DECODE(FP_OP_MUL, FP_GPR_ACCESS_NONE, false);
#ifdef CONFIG_RISCV_D
    case RISCV_FP_FUNCT7_FMUL_D:
        return FP_OP_DECODE(FP_OP_MUL, FP_GPR_ACCESS_NONE, true);
#endif
    case RISCV_FP_FUNCT7_FDIV_S:
        return FP_OP_DECODE(FP_OP_DIV, FP_GPR_ACCESS_NONE, false);
#ifdef CONFIG_RISCV_D
    case RISCV_FP_FUNCT7_FDIV_D:
        return FP_OP_DECODE(FP_OP_DIV, FP_GPR_ACCESS_NONE, true);
#endif
    case RISCV_FP_FUNCT7_FSGNJ_S:
        return FP_OP_DECODE(FP_OP_SIGN, FP_GPR_ACCESS_NONE, false);
#ifdef CONFIG_RISCV_D
    case RISCV_FP_FUNCT7_FSGNJ_D:
        return FP_OP_DECODE(FP_OP_SIGN, FP_GPR_ACCESS_NONE, true);
#endif
    case RISCV_FP_FUNCT7_FMIN_MAX_S:
        return FP_OP_DECODE(FP_OP_MIN_MAX, FP_GPR_ACCESS_NONE, false);
#ifdef CONFIG_RISCV_D
    case RISCV_FP_FUNCT7_FMIN_MAX_D:
        return FP_OP_DECODE(FP_OP_MIN_MAX, FP_GPR_ACCESS_NONE, true);
    case RISCV_FP_FUNCT7_FCVT_S_D:
        return FP_OP_DECODE(
            FP_OP_CROSS_PRECISION, FP_GPR_ACCESS_NONE, false);
    case RISCV_FP_FUNCT7_FCVT_D_S:
        return FP_OP_DECODE(
            FP_OP_CROSS_PRECISION, FP_GPR_ACCESS_NONE, true);
#endif
    case RISCV_FP_FUNCT7_FSQRT_S:
        return FP_OP_DECODE(FP_OP_SQRT, FP_GPR_ACCESS_NONE, false);
#ifdef CONFIG_RISCV_D
    case RISCV_FP_FUNCT7_FSQRT_D:
        return FP_OP_DECODE(FP_OP_SQRT, FP_GPR_ACCESS_NONE, true);
#endif
    case RISCV_FP_FUNCT7_FCOMPARE_S:
        return FP_OP_DECODE(
            FP_OP_COMPARE, FP_GPR_ACCESS_WRITE_RD, false);
#ifdef CONFIG_RISCV_D
    case RISCV_FP_FUNCT7_FCOMPARE_D:
        return FP_OP_DECODE(
            FP_OP_COMPARE, FP_GPR_ACCESS_WRITE_RD, true);
#endif
    case RISCV_FP_FUNCT7_FCVT_INT_S:
        return FP_OP_DECODE(
            FP_OP_FLOAT_TO_INTEGER, FP_GPR_ACCESS_WRITE_RD, false);
#ifdef CONFIG_RISCV_D
    case RISCV_FP_FUNCT7_FCVT_INT_D:
        return FP_OP_DECODE(
            FP_OP_FLOAT_TO_INTEGER, FP_GPR_ACCESS_WRITE_RD, true);
#endif
    case RISCV_FP_FUNCT7_FCVT_S_INT:
        return FP_OP_DECODE(
            FP_OP_INTEGER_TO_FLOAT, FP_GPR_ACCESS_READ_RS1, false);
#ifdef CONFIG_RISCV_D
    case RISCV_FP_FUNCT7_FCVT_D_INT:
        return FP_OP_DECODE(
            FP_OP_INTEGER_TO_FLOAT, FP_GPR_ACCESS_READ_RS1, true);
#endif
    case RISCV_FP_FUNCT7_FMV_X_W_FCLASS_S:
        return FP_OP_DECODE(
            FP_OP_MOVE_OR_CLASS, FP_GPR_ACCESS_WRITE_RD, false);
    case RISCV_FP_FUNCT7_FMV_W_X:
        return FP_OP_DECODE(
            FP_OP_MOVE_OR_CLASS, FP_GPR_ACCESS_READ_RS1, false);
#ifdef CONFIG_RISCV_D
    case RISCV_FP_FUNCT7_FMV_X_D_FCLASS_D:
        return FP_OP_DECODE(
            FP_OP_MOVE_OR_CLASS, FP_GPR_ACCESS_WRITE_RD, true);
#ifdef CONFIG_RV64
    case RISCV_FP_FUNCT7_FMV_D_X:
        return FP_OP_DECODE(
            FP_OP_MOVE_OR_CLASS, FP_GPR_ACCESS_READ_RS1, true);
#endif
#endif
    default:
        return (fp_op_decode_t){0};
    }
}

static void fp_exec_op(Decode *s, uint32_t inst)
{
    const fp_op_decode_t decoded = fp_decode_op(inst);

    switch (decoded.kind)
    {
    case FP_OP_ADD:
        fp_exec_add(s, inst, decoded.is_double);
        return;
    case FP_OP_SUB:
        fp_exec_binary_arithmetic(
            s, inst, decoded.is_double, FP_BINARY_SUB);
        return;
    case FP_OP_MUL:
        fp_exec_binary_arithmetic(
            s, inst, decoded.is_double, FP_BINARY_MUL);
        return;
    case FP_OP_DIV:
        fp_exec_binary_arithmetic(
            s, inst, decoded.is_double, FP_BINARY_DIV);
        return;
    case FP_OP_SIGN:
        fp_exec_sign_injection(s, inst, decoded.is_double);
        return;
    case FP_OP_MIN_MAX:
        fp_exec_min_max(s, inst, decoded.is_double);
        return;
#ifdef CONFIG_RISCV_D
    case FP_OP_CROSS_PRECISION:
        fp_exec_cross_precision(s, inst, decoded.is_double);
        return;
#endif
    case FP_OP_SQRT:
        fp_exec_sqrt(s, inst, decoded.is_double);
        return;
    case FP_OP_COMPARE:
        fp_exec_compare(s, inst, decoded.is_double);
        return;
    case FP_OP_FLOAT_TO_INTEGER:
        fp_exec_float_to_integer(s, inst, decoded.is_double);
        return;
    case FP_OP_INTEGER_TO_FLOAT:
        fp_exec_integer_to_float(s, inst, decoded.is_double);
        return;
    case FP_OP_MOVE_OR_CLASS:
        fp_exec_move_or_class(s, inst);
        return;
    case FP_OP_INVALID:
    default:
        fp_raise_illegal(s);
        return;
    }
}

static bool fp_opcode_is_fused(uint32_t opcode)
{
    switch (opcode)
    {
    case RISCV_FP_OPCODE_FMADD:
    case RISCV_FP_OPCODE_FMSUB:
    case RISCV_FP_OPCODE_FNMSUB:
    case RISCV_FP_OPCODE_FNMADD:
        return true;
    default:
        return false;
    }
}

static void fp_exec_dispatch(Decode *s)
{
    const uint32_t inst = s->isa.inst;
    const uint32_t opcode = fp_opcode(inst);

    if (fp_opcode_is_fused(opcode))
    {
        fp_exec_fused(s, inst);
        return;
    }

    switch (opcode)
    {
    case RISCV_FP_OPCODE_LOAD:
        fp_exec_load(s, inst);
        return;
    case RISCV_FP_OPCODE_STORE:
        fp_exec_store(s, inst);
        return;
    case RISCV_FP_OPCODE_OP:
        fp_exec_op(s, inst);
        return;
    default:
        fp_raise_illegal(s);
        return;
    }
}

riscv_fpu_exec_result_t riscv_fpu_exec(Decode *s)
{
    fp_instruction_trapped = false;

    if (fp_require_enabled(s))
    {
        fp_exec_dispatch(s);
    }

    return fp_instruction_trapped ? RISCV_FPU_EXEC_TRAP : RISCV_FPU_EXEC_OK;
}

uint32_t riscv_fpu_jit_exec(uint32_t instr, vaddr_t pc)
{
    /*
     * The native block has already fetched the instruction, so construct only
     * the execution state that the shared FPU path observes.  Initialising
     * dnpc to the sequential address is important: successful FP instructions
     * do not otherwise write it, while trap helpers replace it explicitly.
     */
    Decode s = {
        .pc = pc,
        .snpc = pc + RISCV_BASE_INSN_BYTES,
        .dnpc = pc + RISCV_BASE_INSN_BYTES,
    };
    s.isa.inst = instr;

    cpu.pc = pc;
    const riscv_fpu_exec_result_t result = riscv_fpu_exec(&s);

    /*
     * Some FP moves and conversions name an integer destination.  The direct
     * interpreter repairs x0 after every decoded instruction; helper execution
     * must preserve that architectural invariant itself.
     */
    gpr(RISCV_GPR_ZERO) = 0;
    cpu.pc = s.dnpc;
    return result == RISCV_FPU_EXEC_OK;
}

/* Return a mask bit only for a mutable architectural integer register. */
static uint32_t fp_gpr_mask(uint32_t reg)
{
    return reg == RISCV_GPR_ZERO ? 0 : UINT32_C(1) << reg;
}

/*
 * Classify every integer-register observation made by the shared FP helper.
 *
 * The shared OP-FP descriptor above is also consumed by execution, so its
 * broad family and register effect cannot drift into independent funct7 lists.
 * Fine sub-encoding validation remains in each executor: a malformed
 * instruction may trap, but its broad family still conservatively describes
 * every GPR read that can occur before validation. In particular,
 * integer-to-FP conversion reads rs1 before validating its type selector and
 * rounding mode.
 *
 * Recognised non-memory helpers do not modify a GPR on a trap. A JIT may
 * therefore defer unrelated dirty stores to its terminal trap stub. The zero
 * descriptor keeps unknown instructions on the full barrier/reset path.
 */
riscv_fpu_gpr_effect_t riscv_fpu_gpr_effect(uint32_t inst)
{
    const riscv_fpu_gpr_effect_t preserve = {
        .precise = true,
        .trap_preserves_gprs = true,
    };
    const uint32_t opcode = fp_opcode(inst);

    if (fp_opcode_is_fused(opcode))
    {
        return preserve;
    }

    if (opcode != RISCV_FP_OPCODE_OP)
    {
        return (riscv_fpu_gpr_effect_t){0};
    }

    const fp_op_decode_t decoded = fp_decode_op(inst);
    if (decoded.kind == FP_OP_INVALID ||
        decoded.gpr_access == FP_GPR_ACCESS_UNKNOWN)
    {
        return (riscv_fpu_gpr_effect_t){0};
    }

    switch (decoded.gpr_access)
    {
    case FP_GPR_ACCESS_WRITE_RD:
        return (riscv_fpu_gpr_effect_t){
            .success_write_mask = fp_gpr_mask(fp_rd(inst)),
            .precise = true,
            .trap_preserves_gprs = true,
        };
    case FP_GPR_ACCESS_READ_RS1:
        return (riscv_fpu_gpr_effect_t){
            .read_mask = fp_gpr_mask(fp_rs1(inst)),
            .precise = true,
            .trap_preserves_gprs = true,
        };
    case FP_GPR_ACCESS_NONE:
        return preserve;
    case FP_GPR_ACCESS_UNKNOWN:
    default:
        return (riscv_fpu_gpr_effect_t){0};
    }
}

#endif
