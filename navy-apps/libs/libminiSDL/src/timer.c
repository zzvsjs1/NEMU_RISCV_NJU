#include <NDL.h>
#include <sdl-timer.h>
#include <stdio.h>
#include <assert.h>

SDL_TimerID SDL_AddTimer(uint32_t interval, SDL_NewTimerCallback callback, void *param) 
{
  assert(0);

  return NULL;
}

int SDL_RemoveTimer(SDL_TimerID id) 
{
  assert(0);

  return 1;
}

uint32_t SDL_GetTicks() 
{
  return NDL_GetTicks();
}

void SDL_Delay(uint32_t ms) 
{
  // Record starting tick.
  const uint32_t start = SDL_GetTicks();

  // Loop until the requested delay has elapsed
  while ((SDL_GetTicks() - start) < ms) 
  {
      // nothing; just spin
  }
}
