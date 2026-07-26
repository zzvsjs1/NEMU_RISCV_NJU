#include <isa.h>

/* Extract one mstatus bit as a boolean-sized integer. */
static inline uint64_t get_bit(uint64_t csr, unsigned bit)
{
    return (csr >> bit) & 1ull;
}

/* Return a CSR value with one bit set or cleared, leaving all other bits intact. */
static inline uint64_t set_bit(uint64_t csr, unsigned bit, bool value)
{
    if (value)
    {
        return csr | (1ull << bit);
    }

    return csr & ~(1ull << bit);
}

/*
 * Replace a contiguous bit field inside a CSR.  value is masked to the field
 * width before insertion, so callers can pass the raw privilege value directly.
 */
static inline uint64_t set_field(uint64_t csr, uint64_t shift, uint64_t width, uint64_t value)
{
    Assert(width > 0 && shift < 64, "set_field: invalid field shift=%lu width=%lu", shift, width);

    const uint64_t value_mask = (width == 64) ? ~0ull : ((1ull << width) - 1ull);
    const uint64_t field_mask = value_mask << shift;

    csr &= ~field_mask;
    csr |= (value & value_mask) << shift;
    return csr;
}

/*
 * Apply the RISC-V trap-entry mstatus rules.  MIE is saved into MPIE, the
 * interrupted privilege is saved into MPP, global machine interrupts are
 * disabled, and execution enters M-mode.
 */
static void update_mstatus_on_trap_entry(void)
{
    /*
     * Trap entry snapshots the interrupted privilege and interrupt-enable state
     * into mstatus before control enters the machine trap vector.
     */
    uint64_t status = cpu.csr.mstatus;

    status = set_bit(status, RISCV_MSTATUS_MPIE_BIT,
                     get_bit(status, RISCV_MSTATUS_MIE_BIT));
    status = set_field(status, RISCV_MSTATUS_MPP_SHIFT,
                       RISCV_MSTATUS_MPP_WIDTH, cpu.prvi);
    status = set_bit(status, RISCV_MSTATUS_MIE_BIT, false);
#ifdef CONFIG_RV64
    /*
     * GVA tells a hypervisor trap handler that mtval contains a guest virtual
     * address.  NEMU does not implement the hypervisor extension or guest
     * translation, so every locally generated trap clears it.  RV32 would hold
     * this high field in mstatush, which this shared CPU state does not model.
     */
    status = set_bit(status, RISCV64_MSTATUS_GVA_BIT, false);
#endif

    cpu.csr.mstatus = riscv_mstatus_normalise(status);
    cpu.prvi = RISCV_PRIV_M;
}

/*
 * Raise an exception or interrupt and return the trap-vector target.  tval is
 * the instruction, address, or zero value that the specific trap type wants to
 * expose through mtval.
 */
word_t isa_raise_intr_tval(word_t NO, vaddr_t epc, word_t tval)
{
    cpu.csr.mepc = epc;
    cpu.csr.mcause = NO;
    cpu.csr.mtval = tval;

#ifdef CONFIG_DIFFTEST
    extern void (*ref_difftest_raise_intr)(uint64_t NO);
    ref_difftest_raise_intr(NO);
#endif

    update_mstatus_on_trap_entry();

    const word_t mtvec = cpu.csr.mtvec;
    /*
     * Remove MODE to recover the aligned BASE before applying Vectored mode.
     * The interrupt marker in mcause is not part of the cause number, so it is
     * stripped before the cause selects a four-byte vector-table entry.
     */
    const word_t mode = mtvec & RISCV_MTVEC_MODE_MASK;
    const word_t base = mtvec & ~(word_t)RISCV_MTVEC_MODE_MASK;
    const word_t interrupt_mask = RISCV_XCAUSE_INTERRUPT_MASK;
    const bool is_interrupt = (NO & interrupt_mask) != 0;
    const word_t cause = NO & ~interrupt_mask;

    if (mode == RISCV_MTVEC_MODE_VECTORED && is_interrupt)
    {
        return base + cause * RISCV_MTVEC_VECTOR_ENTRY_BYTES;
    }

    return base;
}

/* Raise a trap with no mtval payload. */
word_t isa_raise_intr(word_t NO, vaddr_t epc)
{
    return isa_raise_intr_tval(NO, epc, 0);
}

/*
 * Report a pending timer interrupt only when the implementation latch is set
 * and the currently modelled MIE gate allows delivery.  cpu.INTR is not the
 * architectural mip CSR, and this legacy path does not yet model the
 * privilege-dependent global-enable rule or delegation.  Consuming the latch
 * here prevents one device event from being delivered repeatedly.
 */
word_t isa_query_intr()
{
    if (cpu.INTR && (cpu.csr.mstatus & RISCV_MSTATUS_MIE) != 0)
    {
        cpu.INTR = false;
        return RISCV_MCAUSE_MACHINE_TIMER_INTERRUPT;
    }

    return INTR_EMPTY;
}
