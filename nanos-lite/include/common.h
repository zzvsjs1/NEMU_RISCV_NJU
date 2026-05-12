#ifndef __COMMON_H__
#define __COMMON_H__

/* Uncomment these macros to enable corresponding functionality. */
#define HAS_CTE
#define HAS_VME
//#define MULTIPROGRAM
//#define TIME_SHARING

// #define STRACE

#include <am.h>
#include <klib.h>
#include <klib-macros.h>
#include <debug.h>
#include <inttypes.h>

/*
 * These time types are part of the Nanos-to-Navy syscall ABI.  Navy newlib in
 * this tree defines _USE_LONG_TIME_T, so RISC-V32 user programs lay out
 * timeval/timespec as two long-sized fields.  Keep the kernel definitions in
 * lock-step with that ABI; otherwise tv_usec/tv_nsec lands at the wrong offset
 * and user-space timers degrade to whole-second precision.
 */
typedef long time_t;
typedef long suseconds_t;
typedef long clockid_t;

struct timeval
{
    time_t tv_sec;       /* seconds */
    suseconds_t tv_usec; /* and microseconds */
};

struct timezone
{
    int tz_minuteswest; /* minutes west of Greenwich */
    int tz_dsttime;     /* type of dst correction */
};

struct timespec
{
    time_t tv_sec; /* seconds */
    long tv_nsec;  /* nanoseconds */
};

#define CLOCK_REALTIME ((clockid_t)1)
#define CLOCK_MONOTONIC ((clockid_t)4)

#endif
