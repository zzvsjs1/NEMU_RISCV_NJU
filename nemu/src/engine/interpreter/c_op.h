#ifndef __C_OP_H__
#define __C_OP_H__

#include <common.h>

#define c_shift_mask MUXDEF(CONFIG_ISA64, 0x3f, 0x1f)

#define c_add(a, b) ((a) + (b))
#define c_sub(a, b) ((a) - (b))
#define c_and(a, b) ((a) & (b))
#define c_or(a, b) ((a) | (b))
#define c_xor(a, b) ((a) ^ (b))
#define c_sll(a, b) ((a) << ((b) & c_shift_mask))
#define c_srl(a, b) ((a) >> ((b) & c_shift_mask))
#define c_sra(a, b) ((sword_t)(a) >> ((b) & c_shift_mask))

#ifdef CONFIG_ISA64
#define c_sext32to64(a) ((int64_t)(int32_t)(a))
#define c_addw(a, b) c_sext32to64((a) + (b))
#define c_subw(a, b) c_sext32to64((a) - (b))
#define c_sllw(a, b) c_sext32to64((uint32_t)(a) << ((b) & 0x1f))
#define c_srlw(a, b) c_sext32to64((uint32_t)(a) >> ((b) & 0x1f))
#define c_sraw(a, b) c_sext32to64((int32_t)(a) >> ((b) & 0x1f))
#endif

#define c_mulu_lo(a, b) ((a) * (b))
#ifdef CONFIG_ISA64
static inline word_t c_mulsu_hi_impl(word_t lhs, word_t rhs)
{
    /*
     * Compute the upper half of signed(lhs) * unsigned(rhs).
     *
     * A direct signed-by-unsigned __int128 expression is easy to get wrong in C:
     * the usual arithmetic conversions may first reinterpret a negative lhs as a
     * large unsigned value.  Start with the unsigned high half, then subtract rhs
     * when lhs is negative.  This follows from:
     *
     *   signed_lhs = unsigned_lhs - 2^64
     *   high(signed_lhs * rhs) = high(unsigned_lhs * rhs) - rhs
     */
    const __uint128_t product = (__uint128_t)lhs * (__uint128_t)rhs;
    word_t high = (word_t)(product >> 64);
    if ((sword_t)lhs < 0)
    {
        high -= rhs;
    }
    return high;
}

#define c_mulu_hi(a, b) (((__uint128_t)(a) * (__uint128_t)(b)) >> 64)
#define c_muls_hi(a, b) (((__int128_t)(sword_t)(a) * (__int128_t)(sword_t)(b)) >> 64)
#define c_mulsu_hi(a, b) c_mulsu_hi_impl((word_t)(a), (word_t)(b))
#define c_mulw(a, b) c_sext32to64((a) * (b))
#define c_divw(a, b) c_sext32to64((int32_t)(a) / (int32_t)(b))
#define c_divuw(a, b) c_sext32to64((uint32_t)(a) / (uint32_t)(b))
#define c_remw(a, b) c_sext32to64((int32_t)(a) % (int32_t)(b))
#define c_remuw(a, b) c_sext32to64((uint32_t)(a) % (uint32_t)(b))
#else
static inline word_t c_mulsu_hi_impl(word_t lhs, word_t rhs)
{
    /*
     * Same rule as the RV64 helper, but the product is 64 bits wide and the high
     * half starts at bit 32.
     */
    const uint64_t product = (uint64_t)lhs * (uint64_t)rhs;
    word_t high = (word_t)(product >> 32);
    if ((sword_t)lhs < 0)
    {
        high -= rhs;
    }
    return high;
}

#define c_mulu_hi(a, b) (((uint64_t)(a) * (uint64_t)(b)) >> 32)
#define c_muls_hi(a, b) (((int64_t)(sword_t)(a) * (int64_t)(sword_t)(b)) >> 32)
#define c_mulsu_hi(a, b) c_mulsu_hi_impl((word_t)(a), (word_t)(b))
#endif

#define c_divu_q(a, b) ((a) / (b))
#define c_divu_r(a, b) ((a) % (b))
#define c_divs_q(a, b) ((sword_t)(a) / (sword_t)(b))
#define c_divs_r(a, b) ((sword_t)(a) % (sword_t)(b))

static inline bool interpret_relop(const RELOP_TYPE relop, const rtlreg_t src1, const rtlreg_t src2)
{
    switch (relop)
    {
    case RELOP_FALSE:
        return false;
    case RELOP_TRUE:
        return true;
    case RELOP_EQ:
        return src1 == src2;
    case RELOP_NE:
        return src1 != src2;
    case RELOP_LT:
        return (sword_t)src1 < (sword_t)src2;
    case RELOP_LE:
        return (sword_t)src1 <= (sword_t)src2;
    case RELOP_GT:
        return (sword_t)src1 > (sword_t)src2;
    case RELOP_GE:
        return (sword_t)src1 >= (sword_t)src2;
    case RELOP_LTU:
        return src1 < src2;
    case RELOP_LEU:
        return src1 <= src2;
    case RELOP_GTU:
        return src1 > src2;
    case RELOP_GEU:
        return src1 >= src2;
    default:
        panic("unsupport relop = %d", relop);
    }
}

static inline bool compareRegister(const RELOP_TYPE relop, const rtlreg_t *rs1, const rtlreg_t *rs2)
{
    return interpret_relop(relop, *rs1, *rs2);
}

static inline bool compareRegisterI(const RELOP_TYPE relop, const rtlreg_t *rs, const rtlreg_t i)
{
    return interpret_relop(relop, *rs, i);
}

static inline bool compareIRegister(const RELOP_TYPE relop, const rtlreg_t i, const rtlreg_t *rs)
{
    return interpret_relop(relop, i, *rs);
}

#endif
