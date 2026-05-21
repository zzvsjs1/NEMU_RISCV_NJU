#ifndef __RTL_PSEUDO_H__
#define __RTL_PSEUDO_H__

#ifndef __RTL_RTL_H__
#error "Should be only included by <rtl/rtl.h>"
#endif

/* RTL pseudo instructions */

static inline def_rtl(li, rtlreg_t *dest, const rtlreg_t imm)
{
    /* Load an immediate by adding it to the constant zero register. */
    rtl_addi(s, dest, rz, imm);
}

static inline def_rtl(mv, rtlreg_t *dest, const rtlreg_t *src1)
{
    /* Register move is an add-immediate with zero, preserving common RTL flow. */
    rtl_addi(s, dest, src1, 0);
}

static inline def_rtl(not, rtlreg_t *dest, const rtlreg_t *src1)
{
    /* Bitwise inversion across the full host rtlreg_t width. */
    *dest = ~*src1;
}

static inline def_rtl(neg, rtlreg_t *dest, const rtlreg_t *src1)
{
    // dest <- -src1
    *dest = -*src1;
}

static inline def_rtl(sign_ext_pos, rtlreg_t *dest, const rtlreg_t *src1, const size_t pos)
{
    /*
     * Sign-extend from bit pos by moving that bit to the host sign position,
     * then shifting back arithmetically.
     */
    Assert(pos < sizeof(rtlreg_t) * 8, "%zu is more than %zu.\n", pos, sizeof(rtlreg_t) * 8);

    const size_t pos2 = sizeof(rtlreg_t) * 8 - 1 - pos;
    rtl_mv(s, dest, src1);
    rtl_slli(s, dest, dest, pos2);
    rtl_srai(s, dest, dest, pos2);
}

static inline def_rtl(sext, rtlreg_t *dest, const rtlreg_t *src1, int width)
{
    /*
     * Sign-extend a byte/halfword/word field.  width is measured in bytes, so
     * the source sign bit is width * 8 - 1.
     */
    rtl_sign_ext_pos(s, dest, src1, width * 8 - 1);
}

static inline def_rtl(zero_ext_pos, rtlreg_t *dest, const rtlreg_t *src1, const size_t pos)
{
    /*
     * Zero-extend from bit pos with the same shift-pair trick as sign extension,
     * but use a logical right shift so the high bits become zero.
     */
    Assert(pos < sizeof(rtlreg_t) * 8, "%zu is more than %zu.\n", pos, sizeof(rtlreg_t) * 8);

    const size_t pos2 = sizeof(rtlreg_t) * 8 - 1 - pos;
    rtl_mv(s, dest, src1);
    rtl_slli(s, dest, dest, pos2);
    rtl_srli(s, dest, dest, pos2);
}

static inline def_rtl(zext, rtlreg_t *dest, const rtlreg_t *src1, int width)
{
    /*
     * Zero-extend a byte/halfword/word field.  This is the unsigned-load
     * companion to rtl_sext().
     */
    rtl_zero_ext_pos(s, dest, src1, width * 8 - 1);
}

static inline def_rtl(msb, rtlreg_t *dest, const rtlreg_t *src1, int width)
{
    /* Extract the most-significant bit from a width-byte value. */

    // The mask below would be wrong because it keeps several low bits, not just
    // the requested most-significant bit.
    // *dest = *src1 & (width * 8 - 1);

    *dest = (*src1 >> (width * 8 - 1)) & 1;
}

#endif
