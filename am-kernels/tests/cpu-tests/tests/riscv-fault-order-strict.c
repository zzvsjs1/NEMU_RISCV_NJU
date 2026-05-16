#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 32

#include <stdint.h>

volatile uint32_t fault_order_saved_mcause = 0;
volatile uint32_t fault_order_saved_mtval = 0;
volatile uint32_t fault_order_restore_mtvec = 0;

asm(
    ".section .text\n"
    ".option push\n"
    ".option norvc\n"
    ".align 2\n"
    ".globl fault_order_handler\n"
    "fault_order_handler:\n"
    "  csrr t1, mcause\n"
    "  la t0, fault_order_saved_mcause\n"
    "  sw t1, 0(t0)\n"
    "  csrr t1, mtval\n"
    "  la t0, fault_order_saved_mtval\n"
    "  sw t1, 0(t0)\n"
    "  la t0, fault_order_restore_mtvec\n"
    "  lw t1, 0(t0)\n"
    "  csrw mtvec, t1\n"
    "  csrr t0, mepc\n"
    "  addi t0, t0, 4\n"
    "  csrw mepc, t0\n"
    "  mret\n"
    ".option pop\n");

extern void fault_order_handler(void);

static volatile uint32_t fault_order_word __attribute__((aligned(4))) = 0x11223344u;

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

static void prepare_fault_order_trap(void)
{
    fault_order_saved_mcause = 0xffffffffu;
    fault_order_saved_mtval = 0xffffffffu;
    fault_order_restore_mtvec = read_mtvec();
    write_mtvec((uintptr_t)fault_order_handler);
}

static void test_load_to_x0_still_raises_exception(void)
{
    /*
     * The unprivileged ISA says loads to x0 must still raise exceptions and
     * cause other side effects. A misaligned LW is a compact way to check the
     * exception side without depending on any particular device.
     */
    uintptr_t bad = ((uintptr_t)&fault_order_word) + 1u;

    prepare_fault_order_trap();
    asm volatile("lw x0, 0(%0)" : : "r"(bad) : "memory");

    check(fault_order_saved_mcause == 4u);
    check(fault_order_saved_mtval == bad);
}

static void test_faulting_load_does_not_write_destination(void)
{
    /*
     * Synchronous exceptions happen before the faulting instruction commits its
     * destination register. The handler only uses t0/t1, so s1 can carry a
     * sentinel across the trap and prove that the LW did not partially commit.
     */
    uintptr_t bad = ((uintptr_t)&fault_order_word) + 1u;
    uintptr_t after = 0;

    prepare_fault_order_trap();
    asm volatile(
        "li s1, 0x13579\n"
        "lw s1, 0(%[bad])\n"
        "mv %[after], s1\n"
        : [after] "=&r"(after)
        : [bad] "r"(bad)
        : "s1", "memory");

    check(fault_order_saved_mcause == 4u);
    check(fault_order_saved_mtval == bad);
    check(after == 0x13579u);
}

static void test_faulting_store_does_not_write_memory(void)
{
    /*
     * The visible misaligned-store trap must be raised before memory is changed.
     * The word stays byte-for-byte unchanged after the handler skips the store.
     */
    uintptr_t bad = ((uintptr_t)&fault_order_word) + 1u;

    fault_order_word = 0x11223344u;
    prepare_fault_order_trap();
    asm volatile("sw %[value], 0(%[bad])"
                 :
                 : [bad] "r"(bad), [value] "r"(0xaabbccddu)
                 : "memory");

    check(fault_order_saved_mcause == 6u);
    check(fault_order_saved_mtval == bad);
    check(fault_order_word == 0x11223344u);
}

static void test_jalr_rd_equal_rs1_traps_before_link_write(void)
{
    /*
     * JALR calculates rs1+imm, clears bit 0, checks instruction alignment, and
     * only then writes pc+4 to rd. When rd == rs1, a misaligned target must leave
     * the original source register visible after the trap handler returns.
     */
    uintptr_t bad = 0;
    uintptr_t before = 0;
    uintptr_t after = 0;

    prepare_fault_order_trap();
    asm volatile(
        "la s2, 1f\n"
        "addi s2, s2, 2\n"
        "mv %[bad], s2\n"
        "mv %[before], s2\n"
        "jalr s2, 0(s2)\n"
        "1:\n"
        "mv %[after], s2\n"
        : [bad] "=&r"(bad), [before] "=&r"(before), [after] "=&r"(after)
        :
        : "s2", "memory");

    check(fault_order_saved_mcause == 0u);
    check(fault_order_saved_mtval == bad);
    check(after == before);
}

#endif

int main(void)
{
#if defined(__riscv) && __riscv_xlen == 32
    test_load_to_x0_still_raises_exception();
    test_faulting_load_does_not_write_destination();
    test_faulting_store_does_not_write_memory();
    test_jalr_rd_equal_rs1_traps_before_link_write();
#endif

    return 0;
}
