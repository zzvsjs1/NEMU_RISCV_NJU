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
    memcpy(captured_pixels, pixels, (size_t)w * (size_t)h * sizeof(captured_pixels[0]));
}

#include "../libs/libminiSDL/src/video.c"

static void check_32_bit_negative_x_clip(void)
{
    uint32_t backing[32];
    for (uint32_t i = 0; i < sizeof(backing) / sizeof(backing[0]); i++)
        backing[i] = 1000u + i;

    SDL_Surface *surface = SDL_CreateRGBSurfaceFrom(&backing[4], 4, 3, 32,
                                                    4 * (int)sizeof(uint32_t),
                                                    DEFAULT_RMASK, DEFAULT_GMASK,
                                                    DEFAULT_BMASK, DEFAULT_AMASK);
    assert(surface != NULL);

    for (uint32_t i = 0; i < 12; i++)
        ((uint32_t *)surface->pixels)[i] = i;

    SDL_UpdateRect(surface, -1, 1, 3, 1);

    /*
     * The visible part of [-1, 1, 3, 1] is [0, 1, 2, 1].  The two pixels must
     * be the first two values from row 1, not the guard pixel before the row.
     */
    assert(captured_x == 0);
    assert(captured_y == 1);
    assert(captured_w == 2);
    assert(captured_h == 1);
    assert(captured_pixels[0] == 4);
    assert(captured_pixels[1] == 5);

    SDL_FreeSurface(surface);
}

static void check_8_bit_negative_x_clip(void)
{
    uint8_t backing[32];
    memset(backing, 0xee, sizeof(backing));

    SDL_Surface *surface = SDL_CreateRGBSurfaceFrom(&backing[4], 4, 3, 8, 4,
                                                    DEFAULT_RMASK, DEFAULT_GMASK,
                                                    DEFAULT_BMASK, DEFAULT_AMASK);
    assert(surface != NULL);

    for (uint8_t i = 0; i < 12; i++)
        surface->pixels[i] = i;

    for (int i = 0; i < 256; i++)
    {
        surface->format->palette->colors[i].r = (uint8_t)i;
        surface->format->palette->colors[i].g = 0;
        surface->format->palette->colors[i].b = 0;
        surface->format->palette->colors[i].a = 0xff;
    }

    reset_capture();
    SDL_UpdateRect(surface, -1, 2, 3, 1);

    assert(captured_x == 0);
    assert(captured_y == 2);
    assert(captured_w == 2);
    assert(captured_h == 1);
    assert(captured_pixels[0] == 0xff080000u);
    assert(captured_pixels[1] == 0xff090000u);

    SDL_FreeSurface(surface);
}

int main(void)
{
    check_32_bit_negative_x_clip();
    check_8_bit_negative_x_clip();
    return 0;
}
