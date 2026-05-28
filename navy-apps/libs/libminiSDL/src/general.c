#include <NDL.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>

static char sdl_error_message[256] = "No SDL error recorded";

int SDL_Init(uint32_t flags)
{
    return NDL_Init(flags);
}

void SDL_Quit()
{
    NDL_Quit();
}

void SDL_QuitSubSystem(uint32_t flags)
{
    /*
     * miniSDL has a process-wide NDL initialisation model rather than independent
     * SDL subsystems.  Keep this SDL 1.2 entry point as a source-compatible no-op;
     * concrete resources such as audio devices are closed by their own APIs.
     *
     * This matters for old SDL ports that call SDL_QuitSubSystem(SDL_INIT_AUDIO)
     * after Mix_CloseAudio().  Returning here preserves their shutdown sequence
     * without tearing down framebuffer or event state that may still be owned by
     * the rest of the Navy process.
     */
    (void)flags;
}

char *SDL_GetError()
{
    return sdl_error_message;
}

int SDL_SetError(const char *fmt, ...)
{
    /*
     * SDL callers often report a failed operation by immediately printing
     * SDL_GetError().  Returning a fixed placeholder hides the useful reason,
     * which made video-mode failures look like a generic Navy limitation.  A
     * single process-local buffer matches miniSDL's single-threaded execution
     * model and is enough to carry the most recent failure across that API
     * boundary.
     */
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(sdl_error_message, sizeof(sdl_error_message), fmt, ap);
    va_end(ap);

    sdl_error_message[sizeof(sdl_error_message) - 1] = '\0';
    return -1;
}

int SDL_ShowCursor(int toggle)
{
    return 0;
}

void SDL_WM_SetCaption(const char *title, const char *icon)
{
    // Do nothing.
}
