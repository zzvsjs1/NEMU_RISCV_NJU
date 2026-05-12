#include <stdint.h>
#include <stdio.h>
#include <sys/time.h>
#include <time.h>

#include <NDL.h>

static int gettimeofday_calls;
static int clock_gettime_calls;

int gettimeofday(struct timeval *tv, void *tz)
{
    static const long fake_realtime_ms[] = {50000, 50042};
    const int index = gettimeofday_calls < 2 ? gettimeofday_calls : 1;
    const long ms = fake_realtime_ms[index];

    (void)tz;
    gettimeofday_calls++;
    tv->tv_sec = ms / 1000;
    tv->tv_usec = (ms % 1000) * 1000;
    return 0;
}

int clock_gettime(clockid_t clock_id, struct timespec *tp)
{
    static const long fake_monotonic_ms[] = {10002, 10017};
    const int index = clock_gettime_calls < 2 ? clock_gettime_calls : 1;
    const long ms = fake_monotonic_ms[index];

    if (clock_id != CLOCK_MONOTONIC)
    {
        return -1;
    }

    clock_gettime_calls++;
    tp->tv_sec = ms / 1000;
    tp->tv_nsec = (ms % 1000) * 1000000L;
    return 0;
}

int main(void)
{
    if (NDL_Init(0) != 0)
    {
        puts("NDL_Init failed");
        return 1;
    }

    const uint32_t ticks = NDL_GetTicks();

    if (ticks != 15 || clock_gettime_calls != 2 || gettimeofday_calls != 0)
    {
        printf("ticks=%u clock_gettime_calls=%d gettimeofday_calls=%d\n",
               ticks, clock_gettime_calls, gettimeofday_calls);
        return 1;
    }

    puts("ndl_monotonic_ticks test passed");
    return 0;
}
