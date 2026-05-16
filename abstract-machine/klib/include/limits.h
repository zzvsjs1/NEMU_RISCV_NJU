#ifndef KLIB_LIMITS_H__
#define KLIB_LIMITS_H__

/*
 * Freestanding AM programs should not include the host C library headers from
 * the cross sysroot.  Use compiler-provided integer limits instead; GCC defines
 * these macros for the selected target ABI, so RV32 and RV64 both get the right
 * widths without depending on glibc stubs.
 */
#define CHAR_BIT __CHAR_BIT__

#define SCHAR_MIN (-__SCHAR_MAX__ - 1)
#define SCHAR_MAX __SCHAR_MAX__
#define UCHAR_MAX (__SCHAR_MAX__ * 2U + 1U)

#define SHRT_MIN (-__SHRT_MAX__ - 1)
#define SHRT_MAX __SHRT_MAX__
#define USHRT_MAX (__SHRT_MAX__ * 2U + 1U)

#define INT_MIN (-__INT_MAX__ - 1)
#define INT_MAX __INT_MAX__
#define UINT_MAX (__INT_MAX__ * 2U + 1U)

#define LONG_MIN (-__LONG_MAX__ - 1L)
#define LONG_MAX __LONG_MAX__
#define ULONG_MAX (__LONG_MAX__ * 2UL + 1UL)

#define LLONG_MIN (-__LONG_LONG_MAX__ - 1LL)
#define LLONG_MAX __LONG_LONG_MAX__
#define ULLONG_MAX (__LONG_LONG_MAX__ * 2ULL + 1ULL)

#endif
