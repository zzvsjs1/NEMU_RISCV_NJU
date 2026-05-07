#include <am.h>
#include <stdatomic.h>
#include <klib-macros.h>

bool mpe_init(void (*entry)()) {
  // The NEMU AM target is single-core, so MPE startup is a direct call on the
  // boot CPU. A returning entry is a programming error because there are no
  // secondary CPUs or scheduler hand-off point to resume.
  entry();
  panic("MPE entry returns");
}

int cpu_count() {
  // Preserve the AM MPE contract even on a uniprocessor target: portable code
  // can still query the CPU count and run the same loops as on SMP platforms.
  return 1;
}

int cpu_current() {
  return 0;
}

int atomic_xchg(int *addr, int newval) {
  // Use the compiler atomic primitive so spin locks have real exchange
  // semantics under both native testing and the RISC-V cross-compiled guest.
  return atomic_exchange(addr, newval);
}
