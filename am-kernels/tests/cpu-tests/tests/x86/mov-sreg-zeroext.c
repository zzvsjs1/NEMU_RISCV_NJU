#include "trap.h"

static uint32_t read_ds_with_mov_r32_sreg(void)
{
    uint32_t value = 0xffffffffu;

    /*
     * Opcode 8c /r copies a segment selector into a general register.  In 32-bit
     * mode modern IA-32 CPUs zero the upper half of the destination register; a
     * 16-bit write would leave the 0xffff high word below unchanged.
     */
    asm volatile(".byte 0x8c, 0xd8" : "+a"(value)::"memory");
    return value;
}

int main()
{
    check(read_ds_with_mov_r32_sreg() == 0x10u);
    return 0;
}
