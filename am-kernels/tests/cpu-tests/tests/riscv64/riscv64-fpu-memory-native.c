#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 64

#include <stdint.h>

#define MSTATUS_FS_MASK ((uintptr_t)3u << 13)
#define MSTATUS_FS_INITIAL ((uintptr_t)1u << 13)
#define MSTATUS_FS_CLEAN ((uintptr_t)2u << 13)
#define MSTATUS_FS_DIRTY ((uintptr_t)3u << 13)
#define MSTATUS_SD (UINT64_C(1) << 63)

typedef struct
{
    uint32_t word;
    uint32_t padding;
    uint64_t doubleword;
} fp_memory_pair_t;

asm(
    ".section .text\n"
    ".align 2\n"
    ".option push\n"
    ".option norvc\n"
    ".option arch, +f\n"
    ".option arch, +d\n"

    /*
     * The loop head is the real backedge target. A native implementation can
     * therefore keep all four ordinary-PMEM transfers inside one generated
     * loop instead of returning after every FP memory helper.
     */
    ".globl rv64_fp_memory_native_loop\n"
    ".type rv64_fp_memory_native_loop, @function\n"
    "rv64_fp_memory_native_loop:\n"
    "1:\n"
    "  flw f0, 0(a0)\n"
    "  fld f1, 8(a0)\n"
    "  fsw f0, 0(a1)\n"
    "  fsd f1, 8(a1)\n"
    "  addi a2, a2, -1\n"
    "  bne a2, zero, 1b\n"
    "  fmv.x.d a3, f0\n"
    "  fmv.x.d a4, f1\n"
    "  xor a0, a3, a4\n"
    "  ret\n"
    ".size rv64_fp_memory_native_loop, "
    ".-rv64_fp_memory_native_loop\n"

    /*
     * Exercise both extremes of the signed 12-bit immediate while retaining
     * the same cached base registers across a load-to-store FPR round trip.
     */
    ".globl rv64_fp_memory_extreme_offsets\n"
    ".type rv64_fp_memory_extreme_offsets, @function\n"
    "rv64_fp_memory_extreme_offsets:\n"
    "  addi a0, a0, 8\n"
    "  addi a0, a0, -8\n"
    "  addi a1, a1, 8\n"
    "  addi a1, a1, -8\n"
    "  flw f31, -2048(a0)\n"
    "  fld f30, 2040(a0)\n"
    "  fsw f31, -2048(a1)\n"
    "  fsd f30, 2040(a1)\n"
    "  fmv.x.d a2, f31\n"
    "  fmv.x.d a3, f30\n"
    "  xor a0, a2, a3\n"
    "  ret\n"
    ".size rv64_fp_memory_extreme_offsets, "
    ".-rv64_fp_memory_extreme_offsets\n"

    ".globl rv64_fp_memory_load_word_state\n"
    ".type rv64_fp_memory_load_word_state, @function\n"
    "rv64_fp_memory_load_word_state:\n"
    "  flw f2, 0(a0)\n"
    "  addi a0, zero, 9\n"
    "  ret\n"
    ".size rv64_fp_memory_load_word_state, "
    ".-rv64_fp_memory_load_word_state\n"

    ".globl rv64_fp_memory_load_double_state\n"
    ".type rv64_fp_memory_load_double_state, @function\n"
    "rv64_fp_memory_load_double_state:\n"
    "  fld f3, 0(a0)\n"
    "  addi a0, zero, 11\n"
    "  ret\n"
    ".size rv64_fp_memory_load_double_state, "
    ".-rv64_fp_memory_load_double_state\n"

    ".globl rv64_fp_memory_read_f2\n"
    ".type rv64_fp_memory_read_f2, @function\n"
    "rv64_fp_memory_read_f2:\n"
    "  fmv.x.d a0, f2\n"
    "  ret\n"
    ".size rv64_fp_memory_read_f2, .-rv64_fp_memory_read_f2\n"

    ".globl rv64_fp_memory_read_f3\n"
    ".type rv64_fp_memory_read_f3, @function\n"
    "rv64_fp_memory_read_f3:\n"
    "  fmv.x.d a0, f3\n"
    "  ret\n"
    ".size rv64_fp_memory_read_f3, .-rv64_fp_memory_read_f3\n"

    ".globl rv64_fp_memory_seed_store_regs\n"
    ".type rv64_fp_memory_seed_store_regs, @function\n"
    "rv64_fp_memory_seed_store_regs:\n"
    "  fmv.d.x f4, a0\n"
    "  fmv.d.x f5, a1\n"
    "  ret\n"
    ".size rv64_fp_memory_seed_store_regs, "
    ".-rv64_fp_memory_seed_store_regs\n"

    ".globl rv64_fp_memory_store_state\n"
    ".type rv64_fp_memory_store_state, @function\n"
    "rv64_fp_memory_store_state:\n"
    "  fsw f4, 0(a0)\n"
    "  fsd f5, 8(a0)\n"
    "  addi a0, zero, 7\n"
    "  ret\n"
    ".size rv64_fp_memory_store_state, "
    ".-rv64_fp_memory_store_state\n"

    ".option pop\n");

extern uint64_t rv64_fp_memory_native_loop(const fp_memory_pair_t *,
                                           fp_memory_pair_t *, uint64_t);
extern uint64_t rv64_fp_memory_extreme_offsets(const uint8_t *, uint8_t *);
extern uint64_t rv64_fp_memory_load_word_state(const uint32_t *);
extern uint64_t rv64_fp_memory_load_double_state(const uint64_t *);
extern uint64_t rv64_fp_memory_read_f2(void);
extern uint64_t rv64_fp_memory_read_f3(void);
extern void rv64_fp_memory_seed_store_regs(uint64_t, uint64_t);
extern uint64_t rv64_fp_memory_store_state(fp_memory_pair_t *);

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

static void test_native_loop_and_raw_payloads(void)
{
    const fp_memory_pair_t source = {
        .word = UINT32_C(0x7f800123),
        .padding = UINT32_C(0xa5a55a5a),
        .doubleword = UINT64_C(0x7ff0000000000456),
    };
    fp_memory_pair_t destination = {0};
    const uint64_t boxed_word = UINT64_C(0xffffffff7f800123);

    write_fflags(UINT64_C(0x1f));
    uintptr_t status = read_mstatus();
    write_mstatus((status & ~MSTATUS_FS_MASK) | MSTATUS_FS_CLEAN);

    check(rv64_fp_memory_native_loop(&source, &destination, 64) ==
          (boxed_word ^ source.doubleword));
    check(destination.word == source.word);
    check(destination.doubleword == source.doubleword);
    check(destination.padding == 0);
    check(read_fflags() == UINT64_C(0x1f));

    status = read_mstatus();
    check((status & MSTATUS_FS_MASK) == MSTATUS_FS_DIRTY);
    check((status & MSTATUS_SD) != 0);
}

static void test_extreme_offsets_and_high_fprs(void)
{
    static uint8_t source[4096] __attribute__((aligned(8)));
    static uint8_t destination[4096] __attribute__((aligned(8)));
    const uint32_t word = UINT32_C(0xff800321);
    const uint64_t doubleword = UINT64_C(0xfff8000000000789);
    const uint64_t boxed_word = UINT64_C(0xffffffffff800321);

    *(uint32_t *)(void *)&source[0] = word;
    *(uint64_t *)(void *)&source[4088] = doubleword;
    *(uint32_t *)(void *)&destination[0] = 0;
    *(uint64_t *)(void *)&destination[4088] = 0;

    check(rv64_fp_memory_extreme_offsets(&source[2048],
                                         &destination[2048]) ==
          (boxed_word ^ doubleword));
    check(*(uint32_t *)(void *)&destination[0] == word);
    check(*(uint64_t *)(void *)&destination[4088] == doubleword);
    check(read_fflags() == UINT64_C(0x1f));
}

static void test_load_state_effects(void)
{
    const uint32_t word = UINT32_C(0x7fa12345);
    const uint64_t doubleword = UINT64_C(0x7ff0000000000789);
    uintptr_t status = read_mstatus();

    write_mstatus((status & ~MSTATUS_FS_MASK) | MSTATUS_FS_INITIAL);
    check(rv64_fp_memory_load_word_state(&word) == 9);
    status = read_mstatus();
    check((status & MSTATUS_FS_MASK) == MSTATUS_FS_DIRTY);
    check((status & MSTATUS_SD) != 0);
    check(rv64_fp_memory_read_f2() == UINT64_C(0xffffffff7fa12345));
    check(read_fflags() == UINT64_C(0x1f));

    write_mstatus((status & ~MSTATUS_FS_MASK) | MSTATUS_FS_CLEAN);
    check(rv64_fp_memory_load_double_state(&doubleword) == 11);
    status = read_mstatus();
    check((status & MSTATUS_FS_MASK) == MSTATUS_FS_DIRTY);
    check((status & MSTATUS_SD) != 0);
    check(rv64_fp_memory_read_f3() == doubleword);
    check(read_fflags() == UINT64_C(0x1f));
}

static void test_store_state_effects(void)
{
    const uint64_t malformed_word_box =
        UINT64_C(0x012345677f800123);
    const uint64_t doubleword = UINT64_C(0xfff8000000000456);
    fp_memory_pair_t destination = {0};
    uintptr_t status;

    rv64_fp_memory_seed_store_regs(malformed_word_box, doubleword);
    status = read_mstatus();
    write_mstatus((status & ~MSTATUS_FS_MASK) | MSTATUS_FS_CLEAN);

    check(rv64_fp_memory_store_state(&destination) == 7);
    check(destination.word == UINT32_C(0x7f800123));
    check(destination.doubleword == doubleword);
    check(destination.padding == 0);

    status = read_mstatus();
    check((status & MSTATUS_FS_MASK) == MSTATUS_FS_CLEAN);
    check((status & MSTATUS_SD) == 0);
    check(read_fflags() == UINT64_C(0x1f));
}

#endif

int main(void)
{
#if defined(__riscv) && __riscv_xlen == 64
    const uintptr_t old_mstatus = read_mstatus();

    write_mstatus((old_mstatus & ~MSTATUS_FS_MASK) | MSTATUS_FS_INITIAL);
    test_native_loop_and_raw_payloads();
    test_extreme_offsets_and_high_fprs();
    test_load_state_effects();
    test_store_state_effects();
    write_mstatus(old_mstatus);
#endif

    return 0;
}
