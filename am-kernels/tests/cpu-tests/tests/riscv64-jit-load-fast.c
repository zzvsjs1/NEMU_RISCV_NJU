#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 64

#include <stdint.h>

static uint8_t load_data[32] __attribute__((aligned(8))) = {
    0xffu,
    0x80u,
    0x01u, 0x80u,
    0x34u, 0x12u,
    0x00u, 0x00u,
    0x78u, 0x56u, 0x34u, 0x80u,
    0x78u, 0x56u, 0x34u, 0x80u,
    0xefu, 0xcdu, 0xabu, 0x89u, 0x67u, 0x45u, 0x23u, 0x01u,
};

/* Execute all RV64 integer load widths after one JIT-friendly ALU instruction. */
static void run_load_sequence(uint64_t *lb_out, uint64_t *lbu_out,
                              uint64_t *lh_out, uint64_t *lhu_out,
                              uint64_t *lw_out, uint64_t *lwu_out,
                              uint64_t *ld_out)
{
    uint8_t *base = load_data;

    asm volatile(
        "addi t0, zero, 7\n"
        "lb %[lb], 0(%[base])\n"
        "lbu %[lbu], 1(%[base])\n"
        "lh %[lh], 2(%[base])\n"
        "lhu %[lhu], 4(%[base])\n"
        "lw %[lw], 8(%[base])\n"
        "lwu %[lwu], 12(%[base])\n"
        "ld %[ld], 16(%[base])\n"
        : [lb] "=&r"(*lb_out),
          [lbu] "=&r"(*lbu_out),
          [lh] "=&r"(*lh_out),
          [lhu] "=&r"(*lhu_out),
          [lw] "=&r"(*lw_out),
          [lwu] "=&r"(*lwu_out),
          [ld] "=&r"(*ld_out)
        : [base] "r"(base)
        : "t0", "memory");
}

/* Verify signed and unsigned RV64 load extension rules from the unprivileged ISA. */
static void test_native_load_values(void)
{
    uint64_t lb = 0;
    uint64_t lbu = 0;
    uint64_t lh = 0;
    uint64_t lhu = 0;
    uint64_t lw = 0;
    uint64_t lwu = 0;
    uint64_t ld = 0;

    run_load_sequence(&lb, &lbu, &lh, &lhu, &lw, &lwu, &ld);

    check(lb == 0xffffffffffffffffull);
    check(lbu == 0x80ull);
    check(lh == 0xffffffffffff8001ull);
    check(lhu == 0x1234ull);
    check(lw == 0xffffffff80345678ull);
    check(lwu == 0x0000000080345678ull);
    check(ld == 0x0123456789abcdefull);
}

#endif

/* Keep the source buildable outside RV64 while exercising the RV64-only path. */
int main(void)
{
#if defined(__riscv) && __riscv_xlen == 64
    test_native_load_values();
#endif

    return 0;
}
