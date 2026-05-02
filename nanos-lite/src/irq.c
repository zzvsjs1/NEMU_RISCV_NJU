#include <common.h>

Context *schedule(Context *prev);

// Provided by syscall.c
int syscall_need_resched_and_clear(void);
Context *syscall_replacement_context_and_clear(void);

static Context* do_event(Event e, Context* c) 
{
  switch (e.event) {
    case EVENT_YIELD: {
      // Yield event always triggers scheduling.
      return schedule(c);
    }

    case EVENT_SYSCALL: {
      void do_syscall(Context *c);
      do_syscall(c);

      Context *replacement = syscall_replacement_context_and_clear();
      if (replacement != NULL)
      {
        return replacement;
      }

      // Perform scheduling once if the syscall requested it.
      if (syscall_need_resched_and_clear()) 
      {
        return schedule(c);
      }

      break;
    }

    case EVENT_IRQ_TIMER: {
      return schedule(c);
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
