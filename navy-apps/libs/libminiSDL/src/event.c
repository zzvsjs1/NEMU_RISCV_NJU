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

static int mouseX = 0;
static int mouseY = 0;
static uint8_t mouseButtons = 0;

#define NDL_MOUSE_LEFT_MASK   (1 << 0)
#define NDL_MOUSE_MIDDLE_MASK (1 << 1)
#define NDL_MOUSE_RIGHT_MASK  (1 << 2)

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

static uint8_t lookupMouseButton(const char *name)
{
  if (strcmp(name, "LEFT") == 0) return SDL_BUTTON_LEFT;
  if (strcmp(name, "MIDDLE") == 0) return SDL_BUTTON_MIDDLE;
  if (strcmp(name, "RIGHT") == 0) return SDL_BUTTON_RIGHT;
  if (strcmp(name, "WHEELUP") == 0) return SDL_BUTTON_WHEELUP;
  if (strcmp(name, "WHEELDOWN") == 0) return SDL_BUTTON_WHEELDOWN;
  return 0;
}

static uint8_t translateMouseButtons(int buttons)
{
  uint8_t state = 0;
  if (buttons & NDL_MOUSE_LEFT_MASK) state |= SDL_BUTTON(SDL_BUTTON_LEFT);
  if (buttons & NDL_MOUSE_MIDDLE_MASK) state |= SDL_BUTTON(SDL_BUTTON_MIDDLE);
  if (buttons & NDL_MOUSE_RIGHT_MASK) state |= SDL_BUTTON(SDL_BUTTON_RIGHT);
  return state;
}

static void updateMouseStateFromEvent(const SDL_Event *ev)
{
  if (ev->type == SDL_MOUSEMOTION)
  {
    mouseX = ev->motion.x;
    mouseY = ev->motion.y;
    mouseButtons = ev->motion.state;
  }
  else if (ev->type == SDL_MOUSEBUTTONDOWN)
  {
    mouseX = ev->button.x;
    mouseY = ev->button.y;
    if (ev->button.button <= SDL_BUTTON_RIGHT)
    {
      mouseButtons |= SDL_BUTTON(ev->button.button);
    }
  }
  else if (ev->type == SDL_MOUSEBUTTONUP)
  {
    mouseX = ev->button.x;
    mouseY = ev->button.y;
    if (ev->button.button <= SDL_BUTTON_RIGHT)
    {
      mouseButtons &= (uint8_t)~SDL_BUTTON(ev->button.button);
    }
  }
}

/* Append an event without changing derived input state. */
static void enqueueEventRaw(const SDL_Event *ev)
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

/* Enqueue, dropping the oldest event if the fixed queue is full. */
static void enqueueEvent(const SDL_Event *ev)
{
  updateMouseStateFromEvent(ev);
  enqueueEventRaw(ev);
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

/* Read every raw NDL input record and enqueue translated SDL events. */
static void pumpInputEvents(void)
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

      char prefix[3] = {};
      if (sscanf(buf, "%2s", prefix) != 1)
      {
        continue;
      }

      int x = 0, y = 0, buttons = 0, dx = 0, dy = 0;
      char button_name[16] = {};

      if (strcmp(prefix, "mm") == 0 &&
          sscanf(buf, "%*s %d %d %d", &x, &y, &buttons) == 3)
      {
        /* NDL reports physical framebuffer coordinates; SDL apps see the canvas. */
        NDL_TranslateMouse(&x, &y);
        SDL_Event ev = {};
        ev.type = SDL_MOUSEMOTION;
        ev.motion.x = x;
        ev.motion.y = y;
        ev.motion.state = translateMouseButtons(buttons);
        ev.motion.xrel = x - mouseX;
        ev.motion.yrel = y - mouseY;
        enqueueEvent(&ev);
        continue;
      }

      if ((strcmp(prefix, "md") == 0 || strcmp(prefix, "mu") == 0) &&
          sscanf(buf, "%*s %15s %d %d %d", button_name, &x, &y, &buttons) == 4)
      {
        /* Remove the centred canvas border before publishing the button event. */
        NDL_TranslateMouse(&x, &y);
        SDL_Event ev = {};
        ev.type = strcmp(prefix, "md") == 0 ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
        ev.button.button = lookupMouseButton(button_name);
        ev.button.state = translateMouseButtons(buttons);
        ev.button.x = x;
        ev.button.y = y;
        if (ev.button.button != 0) enqueueEvent(&ev);
        continue;
      }

      if (strcmp(prefix, "mw") == 0 &&
          sscanf(buf, "%*s %d %d %d %d %d", &dx, &dy, &x, &y, &buttons) == 5)
      {
        if (dy == 0)
        {
          continue;
        }

        /* Wheel pseudo-buttons keep normal button semantics but use canvas-local x/y. */
        NDL_TranslateMouse(&x, &y);
        SDL_Event ev = {};
        ev.type = SDL_MOUSEBUTTONDOWN;
        ev.button.button = dy > 0 ? SDL_BUTTON_WHEELUP : SDL_BUTTON_WHEELDOWN;
        ev.button.state = translateMouseButtons(buttons);
        ev.button.x = x;
        ev.button.y = y;
        enqueueEvent(&ev);
        continue;
      }

      char name[32] = {};
      if ((strcmp(prefix, "kd") != 0 && strcmp(prefix, "ku") != 0) ||
          sscanf(buf, "%*s %31s", name) != 1)
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

  pumpInputEvents();
  SDL_CheckTimers();

  SDL_PumpAudio();
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
  for (int i = 0; i < tmp_count; i++) enqueueEventRaw(&tmp[i]);

  return matched;
}

uint8_t* SDL_GetKeyState(int *numkeys) 
{
  pumpInputEvents();

  // Return size of our array
  if (numkeys) 
  {
    *numkeys = KEANAME_COUNT;
  }

  // Caller must NOT free this pointer.
  return keyStates;
}

uint8_t SDL_GetMouseState(int *x, int *y)
{
  pumpInputEvents();

  if (x) *x = mouseX;
  if (y) *y = mouseY;
  return mouseButtons;
}
