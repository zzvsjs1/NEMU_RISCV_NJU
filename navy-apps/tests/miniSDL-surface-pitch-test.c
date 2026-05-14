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

#include "../libs/libminiSDL/src/video.c"

int main(void)
{
    uint8_t pixels[10];
    memset(pixels, 0x7e, sizeof(pixels));

    SDL_Surface *surface = SDL_CreateRGBSurfaceFrom(pixels, 3, 2, 8, 5,
                                                    DEFAULT_RMASK, DEFAULT_GMASK,
                                                    DEFAULT_BMASK, DEFAULT_AMASK);
    assert(surface != NULL);
    assert(surface->pitch == 5);

    SDL_Rect bottom_right = {.x = 2, .y = 1, .w = 1, .h = 1};
    SDL_FillRect(surface, &bottom_right, 0x42);

    /*
     * The write must use the caller's padded pitch.  The padding bytes remain
     * untouched, while the real bottom-right pixel is updated.
     */
    assert(pixels[5 + 2] == 0x42);
    assert(pixels[3] == 0x7e);
    assert(pixels[4] == 0x7e);

    SDL_FreeSurface(surface);
    return 0;
}
