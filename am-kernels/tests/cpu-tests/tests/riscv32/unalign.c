#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 32

#include <stdint.h>

volatile uint32_t unalign_saved_mcause = 0;
volatile uint32_t unalign_saved_mtval = 0;
volatile uint32_t unalign_restore_mtvec = 0;

asm(
    ".section .text\n"
    ".option push\n"
    ".option norvc\n"
    ".align 2\n"
    ".globl unalign_trap_handler\n"
    "unalign_trap_handler:\n"
    "  csrr t1, mcause\n"
    "  la t0, unalign_saved_mcause\n"
    "  sw t1, 0(t0)\n"
    "  csrr t1, mtval\n"
    "  la t0, unalign_saved_mtval\n"
    "  sw t1, 0(t0)\n"
    "  la t0, unalign_restore_mtvec\n"
    "  lw t1, 0(t0)\n"
    "  csrw mtvec, t1\n"
    "  csrr t0, mepc\n"
    "  addi t0, t0, 4\n"
    "  csrw mepc, t0\n"
    "  mret\n"
    ".option pop\n");

extern void unalign_trap_handler(void);

static inline uintptr_t read_mtvec(void)
{
    uintptr_t v;
    asm volatile("csrr %0, mtvec" : "=r"(v));
    return v;
}

static inline void write_mtvec(uintptr_t v)
{
    asm volatile("csrw mtvec, %0" : : "r"(v) : "memory");
}

volatile unsigned x = 0xffffffff;
volatile unsigned char buf[16];

int main()
{
    uintptr_t bad = (uintptr_t)(buf + 3);

    unalign_restore_mtvec = read_mtvec();
    write_mtvec((uintptr_t)unalign_trap_handler);
    asm volatile("sw t0, 0(%0)" : : "r"(bad) : "t0", "memory");
    check(unalign_saved_mcause == 6u);
    check(unalign_saved_mtval == bad);

    unalign_restore_mtvec = read_mtvec();
    write_mtvec((uintptr_t)unalign_trap_handler);
    asm volatile("lw t0, 0(%0)" : : "r"(bad) : "t0", "memory");
    check(unalign_saved_mcause == 4u);
    check(unalign_saved_mtval == bad);

    return 0;
}

#else

volatile unsigned x = 0xffffffff;
volatile unsigned char buf[16];

int main()
{
    for (int i = 0; i < 4; i++)
    {
        *((volatile unsigned *)(buf + 3)) = 0xaabbccdd;

        x = *((volatile unsigned *)(buf + 3));
        check(x == 0xaabbccdd);

        buf[0] = buf[1] = 0;
    }

    return 0;
}

#endif
