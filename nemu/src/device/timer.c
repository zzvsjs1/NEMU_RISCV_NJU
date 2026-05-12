#include <device/map.h>
#include <device/alarm.h>
#include <utils.h>

static uint32_t *rtc_port_base = NULL;

enum
{
    RTC_UPTIME_US_LO = 0,
    RTC_UPTIME_US_HI = 4,
    RTC_EPOCH_SEC_LO = 8,
    RTC_EPOCH_SEC_HI = 12,
    RTC_EPOCH_US_LO = 16,
    RTC_EPOCH_US_HI = 20,
    RTC_MMIO_SIZE = 24,
};

static void publish_u64(uint32_t low_offset, uint64_t value)
{
    rtc_port_base[low_offset / sizeof(uint32_t)] = (uint32_t)value;
    rtc_port_base[low_offset / sizeof(uint32_t) + 1] = (uint32_t)(value >> 32);
}

static void publish_realtime()
{
    const uint64_t us = get_real_time_us();
    publish_u64(RTC_EPOCH_SEC_LO, us / 1000000);
    publish_u64(RTC_EPOCH_US_LO, us);
}

static void rtc_io_handler(uint32_t offset, int len, bool is_write)
{
    assert(offset < RTC_MMIO_SIZE);
    assert(offset + len <= RTC_MMIO_SIZE);

    if (is_write)
        return;

    if (offset == RTC_UPTIME_US_LO)
    {
        /*
         * The RTC is exposed as two 32-bit words.  Refresh both halves when the
         * low word is read so guests that read low then high observe one coherent
         * 64-bit microsecond timestamp.
         */
        publish_u64(RTC_UPTIME_US_LO, get_time());
    }
    else if (offset == RTC_EPOCH_SEC_LO || offset == RTC_EPOCH_US_LO)
    {
        /*
         * Real time is a separate snapshot from monotonic uptime.  Refresh it
         * only when a guest asks for wall-clock data, so hot uptime reads used by
         * benchmarks and SDL timers do not pay for a host realtime query.
         */
        publish_realtime();
    }
}

#ifndef CONFIG_TARGET_AM
static void timer_intr()
{
    if (nemu_state.state == NEMU_RUNNING)
    {
        /*
         * Timer signals may arrive while the monitor is stopped.  Only raise a
         * pending guest interrupt during RUNNING state so single-step/debugger
         * sessions do not accumulate stale timer events.
         */
        extern void dev_raise_intr();
        dev_raise_intr();
    }
}
#endif

void init_timer()
{
    rtc_port_base = (uint32_t *)new_space(RTC_MMIO_SIZE);
#ifdef CONFIG_HAS_PORT_IO
    add_pio_map("rtc", CONFIG_RTC_PORT, rtc_port_base, RTC_MMIO_SIZE, rtc_io_handler);
#else
    add_mmio_map("rtc", CONFIG_RTC_MMIO, rtc_port_base, RTC_MMIO_SIZE, rtc_io_handler);
#endif
    IFNDEF(CONFIG_TARGET_AM, add_alarm_handle(timer_intr));
}
