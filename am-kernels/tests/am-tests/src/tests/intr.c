#include <amtest.h>

Context *simple_trap(Event ev, Context *ctx) {
  // Print one character per event so interrupt delivery can be checked even on
  // a very small serial console. The handler returns the same context because
  // this test is about delivery, not scheduling.
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
  // Enabling interrupts after probing the input device ensures device setup has
  // happened before timer or I/O events start reaching the trap handler.
  iset(1);

  int i = 1;
  while (i) {
    for (volatile int i = 0; i < 1000; i++) ;
    // for (volatile int i = 0; i < 1000; i++) ;
    // Keep issuing cooperative yields so the output still shows CTE progress
    // even on targets where timer interrupts are disabled or very sparse.
    yield();
    // --i;
  }
}
