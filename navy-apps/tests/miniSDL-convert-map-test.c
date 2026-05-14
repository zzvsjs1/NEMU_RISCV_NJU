#include <assert.h>
#include <stdint.h>

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
    SDL_Surface *src = SDL_CreateRGBSurface(0, 1, 1, 32,
                                            DEFAULT_RMASK, DEFAULT_GMASK,
                                            DEFAULT_BMASK, DEFAULT_AMASK);
    assert(src != NULL);

    const uint32_t mapped = SDL_MapRGBA(src->format, 0x11, 0x22, 0x33, 0xff);
    assert(mapped == 0xff112233u);
    ((uint32_t *)src->pixels)[0] = mapped;

    SDL_Surface *same = SDL_ConvertSurface(src, src->format, 0);
    assert(same != NULL);
    assert(((uint32_t *)same->pixels)[0] == mapped);

    SDL_PixelFormat swapped = *src->format;
    swapped.Rmask = DEFAULT_BMASK;
    swapped.Rshift = 0;
    swapped.Bmask = DEFAULT_RMASK;
    swapped.Bshift = 16;

    SDL_Surface *converted = SDL_ConvertSurface(src, &swapped, SDL_PREALLOC);
    assert(converted != NULL);
    assert(converted->pixels != NULL);
    assert((((uint8_t *)converted->pixels)[0]) == 0x11);
    assert((((uint8_t *)converted->pixels)[1]) == 0x22);
    assert((((uint8_t *)converted->pixels)[2]) == 0x33);
    assert((((uint8_t *)converted->pixels)[3]) == 0xff);

    SDL_FreeSurface(converted);
    SDL_FreeSurface(same);
    SDL_FreeSurface(src);
    return 0;
}
