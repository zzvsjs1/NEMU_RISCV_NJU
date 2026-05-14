#include <assert.h>
#include <stdint.h>
#include <string.h>

#include <NDL.h>

static uint32_t captured_pixels[16];
static int captured_x = -1;
static int captured_y = -1;
static int captured_w = -1;
static int captured_h = -1;

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
    memcpy(captured_pixels, pixels, (size_t)w * (size_t)h * sizeof(captured_pixels[0]));
}

#include "../libs/libminiSDL/src/video.c"

int main(void)
{
    SDL_Surface *src = SDL_CreateRGBSurface(0, 2, 2, 32,
                                            DEFAULT_RMASK, DEFAULT_GMASK,
                                            DEFAULT_BMASK, DEFAULT_AMASK);
    SDL_Surface *dst = SDL_CreateRGBSurface(SDL_HWSURFACE, 4, 4, 32,
                                            DEFAULT_RMASK, DEFAULT_GMASK,
                                            DEFAULT_BMASK, DEFAULT_AMASK);
    assert(src != NULL && dst != NULL);

    uint32_t *src_pixels = (uint32_t *)src->pixels;
    src_pixels[0] = 1;
    src_pixels[1] = 2;
    src_pixels[2] = 3;
    src_pixels[3] = 4;

    SDL_Rect dstrect = {.x = 0, .y = 0, .w = 4, .h = 4};
    SDL_SoftStretchUpdate(src, NULL, dst, &dstrect);

    static const uint32_t expected[16] = {
        1, 1, 2, 2,
        1, 1, 2, 2,
        3, 3, 4, 4,
        3, 3, 4, 4,
    };

    assert(dstrect.x == 0 && dstrect.y == 0);
    assert(dstrect.w == 4 && dstrect.h == 4);
    assert(captured_x == 0 && captured_y == 0);
    assert(captured_w == 4 && captured_h == 4);
    assert(memcmp(dst->pixels, expected, sizeof(expected)) == 0);
    assert(memcmp(captured_pixels, expected, sizeof(expected)) == 0);

    SDL_FreeSurface(dst);
    SDL_FreeSurface(src);
    return 0;
}
