#include <am.h>
#include <nemu.h>

static uint64_t BootTime = 0;

static uint64_t getTime()
{
	return ((uint64_t)inl(RTC_ADDR)) | (((uint64_t)inl(RTC_ADDR + sizeof(uint32_t))) << 32);
}

void __am_timer_init() 
{
	BootTime = getTime();
}

void __am_timer_uptime(AM_TIMER_UPTIME_T *uptime) 
{
	uptime->us = getTime() - BootTime;
}

void __am_timer_rtc(AM_TIMER_RTC_T *rtc) {
	rtc->second = 0;
	rtc->minute = 0;
	rtc->hour   = 0;
	rtc->day    = 0;
	rtc->month  = 0;
	rtc->year   = 1900;
}
