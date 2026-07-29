#include "thread_runtime.h"

#include <stdint.h>
#include <string.h>

#include "syscall.h"

/*
 * libos's assembly syscall boundary is intentionally kept private from public
 * application headers.  The thread runtime is another libos component, so it
 * calls the same raw boundary directly and leaves policy/error translation to
 * its callers.
 */
extern intptr_t _syscall_(intptr_t type, intptr_t arg0, intptr_t arg1, intptr_t arg2);

enum
{
    /*
     * nanos-lite-mt currently permits eight tasks per process, including the
     * main task.  Eight worker records therefore cover the full kernel limit
     * with one spare relative to the current seven-worker maximum.
     */
    NANOS_REENT_RECORDS = 8,
    NANOS_REENT_UNUSED_TID = 0,
};

typedef struct
{
    /*
     * Publish tid only after record has been completely initialised.  Readers
     * may then perform the common lookup without taking the table mutex.
     */
    int tid;
    struct _reent record;
} NanosReentRecord;

static bool runtime_enabled;
static int main_tid;
static int reent_table_lock_key;
static NanosReentRecord reent_records[NANOS_REENT_RECORDS];

/*
 * A compiler barrier is sufficient here because RV64 NEMU runs one emulated
 * hart.  Kernel scheduling may interrupt between C statements, but a second
 * CPU cannot observe hardware-level memory reordering concurrently.
 */
static void compiler_barrier(void)
{
    __asm__ volatile("" ::: "memory");
}

static NanosReentRecord *find_reent_record(int tid)
{
    for (int index = 0; index < NANOS_REENT_RECORDS; index++)
    {
        if (reent_records[index].tid == tid)
        {
            return &reent_records[index];
        }
    }

    return NULL;
}

/*
 * Allocate from fixed storage rather than malloc.  Calling malloc while
 * selecting malloc's own _reent record would recurse through both
 * __getreent() and the newlib malloc lock.
 */
static struct _reent *allocate_reent_record(int tid)
{
    NanosReentRecord *slot = NULL;

    if (nanos_lock_acquire(&reent_table_lock_key, false, false) != 0)
    {
        /*
         * The MT kernel reserves ample mutex slots for libc.  Returning the
         * already valid global record is the safest degradation if kernel
         * mutex allocation nevertheless fails.
         */
        return _impure_ptr;
    }

    /*
     * Another thread may have installed this TID while the caller waited for
     * the table mutex, so repeat the lookup under the lock.
     */
    slot = find_reent_record(tid);
    if (slot == NULL)
    {
        for (int index = 0; index < NANOS_REENT_RECORDS; index++)
        {
            if (reent_records[index].tid == NANOS_REENT_UNUSED_TID)
            {
                slot = &reent_records[index];

                /*
                 * Keep the slot visibly unused until initialisation finishes.
                 * The table mutex prevents a normally pre-empted allocator from
                 * selecting it twice.  If this task is killed, the kernel
                 * releases its mutex and the still-zero slot can be safely
                 * initialised again by a later worker instead of leaking a
                 * permanent "reserved" record.
                 */
                _REENT_INIT_PTR(&slot->record);
                compiler_barrier();
                slot->tid = tid;
                break;
            }
        }
    }

    (void)nanos_lock_release(&reent_table_lock_key);

    /*
     * Exhaustion cannot occur while the user/kernel thread limits agree.  Use
     * the main record as a valid last resort instead of returning NULL to
     * newlib, which would immediately dereference it.
     */
    return slot != NULL ? &slot->record : _impure_ptr;
}

/*
 * Reclaim a record only after its thread has exited or has been killed.  The
 * quick lock-free scan avoids adding mutex syscalls to joins whose target
 * never touched newlib.
 */
static void release_reent_record(int tid)
{
    NanosReentRecord *slot;

    if (tid <= NANOS_REENT_UNUSED_TID || find_reent_record(tid) == NULL)
    {
        return;
    }

    if (nanos_lock_acquire(&reent_table_lock_key, false, false) != 0)
    {
        return;
    }

    slot = find_reent_record(tid);
    if (slot != NULL)
    {
        /*
         * Keep the TID published while _reclaim_reent() returns allocations to
         * newlib.  A reentrant cleanup path can then still select this record
         * without trying to acquire the table mutex recursively.  No ordinary
         * code from the stopped target can run concurrently on a single hart.
         */
        _reclaim_reent(&slot->record);
        memset(&slot->record, 0, sizeof(slot->record));
        compiler_barrier();
        slot->tid = NANOS_REENT_UNUSED_TID;
    }

    (void)nanos_lock_release(&reent_table_lock_key);
}

int nanos_thread_runtime_enable(void)
{
    int tid;

    if (runtime_enabled)
    {
        return 0;
    }

    tid = (int)_syscall_(SYS_thread_self, 0, 0, 0);
    if (tid < 0)
    {
        return tid;
    }

    main_tid = tid;
    compiler_barrier();
    runtime_enabled = true;
    return 0;
}

int nanos_thread_create(void (*entry)(void *), void *argument, void *stack_top)
{
    return (int)_syscall_(SYS_thread_create,
                          (intptr_t)entry,
                          (intptr_t)argument,
                          (intptr_t)stack_top);
}

void nanos_thread_exit(int status)
{
    (void)_syscall_(SYS_thread_exit, (intptr_t)status, 0, 0);

    /*
     * A successful exit never returns.  If a malformed or legacy kernel does
     * return, remain outside libc and yield forever rather than executing on a
     * stack whose SDL ownership may already have ended.
     */
    for (;;)
    {
        (void)_syscall_(SYS_yield, 0, 0, 0);
    }
}

int nanos_thread_join(int tid, int *status)
{
    int result = (int)_syscall_(SYS_thread_join, (intptr_t)tid, (intptr_t)status, 0);

    if (result == 0)
    {
        /*
         * Join is the sole cleanup owner.  It proves the target cannot resume,
         * and the kernel permits only one successful join.  Cleaning from the
         * target's exit path or from kill would allow a timer interruption
         * inside _reclaim_reent() to race a second cleanup and double-free
         * newlib-owned buffers.
         */
        release_reent_record(tid);
    }

    return result;
}

int nanos_thread_self(void)
{
    return (int)_syscall_(SYS_thread_self, 0, 0, 0);
}

int nanos_thread_kill(int tid)
{
    return (int)_syscall_(SYS_thread_kill, (intptr_t)tid, 0, 0);
}

int nanos_lock_acquire(const void *key, bool recursive, bool try_only)
{
    if (!runtime_enabled)
    {
        return 0;
    }

    return (int)_syscall_(SYS_mutex_lock,
                          (intptr_t)key,
                          recursive ? 1 : 0,
                          try_only ? 1 : 0);
}

int nanos_lock_release(const void *key)
{
    if (!runtime_enabled)
    {
        return 0;
    }

    return (int)_syscall_(SYS_mutex_unlock, (intptr_t)key, 0, 0);
}

struct _reent *__nanos_getreent(void)
{
    int tid;
    NanosReentRecord *slot;

    if (!runtime_enabled)
    {
        return _impure_ptr;
    }

    tid = nanos_thread_self();
    if (tid < 0 || tid == main_tid)
    {
        return _impure_ptr;
    }

    slot = find_reent_record(tid);
    if (slot != NULL)
    {
        return &slot->record;
    }

    return allocate_reent_record(tid);
}

int sched_yield(void)
{
    /*
     * SYS_yield predates the MT ABI and is understood by both Nanos-lite
     * targets.  Providing the POSIX spelling lets audio back-pressure and
     * worker code yield from user mode without nesting a trap in the kernel.
     */
    return (int)_syscall_(SYS_yield, 0, 0, 0);
}
