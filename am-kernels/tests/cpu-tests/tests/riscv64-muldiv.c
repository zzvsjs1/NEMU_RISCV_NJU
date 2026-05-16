#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 64

#include <stdint.h>

static uint64_t op_mul(uint64_t a, uint64_t b)
{
    uint64_t v;
    asm volatile("mul %0, %1, %2" : "=r"(v) : "r"(a), "r"(b));
    return v;
}

static uint64_t op_mulhu(uint64_t a, uint64_t b)
{
    uint64_t v;
    asm volatile("mulhu %0, %1, %2" : "=r"(v) : "r"(a), "r"(b));
    return v;
}

static uint64_t op_mulhsu(uint64_t a, uint64_t b)
{
    uint64_t v;
    asm volatile("mulhsu %0, %1, %2" : "=r"(v) : "r"(a), "r"(b));
    return v;
}

static uint64_t op_div(uint64_t a, uint64_t b)
{
    uint64_t v;
    asm volatile("div %0, %1, %2" : "=r"(v) : "r"(a), "r"(b));
    return v;
}

static uint64_t op_divu(uint64_t a, uint64_t b)
{
    uint64_t v;
    asm volatile("divu %0, %1, %2" : "=r"(v) : "r"(a), "r"(b));
    return v;
}

static uint64_t op_rem(uint64_t a, uint64_t b)
{
    uint64_t v;
    asm volatile("rem %0, %1, %2" : "=r"(v) : "r"(a), "r"(b));
    return v;
}

static uint64_t op_remu(uint64_t a, uint64_t b)
{
    uint64_t v;
    asm volatile("remu %0, %1, %2" : "=r"(v) : "r"(a), "r"(b));
    return v;
}

static uint64_t op_mulw(uint64_t a, uint64_t b)
{
    uint64_t v;
    asm volatile("mulw %0, %1, %2" : "=r"(v) : "r"(a), "r"(b));
    return v;
}

static uint64_t op_divw(uint64_t a, uint64_t b)
{
    uint64_t v;
    asm volatile("divw %0, %1, %2" : "=r"(v) : "r"(a), "r"(b));
    return v;
}

static uint64_t op_remuw(uint64_t a, uint64_t b)
{
    uint64_t v;
    asm volatile("remuw %0, %1, %2" : "=r"(v) : "r"(a), "r"(b));
    return v;
}

static void test_mul_div(void)
{
    check(op_mul(0xffffffffffffffffull, 3) == 0xfffffffffffffffdull);
    check(op_mulhu(0xffffffffffffffffull, 2) == 1);
    check(op_mulhsu(0xffffffffffffffffull, 2) == 0xffffffffffffffffull);
    check(op_div((uint64_t)(int64_t)-42, (uint64_t)(int64_t)5) == (uint64_t)(int64_t)-8);
    check(op_divu(42, 5) == 8);
    check(op_rem((uint64_t)(int64_t)-42, (uint64_t)(int64_t)5) == (uint64_t)(int64_t)-2);
    check(op_remu(42, 5) == 2);
}

static void test_edge_cases(void)
{
    check(op_div(123, 0) == 0xffffffffffffffffull);
    check(op_divu(123, 0) == 0xffffffffffffffffull);
    check(op_rem(123, 0) == 123);
    check(op_remu(123, 0) == 123);
    check(op_div(0x8000000000000000ull, 0xffffffffffffffffull) == 0x8000000000000000ull);
    check(op_rem(0x8000000000000000ull, 0xffffffffffffffffull) == 0);
}

static void test_word_ops(void)
{
    check(op_mulw(0x0000000080000000ull, 2) == 0);
    check(op_divw(0x0000000080000000ull, 0xffffffffull) == 0xffffffff80000000ull);
    check(op_remuw(0xffffffffull, 10) == 5);
}

#endif

int main(void)
{
#if defined(__riscv) && __riscv_xlen == 64
    test_mul_div();
    test_edge_cases();
    test_word_ops();
#endif

    return 0;
}
