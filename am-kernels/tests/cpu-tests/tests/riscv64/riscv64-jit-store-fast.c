#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 64

#include <stdint.h>

static uint8_t store_data[32] __attribute__((aligned(8)));

/* Execute all RV64 integer store widths after one JIT-friendly ALU instruction. */
static void run_store_sequence(void)
{
    uint8_t *base = store_data;
    uint64_t byte_value = 0xaaull;
    uint64_t half_value = 0x1234ull;
    uint64_t word_value = 0x89abcdefull;
    uint64_t dword_value = 0x0123456789abcdefull;

    asm volatile(
        "addi t0, zero, 5\n"
        "sb %[byte_value], 0(%[base])\n"
        "sh %[half_value], 2(%[base])\n"
        "sw %[word_value], 8(%[base])\n"
        "sd %[dword_value], 16(%[base])\n"
        :
        : [base] "r"(base),
          [byte_value] "r"(byte_value),
          [half_value] "r"(half_value),
          [word_value] "r"(word_value),
          [dword_value] "r"(dword_value)
        : "t0", "memory");
}

/* Verify store widths truncate and commit exactly the requested byte lanes. */
static void test_native_store_values(void)
{
    for (int i = 0; i < (int)sizeof(store_data); i++)
    {
        store_data[i] = 0;
    }

    run_store_sequence();

    check(store_data[0] == 0xaau);
    check(store_data[1] == 0x00u);
    check(store_data[2] == 0x34u);
    check(store_data[3] == 0x12u);
    check(store_data[8] == 0xefu);
    check(store_data[9] == 0xcdu);
    check(store_data[10] == 0xabu);
    check(store_data[11] == 0x89u);
    check(store_data[16] == 0xefu);
    check(store_data[17] == 0xcdu);
    check(store_data[18] == 0xabu);
    check(store_data[19] == 0x89u);
    check(store_data[20] == 0x67u);
    check(store_data[21] == 0x45u);
    check(store_data[22] == 0x23u);
    check(store_data[23] == 0x01u);
}

#endif

/* Keep the source buildable outside RV64 while exercising the RV64-only path. */
int main(void)
{
#if defined(__riscv) && __riscv_xlen == 64
    test_native_store_values();
#endif

    return 0;
}
