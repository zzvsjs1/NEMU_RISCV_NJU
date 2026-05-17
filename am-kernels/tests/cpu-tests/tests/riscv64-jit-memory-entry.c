#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 64

#include <stdint.h>

static uint64_t memory_entry_data[2] __attribute__((aligned(8))) = {
    0x123456789abcdef0ull,
    0,
};

typedef uint64_t (*entry_load_fn_t)(uint64_t *);
typedef uint64_t (*entry_store_fn_t)(uint64_t *, uint64_t);

/*
 * NEMU maps the RTC device at 0xa0000048.  Accessing it from a first-instruction
 * load/store makes the JIT side-exit with zero retired instructions, after
 * which cpu_exec() must immediately let the interpreter execute that same
 * instruction.
 */
#define NEMU_RTC_MMIO 0xa0000048ull

static uint32_t entry_load_code[3] __attribute__((aligned(16))) = {
    0x00053503u, /* ld a0, 0(a0): block entry is the load itself. */
    0x00750513u, /* addi a0, a0, 7 */
    0x00008067u, /* ret is jalr zero, 0(ra). */
};

static uint32_t entry_store_code[4] __attribute__((aligned(16))) = {
    0x00b53023u, /* sd a1, 0(a0): block entry is the store itself. */
    0x00358513u, /* addi a0, a1, 3 */
    0x00b54533u, /* xor a0, a0, a1 */
    0x00008067u, /* ret is jalr zero, 0(ra). */
};

static uint32_t entry_mmio_load_code[2] __attribute__((aligned(16))) = {
    0x00053503u, /* ld a0, 0(a0): range guard fails before this executes. */
    0x00008067u, /* ret is jalr zero, 0(ra). */
};

static uint32_t entry_mmio_store_code[3] __attribute__((aligned(16))) = {
    0x00b50023u, /* sb a1, 0(a0): range guard fails before this executes. */
    0x00500513u, /* addi a0, zero, 5 */
    0x00008067u, /* ret is jalr zero, 0(ra). */
};

/* Issue FENCE.I before entering generated code buffers. */
static void local_fence_i(void)
{
    asm volatile("fence.i" : : : "memory");
}

/* Enter a generated block whose first guest instruction is LD. */
static uint64_t entry_load_sequence(uint64_t *p)
{
    return ((entry_load_fn_t)(uintptr_t)entry_load_code)(p);
}

/* Enter a generated block whose first guest instruction is SD. */
static uint64_t entry_store_sequence(uint64_t *p, uint64_t value)
{
    return ((entry_store_fn_t)(uintptr_t)entry_store_code)(p, value);
}

/* Enter a generated block whose first LD must side-exit with zero retired work. */
static uint64_t entry_mmio_load_sequence(void)
{
    return ((entry_load_fn_t)(uintptr_t)entry_mmio_load_code)((uint64_t *)NEMU_RTC_MMIO);
}

/* Enter a generated block whose first SB must side-exit with zero retired work. */
static uint64_t entry_mmio_store_sequence(uint64_t value)
{
    return ((entry_store_fn_t)(uintptr_t)entry_mmio_store_code)((uint64_t *)NEMU_RTC_MMIO, value);
}

/* Cover memory instructions at block entry and safe store continuation. */
static void test_memory_entry_and_store_continue(void)
{
    local_fence_i();

    const uint64_t loaded = entry_load_sequence(&memory_entry_data[0]);
    const uint64_t marker = 0x0f0e0d0c0b0a0908ull;
    const uint64_t store_out = entry_store_sequence(&memory_entry_data[1], marker);
    const uint64_t mmio_load = entry_mmio_load_sequence();
    const uint64_t mmio_store_out = entry_mmio_store_sequence(0x55u);

    check(loaded == 0x123456789abcdef7ull);
    check(memory_entry_data[1] == marker);
    check(store_out == ((marker + 3ull) ^ marker));
    check(mmio_load != 0xffffffffffffffffull);
    check(mmio_store_out == 5);
}

#endif

/* Keep the source buildable outside RV64 while exercising the RV64-only path. */
int main(void)
{
#if defined(__riscv) && __riscv_xlen == 64
    test_memory_entry_and_store_continue();
#endif

    return 0;
}
