.globl  _start
_start:

#if defined(__ISA_AM_NATIVE__)

  movq $0, %rbp

  // rax holds the stack top (same convention as __ISA_X86_64__ here)
  movq %rax, %rsp
  
  // (rsp + 8) should be multiple of 16 when
  // control is transfered to the function entry point.
  // See amd64 ABI manual for more details
  andq $0xfffffffffffffff0, %rsp

  movq %rax, %rdi
  call call_main

#elif defined(__ISA_X86_64__)

  movq $0, %rbp
  movq %rax, %rsp
  movq %rax, %rdi
  call call_main

#elif defined(__ISA_X86__)

  movl $0, %ebp
  movl %eax, %esp
  pushl %eax
  call call_main

#elif defined(__ISA_MIPS32__)

  move $fp, $zero

#ifdef __NAVY_VME__
  # Nanos passes the virtual address of argc through v0/GPRx.  Keep a0 on
  # that exact word while rounding only the active O32 stack pointer down.
  move $a0, $v0
  move $sp, $v0
  li $t0, -8
  and $sp, $sp, $t0
#endif

  # Every O32 caller reserves four argument-home words for its callee.  Keep
  # the argument block in a0 unchanged while placing this area below it.
  addiu $sp, $sp, -16

  jal call_main
  nop

#elif defined(__riscv)
  mv s0, zero

  # a0 holds the user stack top passed by Nanos-lite via GPRx.
  mv sp, a0

  # Keep ABI alignment: sp should be 16-byte aligned.
  andi sp, sp, -16

  jal call_main

#elif defined(__ISA_LOONGARCH32R__)

  move $fp, $zero
  bl call_main

#else
#error Unsupported ISA
#endif
