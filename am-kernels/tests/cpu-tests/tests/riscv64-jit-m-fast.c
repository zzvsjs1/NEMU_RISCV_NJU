#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 64

#include <stdint.h>

/* Execute M-extension operations after one JIT-friendly ALU instruction. */
static void run_m_sequence(uint64_t *mulh_out, uint64_t *mulhu_out,
                           uint64_t *div_out, uint64_t *divu_out,
                           uint64_t *rem_out, uint64_t *remu_out,
                           uint64_t *divw_out, uint64_t *remuw_out)
{
    uint64_t neg = 0xffffffffffffff00ull;
    uint64_t pos = 0x1000000000000001ull;
    uint64_t dividend = 0x7fffffffffffffffull;
    uint64_t divisor = 17;
    uint64_t word_dividend = 0x0000000080001234ull;
    uint64_t word_divisor = 0;

    asm volatile(
        "addi t0, zero, 13\n"
        "mulh %[mulh], %[neg], %[pos]\n"
        "mulhu %[mulhu], %[pos], %[dividend]\n"
        "div %[div], %[dividend], %[divisor]\n"
        "divu %[divu], %[dividend], %[divisor]\n"
        "rem %[rem], %[dividend], %[divisor]\n"
        "remu %[remu], %[dividend], %[divisor]\n"
        "divw %[divw], %[word_dividend], %[word_divisor]\n"
        "remuw %[remuw], %[word_dividend], %[word_divisor]\n"
        : [mulh] "=&r"(*mulh_out),
          [mulhu] "=&r"(*mulhu_out),
          [div] "=&r"(*div_out),
          [divu] "=&r"(*divu_out),
          [rem] "=&r"(*rem_out),
          [remu] "=&r"(*remu_out),
          [divw] "=&r"(*divw_out),
          [remuw] "=&r"(*remuw_out)
        : [neg] "r"(neg),
          [pos] "r"(pos),
          [dividend] "r"(dividend),
          [divisor] "r"(divisor),
          [word_dividend] "r"(word_dividend),
          [word_divisor] "r"(word_divisor)
        : "t0", "memory");
}

/* Verify RV64M helper-backed operations, including divide-by-zero W semantics. */
static void test_native_m_ops(void)
{
    uint64_t mulh = 0;
    uint64_t mulhu = 0;
    uint64_t div = 0;
    uint64_t divu = 0;
    uint64_t rem = 0;
    uint64_t remu = 0;
    uint64_t divw = 0;
    uint64_t remuw = 0;

    run_m_sequence(&mulh, &mulhu, &div, &divu, &rem, &remu, &divw, &remuw);

    check(mulh == 0xffffffffffffffefull);
    check(mulhu == 0x0800000000000000ull);
    check(div == 0x0787878787878787ull);
    check(divu == 0x0787878787878787ull);
    check(rem == 8ull);
    check(remu == 8ull);
    check(divw == 0xffffffffffffffffull);
    check(remuw == 0xffffffff80001234ull);
}

#endif

/* Keep the source buildable outside RV64 while exercising the RV64-only path. */
int main(void)
{
#if defined(__riscv) && __riscv_xlen == 64
    test_native_m_ops();
#endif

    return 0;
}
