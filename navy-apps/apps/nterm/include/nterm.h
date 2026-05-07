#ifndef __NTERM_H__
#define __NTERM_H__

#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>

class Terminal {
private:
  // Terminal owns an in-memory character grid, not the SDL surface itself.
  // The renderer reads dirty cells from this grid and turns them into bitmap
  // font blits, keeping process I/O and display refresh loosely coupled.  This
  // mirrors the wider Navy device contracts: app state is stable in memory, and
  // the platform layer decides when pixels are pushed to the device.
  struct Pattern {
    const char *pattern;
    void (Terminal::*handle)(int *args);
  };
  static Pattern esc_seqs[];

  char *buf, input[256], cooked[256];
  uint8_t *color;
  bool *dirty;
  int inp_len;

  void move_one();
  void backspace();
  size_t write_escape(const char *str, size_t count);
  void scroll_up();


  void esc_move(int *args);
  void esc_movefirst(int *args);
  void esc_moveup(int *args);
  void esc_movedown(int *args);
  void esc_moveleft(int *args);
  void esc_moveright(int *args);
  void esc_save(int *args);
  void esc_restore(int *args);
  void esc_clear(int *args);
  void esc_erase(int *args);
  void esc_setattr1(int *args);
  void esc_setattr2(int *args);
  void esc_setattr3(int *args);
  void esc_rawmode(int *args);
  void esc_cookmode(int *args);

public:
  enum class Mode {
    // Raw mode forwards each keypress immediately to the child process.
    // Cooked mode buffers a line locally so the built-in shell can edit and
    // submit commands without needing kernel-side terminal discipline.
    raw,
    cook,
  } mode;

  int w, h;
  struct Cursor {
    int x, y;
  } cursor, saved;
  uint8_t col_f, col_b;

  Terminal(int width, int height);
  ~Terminal();
  void write(const char *str, size_t count);
  bool is_dirty(int x, int y);

  void putch(int x, int y, char ch);
  char getch(int x, int y);
  uint32_t foreground(int x, int y); // get color
  uint32_t background(int x, int y);

  void clear(); // clear dirty states
  void clear_screen();
  const char *keypress(char ch);
};

extern Terminal *term;

void refresh_terminal();
int nterm_selftest(bool to_terminal);

#endif
