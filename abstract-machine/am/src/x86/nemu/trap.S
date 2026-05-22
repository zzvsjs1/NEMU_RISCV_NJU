#include <x86/x86.h>

#define SEG_KCODE 1
#define SEG_KDATA 2

#----|------------entry------------|--errcode--|---irq id---|-----handler-----|
.globl __am_vecsys;    __am_vecsys: pushl $0; pushl $0x80; jmp __am_asm_trap
.globl __am_vectrap;  __am_vectrap: pushl $0; pushl $0x81; jmp __am_asm_trap
.globl __am_irq0;        __am_irq0: pushl $0; pushl   $32; jmp __am_asm_trap
# #PF already has a hardware error code below ESP, so only add the vector id.
.globl __am_vecpf;      __am_vecpf:           pushl   $14; jmp __am_asm_trap
.globl __am_vecnull;  __am_vecnull: pushl $0; pushl   $-1; jmp __am_asm_trap

.globl __am_kcontext_start
__am_kcontext_start:
  // eax = arg, ebx = entry
  pushl %eax
  pushl $__am_panic_on_return
  jmpl *%ebx

__am_asm_trap:
  cli

  /*
   * Reserve the eip/cs/eflags/esp/ss3 slots inside Context.  The real hardware
   * frame lives after irq/errcode; __am_irq_handle() copies it into these slots
   * after deciding whether this trap came from kernel or user mode.
   */
  subl $20, %esp
  pushl %ebp
  pushl %edi
  pushl %esi
  pushl $0
  pushl %edx
  pushl %ecx
  pushl %ebx
  pushl %eax
  movw %ds, %ax
  pushl %eax
  pushl $0

  movw $KSEL(SEG_KDATA), %ax
  movw %ax, %ds
  movw %ax, %es
  movw %ax, %ss

  pushl %esp
  call __am_irq_handle

  /* __am_irq_handle() finishes by calling __am_iret(); it must not return. */
  hlt

.globl __am_iret
__am_iret:
  addl $4, %esp
  popl %eax
  movl %eax, %esp

  addl $4, %esp
  popl %eax
  movw %ax, %ds
  movw %ax, %es

  cmpw $KSEL(SEG_KCODE), 36(%esp)
  je .kernel_iret

.user_iret:
  popl %eax
  popl %ebx
  popl %ecx
  popl %edx
  addl $4, %esp
  popl %esi
  popl %edi
  popl %ebp
  iret

.kernel_iret:
  popl %eax
  popl %ebx
  popl %ecx
  popl %edx
  addl $4, %esp

  /*
   * For ring-0 return, iret will not pop esp/ss.  Move the register tail and
   * hardware frame to Context.esp so the kernel thread resumes with its expected
   * stack pointer.
   */
  movl %esp, %ebp
  movl 24(%ebp), %edi

  movl 20(%ebp), %esi;  movl %esi,  -4(%edi)
  movl 16(%ebp), %esi;  movl %esi,  -8(%edi)
  movl 12(%ebp), %esi;  movl %esi, -12(%edi)
  movl  8(%ebp), %esi;  movl %esi, -16(%edi)
  movl  4(%ebp), %esi;  movl %esi, -20(%edi)
  movl  0(%ebp), %esi;  movl %esi, -24(%edi)

  leal -24(%edi), %esp

  popl %esi
  popl %edi
  popl %ebp
  iret
