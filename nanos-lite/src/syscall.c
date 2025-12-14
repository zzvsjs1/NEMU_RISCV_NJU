#include <common.h>
#include "syscall.h"
#include "fs.h"
#include <inttypes.h>
#include "memory.h"
#include "proc.h"

void context_uload(PCB *pcb, const char *filename, char *const argv[], char *const envp[]);
extern PCB *current;

void do_syscall(Context *c) 
{
  const uintptr_t num  = c->GPR1;
  const uintptr_t arg1 = c->GPR2;
  const uintptr_t arg2 = c->GPR3;
  const uintptr_t arg3 = c->GPR4;

#ifdef STRACE
  Log(
    "\nsyscall entry: "
    "\n\tid=%" PRIuPTR
    "\n\targ1=0x%016" PRIxPTR
    "\n\targ2=0x%016" PRIxPTR
    "\n\targ3=0x%016" PRIxPTR "",
    num, arg1, arg2, arg3
  );
#endif

  switch (num) {
    case SYS_exit: {
#ifdef STRACE
      Log(
        "SYS_exit called, Exit code = %d",
        (int)arg1
      );
#endif

      halt(arg1);
      break;
    }
    
    case SYS_yield: {
#ifdef STRACE
      Log("SYS_yield called");
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
      Log(
        "SYS_write called, fd=%d"
        ", buf=0x%016" PRIxPTR
        ", count=%zu",
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
      Log(
        "SYS_brk called, new_brk = 0x%016" PRIxPTR "",
        arg1
      );
#endif

      c->GPRx = 0;
      break;
    }

    case SYS_open: {
#ifdef STRACE
      Log(
        "SYS_open called, path=0x%016" PRIxPTR
        ", flags=%d"
        ", mode=%d",
        arg1, (int)arg2, (int)arg3
      );
#endif

      c->GPRx = fs_open((char*)arg1, (int)arg2, (int)arg3);
      break;
    }

    case SYS_read: {
#ifdef STRACE
      Log(
        "SYS_read called, fd=%d"
        ", buf=0x%016" PRIxPTR
        ", count=%zu",
        (int)arg1, arg2, (size_t)arg3
      );
#endif

      c->GPRx = fs_read((int)arg1, (void*)arg2, (size_t)arg3);
      break;
    }

    case SYS_close: {
#ifdef STRACE
      Log(
        "SYS_close called, fd=%d",
        (int)arg1
      );
#endif

      c->GPRx = fs_close((int)arg1);
      break;
    }
    
    case SYS_lseek: {
#ifdef STRACE
      Log(
        "SYS_lseek called, fd=%d"
        ", offset=0x%016" PRIxPTR
        ", whence=%d",
        (int)arg1, arg2, (int)arg3
      );
#endif

      c->GPRx = fs_lseek((int)arg1, (size_t)arg2, (int)arg3);
      break;
    }

    case SYS_gettimeofday: {
#ifdef STRACE
        Log(
          "SYS_gettimeofday entry: tv_ptr=0x%016" PRIxPTR
          ", tz_ptr=0x%016" PRIxPTR,
          arg1, arg2
        );
#endif

        struct timeval  *tv = (struct timeval *)arg1;
        struct timezone *tz = (struct timezone *)arg2;
        // read uptime (microseconds) from the abstract machine
        const uint64_t uptimeUs = io_read(AM_TIMER_UPTIME).us;

        // If tv is non-NULL, fill in time since Epoch (here: time since boot)
        if (tv) 
        {
            tv->tv_sec  = uptimeUs / 1000000;
            tv->tv_usec = uptimeUs % 1000000;
#ifdef STRACE
            Log("  -> tv_sec=%ld, tv_usec=%06ld", (long)tv->tv_sec, (long)tv->tv_usec);
#endif
        }

        // If tz is non-NULL, zero it out (timezone support is obsolete)
        if (tz) 
        {
            tz->tz_minuteswest = 0;
            tz->tz_dsttime     = 0;

#ifdef STRACE
            Log("  -> tz_minuteswest=0, tz_dsttime=0");
#endif
        }

        // success
        c->GPRx = 0;
      break;
    }

    case SYS_execve: {
      const char *filename = (const char *)arg1;
      char *const *argv = (char *const *)arg2;
      char *const *envp = (char *const *)arg3;

      // Check existence, execvp will probe multiple candidates
      int fd = fs_open((char *)filename, 0, 0);
      if (fd < 0) 
      {
        c->GPRx = (uintptr_t)-2;   // ENOENT
        break;
      }
      
      fs_close(fd);

      context_uload(current, filename, (char *const *)argv, (char *const *)envp);

      void switch_boot_pcb();
      switch_boot_pcb();
      yield();

      // Should not reach here on success
      c->GPRx = (uintptr_t)-1;
      break;
    }

    default: {
      panic("Unhandled syscall ID = %" PRIuPTR, num);
      break;
    }
  }
}
