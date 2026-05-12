#include <amtest.h>

void rtc_test()
{
    AM_TIMER_RTC_T rtc;
    AM_TIMER_REALTIME_T realtime;
    int sec = 1;

    realtime = io_read(AM_TIMER_REALTIME);
    assert(realtime.us / 1000000 >= 1700000000ull);

    while (1)
    {
        // Wait on monotonic uptime and then print the RTC fields. On NEMU the wall
        // clock is backed by Unix epoch time, so the ranges below check both the
        // realtime hardware path and the UTC conversion used by AM_TIMER_RTC.
        while (io_read(AM_TIMER_UPTIME).us / 1000000 < sec)
            ;
        rtc = io_read(AM_TIMER_RTC);
        assert(rtc.year >= 2024);
        assert(rtc.month >= 1 && rtc.month <= 12);
        assert(rtc.day >= 1 && rtc.day <= 31);
        assert(rtc.hour >= 0 && rtc.hour <= 23);
        assert(rtc.minute >= 0 && rtc.minute <= 59);
        assert(rtc.second >= 0 && rtc.second <= 60);
        printf("%d-%d-%d %d:%d:%d GMT (", rtc.year, rtc.month, rtc.day, rtc.hour, rtc.minute, rtc.second);

        if (sec == 1)
        {
            printf("%d second).\n", sec);
        }
        else
        {
            printf("%d seconds).\n", sec);
        }
        sec++;
    }
}
