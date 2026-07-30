#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 64

#include <stdint.h>

volatile uint64_t indirect_link_saved_mcause = 0;
volatile uint64_t indirect_link_saved_mtval = 0;
volatile uint64_t indirect_link_restore_mtvec = 0;

/*
 * Preserve the faulting t0 in mscratch so the test can prove that a misaligned
 * JALR traps before writing its aliased destination register.
 */
asm(
    ".section .text\n"
    ".option push\n"
    ".option norvc\n"
    ".align 2\n"
    ".globl indirect_link_trap_handler\n"
    "indirect_link_trap_handler:\n"
    "  csrrw t0, mscratch, t0\n"
    "  csrr t1, mcause\n"
    "  la t0, indirect_link_saved_mcause\n"
    "  sd t1, 0(t0)\n"
    "  csrr t1, mtval\n"
    "  la t0, indirect_link_saved_mtval\n"
    "  sd t1, 0(t0)\n"
    "  la t0, indirect_link_restore_mtvec\n"
    "  ld t1, 0(t0)\n"
    "  csrw mtvec, t1\n"
    "  csrr t0, mepc\n"
    "  addi t0, t0, 4\n"
    "  csrw mepc, t0\n"
    "  csrr t0, mscratch\n"
    "  csrw mscratch, zero\n"
    "  mret\n"
    ".option pop\n");

extern void indirect_link_trap_handler(void);

static inline uintptr_t read_mtvec(void)
{
    uintptr_t value;
    asm volatile("csrr %0, mtvec" : "=r"(value));
    return value;
}

static inline uintptr_t read_mscratch(void)
{
    uintptr_t value;
    asm volatile("csrr %0, mscratch" : "=r"(value));
    return value;
}

static inline void write_mtvec(uintptr_t value)
{
    asm volatile("csrw mtvec, %0" : : "r"(value) : "memory");
}

static inline void write_mscratch(uintptr_t value)
{
    asm volatile("csrw mscratch, %0" : : "r"(value) : "memory");
}

/*
 * Exercise one alternating indirect-JALR site with rd equal to rs1.
 *
 * Each selected target has bit zero set before JALR, so reaching either label
 * proves the architectural low-bit clearing rule. JALR must preserve the old
 * t0 value long enough to calculate the target, then replace t0 with PC + 4.
 * Both targets verify that link before returning through a non-canonical JALR.
 */
static uint64_t run_two_target_indirect_loop(void)
{
    uint64_t sum = 0;
    uint64_t laps = 4096;
    uint64_t bad_link = 0;

    asm volatile(
        ".option push\n"
        ".option norvc\n"
        "1:\n"
        "  andi t1, %[laps], 1\n"
        "  la t0, 4f\n"
        "  beqz t1, 2f\n"
        "  la t0, 5f\n"
        "2:\n"
        "  ori t0, t0, 1\n"
        "  jalr t0, 0(t0)\n"
        "3:\n"
        "  addi %[sum], %[sum], 7\n"
        "  addi %[laps], %[laps], -1\n"
        "  bnez %[laps], 1b\n"
        "  j 6f\n"
        ".balign 4\n"
        "4:\n"
        "  la t1, 3b\n"
        "  bne t0, t1, 7f\n"
        "  addi %[sum], %[sum], 3\n"
        "  jalr zero, 0(t0)\n"
        ".balign 4\n"
        "5:\n"
        "  la t1, 3b\n"
        "  bne t0, t1, 7f\n"
        "  addi %[sum], %[sum], 5\n"
        "  jalr zero, 0(t0)\n"
        "7:\n"
        "  li %[bad_link], 1\n"
        "  j 3b\n"
        "6:\n"
        ".option pop\n"
        : [sum] "+&r"(sum),
          [laps] "+&r"(laps),
          [bad_link] "+&r"(bad_link)
        :
        : "t0", "t1", "memory");

    check(bad_link == 0);
    return sum;
}

static void test_general_indirect_link(void)
{
    /*
     * Each target runs 2,048 times, while the common continuation runs on all
     * 4,096 laps: 2,048 * (3 + 5) + 4,096 * 7 = 45,056.
     */
    check(run_two_target_indirect_loop() == 45056u);
}

/* Verify alignment failure occurs after target masking but before rd is written. */
static void test_misaligned_indirect_link(void)
{
    const uintptr_t old_mtvec = read_mtvec();
    const uintptr_t old_mscratch = read_mscratch();
    uintptr_t expected_target = 0;
    uintptr_t actual_t0 = 0;
    uint64_t reached_target = 0;

    indirect_link_saved_mcause = UINT64_MAX;
    indirect_link_saved_mtval = UINT64_MAX;
    indirect_link_restore_mtvec = old_mtvec;
    write_mscratch(0);
    write_mtvec((uintptr_t)indirect_link_trap_handler);

    asm volatile(
        ".option push\n"
        ".option norvc\n"
        "  la t0, 1f\n"
        "  addi t0, t0, 2\n"
        "  mv %[expected], t0\n"
        "  jalr t0, 0(t0)\n"
        "  mv %[actual], t0\n"
        "  j 2f\n"
        ".balign 4\n"
        "1:\n"
        "  li %[reached], 1\n"
        "2:\n"
        ".option pop\n"
        : [expected] "=&r"(expected_target),
          [actual] "=&r"(actual_t0),
          [reached] "+&r"(reached_target)
        :
        : "t0", "t1", "memory");

    write_mtvec(old_mtvec);
    write_mscratch(old_mscratch);

    check(reached_target == 0);
    check(indirect_link_saved_mcause == 0u);
    check(indirect_link_saved_mtval == expected_target);
    check(actual_t0 == expected_target);
}

#endif

/* Keep the source buildable outside RV64 while exercising the RV64-only path. */
int main(void)
{
#if defined(__riscv) && __riscv_xlen == 64
    test_general_indirect_link();
    test_misaligned_indirect_link();
#endif

    return 0;
}
