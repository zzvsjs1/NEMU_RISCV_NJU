#include <utils.h>

#ifdef CONFIG_ISA_mips32
#ifdef CONFIG_E_TRACER

void etrace_exception(word_t no, vaddr_t epc, vaddr_t vector, word_t cause, word_t status)
{
    /*
     * Keep each transition on one stable, machine-readable line.  In
     * particular, the exception number is decimal because it maps directly to
     * the MIPS ExcCode field, while architectural addresses and registers are
     * easier to compare with a manual or waveform in hexadecimal.
     */
    _Log("etrace: exception=" FMT_DECIMAL_WORD " epc=" FMT_WORD " vector=" FMT_WORD " cause=" FMT_WORD " status=" FMT_WORD "\n", no, epc, vector,
         cause, status);
}

void etrace_eret(vaddr_t epc, word_t status)
{
    _Log("etrace: eret epc=" FMT_WORD " status=" FMT_WORD "\n", epc, status);
}

#else

void etrace_exception(word_t no, vaddr_t epc, vaddr_t vector, word_t cause, word_t status)
{
    (void)no;
    (void)epc;
    (void)vector;
    (void)cause;
    (void)status;
}

void etrace_eret(vaddr_t epc, word_t status)
{
    (void)epc;
    (void)status;
}

#endif
#endif
