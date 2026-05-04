#include <SDL_ttf.h>
#include <fixedptc.h>
#include <assert.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

int TTF_Init() {
  return 0;
}

static TTF_Font *open_font_from_buffer(void *buf, size_t size, int ptsize)
{
  stbtt_fontinfo *finfo = malloc(sizeof(*finfo));
  assert(finfo);
  int ret = stbtt_InitFont(finfo, buf, stbtt_GetFontOffsetForIndex(buf,0));
  assert(ret == 1);

  TTF_Font *font = malloc(sizeof(*font));
  font->finfo = finfo;
  font->file.buf = buf;
  font->file.size = size;
  font->ptsize = ptsize;

  // Pre-computed metrics shared by glyph rendering and layout.
  fixedpt pixel = fixedpt_muli(fixedpt_rconst(1.333333), ptsize);
  font->factor = stbtt_ScaleForPixelHeight(finfo, pixel);
  stbtt_GetFontVMetrics(finfo, &font->ascent, &font->descent, NULL);
  font->height = fixedpt_toint(fixedpt_muli(font->factor, font->ascent - font->descent));
  font->ascent = fixedpt_toint(fixedpt_muli(font->factor, font->ascent));
  font->descent = fixedpt_toint(fixedpt_muli(font->factor, font->descent));

  return font;
}

TTF_Font* TTF_OpenFont(const char *file, int ptsize) {
  SDL_RWops *f = SDL_RWFromFile(file, "rb");
  if (f == NULL) return NULL;
  return TTF_OpenFontRW(f, 1, ptsize);
}

TTF_Font *TTF_OpenFontRW(SDL_RWops *src, int freesrc, int ptsize) {
  if (src == NULL) return NULL;

  int64_t size64 = SDL_RWsize(src);
  if (size64 <= 0) {
    if (freesrc) SDL_RWclose(src);
    return NULL;
  }

  size_t size = (size_t)size64;
  void *buf = malloc(size);
  assert(buf);
  size_t nread = SDL_RWread(src, buf, size, 1);
  if (freesrc) SDL_RWclose(src);
  assert(nread == 1);

  return open_font_from_buffer(buf, size, ptsize);
}

int TTF_GlyphMetrics(TTF_Font *font, Uint16 ch, int *minx, int *maxx, int *miny, int *maxy, int *advance) {
  stbtt_fontinfo *finfo = font->finfo;
  int glyphIndex = stbtt_FindGlyphIndex(finfo, ch);
  if (glyphIndex == 0) return -1;

  int advanceWidth;
  stbtt_GetGlyphHMetrics(finfo, glyphIndex, &advanceWidth, NULL);
  if (advance) *advance = fixedpt_toint(fixedpt_muli(font->factor, advanceWidth));

  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  int ret = stbtt_GetGlyphBox(finfo, glyphIndex, &x0, &y0, &x1, &y1);
  if (ret == 0) {
    /*
     * Spaces and similar glyphs are real glyphs with an advance width, but
     * they have no ink box.  SDL_ttf reports them as a zero-sized box instead
     * of failing; callers such as ONScripter rely on that distinction.
     */
    if (minx) *minx = 0;
    if (miny) *miny = 0;
    if (maxx) *maxx = 0;
    if (maxy) *maxy = 0;
    return 0;
  }

  if (minx) *minx = fixedpt_toint(fixedpt_muli(font->factor, x0));
  if (miny) *miny = fixedpt_toint(fixedpt_muli(font->factor, y0));
  if (maxx) *maxx = fixedpt_toint(fixedpt_muli(font->factor, x1));
  if (maxy) *maxy = fixedpt_toint(fixedpt_muli(font->factor, y1));
  return 0;
}

int TTF_FontAscent(TTF_Font *font) {
  return font->ascent;
}

int TTF_FontHeight(TTF_Font *font) {
  return font->height;
}

static struct {
  SDL_Color fg, bg;
  SDL_Color palette[256];
} palCache = {};

SDL_Surface *TTF_RenderGlyph_Shaded(TTF_Font *font, Uint16 ch, SDL_Color fg, SDL_Color bg) {
  stbtt_fontinfo *finfo = font->finfo;
  int glyphIndex = stbtt_FindGlyphIndex(finfo, ch);
  if (glyphIndex == 0) return NULL;
  int w, h;
  uint8_t *pixels = stbtt_GetGlyphBitmap(finfo, 0, font->factor, glyphIndex, &w, &h, NULL, NULL);

  SDL_Surface *s = SDL_CreateRGBSurfaceFrom(pixels, w, h, 8, w, 0, 0, 0, 0);
  s->flags &= ~SDL_PREALLOC;
  if (!(fg.val == palCache.fg.val && bg.val == palCache.bg.val)) {
    // cache miss
    int rdiff = fg.r - bg.r;
    int gdiff = fg.g - bg.g;
    int bdiff = fg.b - bg.b;
    int adiff = fg.a - bg.a;
    for (int i = 0; i < 256; i ++) {
      palCache.palette[i].r = bg.r + (i*rdiff) / 255;
      palCache.palette[i].g = bg.g + (i*gdiff) / 255;
      palCache.palette[i].b = bg.b + (i*bdiff) / 255;
      palCache.palette[i].a = bg.a + (i*adiff) / 255;
    }
    palCache.palette[0].a = bg.a;
    palCache.fg.val = fg.val;
    palCache.bg.val = bg.val;
  }
  memcpy(s->format->palette->colors, palCache.palette, sizeof(palCache.palette));
  return s;
}
