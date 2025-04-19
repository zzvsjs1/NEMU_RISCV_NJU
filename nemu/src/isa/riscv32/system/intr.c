#include <isa.h>

word_t isa_raise_intr(word_t NO, vaddr_t epc) 
{
    /* Trigger an interrupt/exception with ``NO''.
    * Then return the address of the interrupt/exception vector.
    */

#ifdef CONFIG_E_TRACER
    printf(ANSI_FMT(
        "Exception mcause: " FMT_WORD "  Exception mepc: " FMT_WORD "  Exception mtvec: " FMT_WORD "\n",
        ANSI_FG_YELLOW
    ),
        NO,
        epc,
        cpu.csr.mtvec
    );
#endif

    cpu.csr.mepc = epc;
    cpu.csr.mcause = NO;
    return cpu.csr.mtvec;
}

word_t isa_query_intr() 
{
    return INTR_EMPTY;
}
