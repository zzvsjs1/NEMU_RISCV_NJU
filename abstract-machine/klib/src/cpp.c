#include <am.h>
#include <klib.h>

#ifndef __ISA_NATIVE__

// Minimal C++ ABI hooks let simple C++ objects link in the freestanding AM
// environment. They intentionally do not implement full destructor registration
// because AM kernels terminate through halt(), not a hosted C runtime shutdown.
void __dso_handle() {
}

void __cxa_guard_acquire() {
}

void __cxa_guard_release() {
}

void __cxa_atexit() {
  assert(0);
}

#endif
