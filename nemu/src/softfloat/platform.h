#ifndef NEMU_SOFTFLOAT_PLATFORM_H
#define NEMU_SOFTFLOAT_PLATFORM_H

#include "config.h"

#ifndef WORDS_BIGENDIAN
#define LITTLEENDIAN 1
#endif

/*
 * NEMU executes on a normal 32-bit or 64-bit host with efficient 64-bit
 * integer arithmetic. These options match the former Spike-derived build.
 */
#define INLINE_LEVEL 5
#define SOFTFLOAT_FAST_INT64
#define SOFTFLOAT_FAST_DIV64TO32
#define SOFTFLOAT_ROUND_ODD
#define INLINE static inline

/*
 * Release 3e's RISC-V canonical-NaN macros deliberately ignore their
 * common-NaN pointer.  The generic cross-format conversion sources still
 * declare that temporary, so GCC reports it as unused.  Keep the pinned
 * upstream checkout unmodified and limit this diagnostic exception to
 * SoftFloat translation units, all of which include this platform header.
 */
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-variable"
#endif

#endif
