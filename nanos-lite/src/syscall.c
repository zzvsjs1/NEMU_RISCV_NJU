#include <common.h>
#include "syscall.h"
void do_syscall(Context *c) {
  uintptr_t a[4];
  a[0] = c->GPR1;
  a[1] = c->GPR2;
  a[2] = c->GPR3;
  a[3] = c->GPR4;

  switch (a[0]) {
    case SYS_exit: {
      // Exit code.
#ifdef STRACE
      printf("SYS_exit called, Exit code = %d\n", c->GPR2);
#endif

      halt(c->GPR2);
      break;
    }
    case SYS_yield: {
#ifdef STRACE
      printf("SYS_yield called\n");
#endif

      yield();
      c->GPRx = 0;
      break;
    }
    case SYS_write: {
#ifdef STRACE
      printf("SYS_write called, fd = %d, buf = 0x%x, count = %d\n", a[1], a[2], a[3]);
#endif

      int fd = a[1];
      char* buf = (char*)a[2];
      size_t count = a[3];
      if (fd == 1 || fd == 2)
      {
        for (size_t i = 0; i < count; i++)
        {
          putch(buf[i]);
        }
        
        c->GPRx = count;
      }
      else
      {
        c->GPRx = -1;
      }

      break;
    }
    case SYS_brk: {
#ifdef STRACE
    printf("SYS_brk called, new_brk = 0x%x\n", a[1]);
#endif

      c->GPRx = 0;
      break;
    }
    default: {
      panic("Unhandled syscall ID = %d", a[0]);
      break;
    }
  }
}
