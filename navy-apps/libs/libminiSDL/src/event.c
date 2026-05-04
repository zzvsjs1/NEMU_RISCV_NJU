#include <NDL.h>
#include <SDL.h>
#include <assert.h>
#include <string.h>

#define keyname(k) #k,

static const char *keyname[] = {
  "NONE",
  _KEYS(keyname)
};
#define KEANAME_COUNT (sizeof(keyname) / sizeof(keyname[0]))

static uint8_t keyStates[KEANAME_COUNT] = { 0 };


/* Ring buffer for outgoing SDL_Events. */
#define EVENT_QUEUE_SIZE 128
static SDL_Event  eventQueue[EVENT_QUEUE_SIZE];
static int queueHead = 0;
static int queueTail = 0;

static int queueEmpty(void)
{
  return queueHead == queueTail;
}

static int eventMatchesMask(const SDL_Event *ev, uint32_t mask)
{
  return (mask & SDL_EVENTMASK(ev->type)) != 0;
}

/* Enqueue, dropping the oldest event if the fixed queue is full. */
static void enqueueEvent(const SDL_Event *ev) 
{
  const int next = (queueTail + 1) % EVENT_QUEUE_SIZE;
  if (next == queueHead) 
  {
      /* Buffer full: overwrite the oldest event to keep recent input live. */
      queueHead = (queueHead + 1) % EVENT_QUEUE_SIZE;
  }

  eventQueue[queueTail] = *ev;
  queueTail = next;
}

/* Dequeue one event. Return 1 on success and 0 when the queue is empty. */
static int dequeueEvent(SDL_Event *ev) 
{
  if (queueEmpty())
  {
    return 0;
  }

  *ev = eventQueue[queueHead];
  queueHead = (queueHead + 1) % EVENT_QUEUE_SIZE;
  return 1;
}


// Find the index of 'name' in keyname[], or -1 if not found.
static int lookupKeycode(const char *name) 
{
  for (int i = 0; i < KEANAME_COUNT; i++) 
  {
      if (strcmp(name, keyname[i]) == 0) 
      {
          return i;
      }
  }

  return -1;
}

/* Read every raw NDL event, update keyStates, and enqueue translated events. */
static void pumpKeyboardEvents(void) 
{
  char buf[64];

  while (true) 
  {
      const int n = NDL_PollEvent(buf, sizeof(buf));
      if (n <= 0) 
      {
        break;
      }

      buf[(n < (int)sizeof(buf) ? n : sizeof(buf)-1)] = '\0';

      char prefix[3], name[32];
      if (sscanf(buf, "%2s %31s", prefix, name) != 2) 
      {
        continue;
      }

      int kc = lookupKeycode(name);
      if (kc < 0) 
      {
        continue;
      }

      SDL_Event ev = {0};
      if (strcmp(prefix, "kd") == 0) 
      {
          ev.type = SDL_KEYDOWN;
          keyStates[kc] = 1;
      }
      else if (strcmp(prefix, "ku") == 0) 
      {
          ev.type = SDL_KEYUP;
          keyStates[kc] = 0;
      }
      else 
      {
        SDL_PrintErr("Should not reach here.\n");
      }

      ev.key.keysym.sym = kc;
      enqueueEvent(&ev);
  }
}

int SDL_PushEvent(SDL_Event *ev) 
{
  if (ev == NULL) return -1;
  enqueueEvent(ev);
  return 0;
}

void SDL_PumpEvents(void) 
{
  void SDL_CheckTimers(void);

  pumpKeyboardEvents();
  SDL_CheckTimers();

  void CallbackHelper(void);
  extern int g_in_audio_cb;
  if (!g_in_audio_cb) CallbackHelper(); // keep audio flowing
}

int SDL_PollEvent(SDL_Event *ev) 
{
  SDL_PumpEvents();
  return dequeueEvent(ev);
}

int SDL_WaitEvent(SDL_Event *event) 
{
  while (!SDL_PollEvent(event)) { /* busy-wait */ }
  return 1;
}

int SDL_PeepEvents(SDL_Event *ev, int numevents, int action, uint32_t mask) 
{
  if (numevents <= 0) return 0;

  if (action == SDL_ADDEVENT)
  {
    for (int i = 0; i < numevents; i++) enqueueEvent(&ev[i]);
    return numevents;
  }

  SDL_PumpEvents();

  int matched = 0;
  SDL_Event tmp[EVENT_QUEUE_SIZE];
  int tmp_count = 0;

  /*
   * Rebuild the queue after scanning. This keeps event ordering stable while
   * allowing SDL_GETEVENT to remove only the events that matched the mask.
   */
  while (!queueEmpty())
  {
    SDL_Event cur;
    dequeueEvent(&cur);

    const int take = matched < numevents && eventMatchesMask(&cur, mask);
    if (take)
    {
      if (ev != NULL) ev[matched] = cur;
      matched++;
    }

    if (action != SDL_GETEVENT || !take)
    {
      assert(tmp_count < EVENT_QUEUE_SIZE);
      tmp[tmp_count++] = cur;
    }
  }

  queueHead = queueTail = 0;
  for (int i = 0; i < tmp_count; i++) enqueueEvent(&tmp[i]);

  return matched;
}

uint8_t* SDL_GetKeyState(int *numkeys) 
{
  pumpKeyboardEvents();

  // Return size of our array
  if (numkeys) 
  {
    *numkeys = KEANAME_COUNT;
  }

  // Caller must NOT free this pointer.
  return keyStates;
}
