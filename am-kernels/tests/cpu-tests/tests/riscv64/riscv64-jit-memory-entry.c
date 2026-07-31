#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 64

#include <stdint.h>

static uint64_t memory_entry_data[2] __attribute__((aligned(8))) = {
    0x123456789abcdef0ull,
    0,
};

static uint64_t memory_entry_route_data __attribute__((aligned(8))) =
    0xfedcba980badc0deull;

volatile uint64_t memory_entry_saved_mcause = 0;
volatile uint64_t memory_entry_saved_mtval = 0;
volatile uint64_t memory_entry_restore_mtvec = 0;

typedef uint64_t (*entry_load_fn_t)(uint64_t *);
typedef uint64_t (*entry_store_fn_t)(uint64_t *, uint64_t);
typedef uint64_t (*entry_seed_guard_fn_t)(uint64_t, uint64_t);

/*
 * NEMU maps the RTC device at 0xa0000048.  Aligned MMIO loads are allowed to
 * call the architectural helper and continue in native code, while unsafe loads
 * such as misaligned LD still side-exit before the interpreter raises the trap.
 */
#define NEMU_RTC_MMIO 0xa0000048ull
#define NEMU_PMEM_BASE 0x80000000ull
#define NEMU_MOUSE_MMIO 0xa0000070ull
#define NEMU_SERIAL_MMIO 0xa00003f8ull
#define NEMU_VGACTL_MMIO 0xa0000100ull
#define NEMU_VGACTL_TEST_MMIO (NEMU_VGACTL_MMIO + 8ull)
#define NEMU_VGACTL_BLIT_POS_MMIO (NEMU_VGACTL_MMIO + 12ull)
#define NEMU_VGACTL_BLIT_SIZE_MMIO (NEMU_VGACTL_MMIO + 16ull)
#define NEMU_VGACTL_BLIT_CMD_MMIO (NEMU_VGACTL_MMIO + 20ull)
#define NEMU_VGACTL_BLIT_CMD_COPY 1u
#define NEMU_VGACTL_INITIAL_DIRECT_PATTERN 0xa5c36e91u
#define ENTRY_MMIO_ROUTE_STORE_SW 0x00b52023u
#define ENTRY_MMIO_ROUTE_STORE_SD 0x00b53023u

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
    0x00053503u, /* ld a0, 0(a0): run-time address selects direct or helper. */
    0x00008067u, /* ret is jalr zero, 0(ra). */
};

static uint32_t entry_mmio_load32_code[2] __attribute__((aligned(16))) = {
    0x00056503u, /* lwu a0, 0(a0): run-time address selects direct or helper. */
    0x00008067u, /* ret is jalr zero, 0(ra). */
};

/*
 * Each row loads the same device bytes with a different RV64 width or
 * signedness. The literal opcodes keep this test independent of compiler load
 * selection and make every typed generated MMIO load observable.
 */
static uint32_t entry_mmio_width_load_code[7][2]
    __attribute__((aligned(16))) = {
        {0x00050503u, 0x00008067u}, /* lb a0, 0(a0); ret */
        {0x00054503u, 0x00008067u}, /* lbu a0, 0(a0); ret */
        {0x00051503u, 0x00008067u}, /* lh a0, 0(a0); ret */
        {0x00055503u, 0x00008067u}, /* lhu a0, 0(a0); ret */
        {0x00052503u, 0x00008067u}, /* lw a0, 0(a0); ret */
        {0x00056503u, 0x00008067u}, /* lwu a0, 0(a0); ret */
        {0x00053503u, 0x00008067u}, /* ld a0, 0(a0); ret */
};

static uint32_t entry_misaligned_load_code[2] __attribute__((aligned(16))) = {
    0x00053503u, /* ld a0, 0(a0): alignment guard side-exits first. */
    0x00008067u, /* ret is jalr zero, 0(ra). */
};

static uint32_t entry_mmio_store_code[3] __attribute__((aligned(16))) = {
    0x00b50023u, /* sb a1, 0(a0): aligned MMIO commits through the helper. */
    0x00500513u, /* addi a0, zero, 5: must execute in the same native entry. */
    0x00008067u, /* ret is jalr zero, 0(ra). */
};

static uint32_t entry_mmio_store32_code[3] __attribute__((aligned(16))) = {
    0x00b52023u, /* sw a1, 0(a0): run-time address selects direct or helper. */
    0x00500513u, /* addi a0, zero, 5: both routes must continue natively. */
    0x00008067u, /* ret is jalr zero, 0(ra). */
};

/*
 * These two mutable sites give the route-cache test one stable guest PC for
 * every address transition. The store result is derived from rs2, so a fast
 * arm which corrupts or fails to materialise the source register is visible.
 */
static volatile uint32_t entry_mmio_route_load_code[2]
    __attribute__((aligned(16))) = {
        0x00056503u, /* lwu a0, 0(a0) */
        0x00008067u, /* ret */
};

static volatile uint32_t entry_mmio_route_store_code[3]
    __attribute__((aligned(16))) = {
        ENTRY_MMIO_ROUTE_STORE_SW, /* sw a1, 0(a0) */
        0x00558513u,               /* addi a0, a1, 5 */
        0x00008067u,               /* ret */
};

/*
 * Compilation observes the direct address in a0, but the first instruction
 * replaces it with a1 before the load. This proves a route seed is only a hint:
 * CONFIG_MBASE must take the ordinary PMEM path instead of matching an invalid
 * sidecar state.
 */
static volatile uint32_t entry_mmio_route_seed_guard_code[3]
    __attribute__((aligned(16))) = {
        0x00058513u, /* addi a0, a1, 0 */
        0x00056503u, /* lwu a0, 0(a0) */
        0x00008067u, /* ret */
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

/*
 * Start the body after a JAL boundary so t1 has a deterministic stale backing
 * value. The body dirties all six ordinary cache slots, then makes reading a0
 * evict t0 and writing the MMIO result to t6 evict t1. Both control-flow arms
 * must spill and map the same architectural registers.
 */
asm(
    ".section .text\n"
    ".option push\n"
    ".option norvc\n"
    ".align 2\n"
    ".globl memory_entry_mmio_spill_load\n"
    "memory_entry_mmio_spill_load:\n"
    "  addi t1, zero, 99\n"
    "  jal zero, memory_entry_mmio_spill_body\n"
    "memory_entry_mmio_spill_body:\n"
    "  addi t0, zero, 11\n"
    "  addi t1, zero, 22\n"
    "  addi t2, zero, 33\n"
    "  addi t3, zero, 44\n"
    "  addi t4, zero, 55\n"
    "  addi t5, zero, 66\n"
    "  lwu t6, 0(a0)\n"
    "  add a0, t0, t1\n"
    "  add a0, a0, t2\n"
    "  add a0, a0, t3\n"
    "  add a0, a0, t4\n"
    "  add a0, a0, t5\n"
    "  add a0, a0, t6\n"
    "  ret\n"
    ".option pop\n");

extern uint64_t memory_entry_mmio_spill_load(uint64_t *addr);

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

/*
 * Enter one generated LD block with a run-time-selected MMIO address.  Calling
 * the same native site with callback-driven and direct-readable maps proves
 * that route selection follows the address on every execution.
 */
static uint64_t entry_mmio_load_sequence(uintptr_t addr)
{
    return ((entry_load_fn_t)(uintptr_t)entry_mmio_load_code)((uint64_t *)addr);
}

/* Enter one generated LWU block with a run-time-selected MMIO address. */
static uint64_t entry_mmio_load32_sequence(uintptr_t addr)
{
    return ((entry_load_fn_t)(uintptr_t)entry_mmio_load32_code)(
        (uint64_t *)addr);
}

/* Enter one generated block for each signed or unsigned MMIO load width. */
static uint64_t entry_mmio_width_load_sequence(uint32_t index)
{
    return ((entry_load_fn_t)(uintptr_t)entry_mmio_width_load_code[index])(
        (uint64_t *)NEMU_VGACTL_TEST_MMIO);
}

/* Enter a generated block whose first LD must side-exit before trapping. */
static uint64_t entry_misaligned_load_sequence(uintptr_t bad)
{
    return ((entry_load_fn_t)(uintptr_t)entry_misaligned_load_code)((uint64_t *)bad);
}

/* Enter a generated block whose first SB must commit exactly once. */
static uint64_t entry_mmio_store_sequence(uintptr_t addr, uint64_t value)
{
    return ((entry_store_fn_t)(uintptr_t)entry_mmio_store_code)(
        (uint64_t *)addr, value);
}

/* Enter one generated SW block with a run-time-selected MMIO address. */
static uint64_t entry_mmio_store32_sequence(uintptr_t addr, uint64_t value)
{
    return ((entry_store_fn_t)(uintptr_t)entry_mmio_store32_code)(
        (uint64_t *)addr, value);
}

/* Enter the dedicated mutable LWU route-cache site. */
static uint64_t entry_mmio_route_load_sequence(uintptr_t addr)
{
    return ((entry_load_fn_t)(uintptr_t)entry_mmio_route_load_code)(
        (uint64_t *)addr);
}

/* Enter the dedicated mutable SW/SD route-cache site. */
static uint64_t entry_mmio_route_store_sequence(
    uintptr_t addr, uint64_t value)
{
    return ((entry_store_fn_t)(uintptr_t)entry_mmio_route_store_code)(
        (uint64_t *)addr, value);
}

/* Enter a block whose observed direct address changes before its first load. */
static uint64_t entry_mmio_route_seed_guard_sequence(
    uintptr_t observed_addr, uintptr_t runtime_addr)
{
    return ((entry_seed_guard_fn_t)(uintptr_t)
                entry_mmio_route_seed_guard_code)(
        observed_addr, runtime_addr);
}

/* Cover memory instructions at block entry and safe store continuation. */
static void test_memory_entry_and_store_continue(void)
{
    local_fence_i();

    const uint64_t loaded = entry_load_sequence(&memory_entry_data[0]);
    const uint64_t marker = 0x0f0e0d0c0b0a0908ull;
    const uint64_t store_out = entry_store_sequence(&memory_entry_data[1], marker);
    const uint64_t mmio_load = entry_mmio_load_sequence(NEMU_RTC_MMIO);
    const uint64_t mmio_once_load =
        entry_mmio_load32_sequence(NEMU_MOUSE_MMIO);
    const uint64_t mmio_width_pattern = 0x88776655f0e2d4c3ull;
    uint64_t mmio_width_loads[7] = {};
    const uintptr_t bad_load = ((uintptr_t)&memory_entry_data[0]) + 1u;
    uint64_t mmio_store_out = 0;

    /*
     * This VGA control-map slot is ordinary backing storage unless a separate
     * command register is written. An eight-byte write at this offset therefore
     * provides stable bytes without asking the device to update the display.
     */
    (void)entry_store_sequence(
        (uint64_t *)NEMU_VGACTL_TEST_MMIO, mmio_width_pattern);

    const uint64_t mmio_shared_direct_load =
        entry_mmio_load_sequence(NEMU_VGACTL_TEST_MMIO);
    const uint64_t mmio_spill_load = memory_entry_mmio_spill_load(
        (uint64_t *)NEMU_VGACTL_TEST_MMIO);

    for (uint32_t index = 0; index < 7u; index++)
    {
        mmio_width_loads[index] =
            entry_mmio_width_load_sequence(index);
    }

    /*
     * BLIT_SRC is the only contracted four-byte staging register. Route the
     * same generated SW site through adjacent BLIT_POS and BLIT_SIZE words to
     * prove that the direct region ends exactly after BLIT_SRC. Initialising
     * position and size also makes the following command callback deterministic
     * even though new_space() does not clear its backing arena.
     */
    const uint64_t direct_store_out =
        entry_mmio_store32_sequence(
            NEMU_VGACTL_TEST_MMIO,
            NEMU_VGACTL_INITIAL_DIRECT_PATTERN);
    const uint64_t direct_store_readback =
        entry_mmio_load32_sequence(NEMU_VGACTL_TEST_MMIO);
    /*
     * The preceding eight-byte setup leaves a non-zero BLIT_POS word adjacent
     * to BLIT_SRC. Check it before resetting the command operands: an emitter
     * which accidentally widened SW to a host qword store would zero this word
     * because the guest source argument is zero-extended.
     */
    const uint64_t direct_store_adjacent_readback =
        entry_mmio_load32_sequence(NEMU_VGACTL_BLIT_POS_MMIO);
    const uint64_t position_store_out =
        entry_mmio_store32_sequence(NEMU_VGACTL_BLIT_POS_MMIO, 0);
    const uint64_t size_store_out =
        entry_mmio_store32_sequence(NEMU_VGACTL_BLIT_SIZE_MMIO, 0);
    const uint64_t command_store_out =
        entry_mmio_store32_sequence(
            NEMU_VGACTL_BLIT_CMD_MMIO, NEMU_VGACTL_BLIT_CMD_COPY);
    const uint64_t command_readback =
        entry_mmio_load32_sequence(NEMU_VGACTL_BLIT_CMD_MMIO);

    /*
     * Use several stores so a host timer signal arriving inside one helper call
     * cannot turn this continuation test into a one-sample race. A genuine CPU
     * boundary may consume one attempt; the remaining calls must still prove
     * that ordinary MMIO returns to the following native instruction.
     */
    for (uint32_t attempt = 0; attempt < 16u; attempt++)
    {
        mmio_store_out =
            entry_mmio_store_sequence(NEMU_RTC_MMIO, 0x55u);
        check(mmio_store_out == 5);
    }

    /*
     * Serial output is deliberately non-idempotent and host-visible. The gate
     * counts this otherwise-unused marker, proving that one guest SB invokes
     * the device callback exactly once rather than merely balancing JIT stats.
     */
    const uint64_t serial_store_out =
        entry_mmio_store_sequence(NEMU_SERIAL_MMIO, '~');
    const uint64_t serial_newline_out =
        entry_mmio_store_sequence(NEMU_SERIAL_MMIO, '\n');

    memory_entry_saved_mcause = 0;
    memory_entry_saved_mtval = 0;
    memory_entry_restore_mtvec = read_mtvec();
    write_mtvec((uintptr_t)memory_entry_trap_handler);
    const uint64_t misaligned_load = entry_misaligned_load_sequence(bad_load);

    check(loaded == 0x123456789abcdef7ull);
    check(memory_entry_data[1] == marker);
    check(store_out == ((marker + 3ull) ^ marker));
    check(mmio_load != 0xffffffffffffffffull);
    check(mmio_shared_direct_load == mmio_width_pattern);
    /*
     * The scripted queue contains MOVE followed by WHEEL. No callback would
     * leave zero, exactly one returns MOVE (1), and two callbacks would return
     * WHEEL (4), so this checks that the device read occurs exactly once.
     */
    check(mmio_once_load == 1ull);
    check(mmio_width_loads[0] == 0xffffffffffffffc3ull);
    check(mmio_width_loads[1] == 0x00000000000000c3ull);
    check(mmio_width_loads[2] == 0xffffffffffffd4c3ull);
    check(mmio_width_loads[3] == 0x000000000000d4c3ull);
    check(mmio_width_loads[4] == 0xfffffffff0e2d4c3ull);
    check(mmio_width_loads[5] == 0x00000000f0e2d4c3ull);
    check(mmio_width_loads[6] == 0x88776655f0e2d4c3ull);
    check(mmio_spill_load == 0x00000000f0e2d5aaull);
    check(direct_store_out == 5);
    check(direct_store_readback == NEMU_VGACTL_INITIAL_DIRECT_PATTERN);
    check(direct_store_adjacent_readback ==
          (uint32_t)(mmio_width_pattern >> 32));
    check(position_store_out == 5);
    check(size_store_out == 5);
    check(command_store_out == 5);
    check(command_readback == 0);
    check(mmio_store_out == 5);
    check(serial_store_out == 5);
    check(serial_newline_out == 5);
    check(memory_entry_saved_mcause == 4u);
    check(memory_entry_saved_mtval == bad_load);
    check(misaligned_load == bad_load);
}

/*
 * Exercise a monomorphic route cache with address changes at the same guest
 * PC. Every cold or unsafe arm already has a correct implementation, so this
 * remains an architectural test before the cache exists; the statistics gate
 * supplies the initial RED assertion for warm hits, misses, and fills.
 */
static void test_direct_mmio_route_cache(void)
{
    const uint32_t store_a = 0x10213243u;
    const uint32_t store_b = 0x54657687u;
    const uint32_t store_c = 0x98a9bacbu;
    const uint32_t store_d = 0xdcedfe0fu;
    const uint32_t store_e = 0x13579bdfu;
    const uint32_t store_f = 0x2468ace0u;
    const uint32_t store_g = 0xf0e1d2c3u;
    const uint32_t pmem_store = 0x11223344u;
    const uint32_t misaligned_store_data = 0xdeadbeefu;
    const uint64_t sd_pattern = 0x8877665544332211ull;
    const uintptr_t bad_load = NEMU_VGACTL_TEST_MMIO + 1u;
    const uintptr_t bad_store = NEMU_VGACTL_TEST_MMIO + 1u;

    local_fence_i();

    /*
     * A callback-backed mouse read sits between direct accesses at the same
     * LWU PC. The existing test consumed MOVE=1; exactly one new callback must
     * consume WHEEL=4. Zero or a later event exposes a skipped or duplicated
     * device effect.
     */
    const uint64_t cold_src =
        entry_mmio_route_load_sequence(NEMU_VGACTL_TEST_MMIO);
    const uint64_t warm_src =
        entry_mmio_route_load_sequence(NEMU_VGACTL_TEST_MMIO);
    const uint64_t mouse_once =
        entry_mmio_route_load_sequence(NEMU_MOUSE_MMIO);
    const uint64_t src_after_helper =
        entry_mmio_route_load_sequence(NEMU_VGACTL_TEST_MMIO);
    const uint32_t pmem_base_expected =
        *(volatile uint32_t *)(uintptr_t)NEMU_PMEM_BASE;
    const uint64_t pmem_base_load =
        entry_mmio_route_load_sequence(NEMU_PMEM_BASE);
    const uint64_t pmem_base_seed_guard =
        entry_mmio_route_seed_guard_sequence(
            NEMU_VGACTL_TEST_MMIO, NEMU_PMEM_BASE);
    const uint64_t pmem_load =
        entry_mmio_route_load_sequence(
            (uintptr_t)&memory_entry_route_data);
    const uint64_t src_after_pmem =
        entry_mmio_route_load_sequence(NEMU_VGACTL_TEST_MMIO);

    memory_entry_saved_mcause = 0;
    memory_entry_saved_mtval = 0;
    memory_entry_restore_mtvec = read_mtvec();
    write_mtvec((uintptr_t)memory_entry_trap_handler);
    const uint64_t misaligned_load =
        entry_mmio_route_load_sequence(bad_load);
    const uint64_t load_mcause = memory_entry_saved_mcause;
    const uint64_t load_mtval = memory_entry_saved_mtval;
    const uint64_t src_after_bad_load =
        entry_mmio_route_load_sequence(NEMU_VGACTL_TEST_MMIO);
    const uint64_t info_expected =
        entry_mmio_load32_sequence(NEMU_VGACTL_MMIO);
    const uint64_t info_via_changed_route =
        entry_mmio_route_load_sequence(NEMU_VGACTL_MMIO);
    const uint64_t src_after_direct_change =
        entry_mmio_route_load_sequence(NEMU_VGACTL_TEST_MMIO);

    check(cold_src == NEMU_VGACTL_INITIAL_DIRECT_PATTERN);
    check(warm_src == NEMU_VGACTL_INITIAL_DIRECT_PATTERN);
    check(mouse_once == 4u);
    check(src_after_helper == NEMU_VGACTL_INITIAL_DIRECT_PATTERN);
    check(pmem_base_load == pmem_base_expected);
    check(pmem_base_seed_guard == pmem_base_expected);
    check(pmem_load == (uint32_t)memory_entry_route_data);
    check(src_after_pmem == NEMU_VGACTL_INITIAL_DIRECT_PATTERN);
    check(load_mcause == 4u);
    check(load_mtval == bad_load);
    check(misaligned_load == bad_load);
    check(src_after_bad_load == NEMU_VGACTL_INITIAL_DIRECT_PATTERN);
    check(info_via_changed_route == info_expected);
    check(src_after_direct_change == NEMU_VGACTL_INITIAL_DIRECT_PATTERN);

    /*
     * The same SW PC alternates direct MMIO, PMEM, BLIT_CMD, and a misaligned
     * address. A helper miss must not poison the direct entry, and a PMEM
     * access must remain on the unchanged ordinary-memory path.
     */
    check(entry_mmio_route_store_sequence(
              NEMU_VGACTL_TEST_MMIO, store_a) ==
          (uint64_t)store_a + 5u);
    check(entry_mmio_route_store_sequence(
              NEMU_VGACTL_TEST_MMIO, store_b) ==
          (uint64_t)store_b + 5u);
    const uint64_t observed_store_b =
        entry_mmio_route_load_sequence(NEMU_VGACTL_TEST_MMIO);

    check(entry_mmio_route_store_sequence(
              (uintptr_t)&memory_entry_route_data, pmem_store) ==
          (uint64_t)pmem_store + 5u);
    check(memory_entry_route_data == 0xfedcba9811223344ull);

    check(entry_mmio_route_store_sequence(
              NEMU_VGACTL_TEST_MMIO, store_c) ==
          (uint64_t)store_c + 5u);
    check(entry_mmio_route_store_sequence(
              NEMU_VGACTL_BLIT_CMD_MMIO,
              NEMU_VGACTL_BLIT_CMD_COPY) ==
          (uint64_t)NEMU_VGACTL_BLIT_CMD_COPY + 5u);
    const uint64_t command_readback =
        entry_mmio_load32_sequence(NEMU_VGACTL_BLIT_CMD_MMIO);
    check(entry_mmio_route_store_sequence(
              NEMU_VGACTL_TEST_MMIO, store_d) ==
          (uint64_t)store_d + 5u);

    memory_entry_saved_mcause = 0;
    memory_entry_saved_mtval = 0;
    memory_entry_restore_mtvec = read_mtvec();
    write_mtvec((uintptr_t)memory_entry_trap_handler);
    const uint64_t misaligned_store =
        entry_mmio_route_store_sequence(
            bad_store, misaligned_store_data);
    const uint64_t store_mcause = memory_entry_saved_mcause;
    const uint64_t store_mtval = memory_entry_saved_mtval;
    const uint64_t full_after_bad_store =
        entry_mmio_load_sequence(NEMU_VGACTL_TEST_MMIO);

    check(entry_mmio_route_store_sequence(
              NEMU_VGACTL_TEST_MMIO, store_e) ==
          (uint64_t)store_e + 5u);

    /*
     * Recompile the same source address with an unsupported width. SD spans
     * BLIT_SRC and BLIT_POS and must use the helper. Restoring SW must create a
     * fresh valid seed for the new block rather than reuse the old generation's
     * sidecar.
     */
    entry_mmio_route_store_code[0] = ENTRY_MMIO_ROUTE_STORE_SD;
    local_fence_i();
    check(entry_mmio_route_store_sequence(
              NEMU_VGACTL_TEST_MMIO, sd_pattern) ==
          sd_pattern + 5u);
    const uint64_t full_after_sd =
        entry_mmio_load_sequence(NEMU_VGACTL_TEST_MMIO);

    entry_mmio_route_store_code[0] = ENTRY_MMIO_ROUTE_STORE_SW;
    local_fence_i();
    check(entry_mmio_route_store_sequence(
              NEMU_VGACTL_TEST_MMIO, store_f) ==
          (uint64_t)store_f + 5u);
    check(entry_mmio_route_store_sequence(
              NEMU_VGACTL_TEST_MMIO, store_g) ==
          (uint64_t)store_g + 5u);
    const uint64_t observed_store_g =
        entry_mmio_route_load_sequence(NEMU_VGACTL_TEST_MMIO);

    check(observed_store_b == store_b);
    check(command_readback == 0u);
    check(store_mcause == 6u);
    check(store_mtval == bad_store);
    check(misaligned_store ==
          (uint64_t)misaligned_store_data + 5u);
    check(full_after_bad_store == store_d);
    check(full_after_sd == sd_pattern);
    check(observed_store_g == store_g);
}

#endif

/* Keep the source buildable outside RV64 while exercising the RV64-only path. */
int main(void)
{
#if defined(__riscv) && __riscv_xlen == 64
    test_memory_entry_and_store_continue();
    test_direct_mmio_route_cache();
#endif

    return 0;
}
