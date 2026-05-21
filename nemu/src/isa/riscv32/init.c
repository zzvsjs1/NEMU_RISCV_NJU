#include <isa.h>
#include <memory/paddr.h>
#include "local-include/reg.h"

// this is not consistent with uint8_t
// but it is ok since we do not access the array directly
static const uint32_t img[] = {
#ifdef CONFIG_RV64
    0x00000297, // auipc t0,0
    0x0002b823, // sd  zero,16(t0)
    0x0102b503, // ld  a0,16(t0)
    0x0000006b, // nemu_trap
    0xdeadbeef, // some data
#else
    /*
     * The built-in image is a minimal smoke test used when no guest binary is
     * supplied.  It touches RESET_VECTOR-backed memory, loads the value back,
     * then exits through the NEMU private trap instruction.
     */
    0x800002b7, // lui t0,0x80000
    0x0002a023, // sw  zero,0(t0)
    0x0002a503, // lw  a0,0(t0)
    0x0000006b, // nemu_trap
#endif
};

/*
 * Reset architectural RISC-V state to the power-on values used by NEMU.  Guest
 * memory is loaded separately; this function only prepares pc, GPR/CSR state,
 * privilege mode, and the pending-interrupt latch.
 */
static void restart()
{
    /* Set the initial program counter. */
    cpu.pc = RESET_VECTOR;

    /* The zero register is always 0. */
    gpr(0) = 0;

    cpu.csr.satp = 0;
    cpu.csr.mstatus = riscv_mstatus_normalise(0);
    cpu.csr.mtvec = 0;
    cpu.csr.mscratch = 0;
    cpu.csr.mepc = 0;
    cpu.csr.mcause = 0;
    cpu.csr.mtval = 0;

    /* NEMU starts bare-metal RISC-V code in machine mode. */
    cpu.prvi = RISCV_PRIV_M;
    cpu.INTR = false;
}

/*
 * Install the built-in image at RESET_VECTOR, then initialise the CPU state.
 * A real guest binary loaded by the monitor overwrites this image before run.
 */
void init_isa()
{
    /* Load built-in image. */
    memcpy(guest_to_host(RESET_VECTOR), img, sizeof(img));

    /* Initialize this virtual computer system. */
    restart();
}
