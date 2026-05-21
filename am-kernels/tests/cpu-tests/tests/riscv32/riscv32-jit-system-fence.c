#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 32

#include <stdint.h>

#define MSTATUS_MPIE (1u << 7)
#define MSTATUS_MPP_MASK (3u << 11)
#define MSTATUS_MPP_S (1u << 11)

/*
 * Execute fence-like SYSTEM/MISC-MEM instructions as raw words.  The RV32 test
 * build does not advertise every privileged mnemonic to the assembler, but the
 * emulator still needs to decode the architectural encodings that RV64 already
 * accepts.
 */
static uint32_t run_misc_mem_and_wfi(uint32_t value)
{
    uint32_t out = value;

    asm volatile(
        "addi %[out], %[out], 1\n"
        ".word 0x0000000f\n" /* fence */
        ".word 0x0000100f\n" /* fence.i */
        "addi %[out], %[out], 1\n"
        ".word 0x10500073\n" /* wfi */
        "addi %[out], %[out], 1\n"
        : [out] "+r"(out)
        :
        : "memory");

    return out;
}

static void enter_supervisor_and_sfence(void)
{
    uintptr_t mstatus;

    /*
     * SFENCE.VMA is legal in M-mode and in S-mode when mstatus.TVM is clear.
     * Enter S-mode so this catches the supervisor decode path, not just the
     * always-privileged M-mode case.
     */
    asm volatile(
        "csrr %[mstatus], mstatus\n"
        "li t0, %[mpp_mask]\n"
        "not t0, t0\n"
        "and %[mstatus], %[mstatus], t0\n"
        "li t0, %[mpp_s]\n"
        "or %[mstatus], %[mstatus], t0\n"
        "ori %[mstatus], %[mstatus], %[mpie]\n"
        "csrw mstatus, %[mstatus]\n"
        "la t0, 1f\n"
        "csrw mepc, t0\n"
        "mret\n"
        "1:\n"
        ".word 0x12000073\n" /* sfence.vma x0, x0 */
        : [mstatus] "=&r"(mstatus)
        : [mpp_mask] "i"(MSTATUS_MPP_MASK),
          [mpp_s] "i"(MSTATUS_MPP_S),
          [mpie] "i"(MSTATUS_MPIE)
        : "t0", "memory");
}

#endif

int main(void)
{
#if defined(__riscv) && __riscv_xlen == 32
    check(run_misc_mem_and_wfi(4) == 7);
    enter_supervisor_and_sfence();
#endif

    return 0;
}
