#include <NDL.h>
#include <SDL.h>
#include <assert.h>
#include <string.h>

#define keyname(k) #k,

static const char *keyname[] = {
  "NONE",
  _KEYS(keyname)
};
static const int KEANAME_COUNT = sizeof(keyname) / sizeof(keyname[0]);

int SDL_PushEvent(SDL_Event *ev) {
  return 0;
}

// Helper: find the index of 'name' in keyname[], or -1 if not found
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

int SDL_PollEvent(SDL_Event *ev) 
{
  char buf[64];

  // non-blocking fetch from NDL
  const int n = NDL_PollEvent(buf, sizeof(buf));
  if (n <= 0) 
  {
      // no event
      return 0;
  }

  // ensure NUL termination
  buf[n < (int)sizeof(buf) ? n : sizeof(buf) - 1] = '\0';

  char prefix[3];
  char name[32];

  // parse two strings: prefix ("kd" or "ku"), and key name
  if (sscanf(buf, "%2s %31s", prefix, name) != 2) 
  {
      // malformed event
      return 0;
  }

  // map name → keycode
  const int keycode = lookupKeycode(name);
  assert(keycode >= 0);

  // fill SDL_Event
  if (strcmp(prefix, "kd") == 0) 
  {
      ev->type = SDL_KEYDOWN;
  }
  else if (strcmp(prefix, "ku") == 0) 
  {
      ev->type = SDL_KEYUP;
  }
  else 
  {
      assert(0);
      // unknown prefix
      return 0;
  }

  ev->key.keysym.sym = keycode;
  return 1;
}

int SDL_WaitEvent(SDL_Event *event) {
  while (!SDL_PollEvent(event)) { /* busy-wait */ }
  return 1;
}

int SDL_PeepEvents(SDL_Event *ev, int numevents, int action, uint32_t mask) {
  return 0;
}

uint8_t* SDL_GetKeyState(int *numkeys) {
  return NULL;
}
