#include <common.h>
#include "syscall.h"
#include "fs.h"
#include <inttypes.h>   // for PRIxPTR, PRIuPTR, etc.

void do_syscall(Context *c) 
{
  const uintptr_t num  = c->GPR1;
  const uintptr_t arg1 = c->GPR2;
  const uintptr_t arg2 = c->GPR3;
  const uintptr_t arg3 = c->GPR4;

#ifdef STRACE
  printf(
    "syscall entry: id=%" PRIuPTR
    ", arg1=0x%016" PRIxPTR
    ", arg2=0x%016" PRIxPTR
    ", arg3=0x%016" PRIxPTR "\n",
    num, arg1, arg2, arg3
  );
#endif

  switch (num) {
    case SYS_exit: {
#ifdef STRACE
      printf(
        "SYS_exit called, Exit code = %d\n\n",
        (int)arg1
      );
#endif

      halt(arg1);
      break;
    }
    
    case SYS_yield: {
#ifdef STRACE
      printf("SYS_yield called\n\n");
#endif

      yield();
      c->GPRx = 0;

      break;
    }

    case SYS_write: {
      const int    fd    = (int)arg1;
      const char  *buf   = (char*)arg2;
      const size_t count = (size_t)arg3;

#ifdef STRACE
      printf(
        "SYS_write called, fd=%d"
        ", buf=0x%016" PRIxPTR
        ", count=%zu\n\n",
        fd, (uintptr_t)buf, count
      );
#endif

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
        c->GPRx = fs_write(fd, buf, count);
      }

      break;
    }

    case SYS_brk: {
#ifdef STRACE
      printf(
        "SYS_brk called, new_brk = 0x%016" PRIxPTR "\n\n",
        arg1
      );
#endif

      c->GPRx = 0;
      break;
    }

    case SYS_open: {
#ifdef STRACE
      printf(
        "SYS_open called, path=0x%016" PRIxPTR
        ", flags=%d"
        ", mode=%d\n\n",
        arg1, (int)arg2, (int)arg3
      );
#endif

      c->GPRx = fs_open((char*)arg1, (int)arg2, (int)arg3);
      break;
    }

    case SYS_read: {
#ifdef STRACE
      printf(
        "SYS_read called, fd=%d"
        ", buf=0x%016" PRIxPTR
        ", count=%zu\n\n",
        (int)arg1, arg2, (size_t)arg3
      );
#endif

      c->GPRx = fs_read((int)arg1, (void*)arg2, (size_t)arg3);
      break;
    }

    case SYS_close: {
#ifdef STRACE
      printf(
        "SYS_close called, fd=%d\n\n",
        (int)arg1
      );
#endif

      c->GPRx = fs_close((int)arg1);
      break;
    }
    
    case SYS_lseek: {
#ifdef STRACE
      printf(
        "SYS_lseek called, fd=%d"
        ", offset=0x%016" PRIxPTR
        ", whence=%d\n\n",
        (int)arg1, arg2, (int)arg3
      );
#endif

      c->GPRx = fs_lseek((int)arg1, (size_t)arg2, (int)arg3);
      break;
    }
    
    default: {
      panic("Unhandled syscall ID = %" PRIuPTR, num);
      break;
    }
  }
}
