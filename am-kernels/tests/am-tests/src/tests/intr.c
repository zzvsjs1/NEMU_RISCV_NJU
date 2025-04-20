#include <amtest.h>

Context *simple_trap(Event ev, Context *ctx) {
  switch(ev.event) {
    case EVENT_IRQ_TIMER:
      putch('t'); break;
    case EVENT_IRQ_IODEV:
      putch('d'); break;
    case EVENT_YIELD:
      putch('y'); break;
    default:
      panic("Unhandled event"); break;
      break;
  }
  return ctx;
}

void hello_intr() {
  printf("Hello, AM World @ " __ISA__ "\n");
  printf("  t = timer, d = device, y = yield\n");
  io_read(AM_INPUT_CONFIG);
  iset(1);

  int i = 1;
  while (i) {
    for (volatile int i = 0; i < 1000; i++) ;
    // for (volatile int i = 0; i < 1000; i++) ;
    yield();
    // --i;
  }
}
