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
    SDL_Surface *surface = SDL_CreateRGBSurface(0, 4, 4, 8,
                                                DEFAULT_RMASK, DEFAULT_GMASK,
                                                DEFAULT_BMASK, DEFAULT_AMASK);
    assert(surface != NULL);

    for (uint8_t i = 0; i < 16; i++)
        surface->pixels[i] = i;

    SDL_Rect src = {.x = 0, .y = 0, .w = 4, .h = 3};
    SDL_Rect dst = {.x = 0, .y = 1, .w = 4, .h = 3};
    SDL_BlitSurface(surface, &src, surface, &dst);

    static const uint8_t expected[16] = {
        0, 1, 2, 3,
        0, 1, 2, 3,
        4, 5, 6, 7,
        8, 9, 10, 11,
    };

    assert(memcmp(surface->pixels, expected, sizeof(expected)) == 0);

    SDL_FreeSurface(surface);
    return 0;
}
