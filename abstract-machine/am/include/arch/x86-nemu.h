#ifndef ARCH_H__
#define ARCH_H__

struct Context {
  void *cr3;
  uintptr_t ds;
  uintptr_t eax, ebx, ecx, edx, esp0, esi, edi, ebp;
  uintptr_t eip, cs, eflags, esp, ss3;
};

#define GPR1 eax
#define GPR2 ebx
#define GPR3 ecx
#define GPR4 edx
#define GPRx eax
#define GPRSP esp

#endif
