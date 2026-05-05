#include <NDL.h>
#include <sdl-audio.h>
#include <sdl-timer.h>
#include <stdio.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#define MAX_TIMERS 32

typedef struct {
  bool active;
  uint32_t interval;
  uint32_t next_tick;
  SDL_NewTimerCallback callback;
  void *param;
} TimerSlot;

static TimerSlot timers[MAX_TIMERS];

static bool tick_reached(uint32_t now, uint32_t target)
{
  return (int32_t)(now - target) >= 0;
}

SDL_TimerID SDL_AddTimer(uint32_t interval, SDL_NewTimerCallback callback, void *param) 
{
  if (callback == NULL) return NULL;
  if (interval == 0) interval = 1;

  for (int i = 0; i < MAX_TIMERS; i++)
  {
    if (!timers[i].active)
    {
      timers[i] = (TimerSlot) {
        .active = true,
        .interval = interval,
        .next_tick = SDL_GetTicks() + interval,
        .callback = callback,
        .param = param,
      };
      return &timers[i];
    }
  }

  return NULL;
}

int SDL_RemoveTimer(SDL_TimerID id) 
{
  TimerSlot *slot = (TimerSlot *)id;
  if (slot == NULL) return 0;

  for (int i = 0; i < MAX_TIMERS; i++)
  {
    if (&timers[i] == slot && timers[i].active)
    {
      timers[i].active = false;
      return 1;
    }
  }

  return 0;
}

void SDL_CheckTimers(void)
{
  const uint32_t now = SDL_GetTicks();

  for (int i = 0; i < MAX_TIMERS; i++)
  {
    TimerSlot *slot = &timers[i];
    if (!slot->active || !tick_reached(now, slot->next_tick)) continue;

    /*
     * The callback may remove its own timer.  We only reschedule if the slot
     * is still active after the callback returns.
     */
    uint32_t next_interval = slot->callback(slot->interval, slot->param);
    if (!slot->active) continue;

    if (next_interval == 0)
    {
      slot->active = false;
    }
    else
    {
      slot->interval = next_interval;
      slot->next_tick = SDL_GetTicks() + next_interval;
    }
  }
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
      SDL_CheckTimers();
      SDL_PumpAudio();
      // A very short host sleep would be nice on native OS, but is not required.
  }

  SDL_CheckTimers();
}
