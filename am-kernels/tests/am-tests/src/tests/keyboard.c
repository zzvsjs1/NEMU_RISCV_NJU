#include <amtest.h>

#define NAMEINIT(key)  [ AM_KEY_##key ] = #key,
static const char *names[] = {
  AM_KEYS(NAMEINIT)
};

static bool has_uart, has_kbd;

static void drain_keys() {
  // UART and keyboard are separate AM input contracts. Drain both queues until
  // their sentinel values appear, so a slow terminal or backend cannot leave
  // stale events that confuse the next polling iteration.
  if (has_uart) {
    while (1) {
      char ch = io_read(AM_UART_RX).data;
      if (ch == -1) break;
      printf("Got (uart): %c (%d)\n", ch, ch & 0xff);
    }
  }

  if (has_kbd) {
    while (1) {
      AM_INPUT_KEYBRD_T ev = io_read(AM_INPUT_KEYBRD);
      if (ev.keycode == AM_KEY_NONE) break;
      printf("Got  (kbd): %s (%d) %s\n", names[ev.keycode], ev.keycode, ev.keydown ? "DOWN" : "UP");
    }
  }
}

void keyboard_test() {
  printf("Try to press any key (uart or keyboard)...\n");
  // Query presence once at startup. Some AM platforms do not provide both input
  // devices, and repeatedly probing config registers would obscure the actual
  // event stream this test is meant to display.
  has_uart = io_read(AM_UART_CONFIG).present;
  has_kbd  = io_read(AM_INPUT_CONFIG).present;
  while (1) {
    drain_keys();
  }
}
