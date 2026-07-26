#include "local-include/fpu.h"

#ifdef CONFIG_RV64_FPU

#include "local-include/reg.h"
#include <cpu/difftest.h>
#include <memory/vaddr.h>
#include <softfloat.h>

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

#define RISCV_FP32_CANONICAL_NAN UINT32_C(0x7fc00000)
#define RISCV_FP64_CANONICAL_NAN UINT64_C(0x7ff8000000000000)
#define RISCV_FP32_BOX_MASK UINT64_C(0xffffffff00000000)
#define RISCV_FP32_SIGN UINT32_C(0x80000000)
#define RISCV_FP64_SIGN UINT64_C(0x8000000000000000)

static inline uint32_t fp_opcode(uint32_t inst)
{
    return inst & 0x7fu;
}

static inline uint32_t fp_rd(uint32_t inst)
{
    return BITS(inst, 11, 7);
}

static inline uint32_t fp_rm(uint32_t inst)
{
    return BITS(inst, 14, 12);
}

static inline uint32_t fp_rs1(uint32_t inst)
{
    return BITS(inst, 19, 15);
}

static inline uint32_t fp_rs2(uint32_t inst)
{
    return BITS(inst, 24, 20);
}

static inline uint32_t fp_funct7(uint32_t inst)
{
    return BITS(inst, 31, 25);
}

static inline uint32_t fp_rs3(uint32_t inst)
{
    return BITS(inst, 31, 27);
}

static inline uint32_t fp_fmt(uint32_t inst)
{
    return BITS(inst, 26, 25);
}

static inline word_t fp_imm_i(uint32_t inst)
{
    return (word_t)SEXT(BITS(inst, 31, 20), 12);
}

static inline word_t fp_imm_s(uint32_t inst)
{
    const uint32_t raw =
        (BITS(inst, 31, 25) << 5) | BITS(inst, 11, 7);
    return (word_t)SEXT(raw, 12);
}

/*
 * Floating-point traps must follow the same direct-interpreter contract as
 * integer traps: redirect before writeback and keep Spike DiffTest from
 * stepping a reference instruction after NEMU has already taken the trap.
 */
static void fp_raise_trap(Decode *s, word_t cause, word_t tval)
{
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

static inline void fp_mark_dirty(void)
{
    cpu.csr.mstatus = riscv_mstatus_mark_fp_dirty(cpu.csr.mstatus);
}

static inline uint64_t fp_box_s(uint32_t value)
{
    return RISCV_FP32_BOX_MASK | value;
}

static inline uint32_t fp_unbox_s(uint64_t value)
{
    return (value >> 32) == UINT32_MAX
               ? (uint32_t)value
               : RISCV_FP32_CANONICAL_NAN;
}

static inline bool fp_is_nan_s(uint32_t value)
{
    return (value & UINT32_C(0x7fffffff)) > UINT32_C(0x7f800000);
}

static inline bool fp_is_nan_d(uint64_t value)
{
    return (value & UINT64_C(0x7fffffffffffffff)) >
           UINT64_C(0x7ff0000000000000);
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
 * value above four is rejected before arithmetic state can change.
 */
static bool fp_resolve_rounding_mode(Decode *s, uint32_t encoded,
                                     uint_fast8_t *resolved)
{
    uint32_t rm = encoded;

    if (rm == 7)
    {
        rm = (cpu.fcsr & RISCV_FRM_MASK) >> 5;
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
    case 2: /* FLW */
        if (fp_check_load_alignment(s, address, 4))
        {
            const float32_t value = {.v = (uint32_t)vaddr_read(address, 4)};
            fp_write_s(rd, value);
        }
        return;
    case 3: /* FLD */
        if (fp_check_load_alignment(s, address, 8))
        {
            const float64_t value = {.v = (uint64_t)vaddr_read(address, 8)};
            fp_write_d(rd, value);
        }
        return;
    default:
        fp_raise_illegal(s);
        return;
    }
}

static void fp_exec_store(Decode *s, uint32_t inst)
{
    const uint32_t funct3 = fp_rm(inst);
    const word_t address = gpr(fp_rs1(inst)) + fp_imm_s(inst);
    const uint64_t value = cpu.fpr[fp_rs2(inst)];

    switch (funct3)
    {
    case 2: /* FSW transfers the raw low word without an unboxing check. */
        if (fp_check_store_alignment(s, address, 4))
        {
            vaddr_write(address, 4, (uint32_t)value);
        }
        return;
    case 3: /* FSD transfers every raw bit. */
        if (fp_check_store_alignment(s, address, 8))
        {
            vaddr_write(address, 8, value);
        }
        return;
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

    if (fp_rs2(inst) != 0 ||
        !fp_resolve_rounding_mode(s, fp_rm(inst), &rm))
    {
        if (fp_rs2(inst) != 0)
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

    if (mode > 2)
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
        case 0: /* FSGNJ: copy rs2's sign. */
            sign = rhs & RISCV_FP64_SIGN;
            break;
        case 1: /* FSGNJN: copy the inverse of rs2's sign. */
            sign = (~rhs) & RISCV_FP64_SIGN;
            break;
        case 2: /* FSGNJX: XOR both source signs. */
            sign = (lhs ^ rhs) & RISCV_FP64_SIGN;
            break;
        }

        const float64_t result = {
            .v = (lhs & ~RISCV_FP64_SIGN) | sign,
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
        case 0:
            sign = rhs & RISCV_FP32_SIGN;
            break;
        case 1:
            sign = (~rhs) & RISCV_FP32_SIGN;
            break;
        case 2:
            sign = (lhs ^ rhs) & RISCV_FP32_SIGN;
            break;
        }

        const float32_t result = {
            .v = (lhs & ~RISCV_FP32_SIGN) | sign,
        };
        fp_write_s(fp_rd(inst), result);
    }
}

static void fp_exec_min_max(Decode *s, uint32_t inst, bool is_double)
{
    const uint32_t mode = fp_rm(inst);

    if (mode > 1)
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
            if (mode == 0)
            {
                const bool less =
                    f64_lt_quiet(lhs, rhs) ||
                    (f64_eq(lhs, rhs) && (lhs.v & RISCV_FP64_SIGN));
                result = (less || rhs_nan) ? lhs : rhs;
            }
            else
            {
                const bool greater =
                    f64_lt_quiet(rhs, lhs) ||
                    (f64_eq(rhs, lhs) && (rhs.v & RISCV_FP64_SIGN));
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
            if (mode == 0)
            {
                const bool less =
                    f32_lt_quiet(lhs, rhs) ||
                    (f32_eq(lhs, rhs) && (lhs.v & RISCV_FP32_SIGN));
                result = (less || rhs_nan) ? lhs : rhs;
            }
            else
            {
                const bool greater =
                    f32_lt_quiet(rhs, lhs) ||
                    (f32_eq(rhs, lhs) && (rhs.v & RISCV_FP32_SIGN));
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

    if (relation > 2)
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
        case 0:
            result = f64_le(lhs, rhs);
            break;
        case 1:
            result = f64_lt(lhs, rhs);
            break;
        case 2:
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
        case 0:
            result = f32_le(lhs, rhs);
            break;
        case 1:
            result = f32_lt(lhs, rhs);
            break;
        case 2:
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
static word_t fp_classify_s(float32_t value)
{
    const uint32_t bits = value.v;
    const bool sign = (bits >> 31) != 0;
    const uint32_t exponent = (bits >> 23) & UINT32_C(0xff);
    const uint32_t fraction = bits & UINT32_C(0x007fffff);

    if (exponent == UINT32_C(0xff))
    {
        if (fraction == 0)
        {
            return (word_t)1u << (sign ? 0 : 7);
        }
        return (word_t)1u <<
               ((fraction & UINT32_C(0x00400000)) ? 9 : 8);
    }
    if (exponent == 0)
    {
        if (fraction == 0)
        {
            return (word_t)1u << (sign ? 3 : 4);
        }
        return (word_t)1u << (sign ? 2 : 5);
    }
    return (word_t)1u << (sign ? 1 : 6);
}

/*
 * The binary64 layout follows the same decision tree.  Bit 51 is the NaN
 * quiet bit; a clear bit therefore selects signalling NaN without invoking a
 * helper that might accrue an invalid-operation exception.
 */
static word_t fp_classify_d(float64_t value)
{
    const uint64_t bits = value.v;
    const bool sign = (bits >> 63) != 0;
    const uint64_t exponent = (bits >> 52) & UINT64_C(0x7ff);
    const uint64_t fraction = bits & UINT64_C(0x000fffffffffffff);

    if (exponent == UINT64_C(0x7ff))
    {
        if (fraction == 0)
        {
            return (word_t)1u << (sign ? 0 : 7);
        }
        return (word_t)1u <<
               ((fraction & UINT64_C(0x0008000000000000)) ? 9 : 8);
    }
    if (exponent == 0)
    {
        if (fraction == 0)
        {
            return (word_t)1u << (sign ? 3 : 4);
        }
        return (word_t)1u << (sign ? 2 : 5);
    }
    return (word_t)1u << (sign ? 1 : 6);
}

static void fp_exec_move_or_class(Decode *s, uint32_t inst)
{
    const uint32_t funct7 = fp_funct7(inst);
    const uint32_t funct3 = fp_rm(inst);
    const uint32_t rs2 = fp_rs2(inst);
    const uint32_t rd = fp_rd(inst);
    const uint32_t rs1 = fp_rs1(inst);

    if (rs2 != 0)
    {
        fp_raise_illegal(s);
        return;
    }

    switch (funct7)
    {
    case 0x70: /* FMV.X.W sign-extends the raw low word. */
        if (funct3 == 0)
        {
            gpr(rd) = (word_t)(int64_t)(int32_t)cpu.fpr[rs1];
        }
        else if (funct3 == 1) /* FCLASS.S uses a computational unbox. */
        {
            gpr(rd) = fp_classify_s(fp_read_s(rs1));
        }
        else
        {
            fp_raise_illegal(s);
        }
        return;
    case 0x71: /* FMV.X.D is a raw RV64 transfer. */
        if (funct3 == 0)
        {
            gpr(rd) = cpu.fpr[rs1];
        }
        else if (funct3 == 1)
        {
            gpr(rd) = fp_classify_d(fp_read_d(rs1));
        }
        else
        {
            fp_raise_illegal(s);
        }
        return;
    case 0x78: /* FMV.W.X boxes the raw low integer word. */
    {
        if (funct3 != 0)
        {
            fp_raise_illegal(s);
            return;
        }
        const float32_t value = {.v = (uint32_t)gpr(rs1)};
        fp_write_s(rd, value);
        return;
    }
    case 0x79: /* FMV.D.X transfers all XLEN bits. */
    {
        if (funct3 != 0)
        {
            fp_raise_illegal(s);
            return;
        }
        const float64_t value = {.v = (uint64_t)gpr(rs1)};
        fp_write_d(rd, value);
        return;
    }
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

    if (fp_rs2(inst) > 3 ||
        !fp_resolve_rounding_mode(s, fp_rm(inst), &rm))
    {
        if (fp_rs2(inst) > 3)
        {
            fp_raise_illegal(s);
        }
        return;
    }

    fp_begin_softfloat(rm);

    if (is_double)
    {
        const float64_t source = fp_read_d(fp_rs1(inst));

        switch (fp_rs2(inst))
        {
        case 0:
            result = fp_sign_extend_word(
                (uint32_t)f64_to_i32(source, rm, true));
            break;
        case 1:
            /*
             * RV64 sign-extends both W and WU conversion results from bit 31,
             * even though the WU operation itself produces an unsigned word.
             */
            result = fp_sign_extend_word(
                (uint32_t)f64_to_ui32(source, rm, true));
            break;
        case 2:
            result = (word_t)f64_to_i64(source, rm, true);
            break;
        case 3:
            result = (word_t)f64_to_ui64(source, rm, true);
            break;
        }
    }
    else
    {
        const float32_t source = fp_read_s(fp_rs1(inst));

        switch (fp_rs2(inst))
        {
        case 0:
            result = fp_sign_extend_word(
                (uint32_t)f32_to_i32(source, rm, true));
            break;
        case 1:
            result = fp_sign_extend_word(
                (uint32_t)f32_to_ui32(source, rm, true));
            break;
        case 2:
            result = (word_t)f32_to_i64(source, rm, true);
            break;
        case 3:
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

    if (source_type > 3 ||
        !fp_resolve_rounding_mode(s, fp_rm(inst), &rm))
    {
        if (source_type > 3)
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
        case 0:
            result = i32_to_f64((int32_t)source);
            break;
        case 1:
            result = ui32_to_f64((uint32_t)source);
            break;
        case 2:
            result = i64_to_f64((int64_t)source);
            break;
        case 3:
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
        case 0:
            result = i32_to_f32((int32_t)source);
            break;
        case 1:
            result = ui32_to_f32((uint32_t)source);
            break;
        case 2:
            result = i64_to_f32((int64_t)source);
            break;
        case 3:
            result = ui64_to_f32((uint64_t)source);
            break;
        }

        fp_finish_softfloat();
        fp_write_s(fp_rd(inst), result);
    }
}

static void fp_exec_cross_precision(Decode *s, uint32_t inst,
                                    bool destination_double)
{
    uint_fast8_t rm = 0;
    const uint32_t required_rs2 = destination_double ? 0 : 1;

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

static void fp_exec_fused(Decode *s, uint32_t inst)
{
    uint_fast8_t rm = 0;
    const uint32_t format = fp_fmt(inst);
    const uint32_t opcode = fp_opcode(inst);
    const bool negate_product =
        opcode == RISCV_FP_OPCODE_FNMSUB ||
        opcode == RISCV_FP_OPCODE_FNMADD;
    const bool negate_addend =
        opcode == RISCV_FP_OPCODE_FMSUB ||
        opcode == RISCV_FP_OPCODE_FNMADD;

    if (format > 1 ||
        !fp_resolve_rounding_mode(s, fp_rm(inst), &rm))
    {
        if (format > 1)
        {
            fp_raise_illegal(s);
        }
        return;
    }

    fp_begin_softfloat(rm);

    if (format == 1)
    {
        float64_t lhs = fp_read_d(fp_rs1(inst));
        const float64_t rhs = fp_read_d(fp_rs2(inst));
        float64_t addend = fp_read_d(fp_rs3(inst));

        if (negate_product)
        {
            lhs.v ^= RISCV_FP64_SIGN;
        }
        if (negate_addend)
        {
            addend.v ^= RISCV_FP64_SIGN;
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
            lhs.v ^= RISCV_FP32_SIGN;
        }
        if (negate_addend)
        {
            addend.v ^= RISCV_FP32_SIGN;
        }

        const float32_t result = f32_mulAdd(lhs, rhs, addend);
        fp_finish_softfloat();
        fp_write_s(fp_rd(inst), result);
    }
}

static void fp_exec_op(Decode *s, uint32_t inst)
{
    const uint32_t funct7 = fp_funct7(inst);

    switch (funct7)
    {
    case 0x00: /* FADD.S */
        fp_exec_add(s, inst, false);
        return;
    case 0x01: /* FADD.D */
        fp_exec_add(s, inst, true);
        return;
    case 0x04: /* FSUB.S */
        fp_exec_binary_arithmetic(s, inst, false, FP_BINARY_SUB);
        return;
    case 0x05: /* FSUB.D */
        fp_exec_binary_arithmetic(s, inst, true, FP_BINARY_SUB);
        return;
    case 0x08: /* FMUL.S */
        fp_exec_binary_arithmetic(s, inst, false, FP_BINARY_MUL);
        return;
    case 0x09: /* FMUL.D */
        fp_exec_binary_arithmetic(s, inst, true, FP_BINARY_MUL);
        return;
    case 0x0c: /* FDIV.S */
        fp_exec_binary_arithmetic(s, inst, false, FP_BINARY_DIV);
        return;
    case 0x0d: /* FDIV.D */
        fp_exec_binary_arithmetic(s, inst, true, FP_BINARY_DIV);
        return;
    case 0x10: /* FSGNJ[ N/X ].S */
        fp_exec_sign_injection(s, inst, false);
        return;
    case 0x11: /* FSGNJ[ N/X ].D */
        fp_exec_sign_injection(s, inst, true);
        return;
    case 0x14: /* FMIN.S / FMAX.S */
        fp_exec_min_max(s, inst, false);
        return;
    case 0x15: /* FMIN.D / FMAX.D */
        fp_exec_min_max(s, inst, true);
        return;
    case 0x20: /* FCVT.S.D */
        fp_exec_cross_precision(s, inst, false);
        return;
    case 0x21: /* FCVT.D.S */
        fp_exec_cross_precision(s, inst, true);
        return;
    case 0x2c: /* FSQRT.S */
        fp_exec_sqrt(s, inst, false);
        return;
    case 0x2d: /* FSQRT.D */
        fp_exec_sqrt(s, inst, true);
        return;
    case 0x50: /* FLE/FLT/FEQ.S */
        fp_exec_compare(s, inst, false);
        return;
    case 0x51: /* FLE/FLT/FEQ.D */
        fp_exec_compare(s, inst, true);
        return;
    case 0x60: /* FCVT.[W/WU/L/LU].S */
        fp_exec_float_to_integer(s, inst, false);
        return;
    case 0x61: /* FCVT.[W/WU/L/LU].D */
        fp_exec_float_to_integer(s, inst, true);
        return;
    case 0x68: /* FCVT.S.[W/WU/L/LU] */
        fp_exec_integer_to_float(s, inst, false);
        return;
    case 0x69: /* FCVT.D.[W/WU/L/LU] */
        fp_exec_integer_to_float(s, inst, true);
        return;
    case 0x70:
    case 0x71:
    case 0x78:
    case 0x79:
        fp_exec_move_or_class(s, inst);
        return;
    default:
        fp_raise_illegal(s);
        return;
    }
}

void riscv64_fpu_exec(Decode *s)
{
    const uint32_t inst = s->isa.inst;

    if (!fp_require_enabled(s))
    {
        return;
    }

    switch (fp_opcode(inst))
    {
    case RISCV_FP_OPCODE_LOAD:
        fp_exec_load(s, inst);
        return;
    case RISCV_FP_OPCODE_STORE:
        fp_exec_store(s, inst);
        return;
    case RISCV_FP_OPCODE_FMADD:
    case RISCV_FP_OPCODE_FMSUB:
    case RISCV_FP_OPCODE_FNMSUB:
    case RISCV_FP_OPCODE_FNMADD:
        fp_exec_fused(s, inst);
        return;
    case RISCV_FP_OPCODE_OP:
        fp_exec_op(s, inst);
        return;
    default:
        fp_raise_illegal(s);
        return;
    }
}

#endif
