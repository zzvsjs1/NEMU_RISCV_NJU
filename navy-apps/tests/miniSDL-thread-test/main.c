#include <SDL.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Newlib hides this POSIX declaration unless feature macros are enabled, but
 * Navy provides the function from libos for MT applications.
 */
int sched_yield(void);

#define CHECK(condition) \
    do \
    { \
        if (!(condition)) \
        { \
            fprintf(stderr, "CHECK failed: %s at %s:%d\n", #condition, \
                    __FILE__, __LINE__); \
            exit(EXIT_FAILURE); \
        } \
    } while (0)

enum
{
    WORKER_COUNT = 2,
    COUNTER_STEPS = 1000,
    STACK_DEPTH = 40,
    STACK_GUARD_BYTES = 192,
    HEAP_BYTES = 4096,
    SEQUENTIAL_WORKERS = 16,
    KILLED_WORKERS = 10
};

#define SPIN_LIMIT UINT32_C(100000000)

static SDL_mutex *shared_mutex;
static int shared_counter;

static volatile int spin_ready[WORKER_COUNT];
static volatile int saw_concurrent_worker[WORKER_COUNT];
static volatile int deep_stack_ready[WORKER_COUNT];
static volatile int heap_ready[WORKER_COUNT];
static volatile int errno_ready[WORKER_COUNT];
static volatile int worker_ok[WORKER_COUNT];
static volatile int worker_errno_after_yields[WORKER_COUNT];
static volatile int worker_stdio_result[WORKER_COUNT];
static volatile Uint32 worker_tid[WORKER_COUNT];
static void *volatile worker_allocation[WORKER_COUNT];
static volatile int killed_worker_started;
static volatile int completed_worker_count;

static int other_worker(int worker_index)
{
    return 1 - worker_index;
}

static int observe_timer_preemption(int worker_index)
{
    int other = other_worker(worker_index);

    /*
     * This phase deliberately makes no syscall.  The first callback cannot see
     * the second callback start unless the timer pre-empts its compute-bound
     * loop and the kernel schedules the other runnable task.
     */
    spin_ready[worker_index] = 1;
    for (uint32_t iteration = 0; iteration < SPIN_LIMIT; iteration++)
    {
        if (spin_ready[other])
        {
            saw_concurrent_worker[worker_index] = 1;
            return 1;
        }
    }

    return 0;
}

static int exercise_deep_stack(int worker_index, int depth)
{
    volatile unsigned char guard[STACK_GUARD_BYTES];
    unsigned char expected =
        (unsigned char)(0x31u + (unsigned int)(worker_index * 23 + depth));
    int ok = 1;

    for (size_t index = 0; index < sizeof(guard); index++)
    {
        guard[index] = expected;
    }

    if (depth > 0)
    {
        ok = exercise_deep_stack(worker_index, depth - 1);
    }
    else
    {
        int other = other_worker(worker_index);

        /*
         * Both callbacks wait at maximum depth, keeping every guard frame live
         * at once.  Reusing one user stack for both tasks corrupts these guards
         * (and commonly the saved return state) as soon as the peer runs.
         */
        deep_stack_ready[worker_index] = 1;
        while (!deep_stack_ready[other])
        {
            if (sched_yield() != 0)
            {
                ok = 0;
            }
        }

        for (int turn = 0; turn < 8; turn++)
        {
            if (sched_yield() != 0)
            {
                ok = 0;
            }
        }
    }

    for (size_t index = 0; index < sizeof(guard); index++)
    {
        if (guard[index] != expected)
        {
            ok = 0;
        }
    }

    return ok;
}

static int exercise_heap_isolation(int worker_index)
{
    int other = other_worker(worker_index);
    unsigned char expected = (unsigned char)(0x80u + (unsigned int)worker_index);
    unsigned char *allocation = malloc(HEAP_BYTES);
    int ok = allocation != NULL;

    worker_allocation[worker_index] = allocation;
    if (allocation != NULL)
    {
        memset(allocation, expected, HEAP_BYTES);
    }

    /*
     * Publish even a failed allocation so the peer cannot wait forever.  On a
     * healthy run both buffers remain live across several scheduling points.
     */
    heap_ready[worker_index] = 1;
    while (!heap_ready[other])
    {
        if (sched_yield() != 0)
        {
            ok = 0;
        }
    }

    for (int turn = 0; turn < 16; turn++)
    {
        if (sched_yield() != 0)
        {
            ok = 0;
        }
    }

    if (allocation != NULL)
    {
        if (worker_allocation[other] == allocation)
        {
            ok = 0;
        }

        for (size_t index = 0; index < HEAP_BYTES; index++)
        {
            if (allocation[index] != expected)
            {
                ok = 0;
            }
        }
    }

    free(allocation);
    return ok;
}

static int exercise_thread_local_errno(int worker_index)
{
    int other = other_worker(worker_index);
    int expected = 700 + worker_index;
    int ok = 1;

    errno = expected;
    errno_ready[worker_index] = 1;
    while (!errno_ready[other])
    {
        if (sched_yield() != 0)
        {
            ok = 0;
        }
    }

    /*
     * If both callbacks still use newlib's process-global reentrancy record,
     * the peer's distinct assignment replaces this value before we read it.
     */
    for (int turn = 0; turn < 16; turn++)
    {
        if (sched_yield() != 0)
        {
            ok = 0;
        }
    }
    worker_errno_after_yields[worker_index] = errno;

    return ok && errno == expected;
}

static int increment_with_forced_contention(void)
{
    for (int step = 0; step < COUNTER_STEPS; step++)
    {
        int next_value;

        if (SDL_LockMutex(shared_mutex) != 0)
        {
            return 0;
        }

        next_value = shared_counter + 1;
        /*
         * Yielding between the read and write turns a no-op mutex into a
         * deterministic lost update while a real mutex keeps the count exact.
         */
        if (sched_yield() != 0)
        {
            (void)SDL_UnlockMutex(shared_mutex);
            return 0;
        }
        shared_counter = next_value;

        if (SDL_UnlockMutex(shared_mutex) != 0)
        {
            return 0;
        }
    }

    return 1;
}

static int main_worker(void *argument)
{
    int worker_index = (int)(intptr_t)argument;
    int ok = 1;

    /*
     * Run the no-syscall phase before SDL_ThreadID(), since obtaining the ID is
     * itself a kernel call on Navy and must not be what lets the peer run.
     */
    if (!observe_timer_preemption(worker_index))
    {
        ok = 0;
    }

    worker_tid[worker_index] = SDL_ThreadID();
    worker_stdio_result[worker_index] =
        printf("miniSDL worker %d stdio check\n", worker_index);

    if (worker_stdio_result[worker_index] < 0)
    {
        ok = 0;
    }

    if (!exercise_deep_stack(worker_index, STACK_DEPTH))
    {
        ok = 0;
    }
    if (!exercise_heap_isolation(worker_index))
    {
        ok = 0;
    }
    if (!exercise_thread_local_errno(worker_index))
    {
        ok = 0;
    }
    if (!increment_with_forced_contention())
    {
        ok = 0;
    }

    worker_ok[worker_index] = ok;
    return 40 + worker_index;
}

static int slot_reuse_worker(void *argument)
{
    int sequence = (int)(intptr_t)argument;
    volatile unsigned char stack_marker[64];

    /*
     * Touch the newly assigned stack as well as returning a unique join value,
     * so each sequential creation exercises both reusable pool records.
     */
    for (size_t index = 0; index < sizeof(stack_marker); index++)
    {
        stack_marker[index] = (unsigned char)(sequence + (int)index);
    }

    return 100 + sequence + stack_marker[sequence % (int)sizeof(stack_marker)] -
           (sequence + sequence % (int)sizeof(stack_marker));
}

static int worker_waiting_to_be_killed(void *argument)
{
    (void)argument;
    killed_worker_started++;

    for (;;)
    {
        (void)sched_yield();
    }
}

static int worker_finishing_before_kill(void *argument)
{
    (void)argument;
    completed_worker_count++;
    return 77;
}

static void check_kill_after_completion_reaps(void)
{
    /*
     * SDL_KillThread is allowed to race normal callback completion.  Repeating
     * beyond the handle-pool size catches a path that reports ESRCH for the
     * already-zombie task and forgets to join/reclaim its SDL handle.
     */
    for (int sequence = 0; sequence < KILLED_WORKERS; sequence++)
    {
        SDL_Thread *thread =
            SDL_CreateThread(worker_finishing_before_kill, NULL);

        CHECK(thread != NULL);

        while (completed_worker_count <= sequence)
        {
            CHECK(sched_yield() == 0);
        }

        /*
         * The worker does not yield after publishing completion.  One complete
         * round gives it time to enter SYS_thread_exit before kill is attempted.
         */
        CHECK(sched_yield() == 0);
        SDL_KillThread(thread);
    }
}

static void check_kill_and_slot_reuse(void)
{
    Uint32 previous_tid = 0;

    /*
     * Run beyond the seven-handle pool size.  If SDL_KillThread fails to reap
     * either the kernel task or MiniSDL handle, a later creation must fail.
     */
    for (int sequence = 0; sequence < KILLED_WORKERS; sequence++)
    {
        SDL_Thread *thread = SDL_CreateThread(worker_waiting_to_be_killed,
                                               NULL);
        Uint32 tid;

        CHECK(thread != NULL);
        tid = SDL_GetThreadID(thread);
        CHECK(tid != 0);
        CHECK(tid != previous_tid);

        while (killed_worker_started <= sequence)
        {
            CHECK(sched_yield() == 0);
        }

        SDL_KillThread(thread);
        previous_tid = tid;
    }
}

static void check_sequential_slot_reuse(void)
{
    Uint32 previous_tid = 0;

    /*
     * Eight live task slots include the main task, so sixteen create/join
     * cycles cannot succeed unless joined kernel and miniSDL slots are reaped.
     */
    for (int sequence = 0; sequence < SEQUENTIAL_WORKERS; sequence++)
    {
        SDL_Thread *thread =
            SDL_CreateThread(slot_reuse_worker, (void *)(intptr_t)sequence);
        int status = -1;
        Uint32 tid;

        CHECK(thread != NULL);
        tid = SDL_GetThreadID(thread);
        CHECK(tid != 0);
        CHECK(tid != previous_tid);

        SDL_WaitThread(thread, &status);
        CHECK(status == 100 + sequence);
        previous_tid = tid;
    }
}

int main(void)
{
    SDL_Thread *workers[WORKER_COUNT];
    Uint32 created_tid[WORKER_COUNT];
    int worker_status[WORKER_COUNT] = {-1, -1};

    CHECK(SDL_Init(0) == 0);

    shared_mutex = SDL_CreateMutex();
    CHECK(shared_mutex != NULL);

    for (int worker = 0; worker < WORKER_COUNT; worker++)
    {
        workers[worker] =
            SDL_CreateThread(main_worker, (void *)(intptr_t)worker);
        CHECK(workers[worker] != NULL);
        created_tid[worker] = SDL_GetThreadID(workers[worker]);
        CHECK(created_tid[worker] != 0);
    }
    CHECK(created_tid[0] != created_tid[1]);

    for (int worker = 0; worker < WORKER_COUNT; worker++)
    {
        SDL_WaitThread(workers[worker], &worker_status[worker]);
    }

    CHECK(worker_tid[0] != 0);
    CHECK(worker_tid[0] != worker_tid[1]);
    CHECK(created_tid[0] == worker_tid[0]);
    CHECK(created_tid[1] == worker_tid[1]);
    CHECK(worker_status[0] == 40);
    CHECK(worker_status[1] == 41);
    CHECK(shared_counter == 2000);
    CHECK(saw_concurrent_worker[0] && saw_concurrent_worker[1]);
    CHECK(worker_errno_after_yields[0] == 700);
    CHECK(worker_errno_after_yields[1] == 701);
    CHECK(worker_stdio_result[0] > 0);
    CHECK(worker_stdio_result[1] > 0);
    CHECK(worker_ok[0] && worker_ok[1]);

    check_kill_after_completion_reaps();
    check_kill_and_slot_reuse();
    check_sequential_slot_reuse();

    SDL_DestroyMutex(shared_mutex);
    SDL_Quit();
    CHECK(printf("miniSDL main stdio check\n") > 0);
    puts("miniSDL-thread-test PASS");
    return 0;
}
