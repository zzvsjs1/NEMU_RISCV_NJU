/***************************************************************************************
* Copyright (c) 2014-2024 Zihao Yu, Nanjing University
*
* NEMU is licensed under Mulan PSL v2.
* You can use this software according to the terms and conditions of the Mulan PSL v2.
* You may obtain a copy of Mulan PSL v2 at:
*          http://license.coscl.org.cn/MulanPSL2
*
* THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
* EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
* MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
*
* See the Mulan PSL v2 for more details.
***************************************************************************************/

#include <common.h>
// clang-format off
#include MUXDEF(CONFIG_TIMER_GETTIMEOFDAY, <sys/time.h>, <time.h>)
// clang-format on

#ifdef CONFIG_TIMER_CLOCK_GETTIME
static_assert(CLOCKS_PER_SEC == 1000000, "CLOCKS_PER_SEC != 1000000");
static_assert(sizeof(clock_t) == 8, "sizeof(clock_t) != 8");
#endif

static uint64_t boot_time = 0;

static uint64_t get_time_internal()
{
#if defined(CONFIG_TARGET_AM)
    uint64_t us = io_read(AM_TIMER_UPTIME).us;
#elif defined(CONFIG_TIMER_GETTIMEOFDAY)
    struct timeval now;
    gettimeofday(&now, NULL);
    uint64_t us = now.tv_sec * 1000000 + now.tv_usec;
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC_COARSE, &now);
    uint64_t us = now.tv_sec * 1000000 + now.tv_nsec / 1000;
#endif
    return us;
}

uint64_t get_time()
{
    /*
   * All device time is reported relative to the first call, not the host epoch.
   * This keeps snapshots, logs, and RTC reads small and monotonic within one
   * NEMU process regardless of which host clock backend is configured.
   */

    if (boot_time == 0)
        boot_time = get_time_internal();
    uint64_t now = get_time_internal();
    return now - boot_time;
}

uint64_t get_real_time_us()
{
#if defined(CONFIG_TARGET_AM)
    /*
     * NEMU-on-AM has no POSIX host clock.  Keep this fallback simple so the
     * target still builds; normal RISC-V32 system runs use the native host path
     * below and expose Unix epoch time.
     */
    return io_read(AM_TIMER_UPTIME).us;
#elif defined(CONFIG_TIMER_GETTIMEOFDAY)
    struct timeval now;
    gettimeofday(&now, NULL);
    return (uint64_t)now.tv_sec * 1000000 + now.tv_usec;
#else
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return (uint64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
#endif
}

// void init_rand() {
//   srand(get_time_internal());
// }
