#include <common.h>
#include "syscall.h"
#include "fs.h"
#include <inttypes.h>
#include "memory.h"
#include "proc.h"

void context_uload(PCB *pcb, const char *filename, char *const argv[], char *const envp[]);
extern PCB *current;

static volatile int need_resched = 0;
static volatile int context_replaced = 0;

// Called by do_event() to test and clear the reschedule request.
int syscall_need_resched_and_clear(void) 
{
  const int v = need_resched;
  need_resched = 0;
  return v;
}

Context *syscall_replacement_context_and_clear(void)
{
  if (!context_replaced)
  {
    return NULL;
  }

  context_replaced = 0;
  assert(current != NULL);
  assert(current->cp != NULL);
  return current->cp;
}

#ifdef STRACE
static const char *syscall_name(uintptr_t num)
{
  /* Keep unknown IDs printable so the panic path still has context. */
  switch (num) {
    case SYS_exit: return "exit";
    case SYS_yield: return "yield";
    case SYS_open: return "open";
    case SYS_read: return "read";
    case SYS_write: return "write";
    case SYS_close: return "close";
    case SYS_lseek: return "lseek";
    case SYS_brk: return "brk";
    case SYS_execve: return "execve";
    case SYS_gettimeofday: return "gettimeofday";
    default: return "unknown";
  }
}

static void strace_log(uintptr_t num, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t ret)
{
  /* Print one Linux-strace-like line after the syscall has produced GPRx. */
  switch (num) {
    case SYS_open:
      Log("strace: open(\"%s\", %d, %d) = %d", (char *)arg1, (int)arg2, (int)arg3, (int)ret);
      break;
    case SYS_read:
      Log("strace: read(%d, 0x%08" PRIxPTR ", %zu) = %d", (int)arg1, arg2, (size_t)arg3, (int)ret);
      break;
    case SYS_write:
      Log("strace: write(%d, 0x%08" PRIxPTR ", %zu) = %d", (int)arg1, arg2, (size_t)arg3, (int)ret);
      break;
    case SYS_close:
      Log("strace: close(%d) = %d", (int)arg1, (int)ret);
      break;
    case SYS_lseek:
      Log("strace: lseek(%d, %zu, %d) = %d", (int)arg1, (size_t)arg2, (int)arg3, (int)ret);
      break;
    case SYS_brk:
      Log("strace: brk(0x%08" PRIxPTR ") = %d", arg1, (int)ret);
      break;
    case SYS_execve:
      Log("strace: execve(\"%s\", 0x%08" PRIxPTR ", 0x%08" PRIxPTR ") = %d",
          (char *)arg1, arg2, arg3, (int)ret);
      break;
    case SYS_gettimeofday:
      Log("strace: gettimeofday(0x%08" PRIxPTR ", 0x%08" PRIxPTR ") = %d", arg1, arg2, (int)ret);
      break;
    case SYS_yield:
      Log("strace: yield() = %d", (int)ret);
      break;
    default:
      Log("strace: %s(0x%08" PRIxPTR ", 0x%08" PRIxPTR ", 0x%08" PRIxPTR ") = %d",
          syscall_name(num), arg1, arg2, arg3, (int)ret);
      break;
  }
}
#endif

void do_syscall(Context *c) 
{
  const uintptr_t num  = c->GPR1;
  const uintptr_t arg1 = c->GPR2;
  const uintptr_t arg2 = c->GPR3;
  const uintptr_t arg3 = c->GPR4;

  switch (num) {
    case SYS_exit: {
#ifdef STRACE
      /* SYS_exit halts before the common return-value trace can run. */
      Log("strace: exit(%d)", (int)arg1);
#endif

      halt(arg1);
      break;
    }
    
    case SYS_yield: {
      // yield();

      // Do not trigger another trap here, just request rescheduling.
      need_resched = 1;
      c->GPRx = 0;
      break;
    }

    case SYS_write: {
      const int    fd    = (int)arg1;
      const char  *buf   = (char*)arg2;
      const size_t count = (size_t)arg3;

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
      // brk() returns 0 on success, -1 on failure in our convention.
      int mm_brk(uintptr_t brk);
      c->GPRx = mm_brk(arg1);
      break;
    }

    case SYS_open: {
      c->GPRx = fs_open((char*)arg1, (int)arg2, (int)arg3);
      break;
    }

    case SYS_read: {
      c->GPRx = fs_read((int)arg1, (void*)arg2, (size_t)arg3);
      break;
    }

    case SYS_close: {
      c->GPRx = fs_close((int)arg1);
      break;
    }
    
    case SYS_lseek: {
      c->GPRx = fs_lseek((int)arg1, (size_t)arg2, (int)arg3);
      break;
    }

    case SYS_gettimeofday: {
        struct timeval  *tv = (struct timeval *)arg1;
        struct timezone *tz = (struct timezone *)arg2;
        // read uptime (microseconds) from the abstract machine
        const uint64_t uptimeUs = io_read(AM_TIMER_UPTIME).us;

        // If tv is non-NULL, fill in time since Epoch (here: time since boot)
        if (tv) 
        {
            tv->tv_sec  = uptimeUs / 1000000;
            tv->tv_usec = uptimeUs % 1000000;
        }

        // If tz is non-NULL, zero it out (timezone support is obsolete)
        if (tz) 
        {
            tz->tz_minuteswest = 0;
            tz->tz_dsttime     = 0;
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

      // Return through the freshly built user context instead of the old
      // syscall frame, which belongs to the image being replaced. For execve()
      // success there is no user-visible return value: the new program starts
      // from its entry point. This also must not write c->GPRx after
      // context_uload(), because the old syscall frame and the freshly-created
      // ucontext for the same PCB are both placed at the top of the same kernel
      // stack. Writing the old frame's a0 here would overwrite the new image's
      // initial a0, which crt0 uses as the user stack pointer.
      context_replaced = 1;
      return;
    }

    default: {
      panic("Unhandled syscall ID = %" PRIuPTR, num);
      break;
    }
  }

#ifdef STRACE
  /* Normal syscalls trace here so return values are always included. */
  strace_log(num, arg1, arg2, arg3, c->GPRx);
#endif
}
