#include <isa.h>
#include <memory/paddr.h>

// this is not consistent with uint8_t
// but it is ok since we do not access the array directly
static const uint32_t img[] = {
    0x00000297, // auipc t0,0
    0x0002b823, // sd  zero,16(t0)
    0x0102b503, // ld  a0,16(t0)
    0x0000006b, // nemu_trap
    0xdeadbeef, // some data
};

static void restart()
{
    /* Set the initial program counter. */
    cpu.pc = RESET_VECTOR;

    /* The zero register is always 0. */
    cpu.gpr[0]._64 = 0;

    cpu.csr.satp = 0;
    cpu.csr.mstatus = riscv64_mstatus_normalise(0);
    cpu.csr.mtvec = 0;
    cpu.csr.mscratch = 0;
    cpu.csr.mepc = 0;
    cpu.csr.mcause = 0;
    cpu.csr.mtval = 0;

    /* NEMU starts RV64 bare-metal code in machine mode. */
    cpu.prvi = RISCV64_PRIV_M;
    cpu.INTR = false;
}

void init_isa()
{
    /* Load built-in image. */
    memcpy(guest_to_host(RESET_VECTOR), img, sizeof(img));

    /* Initialize this virtual computer system. */
    restart();
}
