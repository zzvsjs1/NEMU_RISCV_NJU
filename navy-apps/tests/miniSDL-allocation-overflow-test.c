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
    assert(ensure_update_argb_buffer(SIZE_MAX / sizeof(uint32_t) + 1) == NULL);
    return 0;
}
