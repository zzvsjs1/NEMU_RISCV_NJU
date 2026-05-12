#include <common.h>
#include "syscall.h"
#include "fs.h"
#include <inttypes.h>
#include "memory.h"
#include "proc.h"

void context_uload(PCB *pcb, const char *filename, char *const argv[], char *const envp[]);
extern PCB *current;

static volatile int need_resched = 0;
static volatile int context_replaced = 0;

enum
{
    NANOS_EFAULT = 14,
    NANOS_EINVAL = 22,
};

typedef unsigned long NanosClockTick;

typedef struct
{
    NanosClockTick tms_utime;
    NanosClockTick tms_stime;
    NanosClockTick tms_cutime;
    NanosClockTick tms_cstime;
} NanosTms;

static uint64_t realtime_us(void)
{
    return io_read(AM_TIMER_REALTIME).us;
}

static uint64_t monotonic_us(void)
{
    return io_read(AM_TIMER_UPTIME).us;
}

// Called by do_event() to test and clear the reschedule request.
int syscall_need_resched_and_clear(void)
{
    /*
     * SYS_yield is handled after do_syscall() returns to the CTE handler. Calling
     * yield() inside the syscall handler would nest another ecall on the same
     * trap frame, so this one-shot flag keeps scheduling at a single boundary.
     */
    const int v = need_resched;
    need_resched = 0;
    return v;
}

Context *syscall_replacement_context_and_clear(void)
{
    /* execve() rebuilds current->cp in-place.  The trap handler uses this flag to
     * return through that new context instead of resuming the syscall frame that
     * belonged to the replaced image.
     */
    if (!context_replaced)
    {
        return NULL;
    }

    context_replaced = 0;
    assert(current != NULL);
    assert(current->cp != NULL);
    return current->cp;
}

#ifdef STRACE
static const char *syscall_name(uintptr_t num)
{
    /* Keep unknown IDs printable so the panic path still has context. */
    switch (num)
    {
    case SYS_exit:
        return "exit";
    case SYS_yield:
        return "yield";
    case SYS_open:
        return "open";
    case SYS_read:
        return "read";
    case SYS_write:
        return "write";
    case SYS_close:
        return "close";
    case SYS_lseek:
        return "lseek";
    case SYS_brk:
        return "brk";
    case SYS_fstat:
        return "fstat";
    case SYS_execve:
        return "execve";
    case SYS_unlink:
        return "unlink";
    case SYS_gettimeofday:
        return "gettimeofday";
    case SYS_stat:
        return "stat";
    case SYS_getdents:
        return "getdents";
    case SYS_mkdir:
        return "mkdir";
    case SYS_rmdir:
        return "rmdir";
    case SYS_rename:
        return "rename";
    case SYS_truncate:
        return "truncate";
    case SYS_ftruncate:
        return "ftruncate";
    case SYS_clock_gettime:
        return "clock_gettime";
    default:
        return "unknown";
    }
}

static void strace_log(uintptr_t num, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t ret)
{
    /* Print one Linux-strace-like line after the syscall has produced GPRx. */
    switch (num)
    {
    case SYS_open:
        Log("strace: open(\"%s\", %d, %d) = %d", (char *)arg1, (int)arg2, (int)arg3, (int)ret);
        break;
    case SYS_read:
        Log("strace: read(%d, 0x%08" PRIxPTR ", %zu) = %d", (int)arg1, arg2, (size_t)arg3, (int)ret);
        break;
    case SYS_write:
        Log("strace: write(%d, 0x%08" PRIxPTR ", %zu) = %d", (int)arg1, arg2, (size_t)arg3, (int)ret);
        break;
    case SYS_close:
        Log("strace: close(%d) = %d", (int)arg1, (int)ret);
        break;
    case SYS_lseek:
        Log("strace: lseek(%d, %zu, %d) = %d", (int)arg1, (size_t)arg2, (int)arg3, (int)ret);
        break;
    case SYS_fstat:
        Log("strace: fstat(%d, 0x%08" PRIxPTR ") = %d", (int)arg1, arg2, (int)ret);
        break;
    case SYS_brk:
        Log("strace: brk(0x%08" PRIxPTR ") = %d", arg1, (int)ret);
        break;
    case SYS_execve:
        Log("strace: execve(\"%s\", 0x%08" PRIxPTR ", 0x%08" PRIxPTR ") = %d",
            (char *)arg1, arg2, arg3, (int)ret);
        break;
    case SYS_gettimeofday:
        Log("strace: gettimeofday(0x%08" PRIxPTR ", 0x%08" PRIxPTR ") = %d", arg1, arg2, (int)ret);
        break;
    case SYS_unlink:
        Log("strace: unlink(\"%s\") = %d", (char *)arg1, (int)ret);
        break;
    case SYS_stat:
        Log("strace: stat(\"%s\", 0x%08" PRIxPTR ") = %d", (char *)arg1, arg2, (int)ret);
        break;
    case SYS_getdents:
        Log("strace: getdents(%d, 0x%08" PRIxPTR ", %zu) = %d", (int)arg1, arg2, (size_t)arg3, (int)ret);
        break;
    case SYS_mkdir:
        Log("strace: mkdir(\"%s\", %d) = %d", (char *)arg1, (int)arg2, (int)ret);
        break;
    case SYS_rmdir:
        Log("strace: rmdir(\"%s\") = %d", (char *)arg1, (int)ret);
        break;
    case SYS_rename:
        Log("strace: rename(\"%s\", \"%s\") = %d", (char *)arg1, (char *)arg2, (int)ret);
        break;
    case SYS_truncate:
        Log("strace: truncate(\"%s\", %zu) = %d", (char *)arg1, (size_t)arg2, (int)ret);
        break;
    case SYS_ftruncate:
        Log("strace: ftruncate(%d, %zu) = %d", (int)arg1, (size_t)arg2, (int)ret);
        break;
    case SYS_clock_gettime:
        Log("strace: clock_gettime(%d, 0x%08" PRIxPTR ") = %d", (int)arg1, arg2, (int)ret);
        break;
    case SYS_yield:
        Log("strace: yield() = %d", (int)ret);
        break;
    default:
        Log("strace: %s(0x%08" PRIxPTR ", 0x%08" PRIxPTR ", 0x%08" PRIxPTR ") = %d",
            syscall_name(num), arg1, arg2, arg3, (int)ret);
        break;
    }
}
#endif

void do_syscall(Context *c)
{
    /* Abstract-machine traps pass the syscall number and first three arguments in
     * guest registers GPR1-GPR4.  Each case translates that minimal ABI into the
     * small Nanos service layer and writes the user-visible return value to GPRx.
     */
    const uintptr_t num = c->GPR1;
    const uintptr_t arg1 = c->GPR2;
    const uintptr_t arg2 = c->GPR3;
    const uintptr_t arg3 = c->GPR4;

    switch (num)
    {
    case SYS_exit:
    {
#ifdef STRACE
        /* SYS_exit halts before the common return-value trace can run. */
        Log("strace: exit(%d)", (int)arg1);
#endif

        halt(arg1);
        break;
    }

    case SYS_yield:
    {
        // yield();

        // Do not trigger another trap here, just request rescheduling.
        need_resched = 1;
        c->GPRx = 0;
        break;
    }

    case SYS_write:
    {
        const int fd = (int)arg1;
        const char *buf = (char *)arg2;
        const size_t count = (size_t)arg3;

        if (fd == 1 || fd == 2)
        {
            for (size_t i = 0; i < count; i++)
            {
                putch(buf[i]);
            }

            c->GPRx = count;
        }
        else
        {
            c->GPRx = fs_write(fd, buf, count);
        }

        break;
    }

    case SYS_brk:
    {
        // brk() returns 0 on success, -1 on failure in our convention.
        int mm_brk(uintptr_t brk);
        c->GPRx = mm_brk(arg1);
        break;
    }

    case SYS_open:
    {
        c->GPRx = fs_open((char *)arg1, (int)arg2, (int)arg3);
        break;
    }

    case SYS_read:
    {
        c->GPRx = fs_read((int)arg1, (void *)arg2, (size_t)arg3);
        break;
    }

    case SYS_close:
    {
        c->GPRx = fs_close((int)arg1);
        break;
    }

    case SYS_lseek:
    {
        c->GPRx = fs_lseek((int)arg1, (size_t)arg2, (int)arg3);
        break;
    }

    case SYS_fstat:
    {
        c->GPRx = fs_fstat((int)arg1, (NanosStat *)arg2);
        break;
    }

    case SYS_unlink:
    {
        c->GPRx = fs_unlink((const char *)arg1);
        break;
    }

    case SYS_stat:
    {
        c->GPRx = fs_stat((const char *)arg1, (NanosStat *)arg2);
        break;
    }

    case SYS_getdents:
    {
        c->GPRx = fs_getdents((int)arg1, (void *)arg2, (int)arg3);
        break;
    }

    case SYS_mkdir:
    {
        c->GPRx = fs_mkdir((const char *)arg1, (int)arg2);
        break;
    }

    case SYS_rmdir:
    {
        c->GPRx = fs_rmdir((const char *)arg1);
        break;
    }

    case SYS_rename:
    {
        c->GPRx = fs_rename((const char *)arg1, (const char *)arg2);
        break;
    }

    case SYS_truncate:
    {
        c->GPRx = fs_truncate((const char *)arg1, (size_t)arg2);
        break;
    }

    case SYS_ftruncate:
    {
        c->GPRx = fs_ftruncate((int)arg1, (size_t)arg2);
        break;
    }

    case SYS_time:
    {
        time_t *out = (time_t *)arg1;
        const time_t seconds = (time_t)(realtime_us() / 1000000);

        if (out)
        {
            *out = seconds;
        }

        /*
         * RV32 has only one normal syscall return register in this small ABI.
         * Newlib's time() uses gettimeofday(), so the pointer result above is
         * the 64-bit path; GPRx keeps a legacy low-word return for old callers.
         */
        c->GPRx = (uintptr_t)seconds;
        break;
    }

    case SYS_times:
    {
        NanosTms *tms = (NanosTms *)arg1;
        const uint64_t uptimeUs = monotonic_us();

        if (tms)
        {
            /*
             * RISC-V newlib sets CLOCKS_PER_SEC to 1000000, so one clock tick is
             * one microsecond.  Nanos-lite does not account per-process CPU time;
             * exposing monotonic guest time is enough for libc clock().
             */
            tms->tms_utime = (NanosClockTick)uptimeUs;
            tms->tms_stime = 0;
            tms->tms_cutime = 0;
            tms->tms_cstime = 0;
        }

        c->GPRx = (uintptr_t)(NanosClockTick)uptimeUs;
        break;
    }

    case SYS_gettimeofday:
    {
        struct timeval *tv = (struct timeval *)arg1;
        struct timezone *tz = (struct timezone *)arg2;
        const uint64_t nowUs = realtime_us();

        if (tv)
        {
            tv->tv_sec = (time_t)(nowUs / 1000000);
            tv->tv_usec = (suseconds_t)(nowUs % 1000000);
        }

        if (tz)
        {
            tz->tz_minuteswest = 0;
            tz->tz_dsttime = 0;
        }

        c->GPRx = 0;
        break;
    }

    case SYS_clock_gettime:
    {
        const clockid_t clockId = (clockid_t)arg1;
        struct timespec *tp = (struct timespec *)arg2;
        uint64_t us;

        if (tp == NULL)
        {
            c->GPRx = (uintptr_t)-NANOS_EFAULT;
            break;
        }

        if (clockId == CLOCK_REALTIME)
        {
            us = realtime_us();
        }
        else if (clockId == CLOCK_MONOTONIC)
        {
            us = monotonic_us();
        }
        else
        {
            c->GPRx = (uintptr_t)-NANOS_EINVAL;
            break;
        }

        tp->tv_sec = (time_t)(us / 1000000);
        tp->tv_nsec = (long)((us % 1000000) * 1000);
        c->GPRx = 0;
        break;
    }

    case SYS_execve:
    {
        const char *filename = (const char *)arg1;
        char *const *argv = (char *const *)arg2;
        char *const *envp = (char *const *)arg3;

        // Check existence, execvp will probe multiple candidates
        // Nanos-lite reports a small errno-like negative value here, but success
        // follows Unix execve semantics: the caller's image disappears and does
        // not receive a normal return value.
        int fd = fs_open((char *)filename, 0, 0);

        if (fd < 0)
        {
            c->GPRx = (uintptr_t)-2; // ENOENT
            break;
        }

        fs_close(fd);

        context_uload(current, filename, (char *const *)argv, (char *const *)envp);

        // Return through the freshly built user context instead of the old
        // syscall frame, which belongs to the image being replaced. For execve()
        // success there is no user-visible return value: the new program starts
        // from its entry point. This also must not write c->GPRx after
        // context_uload(), because the old syscall frame and the freshly-created
        // ucontext for the same PCB are both placed at the top of the same kernel
        // stack. Writing the old frame's a0 here would overwrite the new image's
        // initial a0, which crt0 uses as the user stack pointer.
        context_replaced = 1;
        return;
    }

    default:
    {
        panic("Unhandled syscall ID = %" PRIuPTR, num);
        break;
    }
    }

#ifdef STRACE
    /* Normal syscalls trace here so return values are always included. */
    strace_log(num, arg1, arg2, arg3, c->GPRx);
#endif
}
