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
    uint8_t raw[16];
    memset(raw, 0, sizeof(raw));

    SDL_Surface *surface = SDL_CreateRGBSurfaceFrom(raw + 1, 1, 1, 32, 4,
                                                    DEFAULT_RMASK, DEFAULT_GMASK,
                                                    DEFAULT_BMASK, DEFAULT_AMASK);
    assert(surface != NULL);

    uint32_t colour = SDL_MapRGBA(surface->format, 0x80, 0x01, 0x02, 0xff);
    SDL_FillRect(surface, NULL, colour);

    SDL_PixelFormat swapped = *surface->format;
    swapped.Rmask = DEFAULT_BMASK;
    swapped.Rshift = 0;
    swapped.Bmask = DEFAULT_RMASK;
    swapped.Bshift = 16;

    SDL_Surface *converted = SDL_ConvertSurface(surface, &swapped, 0);
    assert(converted != NULL);

    SDL_FreeSurface(converted);
    SDL_FreeSurface(surface);
    return 0;
}
