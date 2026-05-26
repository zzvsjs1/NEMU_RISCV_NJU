#include "trap.h"

static void movsb_forward(uint8_t *dst, const uint8_t *src)
{
    asm volatile("cld; movsb" : "+D"(dst), "+S"(src)::"memory", "cc");
}

static void movsb_backward(uint8_t *dst, const uint8_t *src)
{
    asm volatile("std; movsb; cld" : "+D"(dst), "+S"(src)::"memory", "cc");
}

static void movsl_forward(uint32_t *dst, const uint32_t *src)
{
    asm volatile("cld; movsl" : "+D"(dst), "+S"(src)::"memory", "cc");
}

int main()
{
    uint8_t src8[] = {0x11, 0x22, 0x33};
    uint8_t dst8[] = {0, 0, 0};
    uint32_t src32[] = {0x12345678};
    uint32_t dst32[] = {0};

    movsb_forward(&dst8[0], &src8[0]);
    check(dst8[0] == 0x11);
    check(dst8[1] == 0x00);

    movsb_backward(&dst8[2], &src8[2]);
    check(dst8[2] == 0x33);
    check(dst8[1] == 0x00);

    movsl_forward(&dst32[0], &src32[0]);
    check(dst32[0] == 0x12345678);

    return 0;
}
