#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 64

#include <stdint.h>

/* Exercise JAL after one native-friendly instruction and return through RA. */
static uint64_t run_jal_sequence(void)
{
    uint64_t out = 0;

    asm volatile(
        "addi t0, zero, 9\n"
        "jal t1, 1f\n"
        "li %[out], 0\n"
        "j 2f\n"
        "1:\n"
        "addi %[out], t1, 11\n"
        "2:\n"
        : [out] "=&r"(out)
        :
        : "t0", "t1", "memory");

    return out;
}

/* Exercise JALR low-bit clearing and link writing after one native instruction. */
static uint64_t run_jalr_sequence(uint64_t marker)
{
    uint64_t out = 0;

    asm volatile(
        "addi t0, zero, 3\n"
        "la t1, 1f\n"
        "ori t1, t1, 1\n"
        "jalr t2, 0(t1)\n"
        "li %[out], 0\n"
        "j 2f\n"
        "1:\n"
        "xor %[out], t2, %[marker]\n"
        "2:\n"
        : [out] "=&r"(out)
        : [marker] "r"(marker)
        : "t0", "t1", "t2", "memory");

    return out;
}

/* Verify native jump paths preserve architectural targets and link registers. */
static void test_native_jumps(void)
{
    uint64_t jal_link_plus = run_jal_sequence();
    uint64_t jalr_link_xor = run_jalr_sequence(0x5a5a123456789abcu);

    check(jal_link_plus != 0);
    check((jal_link_plus & 0x3u) == 3u);
    check(jalr_link_xor != 0);
}

#endif

/* Keep the source buildable outside RV64 while exercising the RV64-only path. */
int main(void)
{
#if defined(__riscv) && __riscv_xlen == 64
    test_native_jumps();
#endif

    return 0;
}
