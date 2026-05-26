#include "trap.h"
#include <stdint.h>

static const int8_t rel8_values[] = {
    -128,
    -7,
    -1,
    0,
    1,
    0x7f,
};

static uint32_t movsx_word_mem_sum(void)
{
    uint32_t sum = 0;

    for (int i = 0; i < (int)(sizeof(rel8_values) / sizeof(rel8_values[0])); i++)
    {
        uint32_t edx = 0x12345678u;
        asm volatile("movsbw %1, %%dx"
                     : "+d"(edx)
                     : "m"(rel8_values[i])
                     : "cc");
        sum += edx;
    }

    return sum;
}

static uint32_t movsx_word_reg_sum(void)
{
    uint32_t sum = 0;

    for (int i = 0; i < (int)(sizeof(rel8_values) / sizeof(rel8_values[0])); i++)
    {
        uint32_t edx = 0x9abc0000u | (uint8_t)rel8_values[i];
        asm volatile("movsbw %%dl, %%dx"
                     : "+d"(edx)
                     :
                     : "cc");
        sum += edx;
    }

    return sum;
}

static uint32_t movsx_word_ref(uint32_t upper)
{
    uint32_t sum = 0;

    for (int i = 0; i < (int)(sizeof(rel8_values) / sizeof(rel8_values[0])); i++)
    {
        sum += upper | (uint16_t)(int16_t)rel8_values[i];
    }

    return sum;
}

int main(void)
{
    check(movsx_word_mem_sum() == movsx_word_ref(0x12340000u));
    check(movsx_word_reg_sum() == movsx_word_ref(0x9abc0000u));
    return 0;
}
