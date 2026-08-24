#include <am.h>
#include <klib.h>
#include <stdint.h>

#if __riscv_xlen != 64
#error "MMark requires RV64"
#endif

enum
{
    MMARK_WARMUP_ROUNDS = 1024,
    MMARK_MUL_ROUNDS = 1000000,
    MMARK_DIV_ROUNDS = 250000,
};

/*
 * Both kernels begin at their own loop header. This lets the RV64 JIT keep the
 * loop-carried values in its stable register cache while measuring the native
 * M instructions themselves rather than a C call or repeated dispatch.
 */
asm(".section .text\n"
    ".align 2\n"
    ".option push\n"
    ".option norvc\n"
    ".option arch, +m\n"

    ".globl rv64_mmark_mul_kernel\n"
    ".type rv64_mmark_mul_kernel, @function\n"
    "rv64_mmark_mul_kernel:\n"
    "  mulh a3, a0, a1\n"
    "  mulhsu a4, a0, a1\n"
    "  mulhu a5, a0, a1\n"
    "  mul a0, a0, a1\n"
    "  xor a0, a0, a3\n"
    "  xor a0, a0, a4\n"
    "  xor a0, a0, a5\n"
    "  addi a0, a0, 17\n"
    "  addi a2, a2, -1\n"
    "  bne a2, zero, rv64_mmark_mul_kernel\n"
    "  ret\n"
    ".size rv64_mmark_mul_kernel, .-rv64_mmark_mul_kernel\n"

    ".globl rv64_mmark_div_kernel\n"
    ".type rv64_mmark_div_kernel, @function\n"
    "rv64_mmark_div_kernel:\n"
    "  div a3, a0, a1\n"
    "  rem a4, a0, a1\n"
    "  xor a0, a3, a4\n"
    "  addi a0, a0, 123\n"
    "  divu a3, a0, a1\n"
    "  remu a4, a0, a1\n"
    "  xor a0, a3, a4\n"
    "  addi a0, a0, 77\n"
    "  addi a2, a2, -1\n"
    "  bne a2, zero, rv64_mmark_div_kernel\n"
    "  ret\n"
    ".size rv64_mmark_div_kernel, .-rv64_mmark_div_kernel\n"

    ".option pop\n");

extern uint64_t rv64_mmark_mul_kernel(uint64_t, uint64_t, uint64_t);
extern uint64_t rv64_mmark_div_kernel(uint64_t, uint64_t, uint64_t);

static uint64_t uptime_us(void)
{
    return io_read(AM_TIMER_UPTIME).us;
}

static uint64_t rotate_left_64(uint64_t value, unsigned amount)
{
    return (value << amount) | (value >> (64u - amount));
}

static uint64_t run_mul_kernel(uint64_t rounds)
{
    return rv64_mmark_mul_kernel(UINT64_C(0x123456789abcdef1), UINT64_C(0xfedcba9876543211), rounds);
}

static uint64_t run_div_kernel(uint64_t rounds)
{
    return rv64_mmark_div_kernel(UINT64_C(0x7123456789abcdef), 97, rounds);
}

int main(void)
{
    ioe_init();

    /*
     * Warm both guest PCs before reading the timer. Cold translation and direct
     * link installation are important operational costs, but they would hide
     * the per-operation improvement this benchmark is intended to isolate.
     */
    (void)run_mul_kernel(MMARK_WARMUP_ROUNDS);
    (void)run_div_kernel(MMARK_WARMUP_ROUNDS);

    const uint64_t mul_start = uptime_us();
    const uint64_t mul_result = run_mul_kernel(MMARK_MUL_ROUNDS);
    const uint64_t mul_end = uptime_us();

    const uint64_t div_start = uptime_us();
    const uint64_t div_result = run_div_kernel(MMARK_DIV_ROUNDS);
    const uint64_t div_end = uptime_us();

    const uint64_t checksum = mul_result ^ rotate_left_64(div_result, 17);
    const uint32_t checksum_hi = (uint32_t)(checksum >> 32);
    const uint32_t checksum_lo = (uint32_t)checksum;

    /*
     * The expected halves are filled from an interpreter-calibrated run, then
     * fixed permanently so later JIT changes cannot silently alter semantics.
     */
    const bool pass = checksum_hi == UINT32_C(0xfc0dd612) && checksum_lo == UINT32_C(0xcc32b3f0);

    printf("mmark_mul_us: %d\n", (int)(mul_end - mul_start));
    printf("mmark_div_us: %d\n", (int)(div_end - div_start));
    printf("mmark_total_us: %d\n", (int)((mul_end - mul_start) + (div_end - div_start)));
    printf("mmark_checksum_hi: 0x%x\n", checksum_hi);
    printf("mmark_checksum_lo: 0x%x\n", checksum_lo);
    printf("MMark %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
