#include <assert.h>
#include <stdint.h>
#include <string.h>

#include <NDL.h>

void NDL_OpenCanvas(int *w, int *h)
{
  (void)w;
  (void)h;
}

void NDL_DrawRect(uint32_t *pixels, int x, int y, int w, int h)
{
  (void)pixels;
  (void)x;
  (void)y;
  (void)w;
  (void)h;
}

/*
 * Include the implementation directly after stubbing NDL, matching the other
 * miniSDL regression tests.  The test keeps all pixels in host memory, so an
 * incorrect fill is visible as a byte change without needing a Navy runtime.
 */
#include "../libs/libminiSDL/src/video.c"

int main(void)
{
  enum {
    SURFACE_W = 4,
    SURFACE_H = 3,
    /*
     * The old clipping bug converted a negative clipped height into a uint16_t
     * value near 65535.  This larger backing store lets that bad loop complete
     * and keeps the failure as an assertion instead of a host segfault.
     */
    GUARD_BYTES = SURFACE_W * 65536,
  };

  static uint8_t pixels[GUARD_BYTES];
  memset(pixels, 0x5a, sizeof(pixels));

  SDL_Surface *surface = SDL_CreateRGBSurfaceFrom(pixels, SURFACE_W, SURFACE_H, 8,
      SURFACE_W, DEFAULT_RMASK, DEFAULT_GMASK, DEFAULT_BMASK, DEFAULT_AMASK);

  SDL_Rect below_surface = {
    .x = 1,
    .y = SURFACE_H + 1,
    .w = 1,
    .h = 1,
  };

  SDL_FillRect(surface, &below_surface, 0xff);

  /*
   * A rectangle that starts below the surface is empty.  The byte selected here
   * is the first location the buggy unsigned-height loop used to overwrite.
   */
  assert(pixels[(SURFACE_H + 1) * SURFACE_W + 1] == 0x5a);

  SDL_FreeSurface(surface);
  return 0;
}
