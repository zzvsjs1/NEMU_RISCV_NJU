#ifndef NANOS_LITE_MT_THREAD_TABLE_H
#define NANOS_LITE_MT_THREAD_TABLE_H

#include <stdbool.h>
#include <stdint.h>

/*
 * The MT kernel uses fixed storage throughout.  A process may occupy at most
 * MT_THREADS_PER_PROCESS entries, while unused entries remain available to the
 * other processes.  Slots are kernel-internal array indexes; user space sees
 * only monotonically increasing TIDs.
 */
enum
{
    MT_MAX_PROCESSES = 4,
    MT_THREADS_PER_PROCESS = 8,
    MT_MAX_THREADS = MT_MAX_PROCESSES * MT_THREADS_PER_PROCESS,
    MT_MAX_MUTEXES = 128,
};

typedef enum
{
    MT_THREAD_UNUSED,
    MT_THREAD_RUNNABLE,
    MT_THREAD_BLOCKED_JOIN,
    MT_THREAD_BLOCKED_MUTEX,
    MT_THREAD_ZOMBIE,
    MT_THREAD_ZOMBIE_JOINED,
} MtThreadState;

typedef enum
{
    MT_JOIN_ERROR = -1,
    MT_JOIN_BLOCKED = 0,
    MT_JOIN_READY = 1,
} MtJoinKind;

typedef struct
{
    MtJoinKind kind;
    int target_slot;
    int status;
} MtJoinResult;

typedef struct
{
    int woken_slot;
    int status;
} MtFinishResult;

typedef enum
{
    MT_LOCK_ERROR = -1,
    MT_LOCK_ACQUIRED = 0,
    MT_LOCK_BLOCKED = 1,
    MT_LOCK_BUSY = 2,
} MtLockResult;

typedef enum
{
    MT_UNLOCK_ERROR = -1,
    MT_UNLOCK_OK = 0,
} MtUnlockKind;

typedef struct
{
    MtUnlockKind kind;
    int woken_slot;
} MtUnlockResult;

/*
 * This structure contains only portable scheduling state.  Architecture
 * contexts and kernel/user stacks live in proc.c, indexed by the same slot.
 */
typedef struct
{
    int process_id;
    int tid;
    MtThreadState state;
    int exit_status;

    /*
     * A target permits one joiner.  The waiter records the target TID so finish
     * can verify that a stale slot relationship is not woken after slot reuse.
     */
    int join_waiter_slot;
    int waiting_for_tid;

    /*
     * Mutex waiters form an intrusive FIFO queue.  Keeping the linkage in the
     * bounded thread table avoids allocation in the kernel trap path.
     */
    int waiting_mutex_index;
    int mutex_wait_next_slot;
    bool waiting_mutex_recursive;

    bool is_main;
} MtThread;

typedef struct
{
    bool in_use;
    bool recursive;
    int process_id;
    uintptr_t key;
    int owner_slot;
    unsigned int depth;
    int wait_head_slot;
    int wait_tail_slot;
} MtMutex;

typedef struct
{
    MtThread threads[MT_MAX_THREADS];
    MtMutex mutexes[MT_MAX_MUTEXES];

    /*
     * A wide counter lets allocation detect exhaustion before narrowing a TID
     * to int.  Reaping a slot never rewinds this value.
     */
    uint64_t next_tid;
} MtThreadTable;

void mt_thread_table_init(MtThreadTable *table);
int mt_thread_register_main(MtThreadTable *table, int process_id);
int mt_thread_allocate(MtThreadTable *table, int process_id);
int mt_thread_find_tid(const MtThreadTable *table, int process_id, int tid);
int mt_thread_pick_next(MtThreadTable *table, int process_id, int after_slot);
MtJoinResult mt_thread_join(MtThreadTable *table, int waiter_slot, int target_tid);
MtFinishResult mt_thread_finish(MtThreadTable *table, int slot, int status);
void mt_thread_reap(MtThreadTable *table, int slot);
void mt_thread_reset_process(MtThreadTable *table, int process_id);

MtLockResult mt_mutex_lock(MtThreadTable *table, int slot, uintptr_t key,
                           bool recursive, bool try_only);
MtUnlockResult mt_mutex_unlock(MtThreadTable *table, int slot, uintptr_t key);
void mt_mutex_release_owned(MtThreadTable *table, int slot);

#endif
