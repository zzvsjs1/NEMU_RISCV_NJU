#include <NDL.h>
#include <SDL.h>

#define keyname(k) #k,

static const char *keyname[] = {
  "NONE",
  _KEYS(keyname)
};

int SDL_PushEvent(SDL_Event *ev) {
  return 0;
}

int SDL_PollEvent(SDL_Event *ev) 
{
  char buf[32];
  int len = NDL_PollEvent(buf, sizeof(buf) - 1);
  if (len <= 0) return 0;
  buf[len] = '\0';

  // parse "kd KEYNAME" or "ku KEYNAME"
  char type[3], name[32];
  if (sscanf(buf, "%2s %31s", type, name) != 2) return 0;

  SDL_KeyboardEvent *ke = &ev->key;
  if (strcmp(type, "kd") == 0) {
    ev->type = SDL_KEYDOWN;
  } else {
    ev->type = SDL_KEYUP;
  }

  // map name back to keycode
  ke->keysym.sym = 0;
  for (int i = 0; i < sizeof(keyname)/sizeof(keyname[0]); i++) {
    if (keyname[i] && strcmp(keyname[i], name) == 0) {
      ke->keysym.sym = i;
      break;
    }
  }

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
