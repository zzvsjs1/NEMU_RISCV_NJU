#include <isa.h>

// Define bit‐positions and widths for the fields we care about.

#define MSTATUS_MIE_BIT       3       // Machine‑level interrupt enable
#define MSTATUS_MPIE_BIT      7       // Machine‑level previous interrupt enable
#define MSTATUS_MPP_SHIFT     11      // Machine‑level previous privilege (2 bits)
#define MSTATUS_MPP_WIDTH     2
#define MSTATUS_MPV_SHIFT     17      // H‑extension: previous virtualization mode (1 bit)
#define MSTATUS_MPV_WIDTH     1
#define MSTATUS_GVA_BIT       18      // H‑extension: guest VA valid on trap

static inline uint64_t get_bit(uint64_t csr, unsigned bit)
{
    return (csr >> bit) & 1ULL;
}

static inline uint64_t set_bit(uint64_t csr, unsigned int bit, bool v)
{
    if (v)
    {
        return csr | (1ULL << bit);
    }
    else
    {
        return csr & ~(1ULL << bit);
    }
}

static inline uint64_t get_field(uint64_t csr, uint64_t shift, uint64_t width)
{
    // If width==0 or shift>=64, no bits to extract.
    Assert(width > 0 || shift < 64, "get_field: width == 0 || shift >= 64\n");

    // If width+shift >= 64, we want all bits from 'shift'...63
    uint64_t mask;

    if (width >= 64 - shift) 
    {
        // e.g., shift=4 → mask = 0x0FFFFFFFFFFFFFFF
        mask = ~0ULL >> shift;
    } 
    else 
    {
        mask = (1ULL << width) - 1;
    }

    return (csr >> shift) & mask;
}

static inline uint64_t set_field(
    uint64_t csr,
    uint64_t shift,
    uint64_t width,
    uint64_t value)
{
    Assert(width > 0 || shift < 64, "set_field: width == 0 || shift >= 64\n");

    // Build the field mask at its shifted position
    uint64_t field_mask;
    if (width >= 64 - shift) 
    {
        // e.g., shift=60 → mask = 0xF000000000000000
        field_mask = ~0ULL << shift;
    } 
    else 
    {
        field_mask = ((1ULL << width) - 1) << shift;
    }

    // Mask off any high bits in value beyond width.
    uint64_t value_mask = (width >= 64 - shift)
        // full-width                     
        ? (~0ULL >> shift)
        : ((1ULL << width) - 1);

    value &= value_mask;

    // Clear the field in csr, then OR in the shifted value
    csr &= ~field_mask;
    csr |= (value << shift) & field_mask;
    return csr;
}

static void updateMstatus()
{
    // 18.6.2. Trap Entry
    // When a trap is taken into M-mode, virtualization mode V gets set to 0, and fields MPV and MPP in
    // mstatus (or mstatush) are set according to Table 35. A trap into M-mode also writes fields GVA, MPIE,
    // and MIE in mstatus/mstatush and writes CSRs mepc, mcause, mtval, mtval2, and mtinst.

    // 1) Read the old mstatus
    uint64_t s = cpu.csr.mstatus;

    // 2) Save old MIE into MPIE
    s = set_bit(s, MSTATUS_MPIE_BIT, get_bit(cpu.csr.mstatus, MSTATUS_MIE_BIT));

    // 3) Save current priv into MPP
    // 0x3 mean Machine, will change later.....
    s = set_field(s, MSTATUS_MPP_SHIFT, MSTATUS_MPP_WIDTH, 0x3);

    // 4) Disable interrupts (clear MIE)
    s = set_bit(s, MSTATUS_MIE_BIT, false);

    // 5) Save prior virtualization‐mode V into MPV.
    // Not support.
    s = set_field(s, MSTATUS_MPV_SHIFT, MSTATUS_MPV_WIDTH, 0);

    // 6) Record whether the trap had a guest VA
    // Always 0.
    s = set_bit(s, MSTATUS_GVA_BIT, false);

    // 7) Write it back
    cpu.csr.mstatus = s;
}

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

    // bool interrupt = (NO & (1 << (32 - 1))) != 0;

    cpu.csr.mepc = epc;

    // We don't check is interrupt now.
    // TODO: May need interrupt support.
    cpu.csr.mcause = NO;

// Skip current instruction to avoid Spilke advance one more step.
#ifdef CONFIG_DIFFTEST
    // extern void (*ref_difftest_regcpy)(void *dut, bool direction);
    // CPU_state ref_r;
    // ref_difftest_regcpy(&ref_r, 0);

    extern void (*ref_difftest_raise_intr)(uint64_t NO);
    ref_difftest_raise_intr(NO);
#endif

    // Update mstatus.
    updateMstatus();

    // 3.1.7. Machine Trap-Vector Base-Address Register (mtvec)
    // 
    // When MODE=Direct, all traps into machine
    // mode cause the pc to be set to the address in the BASE field. When MODE=Vectored, all synchronous
    // exceptions into machine mode cause the pc to be set to the address in the BASE field, whereas
    // interrupts cause the pc to be set to the address in the BASE field plus four times the interrupt cause
    // number. For example, a machine-mode timer interrupt (see Table 14) causes the pc to be set to
    // BASE+0x1c.
    // pc = (mtvec & ~0xF) + (I * 4)
    word_t mtvec = cpu.csr.mtvec;
    word_t mode = mtvec & 0x3;
    word_t base = mtvec & ~0x3UL;

    // vectored: branch to base + 4*cause
    if (mode == 1) 
    {
        return base + (NO * 4);
    }

    // direct
    return base;
}

word_t isa_query_intr() 
{
    return INTR_EMPTY;
}
