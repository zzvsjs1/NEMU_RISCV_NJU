#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include <reent.h>

#include "thread_runtime.h"

#ifndef _REENT_GLOBAL_STDIO_STREAMS
#error "Navy workers must share process-wide stdin, stdout, and stderr streams"
#endif

/*
 * These literals are intentionally independent of syscall.h.  If an existing
 * syscall is accidentally renumbered, or a wrapper dispatches the wrong
 * operation, comparing two uses of the same enum would hide the ABI break.
 */
enum
{
    EXPECTED_SYS_YIELD = 1,
    EXPECTED_SYS_THREAD_CREATE = 28,
    EXPECTED_SYS_THREAD_JOIN = 30,
    EXPECTED_SYS_THREAD_SELF = 31,
    EXPECTED_SYS_THREAD_KILL = 32,
    EXPECTED_SYS_MUTEX_LOCK = 33,
    EXPECTED_SYS_MUTEX_UNLOCK = 34,
    MAX_FAKE_CALLS = 64,
};

int sched_yield(void);

typedef struct
{
    intptr_t number;
    intptr_t arguments[3];
} FakeSyscall;

static FakeSyscall fake_calls[MAX_FAKE_CALLS];
static int fake_call_count;
static bool fake_call_overflow;
static intptr_t fake_default_result;
static int fake_current_tid;

/*
 * The runtime keeps the initially active thread on newlib's already
 * initialised global record.  The host test only needs its identity and errno
 * storage, so zero initialisation is sufficient and avoids linking newlib.
 */
static struct _reent fake_main_reent;
struct _reent *_impure_ptr = &fake_main_reent;
struct _reent *const _global_impure_ptr = &fake_main_reent;
__FILE __sf[3];
static int reclaim_count;
static struct _reent *last_reclaimed;

/*
 * A successful join may reclaim a worker's newlib state.  Reclamation itself
 * belongs to newlib; this fake keeps the host test focused on libos's record
 * selection and syscall boundary.
 */
void _reclaim_reent(struct _reent *reent)
{
    reclaim_count++;
    last_reclaimed = reent;
}

static void reset_fake_calls(void)
{
    fake_call_count = 0;
    fake_call_overflow = false;
    fake_default_result = 0;
}

/*
 * This is the only external boundary replaced by the test.  The real RV64
 * trampoline cannot execute in a host process, so recording its complete input
 * verifies the behaviour libos presents to the kernel without duplicating any
 * runtime state transitions.
 */
intptr_t _syscall_(intptr_t number, intptr_t arg0, intptr_t arg1, intptr_t arg2)
{
    if (fake_call_count < MAX_FAKE_CALLS)
    {
        FakeSyscall *call = &fake_calls[fake_call_count];
        call->number = number;
        call->arguments[0] = arg0;
        call->arguments[1] = arg1;
        call->arguments[2] = arg2;
        fake_call_count++;
    }
    else
    {
        fake_call_overflow = true;
    }

    if (number == EXPECTED_SYS_THREAD_SELF)
    {
        return fake_current_tid;
    }

    return fake_default_result;
}

#define CHECK(condition)                                                                  \
    do                                                                                    \
    {                                                                                     \
        if (!(condition))                                                                 \
        {                                                                                 \
            printf("check failed at line %d: %s\n", __LINE__, #condition);                \
            return 1;                                                                     \
        }                                                                                 \
    } while (0)

static int expect_one_syscall(intptr_t number, intptr_t arg0, intptr_t arg1, intptr_t arg2)
{
    CHECK(!fake_call_overflow);
    CHECK(fake_call_count == 1);
    CHECK(fake_calls[0].number == number);
    CHECK(fake_calls[0].arguments[0] == arg0);
    CHECK(fake_calls[0].arguments[1] == arg1);
    CHECK(fake_calls[0].arguments[2] == arg2);
    return 0;
}

static void fake_worker_entry(void *argument)
{
    (void)argument;
}

/*
 * Failure caught: linking ordinary single-threaded Navy applications starts
 * issuing MT-only mutex syscalls that old nanos-lite does not understand.
 */
static int test_locks_and_reentrancy_make_no_syscalls_before_activation(void)
{
    static int lock_key;

    reset_fake_calls();

    CHECK(__nanos_getreent() == _impure_ptr);
    CHECK(nanos_lock_acquire(&lock_key, false, false) == 0);
    CHECK(nanos_lock_acquire(&lock_key, true, false) == 0);
    CHECK(nanos_lock_acquire(&lock_key, false, true) == 0);
    CHECK(nanos_lock_release(&lock_key) == 0);
    CHECK(!fake_call_overflow);
    CHECK(fake_call_count == 0);
    return 0;
}

/*
 * Failure caught: activation either assumes a fixed main TID or reassigns the
 * main record when enable is called a second time.
 */
static int test_activation_records_the_main_tid_once(void)
{
    struct _reent *main_reent;

    fake_current_tid = 73;
    reset_fake_calls();

    CHECK(nanos_thread_runtime_enable() == 0);
    CHECK(expect_one_syscall(EXPECTED_SYS_THREAD_SELF, 0, 0, 0) == 0);

    main_reent = __nanos_getreent();
    CHECK(main_reent == _impure_ptr);

    /*
     * Activation is one-way.  A later caller may be a worker, so it must not
     * replace the recorded main TID or make another activation syscall.
     */
    fake_current_tid = 74;
    reset_fake_calls();
    CHECK(nanos_thread_runtime_enable() == 0);
    CHECK(!fake_call_overflow);
    CHECK(fake_call_count == 0);

    fake_current_tid = 73;
    CHECK(__nanos_getreent() == main_reent);
    return 0;
}

/*
 * Failure caught: a wrapper dispatches an existing syscall, swaps arguments,
 * or drops the caller-provided entry, stack, TID, or status address.
 */
static int test_thread_wrappers_use_the_appended_syscall_abi(void)
{
    int argument = 19;
    int status = -1;
    unsigned char stack[32];
    int result;

    reset_fake_calls();
    fake_default_result = 401;
    result = nanos_thread_create(fake_worker_entry, &argument, stack + sizeof(stack));
    CHECK(result == 401);
    CHECK(expect_one_syscall(EXPECTED_SYS_THREAD_CREATE,
                             (intptr_t)fake_worker_entry,
                             (intptr_t)&argument,
                             (intptr_t)(stack + sizeof(stack))) == 0);

    reset_fake_calls();
    fake_default_result = 0;
    result = nanos_thread_join(401, &status);
    CHECK(result == 0);
    CHECK(expect_one_syscall(EXPECTED_SYS_THREAD_JOIN,
                             401,
                             (intptr_t)&status,
                             0) == 0);

    fake_current_tid = 73;
    reset_fake_calls();
    result = nanos_thread_self();
    CHECK(result == 73);
    CHECK(expect_one_syscall(EXPECTED_SYS_THREAD_SELF, 0, 0, 0) == 0);

    reset_fake_calls();
    fake_default_result = 0;
    result = nanos_thread_kill(401);
    CHECK(result == 0);
    CHECK(expect_one_syscall(EXPECTED_SYS_THREAD_KILL, 401, 0, 0) == 0);
    return 0;
}

/*
 * Failure caught: recursive and try-only are collapsed into one flag, swapped,
 * or omitted while forwarding the mutex lock syscall.
 */
static int test_recursive_and_try_flags_reach_mutex_lock_independently(void)
{
    static int lock_key;

    reset_fake_calls();
    CHECK(nanos_lock_acquire(&lock_key, true, false) == 0);
    CHECK(expect_one_syscall(EXPECTED_SYS_MUTEX_LOCK,
                             (intptr_t)&lock_key,
                             1,
                             0) == 0);

    reset_fake_calls();
    CHECK(nanos_lock_acquire(&lock_key, false, true) == 0);
    CHECK(expect_one_syscall(EXPECTED_SYS_MUTEX_LOCK,
                             (intptr_t)&lock_key,
                             0,
                             1) == 0);

    reset_fake_calls();
    CHECK(nanos_lock_release(&lock_key) == 0);
    CHECK(expect_one_syscall(EXPECTED_SYS_MUTEX_UNLOCK,
                             (intptr_t)&lock_key,
                             0,
                             0) == 0);
    return 0;
}

/*
 * Failure caught: userspace back-pressure loops spin forever or fail to link
 * because the established SYS_yield service has no POSIX-facing wrapper.
 */
static int test_sched_yield_uses_existing_syscall(void)
{
    reset_fake_calls();
    CHECK(sched_yield() == 0);
    CHECK(expect_one_syscall(EXPECTED_SYS_YIELD, 0, 0, 0) == 0);
    return 0;
}

/*
 * Failure caught: all workers share one _reent record, or lookup uses a table
 * slot rather than the stable kernel TID.  _errno is the field backing
 * newlib's public errno macro.
 */
static int test_distinct_worker_tids_keep_distinct_reent_and_errno(void)
{
    struct _reent *worker_a;
    struct _reent *worker_b;

    _impure_ptr->_errno = 303;

    fake_current_tid = 1001;
    worker_a = __nanos_getreent();
    CHECK(worker_a != NULL);
    CHECK(worker_a != _impure_ptr);
    worker_a->_errno = 111;

    fake_current_tid = 2002;
    worker_b = __nanos_getreent();
    CHECK(worker_b != NULL);
    CHECK(worker_b != _impure_ptr);
    CHECK(worker_b != worker_a);
    worker_b->_errno = 222;

    fake_current_tid = 1001;
    CHECK(__nanos_getreent() == worker_a);
    CHECK(__nanos_getreent()->_errno == 111);

    fake_current_tid = 2002;
    CHECK(__nanos_getreent() == worker_b);
    CHECK(__nanos_getreent()->_errno == 222);

    fake_current_tid = 73;
    CHECK(__nanos_getreent() == _impure_ptr);
    CHECK(__nanos_getreent()->_errno == 303);
    return 0;
}

/*
 * Failure caught: kill and exit both reclaim a worker record, then join
 * reclaims the same newlib allocations a second time.  Join is the one
 * lifecycle operation that proves no task can still use the record.
 */
static int test_join_is_the_only_reent_cleanup_owner(void)
{
    struct _reent *worker;
    const int worker_tid = 3003;

    fake_current_tid = worker_tid;
    worker = __nanos_getreent();
    CHECK(worker != NULL);
    CHECK(worker != _impure_ptr);

    reclaim_count = 0;
    last_reclaimed = NULL;
    fake_current_tid = 73;

    reset_fake_calls();
    CHECK(nanos_thread_kill(worker_tid) == 0);
    CHECK(reclaim_count == 0);
    CHECK(last_reclaimed == NULL);

    reset_fake_calls();
    CHECK(nanos_thread_join(worker_tid, NULL) == 0);
    CHECK(reclaim_count == 1);
    CHECK(last_reclaimed == worker);
    return 0;
}

int main(void)
{
    if (test_locks_and_reentrancy_make_no_syscalls_before_activation() != 0)
    {
        return 1;
    }

    if (test_activation_records_the_main_tid_once() != 0)
    {
        return 1;
    }

    if (test_thread_wrappers_use_the_appended_syscall_abi() != 0)
    {
        return 1;
    }

    if (test_recursive_and_try_flags_reach_mutex_lock_independently() != 0)
    {
        return 1;
    }

    if (test_sched_yield_uses_existing_syscall() != 0)
    {
        return 1;
    }

    if (test_distinct_worker_tids_keep_distinct_reent_and_errno() != 0)
    {
        return 1;
    }

    if (test_join_is_the_only_reent_cleanup_owner() != 0)
    {
        return 1;
    }

    puts("thread-runtime tests passed");
    return 0;
}
