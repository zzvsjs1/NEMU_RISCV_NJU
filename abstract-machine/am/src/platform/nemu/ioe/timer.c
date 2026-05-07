#include <am.h>
#include <nemu.h>

static uint64_t BootTime = 0;

static uint64_t getTime()
{
	// NEMU exposes the uptime counter as two 32-bit RTC words. Reading the low
	// word first mirrors the platform convention and reconstructs AM's
	// microsecond-resolution 64-bit time value.
	return ((uint64_t)inl(RTC_ADDR)) | (((uint64_t)inl(RTC_ADDR + sizeof(uint32_t))) << 32);
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

void __am_timer_rtc(AM_TIMER_RTC_T *rtc) {
	// The NEMU target used here only promises monotonic uptime. Wall-clock RTC
	// fields are therefore filled with the AM baseline date instead of reading
	// host time, which avoids host-dependent test output.
	rtc->second = 0;
	rtc->minute = 0;
	rtc->hour   = 0;
	rtc->day    = 0;
	rtc->month  = 0;
	rtc->year   = 1900;
}
