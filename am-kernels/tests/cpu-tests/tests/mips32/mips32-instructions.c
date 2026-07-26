#include "trap.h"
#include <stdint.h>

#if !defined(__mips__)
#error "This test must be compiled for MIPS32"
#endif

static void test_conditional_moves(void)
{
    const uint32_t source = 0x13579bdfu;
    const uint32_t zero = 0;
    const uint32_t nonzero = 7;
    uint32_t result = 0x2468ace0u;

    asm volatile("movz %0, %1, %2" : "+r"(result) : "r"(source), "r"(zero));
    check(result == source);

    result = 0x2468ace0u;
    asm volatile("movz %0, %1, %2" : "+r"(result) : "r"(source), "r"(nonzero));
    check(result == 0x2468ace0u);

    result = 0x2468ace0u;
    asm volatile("movn %0, %1, %2" : "+r"(result) : "r"(source), "r"(nonzero));
    check(result == source);

    result = 0x2468ace0u;
    asm volatile("movn %0, %1, %2" : "+r"(result) : "r"(source), "r"(zero));
    check(result == 0x2468ace0u);
}

static void test_bit_counts(void)
{
    uint32_t result = 0;
    const uint32_t leading_zero_value = 0x00100000u;
    const uint32_t leading_one_value = 0xfff7ffffu;

    asm volatile("clz %0, %1" : "=r"(result) : "r"(leading_zero_value));
    check(result == 11);

    asm volatile("clz %0, %1" : "=r"(result) : "r"(0u));
    check(result == 32);

    asm volatile("clo %0, %1" : "=r"(result) : "r"(leading_one_value));
    check(result == 12);

    asm volatile("clo %0, %1" : "=r"(result) : "r"(UINT32_MAX));
    check(result == 32);
}

static void test_hi_lo(void)
{
    const uint32_t hi_input = 0x89abcdefu;
    const uint32_t lo_input = 0x01234567u;
    uint32_t hi_output = 0;
    uint32_t lo_output = 0;

    asm volatile(
        "mthi %2\n"
        "mtlo %3\n"
        "mfhi %0\n"
        "mflo %1\n"
        : "=&r"(hi_output), "=&r"(lo_output)
        : "r"(hi_input), "r"(lo_input));

    check(hi_output == hi_input);
    check(lo_output == lo_input);

    /*
     * MUL's effect on HI/LO is architecturally unpredictable.  This model
     * deliberately preserves the pair to give that case deterministic
     * behaviour; the test records that model choice rather than a universal
     * architectural requirement.
     */
    uint32_t product = 0;
    asm volatile(
        "mul %0, %3, %4\n"
        "mfhi %1\n"
        "mflo %2\n"
        : "=&r"(product), "=&r"(hi_output), "=&r"(lo_output)
        : "r"(7u), "r"(9u));

    check(product == 63);
    check(hi_output == hi_input);
    check(lo_output == lo_input);
}

static uint32_t read_badvaddr(void)
{
    uint32_t value;

    /* Keep explicit spacing for the CP0-to-GPR result hazard. */
    asm volatile(
        "mfc0 %0, $8\n"
        "nop\n"
        "nop\n"
        "nop\n"
        : "=r"(value)
        :
        : "memory");
    return value;
}

static void write_badvaddr(uint32_t value)
{
    /* Keep explicit spacing before any later CP0 access. */
    asm volatile(
        "mtc0 %0, $8\n"
        "nop\n"
        "nop\n"
        "nop\n"
        :
        : "r"(value)
        : "memory");
}

static uint32_t read_cause(void)
{
    uint32_t value;

    /* Keep explicit spacing for the CP0-to-GPR result hazard. */
    asm volatile(
        "mfc0 %0, $13\n"
        "nop\n"
        "nop\n"
        "nop\n"
        : "=r"(value)
        :
        : "memory");
    return value;
}

static void write_cause(uint32_t value)
{
    /* Keep explicit spacing before any later CP0 access. */
    asm volatile(
        "mtc0 %0, $13\n"
        "nop\n"
        "nop\n"
        "nop\n"
        :
        : "r"(value)
        : "memory");
}

static void test_cp0_write_semantics(void)
{
    const uint32_t cause_exc_code_mask = 0x1fu << 2;
    const uint32_t badvaddr_before = read_badvaddr();
    const uint32_t badvaddr_attempt = badvaddr_before ^ 0x5aa5f00du;

    /* BadVAddr is read-only: MTC0 must not replace its recorded address. */
    write_badvaddr(badvaddr_attempt);
    const uint32_t badvaddr_after = read_badvaddr();

    /* Restore the snapshot first so a failing model does not retain damage. */
    write_badvaddr(badvaddr_before);
    check(badvaddr_after == badvaddr_before);

    const uint32_t cause_before = read_cause();
    const uint32_t cause_attempt = cause_before ^ cause_exc_code_mask;

    /*
     * Toggle every ExcCode bit while copying all unrelated Cause fields from
     * the original value.  This avoids assuming which other fields are
     * writable, pending, or changing asynchronously.
     */
    write_cause(cause_attempt);
    const uint32_t cause_after = read_cause();

    /* Restore every writable bit to its original value before checking. */
    write_cause(cause_before);
    check((cause_after & cause_exc_code_mask) ==
          (cause_before & cause_exc_code_mask));
}

static void store_unaligned_word(uint8_t *address, uint32_t value)
{
    /* Little-endian MIPS stores an unaligned word with SWL at +3 and SWR at +0. */
    asm volatile(
        "swl %1, 3(%0)\n"
        "swr %1, 0(%0)\n"
        :
        : "r"(address), "r"(value)
        : "memory");
}

static uint32_t load_unaligned_word(const uint8_t *address)
{
    uint32_t value = 0xa5a5a5a5u;

    asm volatile(
        "lwl %0, 3(%1)\n"
        "lwr %0, 0(%1)\n"
        : "+r"(value)
        : "r"(address)
        : "memory");
    return value;
}

static void test_left_right_memory(void)
{
    uint8_t storage[16] __attribute__((aligned(4)));
    const uint32_t expected = 0x12345678u;

    /*
     * Shift the target through every byte position.  Across these four cases,
     * both members of each left/right pair exercise every low-address value.
     */
    for (int offset = 0; offset < 4; offset++)
    {
        for (int i = 0; i < (int)sizeof(storage); i++)
            storage[i] = 0xa5;

        uint8_t *target = storage + 4 + offset;
        store_unaligned_word(target, expected);

        check(target[0] == 0x78);
        check(target[1] == 0x56);
        check(target[2] == 0x34);
        check(target[3] == 0x12);
        check(target[-1] == 0xa5);
        check(target[4] == 0xa5);
        check(load_unaligned_word(target) == expected);
    }
}

int main(void)
{
    test_conditional_moves();
    test_bit_counts();
    test_hi_lo();
    test_cp0_write_semantics();
    test_left_right_memory();
    return 0;
}
