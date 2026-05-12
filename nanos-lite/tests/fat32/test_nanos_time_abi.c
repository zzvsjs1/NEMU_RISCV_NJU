#include <stddef.h>

#include "common.h"

/*
 * Nanos-lite writes these structs directly into Navy user memory.  The kernel
 * layout must therefore match the RISC-V32 newlib ABI, where this tree defines
 * time_t as long through _USE_LONG_TIME_T.
 */
_Static_assert(sizeof(time_t) == sizeof(long),
               "Nanos time_t must match RISC-V32 Navy newlib time_t");
_Static_assert(offsetof(struct timeval, tv_usec) == sizeof(long),
               "struct timeval tv_usec must follow a long tv_sec");
_Static_assert(offsetof(struct timespec, tv_nsec) == sizeof(long),
               "struct timespec tv_nsec must follow a long tv_sec");
_Static_assert(sizeof(struct timeval) == 2 * sizeof(long),
               "struct timeval must contain two long-sized fields");
_Static_assert(sizeof(struct timespec) == 2 * sizeof(long),
               "struct timespec must contain two long-sized fields");

