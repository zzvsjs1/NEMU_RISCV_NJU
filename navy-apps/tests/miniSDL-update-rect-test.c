#include <assert.h>
#include <stdint.h>
#include <string.h>

#include <NDL.h>

static uint32_t captured_pixels[16];
static int captured_x = -1;
static int captured_y = -1;
static int captured_w = -1;
static int captured_h = -1;

static void reset_capture(void)
{
  captured_x = -1;
  captured_y = -1;
  captured_w = -1;
  captured_h = -1;
  memset(captured_pixels, 0, sizeof(captured_pixels));
}

void NDL_OpenCanvas(int *w, int *h)
{
  (void)w;
  (void)h;
}

void NDL_DrawRect(uint32_t *pixels, int x, int y, int w, int h)
{
  captured_x = x;
  captured_y = y;
  captured_w = w;
  captured_h = h;
  memcpy(captured_pixels, pixels, (size_t)w * h * sizeof(captured_pixels[0]));
}

#include "../libs/libminiSDL/src/video.c"

int main(void)
{
  SDL_Surface *surface = SDL_CreateRGBSurface(0, 4, 3, 32,
      DEFAULT_RMASK, DEFAULT_GMASK, DEFAULT_BMASK, DEFAULT_AMASK);

  uint32_t *pixels = (uint32_t *)surface->pixels;
  for (uint32_t i = 0; i < 12; i++) {
    pixels[i] = i;
  }

  SDL_UpdateRect(surface, 1, 1, 2, 2);

  assert(captured_x == 1);
  assert(captured_y == 1);
  assert(captured_w == 2);
  assert(captured_h == 2);

  /*
   * NDL_DrawRect() consumes a tightly packed w*h pixel array.  A partial
   * update from a wider surface must therefore skip the source pitch between
   * rows before passing data to NDL.
   */
  assert(captured_pixels[0] == 5);
  assert(captured_pixels[1] == 6);
  assert(captured_pixels[2] == 9);
  assert(captured_pixels[3] == 10);

  reset_capture();
  SDL_UpdateRect(surface, 0, 0, 0, 0);
  assert(captured_x == 0);
  assert(captured_y == 0);
  assert(captured_w == 4);
  assert(captured_h == 3);

  reset_capture();
  SDL_UpdateRect(surface, 1, 1, 0, 2);
  assert(captured_w == -1);
  assert(captured_h == -1);

  SDL_FreeSurface(surface);
  return 0;
}
