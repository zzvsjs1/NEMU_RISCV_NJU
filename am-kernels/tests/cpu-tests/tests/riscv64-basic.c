#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 64

#include <stdint.h>

static uint64_t rv64_basic_data[] = {
    0x000000000000007full,
    0x0000000000000080ull,
    0x000000000000ffffull,
    0x0000000080000000ull,
    0xffffffff80000000ull,
    0xffffffffffffffffull,
};

static uint64_t read_lbu(const void *p)
{
    uint64_t v;
    asm volatile("lbu %0, 0(%1)" : "=r"(v) : "r"(p) : "memory");
    return v;
}

static uint64_t read_lw(const void *p)
{
    uint64_t v;
    asm volatile("lw %0, 0(%1)" : "=r"(v) : "r"(p) : "memory");
    return v;
}

static uint64_t read_lwu(const void *p)
{
    uint64_t v;
    asm volatile("lwu %0, 0(%1)" : "=r"(v) : "r"(p) : "memory");
    return v;
}

static uint64_t addw_value(uint64_t a, uint64_t b)
{
    uint64_t v;
    asm volatile("addw %0, %1, %2" : "=r"(v) : "r"(a), "r"(b));
    return v;
}

static uint64_t slliw_value(uint64_t a, int shamt)
{
    uint64_t v;

    switch (shamt)
    {
    case 31:
        asm volatile("slliw %0, %1, 31" : "=r"(v) : "r"(a));
        return v;
    default:
        asm volatile("slliw %0, %1, 1" : "=r"(v) : "r"(a));
        return v;
    }
}

static uint64_t slli_40(uint64_t a)
{
    uint64_t v;
    asm volatile("slli %0, %1, 40" : "=r"(v) : "r"(a));
    return v;
}

static uint64_t srli_40(uint64_t a)
{
    uint64_t v;
    asm volatile("srli %0, %1, 40" : "=r"(v) : "r"(a));
    return v;
}

static uint64_t srai_40(uint64_t a)
{
    uint64_t v;
    asm volatile("srai %0, %1, 40" : "=r"(v) : "r"(a));
    return v;
}

static uint64_t jalr_clear_low_bit(uint64_t marker)
{
    uint64_t out = 0;

    asm volatile(
        "  la t0, 1f\n"
        "  ori t0, t0, 1\n"
        "  jalr zero, 0(t0)\n"
        "  li %[out], 0\n"
        "  j 2f\n"
        "1:\n"
        "  mv %[out], %[marker]\n"
        "2:\n"
        : [out] "=&r"(out)
        : [marker] "r"(marker)
        : "t0", "memory");

    return out;
}

static void test_load_extension(void)
{
    check(read_lbu((uint8_t *)rv64_basic_data + 1) == 0u);
    check(read_lw(&rv64_basic_data[3]) == 0xffffffff80000000ull);
    check(read_lwu(&rv64_basic_data[3]) == 0x0000000080000000ull);
}

static void test_word_result_extension(void)
{
    check(addw_value(0x7fffffffull, 1) == 0xffffffff80000000ull);
    check(slliw_value(1, 31) == 0xffffffff80000000ull);
    check(slli_40(1) == 0x0000010000000000ull);
    check(srli_40(0x8000000000000000ull) == 0x0000000000800000ull);
    check(srai_40(0x8000000000000000ull) == 0xffffffffff800000ull);
}

static void test_control_transfer(void)
{
    check(jalr_clear_low_bit(0x123456789abcdef0ull) == 0x123456789abcdef0ull);
}

#endif

int main(void)
{
#if defined(__riscv) && __riscv_xlen == 64
    test_load_extension();
    test_word_result_extension();
    test_control_transfer();
#endif

    return 0;
}
