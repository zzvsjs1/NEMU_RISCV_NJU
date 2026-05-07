#include <isa.h>

void dev_raise_intr() 
{
    /*
     * Device interrupts are edge notifications into the CPU loop.  The ISA side
     * decides whether mstatus currently allows the interrupt and consumes this
     * pending bit in isa_query_intr().
     */
    cpu.INTR = true;
}
