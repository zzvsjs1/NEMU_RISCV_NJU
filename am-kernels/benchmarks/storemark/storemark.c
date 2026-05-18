#include <am.h>
#include <klib.h>
#include <stdint.h>

#define STOREMARK_ITERS 1000000u
#define STOREMARK_WORDS 1024u
#define STOREMARK_EXPECTED 0xed97aa2eu

/* Read AM uptime in microseconds for a compact sanity timing line. */
static uint64_t uptime_us(void)
{
    return io_read(AM_TIMER_UPTIME).us;
}

/*
 * Run a hot loop that performs ordinary PMEM stores.  The data array is far from
 * the text section, so these stores should not invalidate translated source.
 */
__attribute__((noinline)) static uint32_t store_hot_loop(uint32_t iters)
{
    uintptr_t heap_addr = ((uintptr_t)heap.start + 7u) & ~(uintptr_t)7u;
    volatile uint64_t *storemark_buf = (volatile uint64_t *)heap_addr;
    uint64_t acc = 0x123456789abcdef0ull;

    for (uint32_t i = 0; i < iters; i++)
    {
        uint32_t idx = i & (STOREMARK_WORDS - 1u);
        acc = (acc * 0x9e3779b97f4a7c15ull) ^ (uint64_t)i;
        storemark_buf[idx] = acc;
    }

    uint64_t folded = acc;
    for (uint32_t i = 0; i < STOREMARK_WORDS; i++)
    {
        folded ^= storemark_buf[i] + ((uint64_t)i << 32);
    }

    return (uint32_t)(folded ^ (folded >> 32));
}

int main(const char *args)
{
    (void)args;
    ioe_init();

    uint64_t start = uptime_us();
    uint32_t checksum = store_hot_loop(STOREMARK_ITERS);
    uint64_t end = uptime_us();
    int pass = checksum == STOREMARK_EXPECTED;

    printf("storemark_total_us: %d\n", (int)(end - start));
    printf("storemark_checksum: 0x%x\n", checksum);
    printf("StoreMark %s\n", pass ? "PASS" : "FAIL");

    return pass ? 0 : 1;
}
