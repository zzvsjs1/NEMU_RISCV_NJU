#include "trap.h"

static uint32_t stack_words[4] __attribute__((aligned(16)));

int main()
{
    uintptr_t new_esp;

    stack_words[0] = 0xaaaaaaaau;
    stack_words[1] = 0x12345678u;
    stack_words[2] = 0x87654321u;
    stack_words[3] = 0xbbbbbbbbu;

    /*
     * Intel computes a POP memory destination that uses ESP as its base after the
     * pop has advanced ESP.  Therefore popl (%esp) reads stack_words[1], advances
     * ESP to stack_words[2], and writes the popped value to stack_words[2].
     */
    asm volatile("movl %%esp, %%edx\n\t"
                 "movl %%eax, %%esp\n\t"
                 ".byte 0x8f, 0x04, 0x24\n\t"
                 "movl %%esp, %%ecx\n\t"
                 "movl %%edx, %%esp\n\t"
                 : "=c"(new_esp)
                 : "a"((uintptr_t)&stack_words[1])
                 : "edx", "memory", "cc");

    check(new_esp == (uintptr_t)&stack_words[2]);
    check(stack_words[1] == 0x12345678u);
    check(stack_words[2] == 0x12345678u);
    return 0;
}
