#include <SDL_ttf.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#define TTF_POINT_TO_PIXEL fixedpt_rconst(1.333333)
#define TTF_MIN_FONT_BYTES 12

struct TTF_Font
{
    stbtt_fontinfo *finfo;
    struct
    {
        uint8_t *buf;
        size_t size;
    } file;
    int ptsize;
    fixedpt factor;
    int height;
    int ascent, descent;
};

static int scale_floor(fixedpt scale, int value)
{
    return fixedpt_toint(fixedpt_floor(fixedpt_muli(scale, value)));
}

static int scale_ceil(fixedpt scale, int value)
{
    return fixedpt_toint(fixedpt_ceil(fixedpt_muli(scale, value)));
}

static void make_palette(SDL_Color palette[256], SDL_Color fg, SDL_Color bg)
{
    int rdiff = (int)fg.r - (int)bg.r;
    int gdiff = (int)fg.g - (int)bg.g;
    int bdiff = (int)fg.b - (int)bg.b;
    int adiff = (int)fg.a - (int)bg.a;

    for (int i = 0; i < 256; i++)
    {
        palette[i].r = (Uint8)((int)bg.r + (i * rdiff) / 255);
        palette[i].g = (Uint8)((int)bg.g + (i * gdiff) / 255);
        palette[i].b = (Uint8)((int)bg.b + (i * bdiff) / 255);
        palette[i].a = (Uint8)((int)bg.a + (i * adiff) / 255);
    }
}

static int is_half_width_codepoint(Uint16 ch)
{
    return (ch >= 0x20 && ch <= 0x7e) ||
           (ch >= 0xff60 && ch <= 0xff9f);
}

static int glyph_bitmap_width(stbtt_fontinfo *finfo, int glyphIndex, fixedpt scale_x, fixedpt scale_y)
{
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;

    stbtt_GetGlyphBitmapBox(finfo, glyphIndex, scale_x, scale_y, &x0, &y0, &x1, &y1);
    return x1 - x0;
}

static fixedpt glyph_scale_x(TTF_Font *font, Uint16 ch, int glyphIndex)
{
    fixedpt scale_x = font->factor;

    if (!is_half_width_codepoint(ch))
        return scale_x;

    /*
     * ONScripter advances single-byte text by one hankaku cell.  Some bundled
     * CJK fonts have proportional Latin glyphs wider than that cell, so keep
     * half-width codepoints inside the layout cell while leaving full-width
     * CJK glyphs at the normal raster scale.
     */
    int target_width = (font->ptsize + 1) / 2;
    int full_width = glyph_bitmap_width(font->finfo, glyphIndex, scale_x, font->factor);

    if (target_width <= 0 || full_width <= target_width)
        return scale_x;

    scale_x = fixedpt_divi(fixedpt_muli(scale_x, target_width), full_width);

    if (scale_x <= 0)
        return 1;

    /*
     * Bitmap boxes use floor/ceil rounding.  Check the rounded result and back
     * off a little if the first estimate still crosses the half-width cell.
     */
    for (int i = 0; i < 8 && glyph_bitmap_width(font->finfo, glyphIndex, scale_x, font->factor) > target_width; i++)
    {
        scale_x = fixedpt_mul(scale_x, fixedpt_rconst(0.95));

        if (scale_x <= 0)
            return 1;
    }

    return scale_x;
}

int TTF_Init()
{
    return 0;
}

static TTF_Font *open_font_from_buffer(void *buf, size_t size, int ptsize)
{
    if (buf == NULL || size < TTF_MIN_FONT_BYTES || ptsize <= 0)
    {
        free(buf);
        SDL_SetError("Invalid font data");
        return NULL;
    }

    stbtt_fontinfo *finfo = malloc(sizeof(*finfo));

    if (finfo == NULL)
    {
        free(buf);
        SDL_SetError("Out of memory");
        return NULL;
    }

    int offset = stbtt_GetFontOffsetForIndex((const unsigned char *)buf, 0);

    if (offset < 0 || !stbtt_InitFont(finfo, buf, offset))
    {
        free(finfo);
        free(buf);
        SDL_SetError("Invalid TrueType font");
        return NULL;
    }

    TTF_Font *font = malloc(sizeof(*font));

    if (font == NULL)
    {
        free(finfo);
        free(buf);
        SDL_SetError("Out of memory");
        return NULL;
    }

    font->finfo = finfo;
    font->file.buf = buf;
    font->file.size = size;
    font->ptsize = ptsize;

    /*
     * The local stb_truetype is fixed-point, not stock floating-point stb.
     * Keep the point-to-pixel conversion explicit so the scale stored in the
     * font matches the fixedpt rasteriser and the no-FPU Navy targets.
     */
    fixedpt pixel = fixedpt_muli(TTF_POINT_TO_PIXEL, ptsize);
    font->factor = stbtt_ScaleForPixelHeight(finfo, pixel);
    stbtt_GetFontVMetrics(finfo, &font->ascent, &font->descent, NULL);
    font->ascent = scale_ceil(font->factor, font->ascent);
    font->descent = scale_floor(font->factor, font->descent);
    font->height = font->ascent - font->descent;

    return font;
}

TTF_Font *TTF_OpenFont(const char *file, int ptsize)
{
    SDL_RWops *f = SDL_RWFromFile(file, "rb");

    if (f == NULL)
        return NULL;
    return TTF_OpenFontRW(f, 1, ptsize);
}

TTF_Font *TTF_OpenFontRW(SDL_RWops *src, int freesrc, int ptsize)
{
    if (src == NULL)
    {
        SDL_SetError("NULL SDL_RWops");
        return NULL;
    }

    if (ptsize <= 0)
    {
        if (freesrc)
            SDL_RWclose(src);
        SDL_SetError("Invalid point size");
        return NULL;
    }

    int64_t size64 = SDL_RWsize(src);

    if (size64 <= 0 || (uint64_t)size64 > (uint64_t)SIZE_MAX)
    {
        if (freesrc)
            SDL_RWclose(src);
        SDL_SetError("Invalid font stream size");
        return NULL;
    }

    size_t size = (size_t)size64;
    void *buf = malloc(size);

    if (buf == NULL)
    {
        if (freesrc)
            SDL_RWclose(src);
        SDL_SetError("Out of memory");
        return NULL;
    }

    size_t nread = SDL_RWread(src, buf, 1, size);

    if (freesrc)
        SDL_RWclose(src);

    if (nread != size)
    {
        free(buf);
        SDL_SetError("Could not read complete font stream");
        return NULL;
    }

    return open_font_from_buffer(buf, size, ptsize);
}

void TTF_CloseFont(TTF_Font *font)
{
    if (font == NULL)
        return;

    free(font->finfo);
    free(font->file.buf);
    free(font);
}

int TTF_GlyphMetrics(TTF_Font *font, Uint16 ch, int *minx, int *maxx, int *miny, int *maxy, int *advance)
{
    if (font == NULL || font->finfo == NULL)
        return -1;

    stbtt_fontinfo *finfo = font->finfo;
    int glyphIndex = stbtt_FindGlyphIndex(finfo, ch);

    if (glyphIndex == 0)
        return -1;

    int advanceWidth;
    stbtt_GetGlyphHMetrics(finfo, glyphIndex, &advanceWidth, NULL);
    fixedpt scale_x = glyph_scale_x(font, ch, glyphIndex);

    if (advance)
        *advance = fixedpt_toint(fixedpt_muli(scale_x, advanceWidth));

    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int ret = stbtt_GetGlyphBox(finfo, glyphIndex, &x0, &y0, &x1, &y1);

    if (ret == 0)
    {
        /*
         * Spaces and similar glyphs are real glyphs with an advance width, but
         * they have no ink box.  SDL_ttf reports them as a zero-sized box
         * instead of failing; callers such as ONScripter rely on that
         * distinction.
         */

        if (minx)
            *minx = 0;

        if (miny)
            *miny = 0;

        if (maxx)
            *maxx = 0;

        if (maxy)
            *maxy = 0;
        return 0;
    }

    if (minx)
        *minx = scale_floor(scale_x, x0);

    if (miny)
        *miny = scale_floor(font->factor, y0);

    if (maxx)
        *maxx = scale_ceil(scale_x, x1);

    if (maxy)
        *maxy = scale_ceil(font->factor, y1);
    return 0;
}

int TTF_FontAscent(TTF_Font *font)
{
    return font->ascent;
}

int TTF_FontHeight(TTF_Font *font)
{
    return font->height;
}

SDL_Surface *TTF_RenderGlyph_Shaded(TTF_Font *font, Uint16 ch, SDL_Color fg, SDL_Color bg)
{
    if (font == NULL || font->finfo == NULL)
        return NULL;

    stbtt_fontinfo *finfo = font->finfo;
    int glyphIndex = stbtt_FindGlyphIndex(finfo, ch);

    if (glyphIndex == 0)
        return NULL;
    int w, h, xoff, yoff;
    fixedpt scale_x = glyph_scale_x(font, ch, glyphIndex);
    uint8_t *pixels = stbtt_GetGlyphBitmap(finfo, scale_x, font->factor, glyphIndex, &w, &h, &xoff, &yoff);
    SDL_Color palette[256];

    make_palette(palette, fg, bg);

    if (w == 0 || h == 0)
    {
        /*
         * Spaces are valid glyphs with advance but no ink.  Preserve the
         * existing miniSDL_ttf contract: return a valid zero-sized surface
         * rather than collapsing the glyph into an error.
         */
        if (pixels != NULL)
            stbtt_FreeBitmap(pixels, finfo->userdata);

        SDL_Surface *s = SDL_CreateRGBSurfaceFrom(NULL, 0, 0, 8, 0, 0, 0, 0, 0);

        if (s != NULL)
            SDL_SetPalette(s, SDL_LOGPAL, palette, 0, 256);
        return s;
    }

    if (pixels == NULL)
    {
        SDL_SetError("Could not render glyph bitmap");
        return NULL;
    }

    /*
     * ONScripter uses TTF_GlyphMetrics() to place this surface, so the surface
     * itself must stay tight around stb's bitmap.  Adding xoff/yoff padding
     * here applies the same layout offset twice.
     */
    (void)xoff;
    (void)yoff;

    if (w <= 0 || h <= 0 || w > UINT16_MAX || h > UINT16_MAX)
    {
        stbtt_FreeBitmap(pixels, finfo->userdata);
        SDL_SetError("Invalid glyph surface size");
        return NULL;
    }

    SDL_Surface *s = SDL_CreateRGBSurface(0, w, h, 8, 0, 0, 0, 0);

    if (s == NULL)
    {
        stbtt_FreeBitmap(pixels, finfo->userdata);
        SDL_SetError("Could not create glyph surface");
        return NULL;
    }

    SDL_SetPalette(s, SDL_LOGPAL, palette, 0, 256);

    for (int y = 0; y < h; y++)
    {
        uint8_t *dst = s->pixels + y * s->pitch;
        uint8_t *src = pixels + y * w;

        memcpy(dst, src, (size_t)w);
    }

    stbtt_FreeBitmap(pixels, finfo->userdata);
    return s;
}
