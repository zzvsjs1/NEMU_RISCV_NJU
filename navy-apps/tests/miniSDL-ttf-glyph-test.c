#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include <NDL.h>
#include <SDL_ttf.h>

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

int SDL_SetError(const char *fmt, ...)
{
    (void)fmt;
    return -1;
}

static void test_invalid_font_stream_is_rejected(void)
{
    uint8_t bad_font[32] = {0};
    SDL_RWops *rw = SDL_RWFromMem(bad_font, sizeof(bad_font));

    assert(rw != NULL);
    assert(TTF_OpenFontRW(rw, 1, 12) == NULL);
}

static void test_visible_glyph_surface_matches_metrics_box(TTF_Font *font)
{
    SDL_Color fg = {.r = 0xff, .g = 0xff, .b = 0xff, .a = 0xff};
    SDL_Color bg = {.r = 0x00, .g = 0x00, .b = 0x00, .a = 0x00};
    SDL_Surface *surface = TTF_RenderGlyph_Shaded(font, 'g', fg, bg);
    int minx = 0;
    int maxx = 0;
    int miny = 0;
    int maxy = 0;

    assert(surface != NULL);
    assert(TTF_GlyphMetrics(font, 'g', &minx, &maxx, &miny, &maxy, NULL) == 0);
    /*
     * ONScripter positions glyphs itself from SDL_ttf metrics.  The rendered
     * surface therefore has to stay tight around the ink; adding ascent or
     * baseline padding here applies the vertical offset twice.
     */
    assert(surface->w == maxx - minx);
    assert(surface->h == maxy - miny);
    SDL_FreeSurface(surface);
}

static void test_ascii_glyph_fits_half_width_cell(void)
{
    const int font_size = 21;
    const int half_cell = (font_size + 1) / 2;
    SDL_Color fg = {.r = 0xff, .g = 0xff, .b = 0xff, .a = 0xff};
    SDL_Color bg = {.r = 0x00, .g = 0x00, .b = 0x00, .a = 0x00};
    TTF_Font *font = TTF_OpenFont("navy-apps/apps/onscripter/game/default.ttf", font_size);
    SDL_Surface *surface = NULL;

    assert(font != NULL);
    surface = TTF_RenderGlyph_Shaded(font, 'M', fg, bg);
    assert(surface != NULL);
    /*
     * ONScripter advances single-byte characters by one hankaku cell, which is
     * half of the configured full-width font cell.  If a Latin glyph is wider
     * than that, words such as "Master" draw on top of themselves.
     */
    assert(surface->w <= half_cell);
    SDL_FreeSurface(surface);
    TTF_CloseFont(font);
}

int main(void)
{
    TTF_Font *font = TTF_OpenFont("navy-apps/apps/onscripter/game/default.ttf", 24);
    assert(font != NULL);

    /*
   * ONScripter scripts depend on CJK glyphs, punctuation, and blank-space
   * metrics. The table records whether a glyph should produce visible ink
   * while still requiring a positive advance for layout.  Blank glyphs are an
   * edge case because a zero-sized surface is valid, but a zero advance would
   * collapse script text.
   */
    const struct
    {
        uint16_t codepoint;
        bool has_ink;
    } glyphs[] = {
        {0x90a3, true},  /* 那 */
        {0x662f, true},  /* 是 */
        {0x5982, true},  /* 如 */
        {0x95ea, true},  /* 闪 */
        {0x7535, true},  /* 电 */
        {0x67aa, true},  /* 枪 */
        {0x4f46, true},  /* 但 */
        {0xff0c, true},  /* ， */
        {0x3002, true},  /* 。 */
        {0x2014, true},  /* dash-like punctuation often used by scripts */
        {0x0020, false}, /* ASCII space */
        {0x3000, false}, /* full-width space */
    };

    for (size_t i = 0; i < sizeof(glyphs) / sizeof(glyphs[0]); i++)
    {
        int minx = 0;
        int maxx = 0;
        int miny = 0;
        int maxy = 0;
        int advance = 0;
        uint16_t ch = glyphs[i].codepoint;

        if (TTF_GlyphMetrics(font, ch, &minx, &maxx, &miny, &maxy, &advance) != 0)
        {
            fprintf(stderr, "TTF_GlyphMetrics failed for U+%04x\n", ch);
            assert(0);
        }
        assert(advance > 0);

        SDL_Color fg = {.r = 0xff, .g = 0xff, .b = 0xff, .a = 0xff};
        SDL_Color bg = {.r = 0x00, .g = 0x00, .b = 0x00, .a = 0x00};
        SDL_Surface *surface = TTF_RenderGlyph_Shaded(font, ch, fg, bg);
        assert(surface != NULL);

        if (glyphs[i].has_ink)
        {
            assert(maxx > minx);
            assert(maxy > miny);
            assert(surface->w > 0);
            assert(surface->h > 0);
        }
        else
        {
            assert(minx == 0);
            assert(maxx == 0);
            assert(miny == 0);
            assert(maxy == 0);
            assert(surface->w == 0);
            assert(surface->h == 0);
        }
        SDL_FreeSurface(surface);
    }

    test_visible_glyph_surface_matches_metrics_box(font);
    TTF_CloseFont(font);
    test_invalid_font_stream_is_rejected();
    test_ascii_glyph_fits_half_width_cell();

    return 0;
}
