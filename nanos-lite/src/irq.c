#include <common.h>

static Context* do_event(Event e, Context* c) 
{
  switch (e.event) {
    case EVENT_YIELD: {
      break;
    }
    case EVENT_SYSCALL: {
      void do_syscall(Context *c);
      do_syscall(c);
      break;
    }
    default: {
      panic("Unhandled event ID = %d", e.event);
      break;
    }
  }

  return c;
}

void init_irq(void) {
  Log("Initializing interrupt/exception handler...");
  cte_init(do_event);
}
