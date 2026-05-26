#include "trap.h"

static uint32_t divb_eax(uint32_t eax, uint8_t divisor)
{
    asm volatile("divb %1" : "+a"(eax) : "q"(divisor) : "cc");
    return eax;
}

static void divw_edx_eax(uint32_t *edx, uint32_t *eax, uint16_t divisor)
{
    asm volatile("divw %2" : "+d"(*edx), "+a"(*eax) : "r"(divisor) : "cc");
}

int main()
{
    uint32_t eax = 0x12340081;
    uint32_t edx = 0x56780000;

    // divb divides AX by the byte operand, then stores quotient in AL and
    // remainder in AH.  The upper half of EAX is not part of the byte operation.
    check(divb_eax(0x12340080, 0x81) == 0x12348000);
    check(divb_eax(0x12340081, 0x02) == 0x12340140);
    check(divb_eax(0x123400ff, 0x7f) == 0x12340102);

    divw_edx_eax(&edx, &eax, 2);
    check(eax == 0x12340040);
    check(edx == 0x56780001);

    return 0;
}
