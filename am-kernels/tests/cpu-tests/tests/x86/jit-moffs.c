#include "trap.h"

uint32_t jit_moffs_dword __attribute__((aligned(4096)));
uint8_t jit_moffs_byte __attribute__((aligned(4096)));

int main()
{
    uint32_t loaded = 0;
    uint32_t byte_loaded = 0;

    jit_moffs_dword = 0x13572468u;
    jit_moffs_byte = 0x5au;

    asm volatile(".byte 0xa1\n\t"
                 ".long jit_moffs_dword\n\t"
                 : "=a"(loaded)
                 :
                 : "memory");
    check(loaded == 0x13572468u);

    asm volatile("movl $0x89abcdef, %%eax\n\t"
                 ".byte 0xa3\n\t"
                 ".long jit_moffs_dword"
                 :
                 :
                 : "eax", "memory");
    check(jit_moffs_dword == 0x89abcdefu);

    asm volatile("movl $0x12345600, %%eax\n\t"
                 ".byte 0xa0\n\t"
                 ".long jit_moffs_byte"
                 : "=a"(byte_loaded)
                 :
                 : "memory");
    check(byte_loaded == 0x1234565au);

    asm volatile("movb $0xc3, %%al\n\t"
                 ".byte 0xa2\n\t"
                 ".long jit_moffs_byte"
                 :
                 :
                 : "eax", "memory");
    check(jit_moffs_byte == 0xc3u);

    return 0;
}
