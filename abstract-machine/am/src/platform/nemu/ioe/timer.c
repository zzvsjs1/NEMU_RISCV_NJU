#include <am.h>
#include <nemu.h>

static uint64_t BootTime = 0;

static uint64_t readRtc64(uint32_t lowOffset)
{
    /*
     * RISC-V32 reads MMIO naturally as 32-bit words.  NEMU refreshes a coherent
     * snapshot when the low word is read, so AM must read low before high and
     * then join the two halves into the 64-bit value expected by AM callers.
     */
    const uint64_t low = inl(RTC_ADDR + lowOffset);
    const uint64_t high = inl(RTC_ADDR + lowOffset + sizeof(uint32_t));
    return low | (high << 32);
}

static uint64_t getTime()
{
    return readRtc64(NEMU_RTC_UPTIME_US_LO);
}

static uint64_t getRealTimeUs()
{
    return readRtc64(NEMU_RTC_EPOCH_US_LO);
}

static bool isLeapYear(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static int daysInMonth(int year, int month)
{
    static const int monthDays[12] = {
        31, 28, 31, 30, 31, 30,
        31, 31, 30, 31, 30, 31,
    };

    if (month == 2 && isLeapYear(year))
        return 29;
    return monthDays[month - 1];
}

static void epochSecondsToUtc(uint64_t seconds, AM_TIMER_RTC_T *rtc)
{
    /*
     * Split the POSIX epoch into whole days and seconds inside the current day:
     * 86400 = 24 * 60 * 60.  The remaining steps peel complete years and months
     * away from the day count until the leftover is the day-of-month offset.
     */
    uint64_t days = seconds / 86400;
    uint32_t daySeconds = seconds % 86400;
    int year = 1970;
    int month = 1;

    rtc->hour = daySeconds / 3600;
    daySeconds %= 3600;
    rtc->minute = daySeconds / 60;
    rtc->second = daySeconds % 60;

    while (true)
    {
        const int yearDays = isLeapYear(year) ? 366 : 365;

        if (days < (uint64_t)yearDays)
            break;
        days -= yearDays;
        year++;
    }

    while (true)
    {
        const int monthDays = daysInMonth(year, month);

        if (days < (uint64_t)monthDays)
            break;
        days -= monthDays;
        month++;
    }

    rtc->year = year;
    rtc->month = month;
    rtc->day = days + 1;
}

void __am_timer_init()
{
    BootTime = getTime();
}

void __am_timer_uptime(AM_TIMER_UPTIME_T *uptime)
{
    // AM uptime is relative to platform initialisation, not to emulator start.
    // This keeps tests deterministic even if NEMU has already advanced time
    // before the guest calls __am_timer_init().
    uptime->us = getTime() - BootTime;
}

void __am_timer_rtc(AM_TIMER_RTC_T *rtc)
{
    epochSecondsToUtc(readRtc64(NEMU_RTC_EPOCH_SEC_LO), rtc);
}

void __am_timer_realtime(AM_TIMER_REALTIME_T *realtime)
{
    realtime->us = getRealTimeUs();
}
