#include <isa.h>

#define MSTATUS_MIE_BIT 3
#define MSTATUS_MPIE_BIT 7
#define MSTATUS_MPP_SHIFT 11
#define MSTATUS_MPP_WIDTH 2
#define MSTATUS_MPRV_BIT 17
/*
 * In RV64 mstatus, bit 18 is SUM.  The hypervisor GVA field is in the upper
 * status layout, so clearing bit 18 on trap entry would incorrectly change
 * supervisor data-access permissions.
 */
#define MSTATUS_GVA_BIT 38

#define IRQ_TIMER ((word_t)0x8000000000000007ull)

static inline uint64_t get_bit(uint64_t csr, unsigned bit)
{
    return (csr >> bit) & 1ull;
}

static inline uint64_t set_bit(uint64_t csr, unsigned bit, bool value)
{
    if (value)
    {
        return csr | (1ull << bit);
    }

    return csr & ~(1ull << bit);
}

static inline uint64_t set_field(uint64_t csr, uint64_t shift, uint64_t width, uint64_t value)
{
    Assert(width > 0 && shift < 64, "set_field: invalid field shift=%lu width=%lu", shift, width);

    const uint64_t value_mask = (width == 64) ? ~0ull : ((1ull << width) - 1ull);
    const uint64_t field_mask = value_mask << shift;

    csr &= ~field_mask;
    csr |= (value & value_mask) << shift;
    return csr;
}

static void update_mstatus_on_trap_entry(void)
{
    /*
     * Trap entry snapshots the interrupted privilege and interrupt-enable state
     * into mstatus before control enters the machine trap vector.
     */
    uint64_t status = cpu.csr.mstatus;

    status = set_bit(status, MSTATUS_MPIE_BIT, get_bit(status, MSTATUS_MIE_BIT));
    status = set_field(status, MSTATUS_MPP_SHIFT, MSTATUS_MPP_WIDTH, cpu.prvi);
    status = set_bit(status, MSTATUS_MIE_BIT, false);
    status = set_bit(status, MSTATUS_GVA_BIT, false);

    cpu.csr.mstatus = riscv64_mstatus_normalise(status);
    cpu.prvi = RISCV64_PRIV_M;
}

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
    const word_t mode = mtvec & 0x3u;
    const word_t base = mtvec & ~(word_t)0x3u;
    const word_t interrupt_mask = (word_t)1ull << 63;
    const bool is_interrupt = (NO & interrupt_mask) != 0;
    const word_t cause = NO & ~interrupt_mask;

    if (mode == 1 && is_interrupt)
    {
        return base + cause * 4u;
    }

    return base;
}

word_t isa_raise_intr(word_t NO, vaddr_t epc)
{
    return isa_raise_intr_tval(NO, epc, 0);
}

word_t isa_query_intr()
{
    if (cpu.INTR && (cpu.csr.mstatus & ((word_t)1u << MSTATUS_MIE_BIT)))
    {
        cpu.INTR = false;
        return IRQ_TIMER;
    }

    return INTR_EMPTY;
}
