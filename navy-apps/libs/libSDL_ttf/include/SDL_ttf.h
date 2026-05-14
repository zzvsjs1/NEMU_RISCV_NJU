#include <SDL.h>

typedef struct TTF_Font TTF_Font;

#ifdef __cplusplus
extern "C"
{
#endif

    int TTF_Init();
    TTF_Font *TTF_OpenFont(const char *file, int size);
    TTF_Font *TTF_OpenFontRW(SDL_RWops *src, int freesrc, int ptsize);
    void TTF_CloseFont(TTF_Font *font);
    int TTF_GlyphMetrics(TTF_Font *font, Uint16 ch, int *minx, int *maxx, int *miny, int *maxy, int *advance);
    int TTF_FontAscent(TTF_Font *font);
    int TTF_FontHeight(TTF_Font *font);
    SDL_Surface *TTF_RenderGlyph_Shaded(TTF_Font *font, Uint16 ch, SDL_Color fg, SDL_Color bg);

#ifdef __cplusplus
}
#endif
