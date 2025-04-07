#ifndef __RTL_PSEUDO_H__
#define __RTL_PSEUDO_H__

#ifndef __RTL_RTL_H__
#error "Should be only included by <rtl/rtl.h>"
#endif

/* RTL pseudo instructions */

static inline def_rtl(li, rtlreg_t* dest, const rtlreg_t imm)
{
	rtl_addi(s, dest, rz, imm);
}

static inline def_rtl(mv, rtlreg_t* dest, const rtlreg_t *src1)
{
	rtl_addi(s, dest, src1, 0);
}

static inline def_rtl(not, rtlreg_t *dest, const rtlreg_t* src1)
{
	*dest = ~*src1; 
}

static inline def_rtl(neg, rtlreg_t *dest, const rtlreg_t* src1)
{
	// dest <- -src1
	*dest = -*src1;
}

static inline def_rtl(sign_ext_pos, rtlreg_t* dest, const rtlreg_t* src1, const size_t pos)
{
	Assert(pos < sizeof(rtlreg_t) * 8, "%zu is more than %zu.\n", pos, sizeof(rtlreg_t) * 8);

	const size_t pos2 = sizeof(rtlreg_t) * 8 - 1 - pos;
	rtl_mv(s, dest, src1);
	rtl_slli(s, dest, dest, pos2);
	rtl_srai(s, dest, dest, pos2);
}

static inline def_rtl(sext, rtlreg_t* dest, const rtlreg_t* src1, int width)
{
	// dest <- signext(src1[(width * 8 - 1) .. 0])
	rtl_sign_ext_pos(s, dest, src1, width * 8 - 1);
}

static inline def_rtl(zero_ext_pos, rtlreg_t* dest, const rtlreg_t* src1, const size_t pos)
{
	Assert(pos < sizeof(rtlreg_t) * 8, "%zu is more than %zu.\n", pos, sizeof(rtlreg_t) * 8);

	const size_t pos2 = sizeof(rtlreg_t) * 8 - 1 - pos;
	rtl_mv(s, dest, src1);
	rtl_slli(s, dest, dest, pos2);
	rtl_srli(s, dest, dest, pos2);
}

static inline def_rtl(zext, rtlreg_t* dest, const rtlreg_t* src1, int width)
{
	// dest <- zeroext(src1[(width * 8 - 1) .. 0])
	rtl_zero_ext_pos(s, dest, src1, width * 8 - 1);
}

static inline def_rtl(msb, rtlreg_t* dest, const rtlreg_t* src1, int width)
{
	// dest <- src1[width * 8 - 1]
	
	// It this correct?
	// *dest = *src1 & (width * 8 - 1);

	*dest = (*src1 >> (width * 8 - 1)) & 1;
}

#endif
