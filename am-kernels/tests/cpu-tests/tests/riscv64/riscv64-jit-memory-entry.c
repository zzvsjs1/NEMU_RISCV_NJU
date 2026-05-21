#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 64

#include <stdint.h>

static uint64_t memory_entry_data[2] __attribute__((aligned(8))) = {
    0x123456789abcdef0ull,
    0,
};

volatile uint64_t memory_entry_saved_mcause = 0;
volatile uint64_t memory_entry_saved_mtval = 0;
volatile uint64_t memory_entry_restore_mtvec = 0;

typedef uint64_t (*entry_load_fn_t)(uint64_t *);
typedef uint64_t (*entry_store_fn_t)(uint64_t *, uint64_t);

/*
 * NEMU maps the RTC device at 0xa0000048.  Aligned MMIO loads are allowed to
 * call the architectural helper and continue in native code, while unsafe loads
 * such as misaligned LD still side-exit before the interpreter raises the trap.
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
    0x00053503u, /* ld a0, 0(a0): aligned MMIO uses the load helper. */
    0x00008067u, /* ret is jalr zero, 0(ra). */
};

static uint32_t entry_misaligned_load_code[2] __attribute__((aligned(16))) = {
    0x00053503u, /* ld a0, 0(a0): alignment guard side-exits first. */
    0x00008067u, /* ret is jalr zero, 0(ra). */
};

static uint32_t entry_mmio_store_code[3] __attribute__((aligned(16))) = {
    0x00b50023u, /* sb a1, 0(a0): range guard fails before this executes. */
    0x00500513u, /* addi a0, zero, 5 */
    0x00008067u, /* ret is jalr zero, 0(ra). */
};

asm(
    ".section .text\n"
    ".option push\n"
    ".option norvc\n"
    ".align 2\n"
    ".globl memory_entry_trap_handler\n"
    "memory_entry_trap_handler:\n"
    "  csrr t1, mcause\n"
    "  la t0, memory_entry_saved_mcause\n"
    "  sd t1, 0(t0)\n"
    "  csrr t1, mtval\n"
    "  la t0, memory_entry_saved_mtval\n"
    "  sd t1, 0(t0)\n"
    "  la t0, memory_entry_restore_mtvec\n"
    "  ld t1, 0(t0)\n"
    "  csrw mtvec, t1\n"
    "  csrr t0, mepc\n"
    "  addi t0, t0, 4\n"
    "  csrw mepc, t0\n"
    "  mret\n"
    ".option pop\n");

extern void memory_entry_trap_handler(void);

static inline uintptr_t read_mtvec(void)
{
    uintptr_t v;
    asm volatile("csrr %0, mtvec" : "=r"(v));
    return v;
}

static inline void write_mtvec(uintptr_t v)
{
    asm volatile("csrw mtvec, %0" : : "r"(v) : "memory");
}

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

/* Enter a generated block whose first LD must use the MMIO helper path. */
static uint64_t entry_mmio_load_sequence(void)
{
    return ((entry_load_fn_t)(uintptr_t)entry_mmio_load_code)((uint64_t *)NEMU_RTC_MMIO);
}

/* Enter a generated block whose first LD must side-exit before trapping. */
static uint64_t entry_misaligned_load_sequence(uintptr_t bad)
{
    return ((entry_load_fn_t)(uintptr_t)entry_misaligned_load_code)((uint64_t *)bad);
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
    const uintptr_t bad_load = ((uintptr_t)&memory_entry_data[0]) + 1u;
    const uint64_t mmio_store_out = entry_mmio_store_sequence(0x55u);

    memory_entry_saved_mcause = 0;
    memory_entry_saved_mtval = 0;
    memory_entry_restore_mtvec = read_mtvec();
    write_mtvec((uintptr_t)memory_entry_trap_handler);
    const uint64_t misaligned_load = entry_misaligned_load_sequence(bad_load);

    check(loaded == 0x123456789abcdef7ull);
    check(memory_entry_data[1] == marker);
    check(store_out == ((marker + 3ull) ^ marker));
    check(mmio_load != 0xffffffffffffffffull);
    check(mmio_store_out == 5);
    check(memory_entry_saved_mcause == 4u);
    check(memory_entry_saved_mtval == bad_load);
    check(misaligned_load == bad_load);
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
