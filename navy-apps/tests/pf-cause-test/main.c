#include <stdint.h>

#define PF_CAUSE_TEST_ADDR ((volatile uint8_t *)(uintptr_t)0x50000000u)

int main(void)
{
    *PF_CAUSE_TEST_ADDR = 0x5a;
    return 1;
}
