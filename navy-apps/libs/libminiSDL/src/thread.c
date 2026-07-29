#include <SDL.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __ISA_NATIVE__

/*
 * The native miniSDL target does not provide a Navy scheduler.  Execute a new
 * callback synchronously so programs can still use the handle and join API
 * without introducing a host pthread dependency into the existing build.
 */
enum
{
    SDL_NATIVE_THREAD_SLOTS = 7
};

struct SDL_Thread
{
    int allocated;
    int status;
    uint32_t thread_id;
};

static struct SDL_Thread native_threads[SDL_NATIVE_THREAD_SLOTS];
static struct SDL_Thread *native_current_thread;
static uint32_t next_native_thread_id = 1;

SDL_Thread *SDL_CreateThread(int (*fn)(void *), void *data)
{
    struct SDL_Thread *thread = NULL;

    if (fn == NULL)
    {
        SDL_SetError("SDL_CreateThread received a null callback");
        return NULL;
    }

    for (size_t slot = 0; slot < SDL_NATIVE_THREAD_SLOTS; slot++)
    {
        if (!native_threads[slot].allocated)
        {
            thread = &native_threads[slot];
            break;
        }
    }

    if (thread == NULL)
    {
        SDL_SetError("miniSDL native thread handle pool is full");
        return NULL;
    }

    thread->allocated = 1;
    thread->thread_id = next_native_thread_id++;
    native_current_thread = thread;
    thread->status = fn(data);
    native_current_thread = NULL;
    return thread;
}

uint32_t SDL_ThreadID(void)
{
    return native_current_thread == NULL ? 0 : native_current_thread->thread_id;
}

uint32_t SDL_GetThreadID(SDL_Thread *thread)
{
    if (thread == NULL)
    {
        return SDL_ThreadID();
    }
    return thread->allocated ? thread->thread_id : 0;
}

void SDL_WaitThread(SDL_Thread *thread, int *status)
{
    if (thread == NULL || !thread->allocated)
    {
        SDL_SetError("SDL_WaitThread received an invalid thread handle");
        return;
    }

    if (status != NULL)
    {
        *status = thread->status;
    }
    thread->allocated = 0;
}

void SDL_KillThread(SDL_Thread *thread)
{
    if (thread != NULL)
    {
        thread->allocated = 0;
    }
}

#else

#include <stdbool.h>

#include "thread_runtime.h"

enum
{
    SDL_THREAD_SLOTS = 7,
    SDL_THREAD_STACK_BYTES = 64 * 1024
};

struct SDL_Thread
{
    bool allocated;
    int thread_id;
    int (*function)(void *);
    void *argument;

    /*
     * A page-aligned 64 KiB stack is deliberately owned by each live handle.
     * Joined handles return to the pool, but their stack bytes are not cleared:
     * the next callback overwrites them and avoiding that clear keeps join
     * bounded even on the RV64 interpreter.
     */
    _Alignas(4096) unsigned char stack[SDL_THREAD_STACK_BYTES];
};

static struct SDL_Thread thread_pool[SDL_THREAD_SLOTS];
static unsigned char thread_pool_lock;

static int lock_thread_pool(void)
{
    return nanos_lock_acquire(&thread_pool_lock, false, false);
}

static void unlock_thread_pool(void)
{
    /*
     * There is no useful recovery if the private pool lock cannot be released.
     * The syscall runtime nevertheless returns an error, so record it for a
     * caller that subsequently asks SDL_GetError().
     */
    if (nanos_lock_release(&thread_pool_lock) < 0)
    {
        SDL_SetError("failed to release miniSDL's thread-pool lock");
    }
}

static bool thread_handle_is_live(const SDL_Thread *thread)
{
    uintptr_t address = (uintptr_t)thread;
    uintptr_t first = (uintptr_t)&thread_pool[0];
    uintptr_t limit = (uintptr_t)&thread_pool[SDL_THREAD_SLOTS];

    if (address < first || address >= limit)
    {
        return false;
    }
    if ((address - first) % sizeof(thread_pool[0]) != 0)
    {
        return false;
    }

    return thread->allocated;
}

static void thread_trampoline(void *opaque)
{
    struct SDL_Thread *thread = opaque;
    int status = thread->function(thread->argument);

    /*
     * Returning through an uninitialised user return address would trap.  The
     * trampoline therefore turns every normal callback return into the explicit
     * thread-exit syscall and never returns to the kernel-created context.
     */
    nanos_thread_exit(status);
}

SDL_Thread *SDL_CreateThread(int (*fn)(void *), void *data)
{
    struct SDL_Thread *thread = NULL;
    int activation_result;
    int thread_id;

    if (fn == NULL)
    {
        SDL_SetError("SDL_CreateThread received a null callback");
        return NULL;
    }

    /*
     * Activation is intentionally one-way.  Before this call libc and miniSDL
     * locks remain compatibility no-ops, preserving existing single-threaded
     * applications without issuing unknown syscalls to the original Nanos-lite.
     */
    activation_result = nanos_thread_runtime_enable();
    if (activation_result < 0)
    {
        SDL_SetError("could not activate Nanos-lite threading (%d)",
                     activation_result);
        return NULL;
    }

    if (lock_thread_pool() < 0)
    {
        SDL_SetError("failed to lock miniSDL's thread-handle pool");
        return NULL;
    }

    for (size_t slot = 0; slot < SDL_THREAD_SLOTS; slot++)
    {
        if (!thread_pool[slot].allocated)
        {
            thread = &thread_pool[slot];
            thread->allocated = true;
            thread->function = fn;
            thread->argument = data;
            break;
        }
    }

    if (thread == NULL)
    {
        unlock_thread_pool();
        SDL_SetError("miniSDL supports at most %d simultaneous worker threads",
                     SDL_THREAD_SLOTS);
        return NULL;
    }

    thread_id = nanos_thread_create(
        thread_trampoline, thread,
        thread->stack + sizeof(thread->stack));
    if (thread_id < 0)
    {
        thread->allocated = false;
        thread->function = NULL;
        thread->argument = NULL;
        unlock_thread_pool();
        SDL_SetError("Nanos-lite rejected SDL_CreateThread (%d)", thread_id);
        return NULL;
    }

    thread->thread_id = thread_id;
    unlock_thread_pool();
    return thread;
}

uint32_t SDL_ThreadID(void)
{
    int thread_id = nanos_thread_self();

    if (thread_id < 0)
    {
        SDL_SetError("Nanos-lite rejected SDL_ThreadID (%d)", thread_id);
        return 0;
    }
    return (uint32_t)thread_id;
}

uint32_t SDL_GetThreadID(SDL_Thread *thread)
{
    /*
     * SDL 1.2 treats a null handle as a request for the calling task's ID.
     * Keeping that small convenience matters to code shared with full SDL.
     */
    if (thread == NULL)
    {
        return SDL_ThreadID();
    }
    if (!thread_handle_is_live(thread))
    {
        return 0;
    }
    return (uint32_t)thread->thread_id;
}

void SDL_WaitThread(SDL_Thread *thread, int *status)
{
    int joined_status;
    int result;

    if (!thread_handle_is_live(thread))
    {
        SDL_SetError("SDL_WaitThread received an invalid thread handle");
        return;
    }

    /*
     * Do not retain the pool lock while joining.  The caller can block here,
     * and retaining a kernel mutex across that sleep would unnecessarily tie
     * handle management to the target callback's progress.
     */
    result = nanos_thread_join(thread->thread_id, &joined_status);
    if (result < 0)
    {
        SDL_SetError("Nanos-lite rejected SDL_WaitThread (%d)", result);
        return;
    }

    if (status != NULL)
    {
        *status = joined_status;
    }

    if (lock_thread_pool() < 0)
    {
        SDL_SetError("joined a thread but could not reclaim its miniSDL handle");
        return;
    }
    thread->allocated = false;
    thread->thread_id = 0;
    thread->function = NULL;
    thread->argument = NULL;
    unlock_thread_pool();
}

void SDL_KillThread(SDL_Thread *thread)
{
    int discarded_status;
    int kill_result;
    int join_result;

    if (!thread_handle_is_live(thread))
    {
        SDL_SetError("SDL_KillThread received an invalid thread handle");
        return;
    }

    kill_result = nanos_thread_kill(thread->thread_id);

    /*
     * Killing makes a live kernel task a zombie.  A fast callback may already
     * be a zombie, in which case kill reports ESRCH even though its handle is
     * valid and still needs joining.  Always attempt the join: success resolves
     * both races and makes the kernel task and MiniSDL stack slots reusable.
     */
    join_result =
        nanos_thread_join(thread->thread_id, &discarded_status);
    if (join_result < 0)
    {
        SDL_SetError("could not kill/join thread (kill=%d, join=%d)",
                     kill_result, join_result);
        return;
    }

    if (lock_thread_pool() < 0)
    {
        SDL_SetError("killed a thread but could not reclaim its miniSDL handle");
        return;
    }
    thread->allocated = false;
    thread->thread_id = 0;
    thread->function = NULL;
    thread->argument = NULL;
    unlock_thread_pool();
}

#endif
