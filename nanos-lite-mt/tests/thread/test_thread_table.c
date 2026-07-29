#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "thread-table.h"

#define CHECK(condition)                                                        \
    do                                                                          \
    {                                                                           \
        if (!(condition))                                                       \
        {                                                                       \
            printf("check failed at line %d: %s\n", __LINE__, #condition);      \
            return 1;                                                           \
        }                                                                       \
    } while (0)

/*
 * A change that lets one process consume another process's thread allowance
 * must fail this test. TIDs are intentionally checked independently from table
 * slots because the kernel ABI exposes only TIDs to user space.
 */
static int test_allocation_is_bounded_per_process(void)
{
    MtThreadTable table;
    int slots[MT_THREADS_PER_PROCESS];

    mt_thread_table_init(&table);
    slots[0] = mt_thread_register_main(&table, 0);
    CHECK(slots[0] >= 0);
    CHECK(table.threads[slots[0]].tid > 0);

    for (int i = 1; i < MT_THREADS_PER_PROCESS; i++)
    {
        slots[i] = mt_thread_allocate(&table, 0);
        CHECK(slots[i] >= 0);
        CHECK(table.threads[slots[i]].process_id == 0);
        CHECK(table.threads[slots[i]].state == MT_THREAD_RUNNABLE);
        CHECK(table.threads[slots[i]].tid != table.threads[slots[0]].tid);
    }

    CHECK(mt_thread_allocate(&table, 0) == -1);

    /*
     * Exhausting process zero must not consume process one's allowance. This
     * protects the PCB/device-owner boundary from becoming a global task quota.
     */
    int other_main = mt_thread_register_main(&table, 1);
    CHECK(other_main >= 0);
    CHECK(table.threads[other_main].process_id == 1);
    return 0;
}

/*
 * A scheduler mutation that selects a blocked task, crosses a process boundary,
 * or restarts from slot zero instead of round-robin order must fail here.
 */
static int test_round_robin_skips_blocked_and_foreign_tasks(void)
{
    MtThreadTable table;
    mt_thread_table_init(&table);

    int main0 = mt_thread_register_main(&table, 0);
    int worker0a = mt_thread_allocate(&table, 0);
    int worker0b = mt_thread_allocate(&table, 0);
    int main1 = mt_thread_register_main(&table, 1);

    CHECK(mt_thread_pick_next(&table, 0, main0) == worker0a);
    CHECK(mt_thread_pick_next(&table, 0, worker0a) == worker0b);
    CHECK(mt_thread_pick_next(&table, 0, worker0b) == main0);
    CHECK(mt_thread_pick_next(&table, 1, main0) == main1);

    table.threads[worker0a].state = MT_THREAD_BLOCKED_JOIN;
    CHECK(mt_thread_pick_next(&table, 0, main0) == worker0b);

    table.threads[main0].state = MT_THREAD_ZOMBIE;
    table.threads[worker0b].state = MT_THREAD_BLOCKED_MUTEX;
    CHECK(mt_thread_pick_next(&table, 0, worker0a) == -1);
    return 0;
}

/*
 * This test catches lost join wakeups and early task-slot reuse. The exiting
 * task remains joined-zombie until the scheduler has moved off its kernel stack.
 */
static int test_join_blocks_then_receives_exit_status(void)
{
    MtThreadTable table;
    mt_thread_table_init(&table);

    int main_slot = mt_thread_register_main(&table, 0);
    int worker_slot = mt_thread_allocate(&table, 0);
    int worker_tid = table.threads[worker_slot].tid;

    MtJoinResult join = mt_thread_join(&table, main_slot, worker_tid);
    CHECK(join.kind == MT_JOIN_BLOCKED);
    CHECK(join.target_slot == worker_slot);
    CHECK(table.threads[main_slot].state == MT_THREAD_BLOCKED_JOIN);

    MtFinishResult finish = mt_thread_finish(&table, worker_slot, 37);
    CHECK(finish.woken_slot == main_slot);
    CHECK(finish.status == 37);
    CHECK(table.threads[main_slot].state == MT_THREAD_RUNNABLE);
    CHECK(table.threads[worker_slot].state == MT_THREAD_ZOMBIE_JOINED);

    mt_thread_reap(&table, worker_slot);
    CHECK(table.threads[worker_slot].state == MT_THREAD_UNUSED);
    return 0;
}

/*
 * A zombie must remain joinable, but a completed join must free capacity.
 * Repeating beyond the per-process limit proves slots are genuinely reusable.
 */
static int test_zombie_join_and_sequential_slot_reuse(void)
{
    MtThreadTable table;
    mt_thread_table_init(&table);
    int main_slot = mt_thread_register_main(&table, 0);

    for (int iteration = 0; iteration < MT_THREADS_PER_PROCESS * 3; iteration++)
    {
        int worker_slot = mt_thread_allocate(&table, 0);
        CHECK(worker_slot >= 0);
        int worker_tid = table.threads[worker_slot].tid;

        MtFinishResult finish = mt_thread_finish(&table, worker_slot, 100 + iteration);
        CHECK(finish.woken_slot == -1);
        CHECK(table.threads[worker_slot].state == MT_THREAD_ZOMBIE);

        MtJoinResult join = mt_thread_join(&table, main_slot, worker_tid);
        CHECK(join.kind == MT_JOIN_READY);
        CHECK(join.target_slot == worker_slot);
        CHECK(join.status == 100 + iteration);

        mt_thread_reap(&table, worker_slot);
        CHECK(table.threads[worker_slot].state == MT_THREAD_UNUSED);
    }

    return 0;
}

/*
 * These invalid relationships would otherwise deadlock the only runnable
 * process: self-join, cross-process join, and a second waiter are all rejected.
 */
static int test_invalid_and_double_join_are_rejected(void)
{
    MtThreadTable table;
    mt_thread_table_init(&table);

    int main0 = mt_thread_register_main(&table, 0);
    int worker0 = mt_thread_allocate(&table, 0);
    int second0 = mt_thread_allocate(&table, 0);
    int main1 = mt_thread_register_main(&table, 1);
    int worker_tid = table.threads[worker0].tid;

    CHECK(mt_thread_join(&table, main0, table.threads[main0].tid).kind == MT_JOIN_ERROR);
    CHECK(mt_thread_join(&table, main0, 999999).kind == MT_JOIN_ERROR);
    CHECK(mt_thread_join(&table, main1, worker_tid).kind == MT_JOIN_ERROR);
    CHECK(mt_thread_join(&table, main0, worker_tid).kind == MT_JOIN_BLOCKED);
    CHECK(mt_thread_join(&table, second0, worker_tid).kind == MT_JOIN_ERROR);
    return 0;
}

/*
 * A join dependency cycle would leave the process with no runnable task and
 * turn a userspace error into a scheduler panic.  Reject the edge that closes
 * the cycle while leaving its caller runnable.
 */
static int test_join_cycle_is_rejected(void)
{
    MtThreadTable table;
    mt_thread_table_init(&table);

    int main_slot = mt_thread_register_main(&table, 0);
    int worker_slot = mt_thread_allocate(&table, 0);
    int main_tid = table.threads[main_slot].tid;
    int worker_tid = table.threads[worker_slot].tid;

    CHECK(mt_thread_join(&table, main_slot, worker_tid).kind ==
          MT_JOIN_BLOCKED);
    CHECK(table.threads[main_slot].state == MT_THREAD_BLOCKED_JOIN);

    CHECK(mt_thread_join(&table, worker_slot, main_tid).kind ==
          MT_JOIN_ERROR);
    CHECK(table.threads[worker_slot].state == MT_THREAD_RUNNABLE);
    CHECK(table.threads[main_slot].join_waiter_slot == -1);
    return 0;
}

/*
 * The hand-off assertion catches an unlock implementation that merely clears
 * the owner and leaves a waiter asleep. Try-lock must never block its caller.
 */
static int test_mutex_handoff_trylock_and_wrong_owner(void)
{
    const uintptr_t key = 0x40001230u;
    MtThreadTable table;
    mt_thread_table_init(&table);

    int owner = mt_thread_register_main(&table, 0);
    int waiter = mt_thread_allocate(&table, 0);

    CHECK(mt_mutex_lock(&table, owner, key, false, false) == MT_LOCK_ACQUIRED);
    CHECK(mt_mutex_lock(&table, waiter, key, false, true) == MT_LOCK_BUSY);
    CHECK(table.threads[waiter].state == MT_THREAD_RUNNABLE);
    CHECK(mt_mutex_lock(&table, waiter, key, false, false) == MT_LOCK_BLOCKED);
    CHECK(table.threads[waiter].state == MT_THREAD_BLOCKED_MUTEX);

    MtUnlockResult wrong = mt_mutex_unlock(&table, waiter, key);
    CHECK(wrong.kind == MT_UNLOCK_ERROR);
    CHECK(table.threads[waiter].state == MT_THREAD_BLOCKED_MUTEX);

    MtUnlockResult handoff = mt_mutex_unlock(&table, owner, key);
    CHECK(handoff.kind == MT_UNLOCK_OK);
    CHECK(handoff.woken_slot == waiter);
    CHECK(table.threads[waiter].state == MT_THREAD_RUNNABLE);

    MtUnlockResult final = mt_mutex_unlock(&table, waiter, key);
    CHECK(final.kind == MT_UNLOCK_OK);
    CHECK(final.woken_slot == -1);
    return 0;
}

/*
 * Newlib malloc and stdio locks are recursive. This catches both an accidental
 * deadlock on reacquire and an early hand-off before the final recursive unlock.
 */
static int test_recursive_mutex_holds_until_final_unlock(void)
{
    const uintptr_t key = 0x40002000u;
    MtThreadTable table;
    mt_thread_table_init(&table);

    int owner = mt_thread_register_main(&table, 0);
    int waiter = mt_thread_allocate(&table, 0);

    CHECK(mt_mutex_lock(&table, owner, key, true, false) == MT_LOCK_ACQUIRED);
    CHECK(mt_mutex_lock(&table, owner, key, true, false) == MT_LOCK_ACQUIRED);
    CHECK(mt_mutex_lock(&table, owner, key, false, false) == MT_LOCK_ERROR);
    CHECK(mt_mutex_lock(&table, waiter, key, false, false) == MT_LOCK_BLOCKED);

    MtUnlockResult first = mt_mutex_unlock(&table, owner, key);
    CHECK(first.kind == MT_UNLOCK_OK);
    CHECK(first.woken_slot == -1);
    CHECK(table.threads[waiter].state == MT_THREAD_BLOCKED_MUTEX);

    MtUnlockResult second = mt_mutex_unlock(&table, owner, key);
    CHECK(second.kind == MT_UNLOCK_OK);
    CHECK(second.woken_slot == waiter);
    CHECK(table.threads[waiter].state == MT_THREAD_RUNNABLE);
    return 0;
}

/*
 * Killing a task while it owns a mutex must not strand every waiter. Releasing
 * ownership before making the task a zombie transfers the lock deterministically.
 */
static int test_owner_death_releases_mutex_to_waiter(void)
{
    const uintptr_t key = 0x40003000u;
    MtThreadTable table;
    mt_thread_table_init(&table);

    int owner = mt_thread_register_main(&table, 0);
    int waiter = mt_thread_allocate(&table, 0);

    CHECK(mt_mutex_lock(&table, owner, key, false, false) == MT_LOCK_ACQUIRED);
    CHECK(mt_mutex_lock(&table, waiter, key, false, false) == MT_LOCK_BLOCKED);

    mt_mutex_release_owned(&table, owner);
    CHECK(table.threads[waiter].state == MT_THREAD_RUNNABLE);
    CHECK(mt_mutex_unlock(&table, waiter, key).kind == MT_UNLOCK_OK);
    return 0;
}

/*
 * execve() replaces an entire process image, even when a worker issued the
 * call.  Every task and mutex belonging to that process must disappear, while
 * another process and the global monotonic TID sequence remain intact.
 */
static int test_process_reset_is_isolated(void)
{
    const uintptr_t process0_key = 0x40004000u;
    const uintptr_t process1_key = 0x40005000u;
    MtThreadTable table;
    mt_thread_table_init(&table);

    int main0 = mt_thread_register_main(&table, 0);
    int worker0 = mt_thread_allocate(&table, 0);
    int main1 = mt_thread_register_main(&table, 1);
    const int highest_old_tid = table.threads[main1].tid;

    CHECK(mt_mutex_lock(&table, main0, process0_key, false, false) ==
          MT_LOCK_ACQUIRED);
    CHECK(mt_mutex_lock(&table, worker0, process0_key, false, false) ==
          MT_LOCK_BLOCKED);
    CHECK(mt_mutex_lock(&table, main1, process1_key, false, false) ==
          MT_LOCK_ACQUIRED);

    mt_thread_reset_process(&table, 0);

    for (int slot = 0; slot < MT_MAX_THREADS; slot++)
    {
        CHECK(table.threads[slot].process_id != 0);
    }

    CHECK(table.threads[main1].state == MT_THREAD_RUNNABLE);
    CHECK(mt_mutex_unlock(&table, main1, process1_key).kind == MT_UNLOCK_OK);

    int replacement = mt_thread_register_main(&table, 0);
    CHECK(replacement >= 0);
    CHECK(table.threads[replacement].tid > highest_old_tid);
    return 0;
}

/*
 * MiniSDL exposes 64 mutex objects, while its two handle pools and newlib's
 * malloc/stdio locks use additional keys.  Keep enough kernel records that a
 * valid public MiniSDL workload cannot consume the whole table by itself.
 */
static int test_mutex_capacity_covers_guest_runtime(void)
{
    enum
    {
        REQUIRED_MUTEX_RECORDS = 128,
    };

    MtThreadTable table;
    mt_thread_table_init(&table);
    int owner = mt_thread_register_main(&table, 0);

    CHECK((int)MT_MAX_MUTEXES >= (int)REQUIRED_MUTEX_RECORDS);

    for (int index = 0; index < REQUIRED_MUTEX_RECORDS; index++)
    {
        uintptr_t key = 0x40010000u + (uintptr_t)index * 16u;
        CHECK(mt_mutex_lock(&table, owner, key, false, false) ==
              MT_LOCK_ACQUIRED);
    }

    return 0;
}

int main(void)
{
    if (test_allocation_is_bounded_per_process() != 0 ||
        test_round_robin_skips_blocked_and_foreign_tasks() != 0 ||
        test_join_blocks_then_receives_exit_status() != 0 ||
        test_zombie_join_and_sequential_slot_reuse() != 0 ||
        test_invalid_and_double_join_are_rejected() != 0 ||
        test_join_cycle_is_rejected() != 0 ||
        test_mutex_handoff_trylock_and_wrong_owner() != 0 ||
        test_recursive_mutex_holds_until_final_unlock() != 0 ||
        test_owner_death_releases_mutex_to_waiter() != 0 ||
        test_process_reset_is_isolated() != 0 ||
        test_mutex_capacity_covers_guest_runtime() != 0)
    {
        return 1;
    }

    puts("thread-table tests passed");
    return 0;
}
