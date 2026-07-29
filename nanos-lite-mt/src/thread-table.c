#include "thread-table.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

static bool process_id_is_valid(int process_id)
{
    return process_id >= 0 && process_id < MT_MAX_PROCESSES;
}

static bool slot_is_valid(int slot)
{
    return slot >= 0 && slot < MT_MAX_THREADS;
}

static bool thread_is_active(const MtThreadTable *table, int slot)
{
    return table != NULL && slot_is_valid(slot) &&
           table->threads[slot].state != MT_THREAD_UNUSED;
}

static void reset_thread(MtThread *thread)
{
    memset(thread, 0, sizeof(*thread));
    thread->process_id = -1;
    thread->state = MT_THREAD_UNUSED;
    thread->join_waiter_slot = -1;
    thread->waiting_mutex_index = -1;
    thread->mutex_wait_next_slot = -1;
}

static void reset_mutex(MtMutex *mutex)
{
    memset(mutex, 0, sizeof(*mutex));
    mutex->process_id = -1;
    mutex->owner_slot = -1;
    mutex->wait_head_slot = -1;
    mutex->wait_tail_slot = -1;
}

void mt_thread_table_init(MtThreadTable *table)
{
    if (table == NULL)
    {
        return;
    }

    memset(table, 0, sizeof(*table));

    for (int slot = 0; slot < MT_MAX_THREADS; slot++)
    {
        reset_thread(&table->threads[slot]);
    }

    for (int index = 0; index < MT_MAX_MUTEXES; index++)
    {
        reset_mutex(&table->mutexes[index]);
    }

    /* TID zero is reserved as the public "no thread" value. */
    table->next_tid = 1;
}

static int process_thread_count(const MtThreadTable *table, int process_id)
{
    int count = 0;

    for (int slot = 0; slot < MT_MAX_THREADS; slot++)
    {
        const MtThread *thread = &table->threads[slot];

        if (thread->state != MT_THREAD_UNUSED &&
            thread->process_id == process_id)
        {
            count++;
        }
    }

    return count;
}

static int allocate_thread(MtThreadTable *table, int process_id, bool is_main)
{
    if (table == NULL || !process_id_is_valid(process_id) ||
        table->next_tid == 0 || table->next_tid > INT_MAX)
    {
        return -1;
    }

    if (process_thread_count(table, process_id) >= MT_THREADS_PER_PROCESS)
    {
        return -1;
    }

    if (is_main)
    {
        /*
         * Registering the process entry twice would create two tasks that both
         * claim the PCB-owned context.  Workers are deliberately not subject to
         * this check.
         */
        for (int slot = 0; slot < MT_MAX_THREADS; slot++)
        {
            const MtThread *thread = &table->threads[slot];

            if (thread->state != MT_THREAD_UNUSED &&
                thread->process_id == process_id &&
                thread->is_main)
            {
                return -1;
            }
        }
    }

    for (int slot = 0; slot < MT_MAX_THREADS; slot++)
    {
        MtThread *thread = &table->threads[slot];

        if (thread->state != MT_THREAD_UNUSED)
        {
            continue;
        }

        reset_thread(thread);
        thread->process_id = process_id;
        thread->tid = (int)table->next_tid;
        thread->state = MT_THREAD_RUNNABLE;
        thread->is_main = is_main;
        table->next_tid++;
        return slot;
    }

    return -1;
}

int mt_thread_register_main(MtThreadTable *table, int process_id)
{
    return allocate_thread(table, process_id, true);
}

int mt_thread_allocate(MtThreadTable *table, int process_id)
{
    return allocate_thread(table, process_id, false);
}

int mt_thread_find_tid(const MtThreadTable *table, int process_id, int tid)
{
    if (table == NULL || !process_id_is_valid(process_id) || tid <= 0)
    {
        return -1;
    }

    for (int slot = 0; slot < MT_MAX_THREADS; slot++)
    {
        const MtThread *thread = &table->threads[slot];

        if (thread->state != MT_THREAD_UNUSED &&
            thread->process_id == process_id &&
            thread->tid == tid)
        {
            return slot;
        }
    }

    return -1;
}

int mt_thread_pick_next(MtThreadTable *table, int process_id, int after_slot)
{
    if (table == NULL || !process_id_is_valid(process_id))
    {
        return -1;
    }

    /*
     * An invalid starting slot means "start at slot zero".  A complete scan
     * includes after_slot at the final step, which lets a lone runnable task
     * continue without a scheduler special case.
     */
    const int start = slot_is_valid(after_slot) ? after_slot : MT_MAX_THREADS - 1;

    for (int offset = 1; offset <= MT_MAX_THREADS; offset++)
    {
        int slot = start + offset;

        if (slot >= MT_MAX_THREADS)
        {
            slot -= MT_MAX_THREADS;
        }

        const MtThread *thread = &table->threads[slot];

        if (thread->process_id == process_id &&
            thread->state == MT_THREAD_RUNNABLE)
        {
            return slot;
        }
    }

    return -1;
}

static bool join_would_create_cycle(const MtThreadTable *table,
                                    int waiter_slot, int target_slot)
{
    int cursor = target_slot;
    const int process_id = table->threads[waiter_slot].process_id;

    /*
     * Follow the target's existing join dependencies.  The fixed-table bound
     * both detects a pre-existing damaged cycle and guarantees this validation
     * cannot loop in the kernel.
     */
    for (int visited = 0; visited < MT_MAX_THREADS; visited++)
    {
        if (cursor == waiter_slot)
        {
            return true;
        }

        const MtThread *thread = &table->threads[cursor];

        if (thread->state != MT_THREAD_BLOCKED_JOIN ||
            thread->waiting_for_tid <= 0)
        {
            return false;
        }

        cursor =
            mt_thread_find_tid(table, process_id, thread->waiting_for_tid);

        if (cursor < 0)
        {
            return false;
        }
    }

    return true;
}

MtJoinResult mt_thread_join(MtThreadTable *table, int waiter_slot, int target_tid)
{
    MtJoinResult result = {
        .kind = MT_JOIN_ERROR,
        .target_slot = -1,
        .status = 0,
    };

    if (!thread_is_active(table, waiter_slot) ||
        table->threads[waiter_slot].state != MT_THREAD_RUNNABLE)
    {
        return result;
    }

    MtThread *waiter = &table->threads[waiter_slot];
    const int target_slot =
        mt_thread_find_tid(table, waiter->process_id, target_tid);

    if (target_slot < 0 || target_slot == waiter_slot ||
        join_would_create_cycle(table, waiter_slot, target_slot))
    {
        return result;
    }

    MtThread *target = &table->threads[target_slot];
    result.target_slot = target_slot;

    /*
     * State ZOMBIE_JOINED and a recorded waiter both mean the single join right
     * has already been consumed.  This prevents two callers from receiving the
     * same exit status or reaping the same kernel stack.
     */
    if (target->state == MT_THREAD_ZOMBIE_JOINED ||
        target->join_waiter_slot >= 0)
    {
        return result;
    }

    target->join_waiter_slot = waiter_slot;

    if (target->state == MT_THREAD_ZOMBIE)
    {
        target->state = MT_THREAD_ZOMBIE_JOINED;
        result.kind = MT_JOIN_READY;
        result.status = target->exit_status;
        return result;
    }

    waiter->state = MT_THREAD_BLOCKED_JOIN;
    waiter->waiting_for_tid = target_tid;
    result.kind = MT_JOIN_BLOCKED;
    return result;
}

static void cancel_join_wait(MtThreadTable *table, int waiter_slot)
{
    for (int slot = 0; slot < MT_MAX_THREADS; slot++)
    {
        MtThread *target = &table->threads[slot];

        if (target->state != MT_THREAD_UNUSED &&
            target->join_waiter_slot == waiter_slot)
        {
            target->join_waiter_slot = -1;
        }
    }

    table->threads[waiter_slot].waiting_for_tid = 0;
}

static void remove_mutex_waiter(MtThreadTable *table, int waiter_slot)
{
    MtThread *waiter = &table->threads[waiter_slot];
    const int mutex_index = waiter->waiting_mutex_index;

    if (mutex_index < 0 || mutex_index >= MT_MAX_MUTEXES ||
        !table->mutexes[mutex_index].in_use)
    {
        waiter->waiting_mutex_index = -1;
        waiter->mutex_wait_next_slot = -1;
        waiter->waiting_mutex_recursive = false;
        return;
    }

    MtMutex *mutex = &table->mutexes[mutex_index];
    int previous = -1;
    int current = mutex->wait_head_slot;

    /*
     * The guard also makes a damaged intrusive list fail closed instead of
     * looping forever in the kernel.
     */
    for (int visited = 0;
         visited < MT_MAX_THREADS && slot_is_valid(current);
         visited++)
    {
        const int next = table->threads[current].mutex_wait_next_slot;

        if (current == waiter_slot)
        {
            if (previous < 0)
            {
                mutex->wait_head_slot = next;
            }
            else
            {
                table->threads[previous].mutex_wait_next_slot = next;
            }

            if (mutex->wait_tail_slot == waiter_slot)
            {
                mutex->wait_tail_slot = previous;
            }

            break;
        }

        previous = current;
        current = next;
    }

    waiter->waiting_mutex_index = -1;
    waiter->mutex_wait_next_slot = -1;
    waiter->waiting_mutex_recursive = false;
}

MtFinishResult mt_thread_finish(MtThreadTable *table, int slot, int status)
{
    MtFinishResult result = {
        .woken_slot = -1,
        .status = status,
    };

    if (!thread_is_active(table, slot))
    {
        return result;
    }

    MtThread *thread = &table->threads[slot];

    if (thread->state == MT_THREAD_ZOMBIE ||
        thread->state == MT_THREAD_ZOMBIE_JOINED)
    {
        return result;
    }

    /*
     * A killed blocked task must be removed from the relationship on which it
     * was sleeping.  Otherwise a reused slot could be woken by an old target or
     * mutex queue.
     */
    cancel_join_wait(table, slot);
    remove_mutex_waiter(table, slot);

    thread->exit_status = status;
    const int waiter_slot = thread->join_waiter_slot;

    if (thread_is_active(table, waiter_slot))
    {
        MtThread *waiter = &table->threads[waiter_slot];

        if (waiter->state == MT_THREAD_BLOCKED_JOIN &&
            waiter->process_id == thread->process_id &&
            waiter->waiting_for_tid == thread->tid)
        {
            waiter->state = MT_THREAD_RUNNABLE;
            waiter->waiting_for_tid = 0;
            thread->state = MT_THREAD_ZOMBIE_JOINED;
            result.woken_slot = waiter_slot;
            return result;
        }
    }

    thread->join_waiter_slot = -1;
    thread->state = MT_THREAD_ZOMBIE;
    return result;
}

void mt_thread_reap(MtThreadTable *table, int slot)
{
    if (!thread_is_active(table, slot) ||
        table->threads[slot].state != MT_THREAD_ZOMBIE_JOINED)
    {
        return;
    }

    reset_thread(&table->threads[slot]);
}

void mt_thread_reset_process(MtThreadTable *table, int process_id)
{
    if (table == NULL || !process_id_is_valid(process_id))
    {
        return;
    }

    /*
     * This is the process-wide half of execve(): no context from the old
     * address space may remain schedulable.  Do not rewind next_tid, because a
     * userspace handle from the old image must never identify a later task.
     */
    for (int slot = 0; slot < MT_MAX_THREADS; slot++)
    {
        if (table->threads[slot].state != MT_THREAD_UNUSED &&
            table->threads[slot].process_id == process_id)
        {
            reset_thread(&table->threads[slot]);
        }
    }

    /*
     * Mutex keys are user virtual addresses.  They lose their meaning when the
     * process image is replaced, so remove both owned and unowned queue state
     * without disturbing locks belonging to another address space.
     */
    for (int index = 0; index < MT_MAX_MUTEXES; index++)
    {
        if (table->mutexes[index].in_use &&
            table->mutexes[index].process_id == process_id)
        {
            reset_mutex(&table->mutexes[index]);
        }
    }
}

static int find_mutex(const MtThreadTable *table, int process_id, uintptr_t key)
{
    for (int index = 0; index < MT_MAX_MUTEXES; index++)
    {
        const MtMutex *mutex = &table->mutexes[index];

        if (mutex->in_use &&
            mutex->process_id == process_id &&
            mutex->key == key)
        {
            return index;
        }
    }

    return -1;
}

static int allocate_mutex(MtThreadTable *table, int process_id, uintptr_t key,
                          bool recursive)
{
    for (int index = 0; index < MT_MAX_MUTEXES; index++)
    {
        MtMutex *mutex = &table->mutexes[index];

        if (mutex->in_use)
        {
            continue;
        }

        reset_mutex(mutex);
        mutex->in_use = true;
        mutex->recursive = recursive;
        mutex->process_id = process_id;
        mutex->key = key;
        return index;
    }

    return -1;
}

static void enqueue_mutex_waiter(MtThreadTable *table, int mutex_index,
                                 int waiter_slot, bool recursive)
{
    MtMutex *mutex = &table->mutexes[mutex_index];
    MtThread *waiter = &table->threads[waiter_slot];

    waiter->state = MT_THREAD_BLOCKED_MUTEX;
    waiter->waiting_mutex_index = mutex_index;
    waiter->mutex_wait_next_slot = -1;
    waiter->waiting_mutex_recursive = recursive;

    if (mutex->wait_tail_slot < 0)
    {
        mutex->wait_head_slot = waiter_slot;
        mutex->wait_tail_slot = waiter_slot;
        return;
    }

    table->threads[mutex->wait_tail_slot].mutex_wait_next_slot = waiter_slot;
    mutex->wait_tail_slot = waiter_slot;
}

static int pop_mutex_waiter(MtThreadTable *table, int mutex_index)
{
    MtMutex *mutex = &table->mutexes[mutex_index];

    for (int examined = 0;
         examined < MT_MAX_THREADS && slot_is_valid(mutex->wait_head_slot);
         examined++)
    {
        const int waiter_slot = mutex->wait_head_slot;
        MtThread *waiter = &table->threads[waiter_slot];

        mutex->wait_head_slot = waiter->mutex_wait_next_slot;

        if (mutex->wait_head_slot < 0)
        {
            mutex->wait_tail_slot = -1;
        }

        waiter->mutex_wait_next_slot = -1;

        if (waiter->state == MT_THREAD_BLOCKED_MUTEX &&
            waiter->process_id == mutex->process_id &&
            waiter->waiting_mutex_index == mutex_index)
        {
            waiter->waiting_mutex_index = -1;
            return waiter_slot;
        }

        waiter->waiting_mutex_index = -1;
        waiter->waiting_mutex_recursive = false;
    }

    return -1;
}

static int hand_off_or_reset_mutex(MtThreadTable *table, int mutex_index)
{
    MtMutex *mutex = &table->mutexes[mutex_index];
    const int waiter_slot = pop_mutex_waiter(table, mutex_index);

    if (waiter_slot < 0)
    {
        reset_mutex(mutex);
        return -1;
    }

    mutex->owner_slot = waiter_slot;
    mutex->depth = 1;
    mutex->recursive = table->threads[waiter_slot].waiting_mutex_recursive;
    table->threads[waiter_slot].waiting_mutex_recursive = false;
    table->threads[waiter_slot].state = MT_THREAD_RUNNABLE;
    return waiter_slot;
}

MtLockResult mt_mutex_lock(MtThreadTable *table, int slot, uintptr_t key,
                           bool recursive, bool try_only)
{
    if (!thread_is_active(table, slot) ||
        table->threads[slot].state != MT_THREAD_RUNNABLE)
    {
        return MT_LOCK_ERROR;
    }

    MtThread *thread = &table->threads[slot];
    int mutex_index = find_mutex(table, thread->process_id, key);

    if (mutex_index < 0)
    {
        mutex_index =
            allocate_mutex(table, thread->process_id, key, recursive);

        if (mutex_index < 0)
        {
            return MT_LOCK_ERROR;
        }

        table->mutexes[mutex_index].owner_slot = slot;
        table->mutexes[mutex_index].depth = 1;
        return MT_LOCK_ACQUIRED;
    }

    MtMutex *mutex = &table->mutexes[mutex_index];

    if (mutex->owner_slot == slot)
    {
        /*
         * Recursive behaviour belongs to the current acquisition.  A different
         * waiter may request the same key as a normal lock; its requested mode
         * becomes active only when ownership is handed over.
         */
        if (!mutex->recursive || !recursive || mutex->depth == UINT_MAX)
        {
            return MT_LOCK_ERROR;
        }

        mutex->depth++;
        return MT_LOCK_ACQUIRED;
    }

    if (try_only)
    {
        return MT_LOCK_BUSY;
    }

    enqueue_mutex_waiter(table, mutex_index, slot, recursive);
    return MT_LOCK_BLOCKED;
}

MtUnlockResult mt_mutex_unlock(MtThreadTable *table, int slot, uintptr_t key)
{
    MtUnlockResult result = {
        .kind = MT_UNLOCK_ERROR,
        .woken_slot = -1,
    };

    if (!thread_is_active(table, slot))
    {
        return result;
    }

    const int process_id = table->threads[slot].process_id;
    const int mutex_index = find_mutex(table, process_id, key);

    if (mutex_index < 0)
    {
        return result;
    }

    MtMutex *mutex = &table->mutexes[mutex_index];

    if (mutex->owner_slot != slot || mutex->depth == 0)
    {
        return result;
    }

    result.kind = MT_UNLOCK_OK;

    if (mutex->recursive && mutex->depth > 1)
    {
        mutex->depth--;
        return result;
    }

    result.woken_slot = hand_off_or_reset_mutex(table, mutex_index);
    return result;
}

void mt_mutex_release_owned(MtThreadTable *table, int slot)
{
    if (table == NULL || !slot_is_valid(slot))
    {
        return;
    }

    /*
     * Owner death releases every recursive level at once.  Each mutex transfers
     * directly to its first waiter, so no runnable task can steal the lock
     * between wake-up and resumption.
     */
    for (int index = 0; index < MT_MAX_MUTEXES; index++)
    {
        MtMutex *mutex = &table->mutexes[index];

        if (mutex->in_use && mutex->owner_slot == slot)
        {
            mutex->depth = 1;
            (void)hand_off_or_reset_mutex(table, index);
        }
    }
}
