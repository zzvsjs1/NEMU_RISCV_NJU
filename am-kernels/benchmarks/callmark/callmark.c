#include <am.h>
#include <klib.h>
#include <stdint.h>

#ifndef CALLMARK_WARMUP
#define CALLMARK_WARMUP 4096u
#endif

#ifndef CALLMARK_ITERS
#define CALLMARK_ITERS 2000000u
#endif

#define CALLMARK_INITIAL UINT64_C(0x0123456789abcdef)
#define CALLMARK_EXPECTED UINT64_C(0x13757f4d8594a65f)

typedef uint64_t (*callmark_fn_t)(uint64_t, uint64_t);

static volatile uint64_t callmark_warmup_sink;

/* Read AM uptime in microseconds for guest-visible benchmark timing. */
static uint64_t uptime_us(void)
{
    return io_read(AM_TIMER_UPTIME).us;
}

/*
 * Keep a small amount of register-only work around each return.  Unsigned
 * arithmetic gives defined modulo-2^64 wrapping and a stable final checksum.
 */
__attribute__((noinline)) static uint64_t callmark_leaf(uint64_t x, uint64_t iteration)
{
    x ^= x >> 13;
    x += iteration ^ UINT64_C(0x9e3779b97f4a7c15);
    return (x << 9) | (x >> 55);
}

/*
 * The extra wrapper creates a second hot canonical return per iteration while
 * keeping the leaf call direct and easy to identify in the disassembly.
 */
__attribute__((noinline)) static uint64_t callmark_wrapper(uint64_t x, uint64_t iteration)
{
    uint64_t value = callmark_leaf(x + UINT64_C(0xd1b54a32d192ed03), iteration);
    return value ^ UINT64_C(0xa5a5a5a55a5a5a5a);
}

/*
 * A volatile function-pointer load prevents the indirect call from becoming a
 * direct JAL.  Each lap consequently executes one indirect call and two RETs.
 */
static callmark_fn_t volatile callmark_target = callmark_wrapper;

__attribute__((noinline)) static uint64_t run_callmark(uint64_t count)
{
    callmark_fn_t fn = callmark_target;
    uint64_t accumulator = CALLMARK_INITIAL;

    for (uint64_t iteration = 0; iteration < count; iteration++)
    {
        accumulator = fn(accumulator, iteration);
    }

    return accumulator;
}

/* Warm recurring blocks before timing, then print machine-readable results. */
int main(const char *args)
{
    (void)args;
    ioe_init();

    printf("======= Running CallMark =======\n");

    callmark_warmup_sink = run_callmark(CALLMARK_WARMUP);

    uint64_t start = uptime_us();
    uint64_t result = run_callmark(CALLMARK_ITERS);
    uint64_t end = uptime_us();
    int elapsed_us = (int)(end - start);
    int pass = result == CALLMARK_EXPECTED;

    printf("callmark_total_us: %d\n", elapsed_us);
    printf("callmark_checksum: 0x%llx\n", (unsigned long long)result);
    printf("CallMark %s\n", pass ? "PASS" : "FAIL");

    return pass ? 0 : 1;
}
