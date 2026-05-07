.section entry, "ax"
.globl _start
.type _start, @function

_start:
  // AM enters here directly from the emulator, without a C runtime prologue.
  // Clear the frame pointer, install the linker-provided stack, then let TRM
  // call main() and route the return value through halt().
  mv s0, zero
  la sp, _stack_pointer
  call _trm_init

.size _start, . - _start