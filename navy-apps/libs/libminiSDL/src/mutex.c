#include <SDL.h>

#ifndef __ISA_NATIVE__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "thread_runtime.h"

enum
{
    SDL_MUTEX_SLOTS = 64
};

struct SDL_mutex
{
    bool allocated;
};

static struct SDL_mutex mutex_pool[SDL_MUTEX_SLOTS];
static unsigned char mutex_pool_lock;

static int lock_mutex_pool(void)
{
    return nanos_lock_acquire(&mutex_pool_lock, false, false);
}

static void unlock_mutex_pool(void)
{
    if (nanos_lock_release(&mutex_pool_lock) < 0)
    {
        SDL_SetError("failed to release miniSDL's mutex-pool lock");
    }
}

static bool mutex_handle_is_live(const SDL_mutex *mutex)
{
    uintptr_t address = (uintptr_t)mutex;
    uintptr_t first = (uintptr_t)&mutex_pool[0];
    uintptr_t limit = (uintptr_t)&mutex_pool[SDL_MUTEX_SLOTS];

    if (address < first || address >= limit)
    {
        return false;
    }
    if ((address - first) % sizeof(mutex_pool[0]) != 0)
    {
        return false;
    }

    return mutex->allocated;
}

SDL_mutex *SDL_CreateMutex(void)
{
    SDL_mutex *mutex = NULL;

    /*
     * The runtime lock adapter is a no-op until SDL_CreateThread activates
     * threading.  Consequently old single-threaded programs retain their
     * syscall-free behaviour, while later creations are serialised properly.
     */
    if (lock_mutex_pool() < 0)
    {
        SDL_SetError("failed to lock miniSDL's mutex pool");
        return NULL;
    }

    for (size_t slot = 0; slot < SDL_MUTEX_SLOTS; slot++)
    {
        if (!mutex_pool[slot].allocated)
        {
            mutex = &mutex_pool[slot];
            mutex->allocated = true;
            break;
        }
    }

    unlock_mutex_pool();
    if (mutex == NULL)
    {
        SDL_SetError("miniSDL supports at most %d mutex objects",
                     SDL_MUTEX_SLOTS);
    }
    return mutex;
}

void SDL_DestroyMutex(SDL_mutex *mutex)
{
    if (!mutex_handle_is_live(mutex))
    {
        return;
    }

    /*
     * SDL leaves destroying a locked mutex undefined.  Marking an unlocked
     * handle reusable is therefore sufficient; its stable address also lets a
     * later allocation reuse the kernel mutex record without a destroy syscall.
     */
    if (lock_mutex_pool() < 0)
    {
        SDL_SetError("failed to lock miniSDL's mutex pool");
        return;
    }
    mutex->allocated = false;
    unlock_mutex_pool();
}

int SDL_LockMutex(SDL_mutex *mutex)
{
    int result;

    if (!mutex_handle_is_live(mutex))
    {
        return SDL_SetError("SDL_LockMutex received an invalid mutex");
    }

    /*
     * SDL mutexes are recursive: the owning task may lock the same object more
     * than once, and must perform the matching number of unlock operations.
     */
    result = nanos_lock_acquire(mutex, true, false);
    if (result < 0)
    {
        return SDL_SetError("Nanos-lite rejected SDL_LockMutex (%d)", result);
    }
    return 0;
}

int SDL_TryLockMutex(SDL_mutex *mutex)
{
    int result;

    if (!mutex_handle_is_live(mutex))
    {
        return SDL_SetError("SDL_TryLockMutex received an invalid mutex");
    }

    result = nanos_lock_acquire(mutex, true, true);
    if (result > 0)
    {
        return SDL_MUTEX_TIMEDOUT;
    }
    if (result < 0)
    {
        return SDL_SetError("Nanos-lite rejected SDL_TryLockMutex (%d)",
                            result);
    }
    return 0;
}

int SDL_UnlockMutex(SDL_mutex *mutex)
{
    int result;

    if (!mutex_handle_is_live(mutex))
    {
        return SDL_SetError("SDL_UnlockMutex received an invalid mutex");
    }

    result = nanos_lock_release(mutex);
    if (result < 0)
    {
        return SDL_SetError("Nanos-lite rejected SDL_UnlockMutex (%d)", result);
    }
    return 0;
}

int SDL_mutexP(SDL_mutex *mutex)
{
    return SDL_LockMutex(mutex);
}

int SDL_mutexV(SDL_mutex *mutex)
{
    return SDL_UnlockMutex(mutex);
}

#endif
