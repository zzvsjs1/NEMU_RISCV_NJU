#include <stdint.h>

#ifdef __ISA_NATIVE__
#error can not support ISA=native
#endif

#define SYS_yield 1
extern int _syscall_(int, uintptr_t, uintptr_t, uintptr_t);

int main()
{
    /*
   * This tiny binary reaches the syscall trampoline directly. It is useful
   * when checking that nanos-lite can load a user image and return from SYS_yield
   * without involving libc wrappers.
   */
    return _syscall_(SYS_yield, 0, 0, 0);
}
