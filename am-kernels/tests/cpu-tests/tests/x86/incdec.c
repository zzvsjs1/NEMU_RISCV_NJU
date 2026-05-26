#include "trap.h"

static uint16_t decw_reg(uint16_t value)
{
    asm volatile("decw %0" : "+r"(value));
    return value;
}

static uint16_t incw_reg(uint16_t value)
{
    asm volatile("incw %0" : "+r"(value));
    return value;
}

static uint32_t decw_keeps_high_word(uint32_t value)
{
    asm volatile("decw %w0" : "+r"(value));
    return value;
}

static uint32_t incw_keeps_high_word(uint32_t value)
{
    asm volatile("incw %w0" : "+r"(value));
    return value;
}

static uint8_t decb_reg(uint8_t value)
{
    asm volatile("decb %0" : "+q"(value));
    return value;
}

static uint8_t incb_reg(uint8_t value)
{
    asm volatile("incb %0" : "+q"(value));
    return value;
}

int main()
{
    volatile uint8_t mem = 0x00;

    check(decw_reg(0x8001) == 0x8000);
    check(decw_reg(0x0000) == 0xffff);
    check(incw_reg(0x7fff) == 0x8000);
    check(decw_keeps_high_word(0x12340000) == 0x1234ffff);
    check(incw_keeps_high_word(0x1234ffff) == 0x12340000);

    check(decb_reg(0x81) == 0x80);
    check(decb_reg(0x00) == 0xff);
    check(incb_reg(0x7f) == 0x80);

    asm volatile("incb %0" : "+m"(mem));
    check(mem == 0x01);
    asm volatile("decb %0" : "+m"(mem));
    check(mem == 0x00);

    return 0;
}
