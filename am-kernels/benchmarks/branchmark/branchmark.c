#include <am.h>
#include <klib.h>
#include <stdint.h>

#define BRANCHMARK_ITERS 600000u
#define BRANCHMARK_EXPECTED 0x67c146ecu

/* Read AM uptime in microseconds for guest-visible benchmark timing. */
static uint64_t uptime_us(void)
{
    return io_read(AM_TIMER_UPTIME).us;
}

/*
 * Run a register-only RV64 hot loop with one backwards conditional branch.
 * The instruction mix is deliberately limited to the first RV64 JIT subset:
 * integer ALU operations, shifts, immediates and B-type branches.  Keeping
 * memory out of the loop lets the benchmark measure the branch/ALU translator
 * instead of interpreter fallbacks for loads, stores or devices.
 */
__attribute__((noinline)) static uint64_t branch_hot_loop(uint64_t iters)
{
    uint64_t result;

    asm volatile(
        "mv t0, %[iters]\n"
        "li t1, 0x0123456789abcdef\n"
        "li t2, 0xfedcba9876543210\n"
        "li t3, 0x0f0f0f0f0f0f0f0f\n"
        "li t4, 0x13579bdf2468ace0\n"
        "li t5, 0x9e3779b97f4a7c15\n"
        "1:\n"
        "addi t0, t0, -1\n"
        "add t1, t1, t2\n"
        "xor t2, t2, t3\n"
        "or t3, t3, t1\n"
        "and t4, t4, t3\n"
        "slli t5, t5, 7\n"
        "xor t5, t5, t1\n"
        "srli t3, t3, 3\n"
        "srai t4, t4, 1\n"
        "add t3, t3, t4\n"
        "xori t1, t1, 0x5a5\n"
        "ori t2, t2, 0x123\n"
        "andi t3, t3, -33\n"
        "slli t4, t4, 5\n"
        "xor t5, t5, t2\n"
        "add t2, t2, t4\n"
        "sub t1, t1, t5\n"
        "sltu t6, t2, t1\n"
        "add t5, t5, t6\n"
        "bnez t0, 1b\n"
        "xor t1, t1, t2\n"
        "xor t1, t1, t3\n"
        "xor t1, t1, t4\n"
        "xor %[result], t1, t5\n"
        : [result] "=r"(result)
        : [iters] "r"(iters)
        : "t0", "t1", "t2", "t3", "t4", "t5", "t6", "memory");

    return result;
}

/* Run BranchMark and print a compact machine-readable result line. */
int main(const char *args)
{
    (void)args;
    ioe_init();

    printf("======= Running BranchMark =======\n");

    uint64_t start = uptime_us();
    uint64_t result = branch_hot_loop(BRANCHMARK_ITERS);
    uint64_t end = uptime_us();

    uint32_t checksum = (uint32_t)(result ^ (result >> 32) ^ 0x6d2b79f5u);
    int elapsed_us = (int)(end - start);
    int pass = checksum == BRANCHMARK_EXPECTED;

    printf("branchmark_total_us: %d\n", elapsed_us);
    printf("branchmark_checksum: 0x%x\n", checksum);
    printf("BranchMark %s\n", pass ? "PASS" : "FAIL");

    return pass ? 0 : 1;
}
