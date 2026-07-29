#ifndef __SDL_THREAD_H__
#define __SDL_THREAD_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/*
 * Keep the task record private to miniSDL.  SDL applications only retain the
 * pointer as a join handle, matching the opaque SDL 1.2 public interface.
 */
typedef struct SDL_Thread SDL_Thread;

#ifdef __ISA_NATIVE__
/*
 * The native support library itself uses SDL2's three-argument
 * SDL_CreateThread().  Give miniSDL's SDL 1.2-compatible wrappers private
 * linker names so an application cannot interpose its two-argument symbol on
 * the support library's SDL2 call.
 */
#define SDL_CreateThread miniSDL_CreateThread
#define SDL_ThreadID miniSDL_ThreadID
#define SDL_GetThreadID miniSDL_GetThreadID
#define SDL_WaitThread miniSDL_WaitThread
#define SDL_KillThread miniSDL_KillThread
#endif

SDL_Thread *SDL_CreateThread(int (*fn)(void *), void *data);
uint32_t SDL_ThreadID(void);
uint32_t SDL_GetThreadID(SDL_Thread *thread);
void SDL_WaitThread(SDL_Thread *thread, int *status);
void SDL_KillThread(SDL_Thread *thread);

#ifdef __cplusplus
}
#endif

#endif
