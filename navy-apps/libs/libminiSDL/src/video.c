#include <NDL.h>
#include <sdl-video.h>
#include <sdl-file.h>
#include <assert.h>
#include <limits.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static uint32_t *update_argb_buf = NULL;
static size_t update_argb_cap = 0;

extern void SDL_PumpAudio(void) __attribute__((weak));

static void pump_audio_near_video_update(void)
{
    /*
     * miniSDL has no audio thread on Navy.  ONScripter can spend long stretches
     * rendering and flushing frames after starting a voice clip, so video
     * publishes are also safe points to refill the cooperative audio queue.
     */
    if (SDL_PumpAudio != NULL)
    {
        SDL_PumpAudio();
    }
}

static void draw_rect_with_audio_pump(uint32_t *pixels, int x, int y, int w, int h)
{
    pump_audio_near_video_update();
    NDL_DrawRect(pixels, x, y, w, h);
    pump_audio_near_video_update();
}

static uint32_t *ensure_update_argb_buffer(size_t pixels)
{
    /*
   * Framebuffer updates are serialised through the SDL call path, so one
   * process-wide scratch buffer is enough.  It grows to the largest dirty
   * rectangle seen and avoids repeated allocation during palette refreshes.
   */

    if (pixels <= update_argb_cap)
    {
        return update_argb_buf;
    }

    if (pixels > SIZE_MAX / sizeof(uint32_t))
    {
        return NULL;
    }

    uint32_t *new_buf = realloc(update_argb_buf, pixels * sizeof(uint32_t));
    if (new_buf == NULL)
    {
        return NULL;
    }

    update_argb_buf = new_buf;
    update_argb_cap = pixels;
    return update_argb_buf;
}

static int checked_argb_count(int w, int h, size_t *pixels)
{
    if (w <= 0 || h <= 0)
    {
        return 0;
    }

    const size_t sw = (size_t)w;
    const size_t sh = (size_t)h;

    if (sw > SIZE_MAX / sh)
    {
        return 0;
    }

    *pixels = sw * sh;
    return *pixels <= SIZE_MAX / sizeof(uint32_t);
}

static void build_palette_argb_lut(const SDL_Palette *palette, uint32_t lut[256])
{
    assert(palette);
    assert(palette->colors);

    int ncolors = palette->ncolors;

    if (ncolors < 0)
        ncolors = 0;

    if (ncolors > 256)
        ncolors = 256;

    /*
   * Convert the 256-entry palette once per update.  PAL's 8-bit screen path
   * otherwise packs r/g/b/a again for every destination pixel, so a full
   * 800x600 frame repeats the same 256 choices 480,000 times.
   */
    for (int i = 0; i < ncolors; i++)
    {
        const SDL_Color c = palette->colors[i];
        lut[i] = ((uint32_t)c.a << 24) |
                 ((uint32_t)c.r << 16) |
                 ((uint32_t)c.g << 8) |
                 (uint32_t)c.b;
    }

    for (int i = ncolors; i < 256; i++)
    {
        lut[i] = 0;
    }
}

// Performs a fast blit from the source surface to the destination surface.
//
// Copy from https://wiki.libsdl.org/SDL2/SDL_BlitSurface
//
// This assumes that the source and destination rectangles are the same size.
// If either srcrect or dstrect are NULL, the entire surface
// (src or dst) is copied. The final blit rectangle is saved in
// dstrect after all clipping is performed.
void SDL_BlitSurface(SDL_Surface *src, SDL_Rect *srcrect,
                     SDL_Surface *dst, SDL_Rect *dstrect)
{
    assert(src && dst);
    assert(src->format->BitsPerPixel == dst->format->BitsPerPixel);

    /* 1) Initialize the src/dst coords and size */
    int src_x = srcrect ? srcrect->x : 0;
    int src_y = srcrect ? srcrect->y : 0;
    int w = srcrect ? srcrect->w : src->w;
    int h = srcrect ? srcrect->h : src->h;

    int dst_x = dstrect ? dstrect->x : 0;
    int dst_y = dstrect ? dstrect->y : 0;

    /* 2) Clip against the source surface bounds */

    if (src_x < 0)
    {
        /* shift right */
        w += src_x;     /* src_x is negative */
        dst_x -= src_x; /* move dst origin right by same amount */
        src_x = 0;
    }

    if (src_y < 0)
    {
        h += src_y;
        dst_y -= src_y;
        src_y = 0;
    }

    if (src_x + w > src->w)
    {
        w = src->w - src_x;
    }

    if (src_y + h > src->h)
    {
        h = src->h - src_y;
    }

    /* 3) Clip against the destination surface bounds */

    if (dst_x < 0)
    {
        /* source must shift right now */
        w += dst_x; /* dst_x is negative */
        src_x -= dst_x;
        dst_x = 0;
    }

    if (dst_y < 0)
    {
        h += dst_y;
        src_y -= dst_y;
        dst_y = 0;
    }

    if (dst_x + w > dst->w)
    {
        w = dst->w - dst_x;
    }

    if (dst_y + h > dst->h)
    {
        h = dst->h - dst_y;
    }

    /* 4) If nothing to blit, set dstrect to empty and return */

    if (w <= 0 || h <= 0)
    {
        if (dstrect)
        {
            dstrect->x = dst_x;
            dstrect->y = dst_y;
            dstrect->w = 0;
            dstrect->h = 0;
        }

        return;
    }

    /* 5) Write back the final blit rectangle */

    if (dstrect)
    {
        dstrect->x = dst_x;
        dstrect->y = dst_y;
        dstrect->w = w;
        dstrect->h = h;
    }

    /* 6) The actual memcpy loop */
    const uint8_t bpp = src->format->BytesPerPixel;
    const int pitchS = src->pitch;
    const int pitchD = dst->pitch;
    uint8_t *pixelsS = (uint8_t *)src->pixels;
    uint8_t *pixelsD = (uint8_t *)dst->pixels;

    int row_start = 0;
    int row_end = h;
    int row_step = 1;

    if (src == dst)
    {
        uint8_t *first_src = pixelsS + src_y * pitchS + src_x * bpp;
        uint8_t *first_dst = pixelsD + dst_y * pitchD + dst_x * bpp;

        if (first_dst > first_src)
        {
            row_start = h - 1;
            row_end = -1;
            row_step = -1;
        }
    }

    for (int row = row_start; row != row_end; row += row_step)
    {
        uint8_t *rowS = pixelsS + (src_y + row) * pitchS + src_x * bpp;
        uint8_t *rowD = pixelsD + (dst_y + row) * pitchD + dst_x * bpp;
        memmove(rowD, rowS, (size_t)w * bpp);
    }
}

// Perform a fast fill of a rectangle with a specific color.
// dst: The SDL_Surface structure that is the drawing target.
// dstrect: The SDL_Rect structure representing the rectangle to fill, or NULL to fill the entire surface.
// color:	The color to fill with.
void SDL_FillRect(SDL_Surface *dst, SDL_Rect *dstrect, uint32_t color)
{
    // 1) sanity check
    assert(dst);
    assert(dst->pixels);

    // 2) Work with signed dimensions while clipping.
    int fill_x = dstrect ? dstrect->x : 0;
    int fill_y = dstrect ? dstrect->y : 0;
    int fill_w = dstrect ? dstrect->w : dst->w;
    int fill_h = dstrect ? dstrect->h : dst->h;

    // 3) clip to [0..w) × [0..h)

    if (fill_x < 0)
    {
        fill_w += fill_x;
        fill_x = 0;
    }

    if (fill_y < 0)
    {
        fill_h += fill_y;
        fill_y = 0;
    }

    /*
     * A rectangle can start below or to the right of the surface.  SDL_Rect keeps
     * w/h as uint16_t in this miniSDL ABI, so clipping directly into the struct
     * can turn a negative size into a huge positive value.  Keep the signed
     * intermediate values until the empty-rectangle check has finished.
     */

    if (fill_x >= dst->w || fill_y >= dst->h)
    {
        return;
    }

    // Clip again.

    if (fill_x + fill_w > dst->w)
    {
        fill_w = dst->w - fill_x;
    }

    if (fill_y + fill_h > dst->h)
    {
        fill_h = dst->h - fill_y;
    }

    // nothing to do?

    if (fill_w <= 0 || fill_h <= 0)
    {
        return;
    }

    // 4) get format info
    const int bpp = dst->format->BytesPerPixel;

    // Bytes per row, including any padding.
    const int pitch = dst->pitch;
    uint8_t *pixels = dst->pixels;

    // 5) fill each row
    for (int row = 0; row < fill_h; row++)
    {
        // pointer to the first byte of this scanline
        uint8_t *rowp = pixels + (fill_y + row) * pitch + fill_x * bpp;

        for (int col = 0; col < fill_w; col++)
        {
            uint8_t *pixelp = rowp + col * bpp;

            switch (bpp)
            {
            case 1:
            {
                // 8-bit surface
                pixelp[0] = (uint8_t)(color & 0xFF);
                break;
            }

            case 2:
            {
                // 16-bit surface
                uint16_t c16 = (uint16_t)color;
                memcpy(pixelp, &c16, sizeof(c16));
                break;
            }

            case 3:
            {
                // 24-bit surface, copy the lowest 3 bytes of color
                memcpy(pixelp, &color, 3);
                break;
            }

            case 4:
            {
                // 32-bit surface
                uint32_t c32 = color;
                memcpy(pixelp, &c32, sizeof(c32));
                break;
            }

            default:
            {
                // Unsupported bpp.
                assert(0);
                break;
            }
            }
        }
    }
}

static int clip_surface_rect(SDL_Surface *s, int *x, int *y, int *w, int *h)
{
    if (*x == 0 && *y == 0 && *w == 0 && *h == 0)
    {
        *w = s->w;
        *h = s->h;
    }
    else if (*w <= 0 || *h <= 0)
    {
        return 0;
    }

    if (*x < 0)
    {
        *w += *x;
        *x = 0;
    }

    if (*y < 0)
    {
        *h += *y;
        *y = 0;
    }

    if (*x >= s->w || *y >= s->h)
    {
        return 0;
    }

    if (*w > s->w - *x)
    {
        *w = s->w - *x;
    }

    if (*h > s->h - *y)
    {
        *h = s->h - *y;
    }

    return *w > 0 && *h > 0;
}

void SDL_UpdateRect(SDL_Surface *s, int x, int y, int w, int h)
{
    if (s == NULL || s->format == NULL || s->pixels == NULL)
    {
        return;
    }

    /*
   * SDL_UpdateRect(surface, 0, 0, 0, 0) is the conventional full-surface
   * refresh.  A rectangle with only one zero dimension is empty; drawing it as
   * a full row/column can turn a bad glyph or clipping rectangle into a large
   * framebuffer update.
   */

    if (!clip_surface_rect(s, &x, &y, &w, &h))
    {
        return;
    }

    if (s->format->BitsPerPixel == 32)
    {
        /*
       * NDL_DrawRect() expects a tightly packed w*h pixel array.  The screen
       * surface itself is pitched by the full surface width, so passing a
       * pointer into the middle of the surface only works for full-width
       * updates.  ONScripter often updates small dirty rectangles; pack those
       * rows first so every destination row starts from the correct source row.
       * Full-width paths stay zero-copy because scene changes are often already
       * full rows and those writes are batched again by NDL/Nanos/NEMU.
       */
        const int bpp = s->format->BytesPerPixel;
        const int pitch = s->pitch;
        uint8_t *src = (uint8_t *)s->pixels + y * pitch + x * bpp;

        if (x == 0 && bpp > 0 && pitch == w * bpp)
        {
            draw_rect_with_audio_pump((uint32_t *)src, x, y, w, h);
        }
        else
        {
            size_t pixels;
            if (!checked_argb_count(w, h, &pixels))
            {
                return;
            }

            uint32_t *buf = ensure_update_argb_buffer(pixels);
            if (buf == NULL)
            {
                return;
            }

            for (int row = 0; row < h; row++)
            {
                memcpy(buf + row * w, src + row * pitch, (size_t)w * bpp);
            }

            draw_rect_with_audio_pump(buf, x, y, w, h);
        }
    }
    // For The Legend of Sword and Fairy.
    else if (s->format->BitsPerPixel == 8)
    {
        // 8-bit palette path
        const int pitch = s->pitch;
        uint8_t *src = (uint8_t *)s->pixels + y * pitch + x;
        if (s->format->palette == NULL)
        {
            return;
        }

        size_t pixels;
        if (!checked_argb_count(w, h, &pixels))
        {
            return;
        }

        uint32_t *buf = ensure_update_argb_buffer(pixels);
        if (buf == NULL)
        {
            return;
        }

        uint32_t palette_argb[256];
        build_palette_argb_lut(s->format->palette, palette_argb);

        /*
       * NDL_DrawRect() consumes 32-bit ARGB pixels, while PAL keeps its real
       * screen as an 8-bit indexed surface.  Reuse one conversion buffer and
       * look up pre-packed colours; this removes a malloc/free pair per frame
       * and replaces four byte-field loads/shifts per pixel with one table
       * lookup.
       */
        for (int row = 0; row < h; ++row)
        {
            uint8_t *rowp = src + row * pitch;
            uint32_t *dstp = buf + row * w;
            for (int col = 0; col < w; ++col)
            {
                dstp[col] = palette_argb[rowp[col]];
            }
        }

        // Draw it.
        draw_rect_with_audio_pump(buf, x, y, w, h);
    }
    else
    {
        // Unsupported format
        assert(0);
    }
}

// APIs below are already implemented.

static inline int maskToShift(uint32_t mask)
{
    /*
   * This miniSDL supports the canonical byte-aligned masks used by Navy apps.
   * Rejecting arbitrary masks keeps conversion code simple and makes unsupported
   * formats fail at surface creation rather than during later blits.
   */
    switch (mask)
    {
    case 0x000000ff:
        return 0;
    case 0x0000ff00:
        return 8;
    case 0x00ff0000:
        return 16;
    case 0xff000000:
        return 24;
    case 0x00000000:
        return 0;
    default:
        assert(0);
        return 0;
    }
}

SDL_Surface *SDL_CreateRGBSurface(uint32_t flags, int width, int height, int depth,
                                  uint32_t Rmask, uint32_t Gmask, uint32_t Bmask, uint32_t Amask)
{
    if ((depth != 8 && depth != 32) || width < 0 || height < 0)
    {
        return NULL;
    }

    const int bytes_per_pixel = depth / 8;
    if (bytes_per_pixel <= 0 || width > INT_MAX / bytes_per_pixel)
    {
        return NULL;
    }

    const int pitch = width * bytes_per_pixel;
    if (height != 0 && (size_t)pitch > SIZE_MAX / (size_t)height)
    {
        return NULL;
    }

    SDL_Surface *s = calloc(1, sizeof(SDL_Surface));
    if (s == NULL)
    {
        return NULL;
    }

    s->flags = flags;
    s->format = calloc(1, sizeof(SDL_PixelFormat));
    if (s->format == NULL)
    {
        free(s);
        return NULL;
    }

    if (depth == 8)
    {
        s->format->palette = calloc(1, sizeof(SDL_Palette));
        if (s->format->palette == NULL)
        {
            free(s->format);
            free(s);
            return NULL;
        }

        s->format->palette->colors = calloc(256, sizeof(SDL_Color));
        if (s->format->palette->colors == NULL)
        {
            free(s->format->palette);
            free(s->format);
            free(s);
            return NULL;
        }

        s->format->palette->ncolors = 256;
    }
    else
    {
        s->format->palette = NULL;
        s->format->Rmask = Rmask;
        s->format->Rshift = maskToShift(Rmask);
        s->format->Rloss = 0;
        s->format->Gmask = Gmask;
        s->format->Gshift = maskToShift(Gmask);
        s->format->Gloss = 0;
        s->format->Bmask = Bmask;
        s->format->Bshift = maskToShift(Bmask);
        s->format->Bloss = 0;
        s->format->Amask = Amask;
        s->format->Ashift = maskToShift(Amask);
        s->format->Aloss = 0;
    }

    s->format->BitsPerPixel = depth;
    s->format->BytesPerPixel = depth / 8;

    s->w = width;
    s->h = height;
    s->pitch = pitch;

    if (!(flags & SDL_PREALLOC))
    {
        const size_t bytes = (size_t)s->pitch * (size_t)height;
        s->pixels = bytes == 0 ? NULL : malloc(bytes);
        if (bytes != 0 && s->pixels == NULL)
        {
            SDL_FreeSurface(s);
            return NULL;
        }
        /*
     * SDL callers are allowed to update or palette-convert a newly-created
     * surface before they have painted every pixel. PAL does this during
     * startup: gpScreenReal is a hardware 8-bit surface, palette changes can
     * trigger SDL_UpdateRect(), and the first title frames do not necessarily
     * overwrite every byte immediately. Start every owned surface at palette
     * index / pixel value 0 so those early updates show black instead of stale
     * allocator contents or pixels left by the previous Navy app.
     */
        if (bytes != 0)
        {
            memset(s->pixels, 0, bytes);
        }
    }

    return s;
}

SDL_Surface *SDL_CreateRGBSurfaceFrom(void *pixels, int width, int height, int depth,
                                      int pitch, uint32_t Rmask, uint32_t Gmask, uint32_t Bmask, uint32_t Amask)
{
    /*
   * SDL_PREALLOC means ownership stays with the caller.  The surface metadata
   * still uses miniSDL's normal format allocation, but SDL_FreeSurface() must
   * leave the pixel storage alone.
   */
    SDL_Surface *s = SDL_CreateRGBSurface(SDL_PREALLOC, width, height, depth,
                                          Rmask, Gmask, Bmask, Amask);
    if (s == NULL)
    {
        return NULL;
    }

    const int min_pitch = width * (depth / 8);
    if (pitch < min_pitch || (pixels == NULL && height > 0 && min_pitch > 0))
    {
        SDL_FreeSurface(s);
        return NULL;
    }

    s->pixels = pixels;
    s->pitch = pitch;
    return s;
}

void SDL_FreeSurface(SDL_Surface *s)
{
    if (s != NULL)
    {
        if (s->format != NULL)
        {
            if (s->format->palette != NULL)
            {
                if (s->format->palette->colors != NULL)
                    free(s->format->palette->colors);
                free(s->format->palette);
            }
            free(s->format);
        }

        if (s->pixels != NULL && !(s->flags & SDL_PREALLOC))
            free(s->pixels);
        free(s);
    }
}

SDL_Surface *SDL_SetVideoMode(int width, int height, int bpp, uint32_t flags)
{
    if (flags & SDL_HWSURFACE)
        NDL_OpenCanvas(&width, &height);
    SDL_Surface *s = SDL_CreateRGBSurface(flags, width, height, bpp,
                                          DEFAULT_RMASK, DEFAULT_GMASK, DEFAULT_BMASK, DEFAULT_AMASK);

    if (flags & SDL_HWSURFACE)
    {
        /*
     * NDL_OpenCanvas() clears the physical framebuffer, while this sync clears
     * the app's newly-created hardware surface inside that canvas. This covers
     * programs like PAL that open a smaller 8-bit surface after NTerm and then
     * change palettes before the whole logical screen has been repainted.
     */
        SDL_UpdateRect(s, 0, 0, 0, 0);
    }

    return s;
}

void SDL_SoftStretch(SDL_Surface *src, SDL_Rect *srcrect, SDL_Surface *dst, SDL_Rect *dstrect)
{
    assert(src && dst);
    assert(src->format && dst->format);
    assert(dst->format->BitsPerPixel == src->format->BitsPerPixel);

    const int bpp = dst->format->BytesPerPixel;
    if (bpp <= 0)
    {
        return;
    }

    const int src_x = (srcrect == NULL ? 0 : srcrect->x);
    const int src_y = (srcrect == NULL ? 0 : srcrect->y);
    const int src_w = (srcrect == NULL ? src->w : srcrect->w);
    const int src_h = (srcrect == NULL ? src->h : srcrect->h);

    SDL_Rect dst_full = {
        .x = 0,
        .y = 0,
        .w = (uint16_t)dst->w,
        .h = (uint16_t)dst->h,
    };
    SDL_Rect *dst_area = dstrect == NULL ? &dst_full : dstrect;

    if (src_w <= 0 || src_h <= 0 || dst_area->w <= 0 || dst_area->h <= 0)
    {
        return;
    }

    assert(src_x >= 0 && src_y >= 0);
    assert(src_x + src_w <= src->w && src_y + src_h <= src->h);

    if (src_w == dst_area->w && src_h == dst_area->h)
    {
        /*
     * The same-size path keeps SDL_BlitSurface()'s richer clipping behaviour.
     * A local destination rectangle avoids writing back through dst_full when
     * the caller passed NULL, matching normal SDL "whole destination" usage.
     */
        SDL_Rect src_rect = {
            .x = src_x,
            .y = src_y,
            .w = (uint16_t)src_w,
            .h = (uint16_t)src_h,
        };
        SDL_Rect dst_rect = *dst_area;
        SDL_BlitSurface(src, &src_rect, dst, &dst_rect);

        if (dstrect != NULL)
            *dstrect = dst_rect;
        return;
    }

    /*
   * PAL renders into a 320x200 8-bit buffer and stretches it to the real
   * screen. Nearest-neighbour scaling is enough here: it is deterministic,
   * cheap for NEMU, and preserves the indexed palette values until the final
   * SDL_UpdateRect() palette conversion.
   */
    const int dst_x = dst_area->x;
    const int dst_y = dst_area->y;
    const int dst_w = dst_area->w;
    const int dst_h = dst_area->h;

    int clip_x0 = dst_x < 0 ? 0 : dst_x;
    int clip_y0 = dst_y < 0 ? 0 : dst_y;
    int clip_x1 = dst_x + dst_w;
    int clip_y1 = dst_y + dst_h;

    if (clip_x1 > dst->w)
        clip_x1 = dst->w;

    if (clip_y1 > dst->h)
        clip_y1 = dst->h;

    if (clip_x0 >= clip_x1 || clip_y0 >= clip_y1)
    {
        if (dstrect != NULL)
        {
            dstrect->x = clip_x0;
            dstrect->y = clip_y0;
            dstrect->w = 0;
            dstrect->h = 0;
        }
        return;
    }

    /*
   * Scale with integer error accumulators instead of doing one division for
   * every destination pixel.  For a 320x200 -> 800x600 PAL frame this removes
   * 480,000 inner-loop divisions while preserving the same floor-based nearest
   * neighbour mapping:
   *
   *   source column = floor((output column - area x) * source width /
   *                         destination width)
   *
   * The error value is the remainder of that division.  Adding src_w each
   * output pixel and carrying whenever it reaches dst_w is the same arithmetic
   * as the formula above, just spread over the row.
   */
    int y_num = (clip_y0 - dst_y) * src_h;
    int src_row = y_num / dst_h;
    int y_err = y_num % dst_h;

    for (int dy = clip_y0; dy < clip_y1; dy++)
    {
        const uint8_t *src_rowp = (const uint8_t *)src->pixels +
                                  (src_y + src_row) * src->pitch +
                                  src_x * bpp;
        uint8_t *dst_rowp = (uint8_t *)dst->pixels +
                            dy * dst->pitch +
                            clip_x0 * bpp;

        int x_num = (clip_x0 - dst_x) * src_w;
        int src_col = x_num / dst_w;
        int x_err = x_num % dst_w;

        for (int dx = clip_x0; dx < clip_x1; dx++)
        {
            memcpy(dst_rowp + (size_t)(dx - clip_x0) * (size_t)bpp,
                   src_rowp + (size_t)src_col * (size_t)bpp,
                   (size_t)bpp);

            x_err += src_w;
            while (x_err >= dst_w)
            {
                x_err -= dst_w;
                src_col++;
            }
        }

        y_err += src_h;
        while (y_err >= dst_h)
        {
            y_err -= dst_h;
            src_row++;
        }
    }

    if (dstrect != NULL)
    {
        dstrect->x = clip_x0;
        dstrect->y = clip_y0;
        dstrect->w = (uint16_t)(clip_x1 - clip_x0);
        dstrect->h = (uint16_t)(clip_y1 - clip_y0);
    }
}

void SDL_SoftStretchUpdate(SDL_Surface *src, SDL_Rect *srcrect, SDL_Surface *dst, SDL_Rect *dstrect)
{
    assert(src && dst);
    assert(src->format);
    assert(dst->format);

    if (!(dst->flags & SDL_HWSURFACE) ||
        src->format->BitsPerPixel != 8 ||
        dst->format->BitsPerPixel != 8)
    {
        SDL_SoftStretch(src, srcrect, dst, dstrect);

        if (dstrect != NULL)
        {
            SDL_UpdateRect(dst, dstrect->x, dstrect->y, dstrect->w, dstrect->h);
        }
        else
        {
            SDL_UpdateRect(dst, 0, 0, 0, 0);
        }
        return;
    }

    const int src_x = (srcrect == NULL ? 0 : srcrect->x);
    const int src_y = (srcrect == NULL ? 0 : srcrect->y);
    const int src_w = (srcrect == NULL ? src->w : srcrect->w);
    const int src_h = (srcrect == NULL ? src->h : srcrect->h);

    SDL_Rect dst_full = {
        .x = 0,
        .y = 0,
        .w = (uint16_t)dst->w,
        .h = (uint16_t)dst->h,
    };
    SDL_Rect *dst_area = dstrect == NULL ? &dst_full : dstrect;

    if (src_w <= 0 || src_h <= 0 || dst_area->w <= 0 || dst_area->h <= 0)
    {
        return;
    }

    assert(src_x >= 0 && src_y >= 0);
    assert(src_x + src_w <= src->w && src_y + src_h <= src->h);

    const int dst_x = dst_area->x;
    const int dst_y = dst_area->y;
    const int dst_w = dst_area->w;
    const int dst_h = dst_area->h;

    int clip_x0 = dst_x < 0 ? 0 : dst_x;
    int clip_y0 = dst_y < 0 ? 0 : dst_y;
    int clip_x1 = dst_x + dst_w;
    int clip_y1 = dst_y + dst_h;

    if (clip_x1 > dst->w)
        clip_x1 = dst->w;

    if (clip_y1 > dst->h)
        clip_y1 = dst->h;

    if (clip_x0 >= clip_x1 || clip_y0 >= clip_y1)
    {
        if (dstrect != NULL)
        {
            dstrect->x = clip_x0;
            dstrect->y = clip_y0;
            dstrect->w = 0;
            dstrect->h = 0;
        }
        return;
    }

    const int out_w = clip_x1 - clip_x0;
    const int out_h = clip_y1 - clip_y0;
    size_t pixels;
    if (!checked_argb_count(out_w, out_h, &pixels))
    {
        return;
    }

    uint32_t *argb = ensure_update_argb_buffer(pixels);
    if (argb == NULL || dst->format->palette == NULL)
    {
        return;
    }

    uint32_t palette_argb[256];
    build_palette_argb_lut(dst->format->palette, palette_argb);

    /*
   * PAL normally does SDL_SoftStretch() into an 8-bit hardware surface and then
   * SDL_UpdateRect(), which reads the same 800x600 indexed pixels back to build
   * a 32-bit framebuffer image.  This combined path writes both outputs in one
   * nearest-neighbour pass:
   *
   *   - dst->pixels stays current, so later palette-only refreshes still work;
   *   - argb is sent to /dev/fb immediately, avoiding a second full-screen pass.
   *
   * That second point matters on NEMU because every extra full-screen pass also
   * feeds the foreground shadow and VGA dirty-rectangle pipeline.
   */
    int y_num = (clip_y0 - dst_y) * src_h;
    int src_row = y_num / dst_h;
    int y_err = y_num % dst_h;

    for (int dy = clip_y0; dy < clip_y1; dy++)
    {
        const uint8_t *src_rowp = (const uint8_t *)src->pixels + (src_y + src_row) * src->pitch + src_x;
        uint8_t *dst_rowp = (uint8_t *)dst->pixels + dy * dst->pitch + clip_x0;
        uint32_t *argb_rowp = argb + (size_t)(dy - clip_y0) * (size_t)out_w;

        int x_num = (clip_x0 - dst_x) * src_w;
        int src_col = x_num / dst_w;
        int x_err = x_num % dst_w;

        for (int dx = clip_x0; dx < clip_x1; dx++)
        {
            const uint8_t idx = src_rowp[src_col];
            dst_rowp[dx - clip_x0] = idx;
            argb_rowp[dx - clip_x0] = palette_argb[idx];

            x_err += src_w;
            while (x_err >= dst_w)
            {
                x_err -= dst_w;
                src_col++;
            }
        }

        y_err += src_h;
        while (y_err >= dst_h)
        {
            y_err -= dst_h;
            src_row++;
        }
    }

    if (dstrect != NULL)
    {
        dstrect->x = clip_x0;
        dstrect->y = clip_y0;
        dstrect->w = (uint16_t)out_w;
        dstrect->h = (uint16_t)out_h;
    }

    draw_rect_with_audio_pump(argb, clip_x0, clip_y0, out_w, out_h);
}

void SDL_SetPalette(SDL_Surface *s, int flags, SDL_Color *colors, int firstcolor, int ncolors)
{
    (void)flags;

    if (s == NULL || s->format == NULL || s->format->palette == NULL ||
        s->format->palette->colors == NULL ||
        firstcolor < 0 || firstcolor > 256 ||
        ncolors < 0 || ncolors > 256 - firstcolor)
    {
        return;
    }

    if (ncolors == 0)
    {
        return;
    }

    if (colors == NULL)
    {
        return;
    }

    memcpy(&s->format->palette->colors[firstcolor], colors,
           sizeof(SDL_Color) * (size_t)ncolors);
    s->format->palette->ncolors = 256;

    if (s->flags & SDL_HWSURFACE)
    {
        /*
     * The physical framebuffer stores ARGB pixels, not palette indices.  When
     * a hardware-surface palette changes, every existing index can map to a
     * new colour, so the whole canvas must be republished through UpdateRect().
     */
        SDL_UpdateRect(s, 0, 0, 0, 0);
    }
}

static uint8_t pixel_channel(uint32_t pixel, uint32_t mask, uint8_t shift)
{
    return mask == 0 ? 0 : (uint8_t)((pixel & mask) >> shift);
}

SDL_Surface *SDL_ConvertSurface(SDL_Surface *src, SDL_PixelFormat *fmt, uint32_t flags)
{
    if (src == NULL || src->format == NULL || fmt == NULL ||
        src->format->BitsPerPixel != 32 || fmt->BitsPerPixel != 32 ||
        (src->pixels == NULL && src->w > 0 && src->h > 0))
    {
        return NULL;
    }

    SDL_Surface *ret = SDL_CreateRGBSurface(flags & ~SDL_PREALLOC,
                                            src->w, src->h, fmt->BitsPerPixel,
                                            fmt->Rmask, fmt->Gmask, fmt->Bmask, fmt->Amask);
    if (ret == NULL)
    {
        return NULL;
    }

    for (int y = 0; y < src->h; y++)
    {
        const uint8_t *src_row = (const uint8_t *)src->pixels + (size_t)y * src->pitch;
        uint8_t *dst_row = (uint8_t *)ret->pixels + (size_t)y * ret->pitch;

        for (int x = 0; x < src->w; x++)
        {
            uint32_t pixel;
            memcpy(&pixel, src_row + (size_t)x * sizeof(pixel), sizeof(pixel));

            const uint8_t r = pixel_channel(pixel, src->format->Rmask, src->format->Rshift);
            const uint8_t g = pixel_channel(pixel, src->format->Gmask, src->format->Gshift);
            const uint8_t b = pixel_channel(pixel, src->format->Bmask, src->format->Bshift);
            const uint8_t a = src->format->Amask == 0
                                  ? 0xff
                                  : pixel_channel(pixel, src->format->Amask, src->format->Ashift);
            const uint32_t mapped = SDL_MapRGBA(fmt, r, g, b, a);

            memcpy(dst_row + (size_t)x * sizeof(mapped), &mapped, sizeof(mapped));
        }
    }

    return ret;
}

uint32_t SDL_MapRGBA(SDL_PixelFormat *fmt, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    assert(fmt);
    assert(fmt->BytesPerPixel == 4);
    uint32_t p = ((uint32_t)r << fmt->Rshift) |
                 ((uint32_t)g << fmt->Gshift) |
                 ((uint32_t)b << fmt->Bshift);

    if (fmt->Amask)
        p |= ((uint32_t)a << fmt->Ashift);
    return p;
}

int SDL_LockSurface(SDL_Surface *s)
{
    (void)s;

    /*
   * Surfaces are plain memory buffers in miniSDL.  There is no separate video
   * backend lock to acquire, so this only preserves the SDL API shape.
   */
    return 0;
}

void SDL_UnlockSurface(SDL_Surface *s)
{
    (void)s;
}

static int rw_write_exact(SDL_RWops *rw, const void *buf, size_t len)
{
    return SDL_RWwrite(rw, buf, 1, len) == len ? 0 : -1;
}

static void put_u16_le(uint8_t out[2], uint16_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
}

static void put_u32_le(uint8_t out[4], uint32_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

int SDL_SaveBMP_RW(SDL_Surface *surface, SDL_RWops *rw, int freerw)
{
    int ret = -1;

    if (surface == NULL || surface->format == NULL || surface->pixels == NULL ||
        rw == NULL || surface->w <= 0 || surface->h <= 0 ||
        surface->format->BytesPerPixel != 4)
    {
        if (freerw && rw != NULL)
            SDL_RWclose(rw);
        return -1;
    }

    const uint32_t width = (uint32_t)surface->w;
    const uint32_t height = (uint32_t)surface->h;
    if (width > (UINT32_MAX - 3u) / 3u)
    {
        if (freerw)
            SDL_RWclose(rw);
        return -1;
    }

    const uint32_t row_bytes = width * 3u;
    const uint32_t padded_row_bytes = (row_bytes + 3u) & ~3u;
    if (height > (UINT32_MAX - 54u) / padded_row_bytes)
    {
        if (freerw)
            SDL_RWclose(rw);
        return -1;
    }

    const uint32_t image_bytes = padded_row_bytes * height;
    const uint32_t file_bytes = 54u + image_bytes;
    uint8_t header[54];

    memset(header, 0, sizeof(header));
    header[0] = 'B';
    header[1] = 'M';
    put_u32_le(&header[2], file_bytes);
    put_u32_le(&header[10], 54u);
    put_u32_le(&header[14], 40u);
    put_u32_le(&header[18], (uint32_t)surface->w);
    put_u32_le(&header[22], (uint32_t)surface->h);
    put_u16_le(&header[26], 1u);
    put_u16_le(&header[28], 24u);
    put_u32_le(&header[34], image_bytes);

    if (rw_write_exact(rw, header, sizeof(header)) != 0)
        goto out;

    const uint8_t pad[3] = {0, 0, 0};
    const size_t pad_len = (size_t)(padded_row_bytes - row_bytes);
    const int bpp = surface->format->BytesPerPixel;

    /*
     * BMP stores rows bottom-up and pixels as BGR.  ONScripter screenshots are
     * 32-bit software surfaces, so read each source pixel through its masks
     * instead of assuming one fixed host byte order.
     */
    for (int y = surface->h - 1; y >= 0; y--)
    {
        const uint8_t *row = surface->pixels + (size_t)y * surface->pitch;

        for (int x = 0; x < surface->w; x++)
        {
            uint32_t pixel;
            uint8_t bgr[3];

            memcpy(&pixel, row + (size_t)x * bpp, sizeof(pixel));
            bgr[0] = pixel_channel(pixel, surface->format->Bmask, surface->format->Bshift);
            bgr[1] = pixel_channel(pixel, surface->format->Gmask, surface->format->Gshift);
            bgr[2] = pixel_channel(pixel, surface->format->Rmask, surface->format->Rshift);

            if (rw_write_exact(rw, bgr, sizeof(bgr)) != 0)
                goto out;
        }

        if (pad_len != 0 && rw_write_exact(rw, pad, pad_len) != 0)
            goto out;
    }

    ret = 0;

out:
    if (freerw)
    {
        const int close_ret = SDL_RWclose(rw);
        if (close_ret != 0)
            ret = -1;
    }

    return ret;
}
