#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <NDL.h>

#define SDL_DISPINFO_PATH "miniSDL-set-video-mode-fit-test.dispinfo"

static int canvas_open_count = 0;
static int canvas_w = -1;
static int canvas_h = -1;

int NDL_Init(uint32_t flags)
{
    (void)flags;
    return 0;
}

void NDL_Quit(void)
{
}

void NDL_OpenCanvas(int *w, int *h)
{
    canvas_open_count++;
    canvas_w = *w;
    canvas_h = *h;
}

void NDL_DrawRect(uint32_t *pixels, int x, int y, int w, int h)
{
    (void)pixels;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
}

#include "../libs/libminiSDL/src/general.c"
#include "../libs/libminiSDL/src/video.c"

static void write_dispinfo(int w, int h)
{
    FILE *fp = fopen(SDL_DISPINFO_PATH, "w");
    assert(fp != NULL);
    assert(fprintf(fp, "WIDTH:%d\nHEIGHT:%d\n", w, h) > 0);
    assert(fclose(fp) == 0);
}

int main(void)
{
    write_dispinfo(400, 300);

    SDL_Surface *too_large = SDL_SetVideoMode(640, 480, 32, SDL_HWSURFACE);
    assert(too_large == NULL);
    assert(canvas_open_count == 0);
    assert(strstr(SDL_GetError(), "640x480") != NULL);
    assert(strstr(SDL_GetError(), "400x300") != NULL);

    write_dispinfo(800, 600);

    SDL_Surface *fits = SDL_SetVideoMode(640, 480, 32, SDL_HWSURFACE);
    assert(fits != NULL);
    assert(canvas_open_count == 1);
    assert(canvas_w == 640);
    assert(canvas_h == 480);

    SDL_FreeSurface(fits);
    assert(unlink(SDL_DISPINFO_PATH) == 0);
    return 0;
}
