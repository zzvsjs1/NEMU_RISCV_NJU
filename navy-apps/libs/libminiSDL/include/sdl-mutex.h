#ifndef __SDL_MUTEX_H__
#define __SDL_MUTEX_H__

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

/*
 * SDL 1.2 reports a non-blocking lock miss with this positive value.  Negative
 * returns remain available for invalid objects and kernel errors.
 */
#define SDL_MUTEX_TIMEDOUT 1

#ifdef __ISA_NATIVE__

/*
 * Native miniSDL historically supplied no-op mutexes.  Preserve that behaviour
 * for host-side tools, which do not link the Navy syscall runtime.
 */
typedef struct SDL_mutex
{
} SDL_mutex;

static inline SDL_mutex *SDL_CreateMutex(void) { return NULL; }

static inline void SDL_DestroyMutex(SDL_mutex *mutex) { (void)mutex; }

static inline int SDL_mutexP(SDL_mutex *mutex)
{
    (void)mutex;
    return 0;
}

static inline int SDL_mutexV(SDL_mutex *mutex)
{
    (void)mutex;
    return 0;
}

static inline int SDL_LockMutex(SDL_mutex *mutex)
{
    return SDL_mutexP(mutex);
}

static inline int SDL_TryLockMutex(SDL_mutex *mutex)
{
    return SDL_mutexP(mutex);
}

static inline int SDL_UnlockMutex(SDL_mutex *mutex)
{
    return SDL_mutexV(mutex);
}

#else

typedef struct SDL_mutex SDL_mutex;

SDL_mutex *SDL_CreateMutex(void);
void SDL_DestroyMutex(SDL_mutex *mutex);
int SDL_mutexP(SDL_mutex *mutex);
int SDL_mutexV(SDL_mutex *mutex);
int SDL_LockMutex(SDL_mutex *mutex);
int SDL_TryLockMutex(SDL_mutex *mutex);
int SDL_UnlockMutex(SDL_mutex *mutex);

#endif

#ifdef __cplusplus
}
#endif

#endif
