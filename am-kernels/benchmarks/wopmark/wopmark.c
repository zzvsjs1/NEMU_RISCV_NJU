#include <am.h>
#include <klib.h>
#include <stdint.h>

#if __riscv_xlen != 64
#error "WOpMark requires RV64 W-form instructions"
#endif

#define WOPMARK_ROUNDS 20000000u
#define WOPMARK_EXPECTED 0x1fd5ff01u

static uint64_t uptime_us(void)
{
    return io_read(AM_TIMER_UPTIME).us;
}

static uint64_t sext32(uint32_t value)
{
    return (uint64_t)(int64_t)(int32_t)value;
}

static uint64_t addw_raw(uint64_t lhs, uint64_t rhs)
{
    uint64_t out;

    asm volatile("addw %0, %1, %2"
                 : "=r"(out)
                 : "r"(lhs), "r"(rhs));
    return out;
}

static uint64_t mulw_raw(uint64_t lhs, uint64_t rhs)
{
    uint64_t out;

    asm volatile("mulw %0, %1, %2"
                 : "=r"(out)
                 : "r"(lhs), "r"(rhs));
    return out;
}

static bool check_wop_edges(void)
{
    uint64_t value = 0x7fffffffu;
    uint64_t other = 1u;
    uint64_t distinct = addw_raw(value, other);

    asm volatile("addw %0, %0, %1"
                 : "+r"(value)
                 : "r"(other));

    other = 0xffffffffu;
    asm volatile("addw %0, %1, %0"
                 : "+r"(other)
                 : "r"(1u));

    uint64_t product = 0xffffffffu;
    asm volatile("mulw %0, %0, %1"
                 : "+r"(product)
                 : "r"(2u));

    return distinct == sext32(0x80000000u) &&
           value == sext32(0x80000000u) &&
           other == 0 &&
           product == sext32(0xfffffffeu) &&
           mulw_raw(0x80000000u, 1u) == sext32(0x80000000u);
}

static uint32_t run_wop_kernel(void)
{
    uint64_t a = 0x13579bdfu;
    uint64_t b = 0x2468ace1u;
    uint64_t c = 0x10203041u;
    uint64_t d = 0x55667789u;

    for (uint32_t i = 0; i < WOPMARK_ROUNDS; i++)
    {
        /*
         * Keep the hot loop in registers and force the exact W-form mix that is
         * expensive in RV64 fib: two ADDW and two MULW instructions per lap.
         */
        asm volatile("addw %[a], %[a], %[b]\n"
                     "mulw %[b], %[b], %[c]\n"
                     "addw %[c], %[c], %[d]\n"
                     "mulw %[d], %[d], %[a]"
                     : [a] "+r"(a), [b] "+r"(b),
                       [c] "+r"(c), [d] "+r"(d));
    }

    return (uint32_t)(a ^ (b >> 7) ^ (c >> 13) ^ (d >> 19));
}

int main(void)
{
    ioe_init();

    uint64_t start = uptime_us();
    bool edges_pass = check_wop_edges();
    uint32_t checksum = run_wop_kernel();
    uint64_t end = uptime_us();
    bool pass = edges_pass && checksum == WOPMARK_EXPECTED;

    printf("wopmark_total_us: %d\n", (int)(end - start));
    printf("wopmark_checksum: 0x%x\n", checksum);
    printf("WOpMark %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
