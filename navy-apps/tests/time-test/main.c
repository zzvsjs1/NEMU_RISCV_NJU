#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/time.h>
#include <time.h>

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME ((clockid_t)1)
#endif

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC ((clockid_t)4)
#endif

/*
 * Some newlib configurations hide the POSIX timer declaration behind feature
 * macros.  The test still calls the public symbol so libos must provide the
 * same ABI that applications expect.
 */
extern int clock_gettime(clockid_t clock_id, struct timespec *tp);
extern int clock_getres(clockid_t clock_id, struct timespec *res);

static int64_t timeval_us(const struct timeval *tv)
{
    return (int64_t)tv->tv_sec * 1000000 + tv->tv_usec;
}

static int64_t timespec_us(const struct timespec *ts)
{
    return (int64_t)ts->tv_sec * 1000000 + ts->tv_nsec / 1000;
}

static int64_t i64_abs(int64_t value)
{
    return value < 0 ? -value : value;
}

int main(void)
{
    struct timeval tv1;
    struct timeval tv2;
    struct timezone tz;
    struct timespec real_ts;
    struct timespec real_res;
    struct timespec mono1;
    struct timespec mono2;
    time_t now;

    assert(gettimeofday(&tv1, &tz) == 0);
    assert(tv1.tv_sec > 1700000000);
    assert(tv1.tv_usec >= 0 && tv1.tv_usec < 1000000);
    assert(tz.tz_minuteswest == 0);
    assert(tz.tz_dsttime == 0);

    assert(gettimeofday(&tv2, NULL) == 0);
    assert(timeval_us(&tv2) >= timeval_us(&tv1));

    now = time(NULL);
    assert(now >= tv1.tv_sec - 1 && now <= tv2.tv_sec + 1);

    assert(clock_gettime(CLOCK_REALTIME, &real_ts) == 0);
    assert(real_ts.tv_sec > 1700000000);
    assert(real_ts.tv_nsec >= 0 && real_ts.tv_nsec < 1000000000);
    assert(i64_abs(timespec_us(&real_ts) - timeval_us(&tv2)) < 2000000);

    assert(clock_getres(CLOCK_REALTIME, &real_res) == 0);
    assert(real_res.tv_sec == 0);
    assert(real_res.tv_nsec > 0 && real_res.tv_nsec <= 1000);

    assert(clock_gettime(CLOCK_MONOTONIC, &mono1) == 0);
    assert(clock_gettime(CLOCK_MONOTONIC, &mono2) == 0);
    assert(mono1.tv_sec >= 0);
    assert(mono1.tv_nsec >= 0 && mono1.tv_nsec < 1000000000);
    assert(timespec_us(&mono2) >= timespec_us(&mono1));

    errno = 0;
    assert(clock_gettime((clockid_t)12345, &real_ts) == -1);
    assert(errno == EINVAL);

    errno = 0;
    assert(clock_getres((clockid_t)12345, &real_res) == -1);
    assert(errno == EINVAL);

    assert(clock() != (clock_t)-1);

    printf("time-test PASS: realtime=%ld.%06ld monotonic=%ld.%09ld\n",
           (long)tv1.tv_sec, (long)tv1.tv_usec,
           (long)mono2.tv_sec, (long)mono2.tv_nsec);
    return 0;
}
