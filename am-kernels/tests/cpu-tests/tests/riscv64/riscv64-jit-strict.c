#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 64

#include <stdint.h>

static uint64_t data64[] = {
    0x0000000000000080ull,
    0x0000000080000000ull,
    0xffffffffffffffffull,
    0x0123456789abcdefull,
};

/* Execute LD directly so the test observes RV64 sign and width behaviour. */
static uint64_t op_ld(const void *p)
{
    uint64_t v;
    asm volatile("ld %0, 0(%1)" : "=r"(v) : "r"(p) : "memory");
    return v;
}

/* Execute LWU directly; the upper XLEN bits must be zero-filled. */
static uint64_t op_lwu(const void *p)
{
    uint64_t v;
    asm volatile("lwu %0, 0(%1)" : "=r"(v) : "r"(p) : "memory");
    return v;
}

/* Exercise ADDIW, which must sign-extend its 32-bit result on RV64. */
static uint64_t op_addiw(uint64_t a)
{
    uint64_t v;
    asm volatile("addiw %0, %1, 1" : "=r"(v) : "r"(a));
    return v;
}

/* Exercise SRAW, whose 32-bit arithmetic result is sign-extended to XLEN. */
static uint64_t op_sraw(uint64_t a, uint64_t b)
{
    uint64_t v;
    asm volatile("sraw %0, %1, %2" : "=r"(v) : "r"(a), "r"(b));
    return v;
}

/* Check the architectural signed-divide overflow case. */
static uint64_t op_div_edge(uint64_t a, uint64_t b)
{
    uint64_t v;
    asm volatile("div %0, %1, %2" : "=r"(v) : "r"(a), "r"(b));
    return v;
}

/* Check REMW by zero, including the required 32-bit sign extension. */
static uint64_t op_remw_zero(uint64_t a)
{
    uint64_t v;
    asm volatile("remw %0, %1, zero" : "=r"(v) : "r"(a));
    return v;
}

/* Verify a taken branch skips the fall-through write. */
static uint64_t taken_branch_value(void)
{
    uint64_t v = 1;

    asm volatile(
        "beq %[v], %[v], 1f\n"
        "li %[v], 2\n"
        "1:\n"
        : [v] "+r"(v)
        :
        : "memory");
    return v;
}

/* Verify JALR clears bit zero of the computed target address. */
static uint64_t jalr_low_bit_value(uint64_t marker)
{
    uint64_t out = 0;

    asm volatile(
        "la t0, 1f\n"
        "ori t0, t0, 1\n"
        "jalr zero, 0(t0)\n"
        "li %[out], 0\n"
        "j 2f\n"
        "1:\n"
        "mv %[out], %[marker]\n"
        "2:\n"
        : [out] "=&r"(out)
        : [marker] "r"(marker)
        : "t0", "memory");
    return out;
}

/* Group RV64 data-path edge cases that the JIT must not weaken. */
static void test_rv64_jit_data_ops(void)
{
    uint32_t *word = (uint32_t *)&data64[1];

    check(op_ld(&data64[3]) == 0x0123456789abcdefull);
    check(op_lwu(word) == 0x0000000080000000ull);
    check(op_addiw(0x000000007fffffffull) == 0xffffffff80000000ull);
    check(op_sraw(0x0000000080000000ull, 4) == 0xfffffffff8000000ull);
    check(op_div_edge(0x8000000000000000ull, 0xffffffffffffffffull) == 0x8000000000000000ull);
    check(op_remw_zero(0x0000000080001234ull) == 0xffffffff80001234ull);
}

/* Group control-flow edge cases around branches and interpreter fallback. */
static void test_rv64_jit_control_ops(void)
{
    check(taken_branch_value() == 1);
    check(jalr_low_bit_value(0x123456789abcdef0ull) == 0x123456789abcdef0ull);
}

#endif

/* Run the RV64-only checks while keeping the source buildable for other ISAs. */
int main(void)
{
#if defined(__riscv) && __riscv_xlen == 64
    test_rv64_jit_data_ops();
    test_rv64_jit_control_ops();
#endif

    return 0;
}
