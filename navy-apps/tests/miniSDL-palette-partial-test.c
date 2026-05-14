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
    SDL_Surface *surface = SDL_CreateRGBSurface(0, 2, 2, 8,
                                                DEFAULT_RMASK, DEFAULT_GMASK,
                                                DEFAULT_BMASK, DEFAULT_AMASK);
    assert(surface != NULL);

    SDL_Color full[256];
    for (int i = 0; i < 256; i++)
    {
        full[i].r = (uint8_t)i;
        full[i].g = (uint8_t)(255 - i);
        full[i].b = (uint8_t)(i ^ 0x55);
        full[i].a = 0xff;
    }

    SDL_SetPalette(surface, SDL_LOGPAL, full, 0, 256);

    SDL_Color one = {.r = 1, .g = 2, .b = 3, .a = 4};
    SDL_SetPalette(surface, SDL_LOGPAL, &one, 5, 1);

    /*
     * A partial update changes only the requested slot and keeps the logical
     * palette capacity, so indexed pixels above the changed range keep working.
     */
    assert(surface->format->palette->ncolors == 256);
    assert(surface->format->palette->colors[4].r == full[4].r);
    assert(surface->format->palette->colors[5].r == 1);
    assert(surface->format->palette->colors[5].g == 2);
    assert(surface->format->palette->colors[5].b == 3);
    assert(surface->format->palette->colors[5].a == 4);
    assert(surface->format->palette->colors[6].r == full[6].r);

    SDL_FreeSurface(surface);
    return 0;
}
