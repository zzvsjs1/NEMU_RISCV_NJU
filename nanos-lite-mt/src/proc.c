#include <proc.h>

#include "pagewalk.h"
#include "syscall.h"
#include "thread-table.h"

#include <limits.h>

enum
{
    FOREGROUND_QUANTA = 5,
    NANOS_ESRCH = 3,
    NANOS_EAGAIN = 11,
    NANOS_EFAULT = 14,
    NANOS_EBUSY = 16,
    NANOS_EINVAL = 22,
    NANOS_EDEADLK = 35,
    NANOS_ECANCELED = 125,
};

typedef struct
{
    Context *context;
    PCB *owner;

    /*
     * A blocked join cannot complete its userspace store until the target
     * exits.  The pointer has already been checked against the shared process
     * address space before it is kept here.
     */
    int *join_status;

    /*
     * Worker Context objects must live in kernel-mapped memory.  Giving every
     * bounded task slot its own stack also guarantees that a timer interrupt
     * can save one worker while another worker is executing in the same
     * address space.
     */
    uint8_t kstack[STACK_SIZE] PG_ALIGN;
} MtTaskRuntime;

void context_uload(PCB *pcb, const char *filename, char *const argv[],
                   char *const envp[]);
void device_capture_foreground_before_switch(void);
void device_note_foreground_switch(void);
void device_restore_foreground_on_schedule(void);
void syscall_request_resched(void);

static PCB pcb[MAX_NR_PROC] __attribute__((used)) = {};
static PCB pcb_boot = {};
PCB *current = NULL;

static PCB *fg_pcb = &pcb[0];
static int foreground_budget = FOREGROUND_QUANTA;

static MtThreadTable thread_table;
static MtTaskRuntime task_runtime[MT_MAX_THREADS];
static int current_task_slot = -1;
static int last_task_slot[MT_MAX_PROCESSES] = {-1, -1, -1, -1};

void switch_boot_pcb(void)
{
    current = &pcb_boot;
    current_task_slot = -1;
}

static int pcb_index_of(const PCB *target)
{
    for (int index = 0; index < MAX_NR_PROC; index++)
    {
        if (target == &pcb[index])
        {
            return index;
        }
    }

    return -1;
}

int current_pcb_index(void)
{
    return pcb_index_of(current);
}

int foreground_pcb_index(void)
{
    return pcb_index_of(fg_pcb);
}

static bool pcb_has_image(const PCB *candidate)
{
    return candidate != NULL && candidate->cp != NULL;
}

bool switch_fg_pcb(int index)
{
    assert(index >= 0 && index < NR_FOREGROUND_PROC);

    PCB *next = &pcb[index];

    /*
     * Small test profiles intentionally load only PCB 0.  Their device layer
     * still recognises F1/F2/F3, so treat an unloaded selection as a harmless
     * no-op instead of asserting inside an input syscall.
     */
    if (!pcb_has_image(next))
    {
        Log("Ignore foreground switch to unloaded pcb[%d]", index);
        return false;
    }

    if (fg_pcb == next)
    {
        return false;
    }

    /*
     * Device ownership remains process based: all workers of a foreground
     * process share that process's framebuffer and audio state.  Capture the
     * outgoing owner before changing fg_pcb, as the shared device layer uses
     * foreground_pcb_index() to identify the owner being saved.
     */
    device_capture_foreground_before_switch();
    fg_pcb = next;
    foreground_budget = FOREGROUND_QUANTA;
    device_note_foreground_switch();
    Log("Switch foreground to pcb[%d]", index);
    return true;
}

static void clear_task_runtime(int slot)
{
    assert(slot >= 0 && slot < MT_MAX_THREADS);

    /*
     * Deliberately leave kstack bytes untouched.  This helper can run just
     * after switching away from a terminating task, while the kernel still has
     * local values that were produced on that old stack.
     */
    task_runtime[slot].context = NULL;
    task_runtime[slot].owner = NULL;
    task_runtime[slot].join_status = NULL;
}

static int register_main_task(int process_id)
{
    const int slot = mt_thread_register_main(&thread_table, process_id);
    assert(slot >= 0);

    task_runtime[slot].context = pcb[process_id].cp;
    task_runtime[slot].owner = &pcb[process_id];
    task_runtime[slot].join_status = NULL;
    return slot;
}

static int any_runnable_task(int process_id)
{
    return mt_thread_pick_next(&thread_table, process_id, -1);
}

static int choose_process(int previous_process)
{
    const int foreground_process = foreground_pcb_index();
    const bool foreground_runnable =
        any_runnable_task(foreground_process) >= 0;
    const bool background_runnable =
        any_runnable_task(HELLO_PROC) >= 0;

    if (!foreground_runnable && !background_runnable)
    {
        panic("nanos-lite-mt: no runnable task in the selected foreground or background process");
    }

    if (previous_process == foreground_process && foreground_runnable)
    {
        /*
         * Preserve Nanos-lite's foreground bias.  Round-robin selection among
         * threads happens after this process-level decision; the optional
         * background process receives one turn after the same small budget.
         */
        if (background_runnable && foreground_budget-- <= 0)
        {
            foreground_budget = FOREGROUND_QUANTA;
            return HELLO_PROC;
        }

        return foreground_process;
    }

    /*
     * The first switch, a return from the background slot, and a hotkey that
     * changed fg_pcb all prefer the newly selected foreground process.
     */
    if (foreground_runnable)
    {
        return foreground_process;
    }

    return HELLO_PROC;
}

void init_proc(void)
{
    Log("Initializing RV64 multi-thread processes...");

    static char *const envp_empty[] = {NULL};

    mt_thread_table_init(&thread_table);

    for (int slot = 0; slot < MT_MAX_THREADS; slot++)
    {
        clear_task_runtime(slot);
    }

#ifdef NANOS_INIT_MINISDL_THREAD_TEST
    static char *const argv_thread_test[] = {
        "/bin/miniSDL-thread-test",
        NULL,
    };

    context_uload(&pcb[0], "/bin/miniSDL-thread-test", argv_thread_test,
                  envp_empty);
    (void)register_main_task(0);
#else
    static char *argv_doom[] = {
        "/bin/doom",
        "-iwad",
        "/share/games/doom/DOOM.WAD",
        "-nogui",
        NULL,
    };
    static char *argv_fceux_am[] = {
        "/bin/fceux",
        "/share/games/nes/c.nes",
        NULL,
    };
    static char *argv_onscripter[] = {
        "/bin/onscripter",
        "-r",
        "/share/games/ons",
        NULL,
    };

    context_uload(&pcb[2], "/bin/fceux", argv_fceux_am, envp_empty);
    (void)register_main_task(2);
    context_uload(&pcb[1], "/bin/onscripter", argv_onscripter, envp_empty);
    (void)register_main_task(1);
    context_uload(&pcb[0], "/bin/doom", argv_doom, envp_empty);
    (void)register_main_task(0);
#endif

    fg_pcb = &pcb[0];
    foreground_budget = FOREGROUND_QUANTA;
    switch_boot_pcb();
}

Context *schedule(Context *previous_context)
{
    const int previous_slot = current_task_slot;
    const int previous_process =
        previous_slot >= 0 ? thread_table.threads[previous_slot].process_id : -1;

    if (previous_slot >= 0)
    {
        assert(previous_slot < MT_MAX_THREADS);
        task_runtime[previous_slot].context = previous_context;

        /*
         * The shared loader and memory manager still use PCB.cp.  Keep it in
         * sync only for the process's main task; worker contexts live in their
         * independent task stacks.
         */
        if (thread_table.threads[previous_slot].is_main)
        {
            task_runtime[previous_slot].owner->cp = previous_context;
        }
    }
    else if (current == &pcb_boot)
    {
        pcb_boot.cp = previous_context;
    }

    const int next_process = choose_process(previous_process);
    const int next_slot =
        mt_thread_pick_next(&thread_table, next_process,
                            last_task_slot[next_process]);

    assert(next_slot >= 0 && next_slot < MT_MAX_THREADS);
    assert(thread_table.threads[next_slot].state == MT_THREAD_RUNNABLE);
    assert(task_runtime[next_slot].context != NULL);
    assert(task_runtime[next_slot].owner != NULL);

    current_task_slot = next_slot;
    last_task_slot[next_process] = next_slot;
    current = task_runtime[next_slot].owner;
    Context *next_context = task_runtime[next_slot].context;

    /*
     * A naturally exiting task can be reaped only after a different Context has
     * been selected.  Resetting the portable table and metadata is safe here;
     * the worker's stack storage itself is intentionally retained.
     */
    if (previous_slot >= 0 && previous_slot != next_slot &&
        thread_table.threads[previous_slot].state ==
            MT_THREAD_ZOMBIE_JOINED)
    {
        mt_thread_reap(&thread_table, previous_slot);
        clear_task_runtime(previous_slot);
    }

    if (current == fg_pcb)
    {
        device_restore_foreground_on_schedule();
    }

    return next_context;
}

static bool user_memory_is_mapped(uintptr_t address, size_t length)
{
    if (current == NULL || current->as.ptr == NULL || length == 0)
    {
        return false;
    }

    const uintptr_t user_start = (uintptr_t)current->as.area.start;
    const uintptr_t user_end = (uintptr_t)current->as.area.end;

    if (address < user_start || address >= user_end ||
        length - 1 > UINTPTR_MAX - address)
    {
        return false;
    }

    const uintptr_t last = address + length - 1;

    if (last >= user_end)
    {
        return false;
    }

    const uintptr_t first_page = address & ~(uintptr_t)(PGSIZE - 1);
    const uintptr_t last_page = last & ~(uintptr_t)(PGSIZE - 1);

    for (uintptr_t page = first_page;; page += PGSIZE)
    {
        if (nanos_pagewalk_lookup_page(current->as.ptr, page) == NULL)
        {
            return false;
        }

        if (page == last_page)
        {
            break;
        }
    }

    return true;
}

static int current_process_id(void)
{
    if (current_task_slot < 0 ||
        current_task_slot >= MT_MAX_THREADS)
    {
        return -1;
    }

    return thread_table.threads[current_task_slot].process_id;
}

static void complete_join_wakeup(MtFinishResult finish)
{
    if (finish.woken_slot < 0)
    {
        return;
    }

    assert(finish.woken_slot < MT_MAX_THREADS);
    MtTaskRuntime *waiter = &task_runtime[finish.woken_slot];
    assert(waiter->context != NULL);

    if (waiter->join_status != NULL)
    {
        *waiter->join_status = finish.status;
    }

    waiter->join_status = NULL;
    waiter->context->GPRx = 0;
}

static uintptr_t negative_error(int error_number)
{
    return (uintptr_t)(intptr_t)-error_number;
}

static void handle_thread_create(Context *context, uintptr_t entry,
                                 uintptr_t argument, uintptr_t stack_top)
{
    const int process_id = current_process_id();

    if (process_id < 0 || entry == 0 || stack_top == 0 ||
        (stack_top & (uintptr_t)0xf) != 0 ||
        !user_memory_is_mapped(entry, 1) ||
        !user_memory_is_mapped(stack_top - 1, 1))
    {
        context->GPRx = negative_error(NANOS_EINVAL);
        return;
    }

    const int slot = mt_thread_allocate(&thread_table, process_id);

    if (slot < 0)
    {
        context->GPRx = negative_error(NANOS_EAGAIN);
        return;
    }

    Area kernel_stack = {
        .start = task_runtime[slot].kstack,
        .end = task_runtime[slot].kstack + STACK_SIZE,
    };
    Context *worker =
        ucontext(&current->as, kernel_stack, (void *)entry);

    worker->GPRSP = stack_top;
    worker->GPR2 = argument;
    worker->gpr[1] = 0;

    task_runtime[slot].context = worker;
    task_runtime[slot].owner = current;
    task_runtime[slot].join_status = NULL;
    context->GPRx = (uintptr_t)thread_table.threads[slot].tid;
}

static void handle_thread_exit(Context *context, int status)
{
    if (current_task_slot < 0)
    {
        context->GPRx = negative_error(NANOS_EINVAL);
        return;
    }

    mt_mutex_release_owned(&thread_table, current_task_slot);
    const MtFinishResult finish =
        mt_thread_finish(&thread_table, current_task_slot, status);
    complete_join_wakeup(finish);

    /*
     * This Context is now a zombie and must never return to user mode.  The IRQ
     * layer consumes the one-shot request after do_syscall() returns.
     */
    syscall_request_resched();
}

static void handle_thread_join(Context *context, uintptr_t raw_tid,
                               uintptr_t status_address)
{
    if (current_task_slot < 0 || raw_tid > INT_MAX)
    {
        context->GPRx = negative_error(NANOS_EINVAL);
        return;
    }

    if (status_address != 0 &&
        !user_memory_is_mapped(status_address, sizeof(int)))
    {
        context->GPRx = negative_error(NANOS_EFAULT);
        return;
    }

    const int process_id = current_process_id();
    const int target_slot =
        mt_thread_find_tid(&thread_table, process_id, (int)raw_tid);

    if (target_slot < 0)
    {
        context->GPRx = negative_error(NANOS_ESRCH);
        return;
    }

    MtJoinResult result =
        mt_thread_join(&thread_table, current_task_slot, (int)raw_tid);

    if (result.kind == MT_JOIN_ERROR)
    {
        context->GPRx = negative_error(NANOS_EDEADLK);
        return;
    }

    if (result.kind == MT_JOIN_READY)
    {
        if (status_address != 0)
        {
            *(int *)status_address = result.status;
        }

        context->GPRx = 0;
        mt_thread_reap(&thread_table, result.target_slot);
        clear_task_runtime(result.target_slot);
        return;
    }

    task_runtime[current_task_slot].join_status =
        status_address != 0 ? (int *)status_address : NULL;
    context->GPRx = 0;
    syscall_request_resched();
}

static void handle_thread_kill(Context *context, uintptr_t raw_tid)
{
    if (current_task_slot < 0 || raw_tid > INT_MAX)
    {
        context->GPRx = negative_error(NANOS_EINVAL);
        return;
    }

    const int process_id = current_process_id();
    const int target_slot =
        mt_thread_find_tid(&thread_table, process_id, (int)raw_tid);

    if (target_slot < 0)
    {
        context->GPRx = negative_error(NANOS_ESRCH);
        return;
    }

    MtThread *target = &thread_table.threads[target_slot];

    if (target_slot == current_task_slot || target->is_main)
    {
        context->GPRx = negative_error(NANOS_EINVAL);
        return;
    }

    if (target->state == MT_THREAD_ZOMBIE ||
        target->state == MT_THREAD_ZOMBIE_JOINED)
    {
        context->GPRx = negative_error(NANOS_ESRCH);
        return;
    }

    mt_mutex_release_owned(&thread_table, target_slot);
    const MtFinishResult finish =
        mt_thread_finish(&thread_table, target_slot, -NANOS_ECANCELED);
    complete_join_wakeup(finish);

    if (thread_table.threads[target_slot].state ==
        MT_THREAD_ZOMBIE_JOINED)
    {
        /*
         * The caller killed a different task, so its kernel stack is not active
         * and can be made reusable immediately after satisfying its joiner.
         */
        mt_thread_reap(&thread_table, target_slot);
        clear_task_runtime(target_slot);
    }

    context->GPRx = 0;
}

static void handle_mutex_lock(Context *context, uintptr_t key,
                              uintptr_t recursive, uintptr_t try_only)
{
    if (key == 0 || recursive > 1 || try_only > 1 ||
        !user_memory_is_mapped(key, 1))
    {
        context->GPRx = negative_error(NANOS_EINVAL);
        return;
    }

    const MtLockResult result =
        mt_mutex_lock(&thread_table, current_task_slot, key,
                      recursive != 0, try_only != 0);

    switch (result)
    {
    case MT_LOCK_ACQUIRED:
        context->GPRx = 0;
        break;
    case MT_LOCK_BLOCKED:
        /*
         * Ownership is transferred by the unlock path before this task becomes
         * runnable again, so it resumes from the syscall with success.
         */
        context->GPRx = 0;
        syscall_request_resched();
        break;
    case MT_LOCK_BUSY:
        context->GPRx = NANOS_EBUSY;
        break;
    case MT_LOCK_ERROR:
    default:
        context->GPRx = negative_error(NANOS_EDEADLK);
        break;
    }
}

static void handle_mutex_unlock(Context *context, uintptr_t key)
{
    if (key == 0 || !user_memory_is_mapped(key, 1))
    {
        context->GPRx = negative_error(NANOS_EINVAL);
        return;
    }

    const MtUnlockResult result =
        mt_mutex_unlock(&thread_table, current_task_slot, key);
    context->GPRx =
        result.kind == MT_UNLOCK_OK ? 0 : negative_error(NANOS_EINVAL);
}

bool mt_handle_syscall(Context *context, uintptr_t number, uintptr_t argument1,
                       uintptr_t argument2, uintptr_t argument3)
{
    switch (number)
    {
    case SYS_thread_create:
        handle_thread_create(context, argument1, argument2, argument3);
        return true;
    case SYS_thread_exit:
        handle_thread_exit(context, (int)argument1);
        return true;
    case SYS_thread_join:
        handle_thread_join(context, argument1, argument2);
        return true;
    case SYS_thread_self:
        context->GPRx =
            current_task_slot >= 0
                ? (uintptr_t)thread_table.threads[current_task_slot].tid
                : negative_error(NANOS_EINVAL);
        return true;
    case SYS_thread_kill:
        handle_thread_kill(context, argument1);
        return true;
    case SYS_mutex_lock:
        handle_mutex_lock(context, argument1, argument2, argument3);
        return true;
    case SYS_mutex_unlock:
        handle_mutex_unlock(context, argument1);
        return true;
    default:
        return false;
    }
}

void mt_process_context_replaced(Context *replacement)
{
    const int process_id = current_pcb_index();
    assert(process_id >= 0 && process_id < MT_MAX_PROCESSES);
    assert(replacement != NULL);

    /*
     * Clear runtime references before resetting the portable table.  This also
     * handles execve() issued by a worker: the old worker stack remains bytes in
     * fixed storage, but no task can return through its obsolete address space.
     */
    for (int slot = 0; slot < MT_MAX_THREADS; slot++)
    {
        if (task_runtime[slot].owner == current)
        {
            clear_task_runtime(slot);
        }
    }

    mt_thread_reset_process(&thread_table, process_id);
    const int replacement_slot =
        mt_thread_register_main(&thread_table, process_id);
    assert(replacement_slot >= 0);

    task_runtime[replacement_slot].context = replacement;
    task_runtime[replacement_slot].owner = current;
    task_runtime[replacement_slot].join_status = NULL;
    current_task_slot = replacement_slot;
    last_task_slot[process_id] = replacement_slot;
}
