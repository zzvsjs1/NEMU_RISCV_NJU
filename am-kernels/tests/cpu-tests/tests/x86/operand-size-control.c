#include "trap.h"

static uint32_t push_pop16_result(void)
{
    uint32_t packed;

    asm volatile(
        "movl %%esp, %%edx\n\t"
        "movl $0x12345678, %%eax\n\t"
        ".byte 0x66, 0x50\n\t"
        "movl %%edx, %%ecx\n\t"
        "subl %%esp, %%ecx\n\t"
        "xorl %%eax, %%eax\n\t"
        ".byte 0x66, 0x58\n\t"
        "shll $16, %%ecx\n\t"
        "orl %%ecx, %%eax\n\t"
        : "=a"(packed)
        :
        : "ecx", "edx", "memory", "cc");

    return packed;
}

static uint32_t pushf16_result(void)
{
    uint32_t packed;

    asm volatile(
        "movl %%esp, %%edx\n\t"
        ".byte 0x66, 0x9c\n\t"
        "movl %%edx, %%ecx\n\t"
        "subl %%esp, %%ecx\n\t"
        ".byte 0x66, 0x58\n\t"
        "andl $0xffff, %%eax\n\t"
        "shll $16, %%ecx\n\t"
        "orl %%ecx, %%eax\n\t"
        : "=a"(packed)
        :
        : "ecx", "edx", "memory", "cc");

    return packed;
}

static uint32_t run_low_code(uintptr_t entry, const uint8_t *code, size_t len)
{
    uint8_t *dst = (uint8_t *)entry;

    for (size_t i = 0; i < len; i++)
    {
        dst[i] = code[i];
    }

    uint32_t (*fn)(void) = (uint32_t(*)(void))entry;
    return fn();
}

int main()
{
    static const uint8_t call_ret16[] = {
        0x66, 0xe8, 0x01, 0x00,       // callw helper
        0xc3,                         // retl to the C caller
        0xb8, 0xef, 0xbe, 0xad, 0xde, // helper: mov $0xdeadbeef,%eax
        0x66, 0xc3,                   // retw
    };
    static const uint8_t jmp16[] = {
        0x66, 0xe9, 0x01, 0x00,       // jmpw target
        0xc3,                         // wrong path: retl with old eax
        0xb8, 0x78, 0x56, 0x34, 0x12, // target: mov $0x12345678,%eax
        0xc3,                         // retl
    };

    check(push_pop16_result() == 0x00025678u);
    check((pushf16_result() >> 16) == 2);
    check((pushf16_result() & 0x0002u) != 0);
    check(run_low_code(0x8000, call_ret16, sizeof(call_ret16)) == 0xdeadbeefu);
    check(run_low_code(0x8100, jmp16, sizeof(jmp16)) == 0x12345678u);
    return 0;
}
