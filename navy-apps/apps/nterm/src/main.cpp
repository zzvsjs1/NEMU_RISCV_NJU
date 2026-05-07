#include <nterm.h>
#include <SDL.h>
#include <SDL_bdf.h>
#include <unistd.h>

extern char **environ;

static const char *font_fname = "/share/fonts/Courier-7.bdf";
static BDF_Font *font = NULL;
static SDL_Surface *screen = NULL;
Terminal *term = NULL;

void builtin_sh_run();
void extern_app_run(const char *app_path);

int main(int argc, char *argv[]) {
  SDL_Init(0);
  font = new BDF_Font(font_fname);

  // Open the whole physical display, then derive a character grid that fits
  // the selected bitmap font. This keeps NTerm in sync with 800x600 without
  // baking one fixed terminal size into the application.  The display contract
  // belongs to miniSDL/NDL, so terminal geometry is treated as a result of that
  // contract rather than a separate hard-coded app policy.
  screen = SDL_SetVideoMode(0, 0, 32, SDL_HWSURFACE);
  assert(screen);

  const int term_w = screen->w / font->w;
  const int term_h = screen->h / font->h;
  assert(term_w > 0 && term_h > 0);

  term = new Terminal(term_w, term_h);

  if (argc >= 2 && strcmp(argv[1], "--selftest") == 0) {
    // Build/ramdisk tests can execute this mode without synthesizing keyboard
    // events in the graphical terminal.
    return nterm_selftest(false);
  }

  if (argc >= 2 && strcmp(argv[1], "--autopal") == 0) {
    // This mode mirrors the shell `autopal` command but avoids the shell event
    // loop, which makes it suitable for deterministic startup tests.
    const char *pal_argv[] = { "/bin/pal", NULL };
    execve(pal_argv[0], (char **)pal_argv, environ);
    printf("NTERM_AUTOPAL: exec failed\n");
    return 1;
  }

  if (argc < 2) { builtin_sh_run(); }
  else { extern_app_run(argv[1]); }

  // should not reach here
  assert(0);
}

static void draw_ch(int x, int y, char ch, uint32_t fg, uint32_t bg) {
  // Each character is rendered through the BDF helper as a short-lived SDL
  // surface. The terminal keeps only character and colour state, so this is
  // the narrow boundary between terminal emulation and miniSDL drawing.
  SDL_Surface *s = BDF_CreateSurface(font, ch, fg, bg);
  SDL_Rect dstrect = { .x = (int16_t)x, .y = (int16_t)y };
  SDL_BlitSurface(s, NULL, screen, &dstrect);
  SDL_FreeSurface(s);
}

void refresh_terminal() {
  int needsync = 0;
  for (int i = 0; i < term->w; i ++)
    for (int j = 0; j < term->h; j ++)
      if (term->is_dirty(i, j)) {
        draw_ch(i * font->w, j * font->h, term->getch(i, j), term->foreground(i, j), term->background(i, j));
        needsync = 1;
      }
  term->clear();

  static uint32_t last = 0;
  static int flip = 0;
  uint32_t now = SDL_GetTicks();
  if (now - last > 500 || needsync) {
    int x = term->cursor.x, y = term->cursor.y;
    uint32_t color = (flip ? term->foreground(x, y) : term->background(x, y));
    draw_ch(x * font->w, y * font->h, ' ', 0, color);
    SDL_UpdateRect(screen, 0, 0, 0, 0);
    if (now - last > 500) {
      flip = !flip;
      last = now;
    }
  }
}

#define ENTRY(KEYNAME, NOSHIFT, SHIFT) { SDLK_##KEYNAME, #KEYNAME, NOSHIFT, SHIFT }
static const struct {
  int keycode;
  const char *name;
  char noshift, shift;
} SHIFT[] = {
  ENTRY(ESCAPE,       '\033', '\033'),
  ENTRY(SPACE,        ' ' , ' '),
  ENTRY(RETURN,       '\n', '\n'),
  ENTRY(BACKSPACE,    '\b', '\b'),
  ENTRY(1,            '1',  '!'),
  ENTRY(2,            '2',  '@'),
  ENTRY(3,            '3',  '#'),
  ENTRY(4,            '4',  '$'),
  ENTRY(5,            '5',  '%'),
  ENTRY(6,            '6',  '^'),
  ENTRY(7,            '7',  '&'),
  ENTRY(8,            '8',  '*'),
  ENTRY(9,            '9',  '('),
  ENTRY(0,            '0',  ')'),
  ENTRY(GRAVE,        '`',  '~'),
  ENTRY(MINUS,        '-',  '_'),
  ENTRY(EQUALS,       '=',  '+'),
  ENTRY(SEMICOLON,    ';',  ':'),
  ENTRY(APOSTROPHE,   '\'', '"'),
  ENTRY(LEFTBRACKET,  '[',  '{'),
  ENTRY(RIGHTBRACKET, ']',  '}'),
  ENTRY(BACKSLASH,    '\\', '|'),
  ENTRY(COMMA,        ',',  '<'),
  ENTRY(PERIOD,       '.',  '>'),
  ENTRY(SLASH,        '/',  '?'),
  ENTRY(A,            'a',  'A'),
  ENTRY(B,            'b',  'B'),
  ENTRY(C,            'c',  'C'),
  ENTRY(D,            'd',  'D'),
  ENTRY(E,            'e',  'E'),
  ENTRY(F,            'f',  'F'),
  ENTRY(G,            'g',  'G'),
  ENTRY(H,            'h',  'H'),
  ENTRY(I,            'i',  'I'),
  ENTRY(J,            'j',  'J'),
  ENTRY(K,            'k',  'K'),
  ENTRY(L,            'l',  'L'),
  ENTRY(M,            'm',  'M'),
  ENTRY(N,            'n',  'N'),
  ENTRY(O,            'o',  'O'),
  ENTRY(P,            'p',  'P'),
  ENTRY(Q,            'q',  'Q'),
  ENTRY(R,            'r',  'R'),
  ENTRY(S,            's',  'S'),
  ENTRY(T,            't',  'T'),
  ENTRY(U,            'u',  'U'),
  ENTRY(V,            'v',  'V'),
  ENTRY(W,            'w',  'W'),
  ENTRY(X,            'x',  'X'),
  ENTRY(Y,            'y',  'Y'),
  ENTRY(Z,            'z',  'Z'),
};

char handle_key(const char *buf) {
  char key[32];
  static int shift = 0;
  sscanf(buf + 2, "%s", key);

  // The /dev/events text protocol reports key names, so this path translates
  // Navy's window-manager events into the same characters used by the SDL
  // event path below.
  if (strcmp(key, "LSHIFT") == 0 || strcmp(key, "RSHIFT") == 0)  { shift ^= 1; return '\0'; }

  if (buf[0] == 'd') {
    if (key[0] >= 'A' && key[0] <= 'Z' && key[1] == '\0') {
      if (shift) return key[0];
      else return key[0] - 'A' + 'a';
    }
    for (auto item: SHIFT) {
      if (strcmp(item.name, key) == 0) {
        if (shift) return item.shift;
        else return item.noshift;
      }
    }
  }
  return '\0';
}

char handle_key(SDL_Event *ev) {
  static int shift = 0;
  int key = ev->key.keysym.sym;
  // Built-in shell mode receives miniSDL key symbols directly. Keeping this
  // mapping parallel with the textual event mapping lets external and built-in
  // modes share Terminal::keypress() without extra OS support.
  if (key == SDLK_LSHIFT || key == SDLK_RSHIFT) { shift ^= 1; return '\0'; }

  if (ev->type == SDL_KEYDOWN) {
    for (auto item: SHIFT) {
      if (item.keycode == key) {
        if (shift) return item.shift;
        else return item.noshift;
      }
    }
  }
  return '\0';
}
