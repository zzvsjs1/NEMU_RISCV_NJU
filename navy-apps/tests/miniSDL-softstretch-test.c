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
 * Pull in the implementation under test after stubbing NDL. The surface data
 * stays in process memory, so failures point at SDL_SoftStretch arithmetic
 * rather than display output.
 */
#include "../libs/libminiSDL/src/video.c"

static void fill_source(SDL_Surface *src)
{
  uint8_t *pixels = src->pixels;
  pixels[0] = 1;
  pixels[1] = 2;
  pixels[2] = 3;
  pixels[3] = 4;
}

static void check_scaled_pixels(SDL_Surface *dst)
{
  /*
   * Use tiny labelled pixels so the expected 2x stretch is easy to audit:
   * each source pixel expands into a 2x2 block, and row order must remain
   * top-to-bottom.  This guards the PAL compatibility path used on larger Navy
   * displays.
   */
  static const uint8_t expected[16] = {
    1, 1, 2, 2,
    1, 1, 2, 2,
    3, 3, 4, 4,
    3, 3, 4, 4,
  };

  assert(memcmp(dst->pixels, expected, sizeof(expected)) == 0);
}

int main(void)
{
  SDL_Surface *src = SDL_CreateRGBSurface(0, 2, 2, 8,
      DEFAULT_RMASK, DEFAULT_GMASK, DEFAULT_BMASK, DEFAULT_AMASK);
  SDL_Surface *dst = SDL_CreateRGBSurface(0, 4, 4, 8,
      DEFAULT_RMASK, DEFAULT_GMASK, DEFAULT_BMASK, DEFAULT_AMASK);
  SDL_Surface *dst_null_rect = SDL_CreateRGBSurface(0, 4, 4, 8,
      DEFAULT_RMASK, DEFAULT_GMASK, DEFAULT_BMASK, DEFAULT_AMASK);

  fill_source(src);

  SDL_Rect dstrect = { .x = 0, .y = 0, .w = 4, .h = 4 };
  SDL_SoftStretch(src, NULL, dst, &dstrect);
  assert(dstrect.x == 0 && dstrect.y == 0);
  assert(dstrect.w == 4 && dstrect.h == 4);
  check_scaled_pixels(dst);

  /*
   * PAL has compatibility paths that pass a NULL destination rectangle. Treat
   * that as "stretch into the whole destination surface" rather than asserting,
   * so old apps keep using the current miniSDL contract after the 800x600 work.
   */
  SDL_SoftStretch(src, NULL, dst_null_rect, NULL);
  check_scaled_pixels(dst_null_rect);

  SDL_FreeSurface(dst_null_rect);
  SDL_FreeSurface(dst);
  SDL_FreeSurface(src);
  return 0;
}
