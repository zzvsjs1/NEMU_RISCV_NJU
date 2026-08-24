#include <am.h>
#include <klib.h>
#include <stdint.h>

#if __riscv_xlen != 64
#error "FPMemMark requires RV64"
#endif

enum
{
    FPMEMMARK_WARMUP_ROUNDS = 1024,
    FPMEMMARK_TIMED_ROUNDS = 1000000,
    MSTATUS_FS_SHIFT = 13,
};

#define MSTATUS_FS_MASK ((uintptr_t)3u << MSTATUS_FS_SHIFT)
#define MSTATUS_FS_INITIAL ((uintptr_t)1u << MSTATUS_FS_SHIFT)
#define MSTATUS_FS_DIRTY ((uintptr_t)3u << MSTATUS_FS_SHIFT)
#define MSTATUS_SD (UINT64_C(1) << 63)
#define FPMEMMARK_ROLL_SEED UINT64_C(0x6a09e667f3bcc909)
#define FPMEMMARK_WORD_BASE UINT32_C(0x7f800000)
#define FPMEMMARK_DOUBLEWORD_BASE UINT64_C(0x7ff0000000000000)
#define FPMEMMARK_PAYLOAD_MASK UINT64_C(0x003fffff)

_Static_assert(FPMEMMARK_WARMUP_ROUNDS > 0, "the warm-up must execute the loop");
_Static_assert(FPMEMMARK_TIMED_ROUNDS > 0, "the timed run must execute the loop");
_Static_assert(FPMEMMARK_WARMUP_ROUNDS <= FPMEMMARK_PAYLOAD_MASK, "the warm-up counter must fit below the NaN fields");
_Static_assert(FPMEMMARK_TIMED_ROUNDS <= FPMEMMARK_PAYLOAD_MASK, "the timed counter must fit below the NaN fields");

typedef struct
{
    uint32_t word;
    uint32_t padding;
    uint64_t doubleword;
} fp_memory_pair_t;

/*
 * Every lap first writes counter-dependent raw values to the source, transfers
 * them through FLW/FLD and FSW/FSD, then reads the destination with integer
 * loads. The comparisons make every individual FP load and store observable:
 * stale FPR or destination contents differ from the current lap immediately.
 * Their XOR differences are ORed into a permanent mismatch result, so a later
 * lap cannot cancel an earlier failure. Folding the checked destination values
 * into a separate checksum gives C a second end-to-end oracle without relying
 * on FMV.X.D lowering.
 */
asm(".section .text\n"
    ".align 2\n"
    ".option push\n"
    ".option norvc\n"
    ".option arch, +f\n"
    ".option arch, +d\n"
    ".globl rv64_fpmemmark_kernel\n"
    ".type rv64_fpmemmark_kernel, @function\n"
    "rv64_fpmemmark_kernel:\n"
    "  li a4, 0x6a09e667f3bcc909\n"
    "  li a5, 0\n"
    "  li a6, 0x7f800000\n"
    "  li a7, 0x7ff0000000000000\n"
    "1:\n"
    "  or t0, a6, a2\n"
    "  or t1, a7, a2\n"
    "  sw t0, 0(a0)\n"
    "  sd t1, 8(a0)\n"
    "  flw f0, 0(a0)\n"
    "  fld f1, 8(a0)\n"
    "  fsw f0, 0(a1)\n"
    "  fsd f1, 8(a1)\n"
    "  lwu t2, 0(a1)\n"
    "  ld t3, 8(a1)\n"
    "  xor t0, t0, t2\n"
    "  xor t1, t1, t3\n"
    "  or a5, a5, t0\n"
    "  or a5, a5, t1\n"
    "  xor a4, a4, t2\n"
    "  slli t0, a4, 7\n"
    "  srli t1, a4, 57\n"
    "  or a4, t0, t1\n"
    "  xor a4, a4, t3\n"
    "  add a4, a4, a2\n"
    "  addi a2, a2, -1\n"
    "  bne a2, zero, 1b\n"
    "  sd a4, 0(a3)\n"
    "  addi a0, a5, 0\n"
    "  ret\n"
    ".size rv64_fpmemmark_kernel, .-rv64_fpmemmark_kernel\n"
    ".option pop\n");

extern uint64_t rv64_fpmemmark_kernel(fp_memory_pair_t *, fp_memory_pair_t *, uint64_t, uint64_t *);

static uint64_t uptime_us(void)
{
    return io_read(AM_TIMER_UPTIME).us;
}

static uintptr_t read_mstatus(void)
{
    uintptr_t value;
    asm volatile("csrr %0, mstatus" : "=r"(value));
    return value;
}

static void write_mstatus(uintptr_t value)
{
    asm volatile("csrw mstatus, %0" : : "r"(value) : "memory");
}

static uintptr_t read_fflags(void)
{
    uintptr_t value;
    asm volatile("csrr %0, 0x001" : "=r"(value));
    return value;
}

static void write_fflags(uintptr_t value)
{
    asm volatile("csrw 0x001, %0" : : "r"(value) : "memory");
}

static uint64_t expected_rolling_checksum(uint64_t rounds)
{
    uint64_t checksum = FPMEMMARK_ROLL_SEED;

    while (rounds != 0)
    {
        const uint64_t word = FPMEMMARK_WORD_BASE | rounds;
        const uint64_t doubleword = FPMEMMARK_DOUBLEWORD_BASE | rounds;

        checksum ^= word;
        checksum = (checksum << 7) | (checksum >> 57);
        checksum ^= doubleword;
        checksum += rounds;
        rounds--;
    }

    return checksum;
}

int main(void)
{
    static fp_memory_pair_t source = {
        .word = FPMEMMARK_WORD_BASE,
        .padding = UINT32_C(0xa5a55a5a),
        .doubleword = FPMEMMARK_DOUBLEWORD_BASE,
    };
    static fp_memory_pair_t destination = {
        .word = UINT32_C(0xdeadbeef),
        .padding = UINT32_C(0x5aa5c33c),
        .doubleword = UINT64_C(0x0123456789abcdef),
    };
    uint64_t warmup_checksum = 0;
    uint64_t checksum = 0;
    const uint64_t expected_checksum = expected_rolling_checksum(FPMEMMARK_TIMED_ROUNDS);
    const uint64_t expected_warmup_checksum = expected_rolling_checksum(FPMEMMARK_WARMUP_ROUNDS);
    const uint32_t expected_final_word = FPMEMMARK_WORD_BASE | UINT32_C(1);
    const uint64_t expected_final_doubleword = FPMEMMARK_DOUBLEWORD_BASE | UINT64_C(1);

    ioe_init();

    const uintptr_t old_mstatus = read_mstatus();
    write_mstatus((old_mstatus & ~MSTATUS_FS_MASK) | MSTATUS_FS_INITIAL);
    write_fflags(UINT64_C(0x1f));

    /*
     * Warm the exact loop PC before timing, excluding compilation, cache
     * publication, and direct-backedge installation from the steady-state
     * measurement.
     */
    const uint64_t warmup_mismatch = rv64_fpmemmark_kernel(&source, &destination, FPMEMMARK_WARMUP_ROUNDS, &warmup_checksum);

    const uint64_t start = uptime_us();
    const uint64_t mismatch = rv64_fpmemmark_kernel(&source, &destination, FPMEMMARK_TIMED_ROUNDS, &checksum);
    const uint64_t end = uptime_us();
    const uintptr_t final_mstatus = read_mstatus();
    const uint32_t checksum_hi = (uint32_t)(checksum >> 32);
    const uint32_t checksum_lo = (uint32_t)checksum;
    const bool pass = warmup_mismatch == 0 && warmup_checksum == expected_warmup_checksum && mismatch == 0 && checksum == expected_checksum &&
                      source.word == expected_final_word && source.padding == UINT32_C(0xa5a55a5a) &&
                      source.doubleword == expected_final_doubleword && destination.word == expected_final_word &&
                      destination.padding == UINT32_C(0x5aa5c33c) && destination.doubleword == expected_final_doubleword &&
                      read_fflags() == UINT64_C(0x1f) && (final_mstatus & MSTATUS_FS_MASK) == MSTATUS_FS_DIRTY && (final_mstatus & MSTATUS_SD) != 0;

    printf("fpmemmark_us: %d\n", (int)(end - start));
    printf("fpmemmark_checksum_hi: 0x%x\n", checksum_hi);
    printf("fpmemmark_checksum_lo: 0x%x\n", checksum_lo);
    printf("FPMemMark %s\n", pass ? "PASS" : "FAIL");

    write_mstatus(old_mstatus);
    return pass ? 0 : 1;
}
