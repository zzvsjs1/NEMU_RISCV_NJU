#ifndef __SDL_GENERAL_H__
#define __SDL_GENERAL_H__

#include <stdint.h>
#include <stdio.h>

#define SDL_INIT_VIDEO 0x01
#define SDL_INIT_TIMER 0x02
#define SDL_INIT_AUDIO 0x04
#define SDL_INIT_NOPARACHUTE 0x08
#define SDL_INIT_JOYSTICK 0x10

/*
 * SDL 1.2 exposes this compact version structure, and SDL_mixer users often
 * inspect it at start-up to enable or disable old library workarounds.  Navy's
 * miniSDL does not use runtime-loaded SDL libraries, but keeping the public type
 * lets those callers compile without pulling in a full SDL compatibility layer.
 */
typedef struct
{
    uint8_t major;
    uint8_t minor;
    uint8_t patch;
} SDL_version;

/*
 * Match SDL's numeric ordering helper: major has the largest weight, then minor,
 * then patch.  This is intentionally a macro because upstream SDL headers expose
 * it that way, and existing code may use it in preprocessor-friendly contexts.
 */
#define SDL_VERSIONNUM(X, Y, Z) ((X) * 1000 + (Y) * 100 + (Z))

int SDL_Init(uint32_t flags);
void SDL_Quit();
/*
 * miniSDL initialises NDL as a single process-wide runtime.  Subsystem shutdown
 * is still declared for SDL 1.2 source compatibility; concrete resources are
 * released by SDL_CloseAudio(), SDL_FreeSurface(), and the other owner APIs.
 */
void SDL_QuitSubSystem(uint32_t flags);
char *SDL_GetError();
int SDL_SetError(const char *fmt, ...);
int SDL_ShowCursor(int toggle);
void SDL_WM_SetCaption(const char *title, const char *icon);

typedef struct SDL_mutex
{
} SDL_mutex;

static inline SDL_mutex *SDL_CreateMutex() { return NULL; }

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

#endif
