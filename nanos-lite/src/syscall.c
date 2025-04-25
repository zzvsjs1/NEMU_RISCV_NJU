#include <common.h>
#include "syscall.h"
#include "fs.h"

void do_syscall(Context *c) 
{
  const uintptr_t num  = c->GPR1;
  const uintptr_t arg1 = c->GPR2;
  const uintptr_t arg2 = c->GPR3;
  const uintptr_t arg3 = c->GPR4;

#ifdef STRACE
  printf("syscall entry: id=%d, arg1=0x%x, arg2=0x%x, arg3=0x%x\n",
         num, arg1, arg2, arg3);
#endif

  int ret = -1;

  switch (num) {
    case SYS_exit: {
      // Exit code.
#ifdef STRACE
      printf("SYS_exit called, Exit code = %d\n", c->GPR2);
#endif

      halt(arg1);
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
      const int fd = arg1;
      char* buf = (char*)arg2;
      const size_t count = arg3;

#ifdef STRACE
      printf("SYS_write called, fd = %d, buf = 0x%x, count = %d\n", fd, (uintptr_t)buf, count);
#endif

      // Is stdout stderr?
      if (fd == 1 || fd == 2)
      {
        for (size_t i = 0; i < count; i++)
        {
          putch(buf[i]);
        }
        
        c->GPRx = count;
      }
      // Actual fd.
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

    case SYS_open {
#ifdef STRACE
        printf("SYS_open, path=0x%x, flags=%d, mode=%d\n", arg1, (int)arg2, (int)arg3);
#endif

      c->GPRx = fs_open((char*)a[1], (int)a[2], (int)a[3]);
      break;
    }

    case SYS_read {
#ifdef STRACE
      printf("SYS_read, fd=%d, buf=0x%x, count=%d\n",
             arg1, arg2, (int)arg3);
#endif

      c->GPRx = fs_read((int)arg1, (void*)arg2, (size_t)arg3);
      break;
    }

    case SYS_write {
#ifdef STRACE
      printf("SYS_write, fd=%d, buf=0x%x, count=%d\n",
             arg1, arg2, (int)arg3);
#endif

      c->GPRx = fs_write((int)arg1, (void*)arg2, (size_t)arg3);
      break;
    }

    case SYS_close {
#ifdef STRACE
        printf("SYS_close, fd=%d\n", (int)arg1);
#endif

      c->GPRx = fs_close((int)arg1);
      break;
    }
    
    case SYS_lseek {
#ifdef STRACE
        printf("SYS_lseek, fd=%d, offset=%d, whence=%d\n",
               (int)arg1, (int)arg2, (int)arg3);
#endif

      c->GPRx = fs_lseek((int)arg1, (size_t)arg2, (int)arg3);
      break;
    }
    
    default: {
      panic("Unhandled syscall ID = %d", a[0]);
      break;
    }
  }
}
