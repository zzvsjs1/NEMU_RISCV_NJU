/*
 * Link this fixture against the normal statistics-enabled NEMU objects, omitting
 * nemu-main.o and wrapping rv64_jit_emit_load_instr and rv64_jit_perf_map_publish.
 * Every load is emitted by the real backend before the wrapper injects a failure.  This
 * exercises rollback, retry and publication without a production testing knob.
 */
#include "../../nemu/src/isa/riscv32/jit-rv64-internal.h"
#include <errno.h>
#include <time.h>

#ifdef CONFIG_DEVICE
#include <device/map.h>
#endif

#if !RV64_JIT_ENABLED || !RV64_JIT_STATS
#error "The compilation lifecycle fixture requires native RV64 JIT statistics"
#endif

void init_mem(void);

bool __real_rv64_jit_emit_load_instr(rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs, uint32_t instr, vaddr_t pc,
                                     uint32_t completed_count, rv64_jit_mmio_route_builder_t *mmio_routes);
void __real_rv64_jit_perf_map_publish(const rv64_jit_block_t *block, const uint8_t *native_start, size_t native_size);

typedef enum
{
    EMIT_NORMALLY,
    REJECT_FIRST_LOAD,
    REJECT_SECOND_LOAD,
    EXHAUST_ONCE,
    EXHAUST_BOTH_ATTEMPTS,
} injection_mode_t;

enum
{
    FIXTURE_LOAD_COUNT = 4,
    FIXTURE_ADDRESS_REG = 10,
    FIXTURE_RESULT_REG = 11,
};

static injection_mode_t injection_mode;
static uint32_t attempts;
static uint32_t emitted_loads;
static uint32_t injected_failures;
static uint64_t previous_publication;
static uint64_t initial_epoch;
static uint64_t initial_ifetch_generation;
static rv64_jit_emitted_site_stats_t complete_sites;
static const uint64_t load_value = UINT64_C(0x123456789abcdef0);
static size_t published_native_bytes;

/* Observe the existing publication boundary without guessing sidecar padding. */
void __wrap_rv64_jit_perf_map_publish(const rv64_jit_block_t *block, const uint8_t *native_start, size_t native_size)
{
    published_native_bytes = native_size;
    __real_rv64_jit_perf_map_publish(block, native_start, native_size);
}

bool __wrap_rv64_jit_emit_load_instr(rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs, uint32_t instr, vaddr_t pc,
                                     uint32_t completed_count, rv64_jit_mmio_route_builder_t *mmio_routes)
{
    if (pc == RESET_VECTOR)
    {
        attempts++;
    }

    assert(__real_rv64_jit_emit_load_instr(w, regs, instr, pc, completed_count, mmio_routes));
    emitted_loads++;

    const bool reject = (injection_mode == REJECT_FIRST_LOAD && completed_count == 0) ||
                        (injection_mode == REJECT_SECOND_LOAD && completed_count == 1);
    const bool exhaust = completed_count == 1 &&
                         (injection_mode == EXHAUST_BOTH_ATTEMPTS || (injection_mode == EXHAUST_ONCE && injected_failures == 0));

    if (!reject && !exhaust)
    {
        return true;
    }

    injected_failures++;

    if (exhaust)
    {
        /* The overflow latch belongs to the attempt, beyond instruction rollback. */
        w->cur = w->end;
        w->overflowed = true;
    }

    return false;
}

static void begin_case(injection_mode_t mode)
{
    rv64_jit_arena_reset();
    init_isa();
    const paddr_t data_address = RESET_VECTOR + PAGE_SIZE;
    const uint32_t load = (FIXTURE_ADDRESS_REG << 15) | (RV64_FUNCT3_LD << 12) | (FIXTURE_RESULT_REG << 7) | RISCV_OPCODE_LOAD;

    /* Four LD a1,0(a0) instructions keep the real source and register paths simple. */
    for (uint32_t i = 0; i < FIXTURE_LOAD_COUNT; i++)
    {
        paddr_write(RESET_VECTOR + i * RISCV_BASE_INSN_BYTES, RISCV_BASE_INSN_BYTES, load);
    }

    paddr_write(data_address, sizeof(load_value), load_value);
    cpu.gpr[FIXTURE_ADDRESS_REG]._64 = data_address;
    cpu.gpr[FIXTURE_RESULT_REG]._64 = 0;
    injection_mode = mode;
    attempts = 0;
    emitted_loads = 0;
    injected_failures = 0;
    initial_epoch = rv64_jit_native_cache_epoch;
    initial_ifetch_generation = rv64_jit_ifetch_generation;
    memset(&rv64_jit_stats, 0, sizeof(rv64_jit_stats));
}

/* A publication must be executable, own its source, and consume exactly its prefix. */
static void check_published(rv64_jit_block_t *block, uint32_t instructions)
{
    assert(block != NULL && block->valid && block->entry != NULL);
    assert(block->insn_count == instructions);
    assert(block->source_len == instructions * RISCV_BASE_INSN_BYTES);
    assert(rv64_jit_stats.blocks_compiled == 1 && rv64_jit_stats.compiled_insns == instructions);
    assert(rv64_jit_stats.blocks_unsupported == 0);
    assert(rv64_jit_stats.emitted_sites.native_loads == instructions);
    assert(rv64_jit_write_may_touch_source_chunk(RESET_VECTOR, RISCV_BASE_INSN_BYTES));
    assert(rv64_jit_code_used != 0);

    /* Failed attempts and arena epochs must not consume publication identities. */
    assert(block->generation != 0);

    if (previous_publication != 0)
    {
        assert(block->generation == previous_publication + 1);
    }

    previous_publication = block->generation;
    rv64_jit_entry_budget = FIXTURE_LOAD_COUNT;
    rv64_jit_loop_extra = 0;
    rv64_jit_cpu_boundary_requested = false;
    assert(block->entry() == instructions);
    assert(cpu.gpr[FIXTURE_RESULT_REG]._64 == load_value);
    assert(cpu.pc == RESET_VECTOR + instructions * RISCV_BASE_INSN_BYTES);
    assert(rv64_jit_ifetch_generation == initial_ifetch_generation);
}

static uint64_t monotonic_ns(void)
{
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

/* Measure the real compiler and whole-arena reset separately; execute no guest. */
static void benchmark_compilation(unsigned long iterations)
{
    begin_case(EMIT_NORMALLY);
    rv64_jit_block_t *warmup = rv64_jit_compile_block(RESET_VECTOR, FIXTURE_LOAD_COUNT);
    assert(warmup != NULL && warmup->valid && warmup->insn_count == FIXTURE_LOAD_COUNT);
    const size_t native_bytes = published_native_bytes;
    const size_t allocation_bytes = rv64_jit_code_used;
    rv64_jit_arena_reset();
    memset(&rv64_jit_stats, 0, sizeof(rv64_jit_stats));
    uint64_t compilation_ns = 0;
    uint64_t reset_ns = 0;

    for (unsigned long i = 0; i < iterations; i++)
    {
        const uint64_t compile_start = monotonic_ns();
        rv64_jit_block_t *block = rv64_jit_compile_block(RESET_VECTOR, FIXTURE_LOAD_COUNT);
        compilation_ns += monotonic_ns() - compile_start;
        assert(block != NULL && block->valid && block->insn_count == FIXTURE_LOAD_COUNT);
        assert(published_native_bytes == native_bytes && rv64_jit_code_used == allocation_bytes);
        const uint64_t reset_start = monotonic_ns();
        rv64_jit_arena_reset();
        reset_ns += monotonic_ns() - reset_start;
    }

    assert(rv64_jit_stats.blocks_compiled == iterations && rv64_jit_stats.arena_resets == iterations);
    printf("compile-benchmark iterations=%lu compilation_ns=%" PRIu64 " reset_ns=%" PRIu64
           " native_bytes=%zu allocation_bytes=%zu guest_executions=0\n",
           iterations, compilation_ns, reset_ns, native_bytes, allocation_bytes);
}

int main(int argc, char **argv)
{
    unsigned long benchmark_iterations = 0;

    if (argc == 3 && strcmp(argv[1], "--benchmark") == 0)
    {
        char *end = NULL;
        errno = 0;
        benchmark_iterations = strtoul(argv[2], &end, 10);
        assert(errno == 0 && end != argv[2] && *end == '\0' && benchmark_iterations > 0);
        assert(benchmark_iterations < UINT32_MAX / FIXTURE_LOAD_COUNT);
    }
    else
    {
        assert(argc == 1);
    }

    init_mem();
    init_isa();

#ifdef CONFIG_DEVICE
    /* This PMEM-only guest has no devices; freeze the valid empty direct-route table. */
    mmio_freeze_direct_routes();
#endif

    assert(rv64_jit_code_init());

    if (benchmark_iterations != 0)
    {
        benchmark_compilation(benchmark_iterations);
        return 0;
    }

    begin_case(EMIT_NORMALLY);
    check_published(rv64_jit_compile_block(RESET_VECTOR, FIXTURE_LOAD_COUNT), FIXTURE_LOAD_COUNT);
    complete_sites = rv64_jit_stats.emitted_sites;
    assert(attempts == 1 && emitted_loads == 4 && injected_failures == 0);
    assert(rv64_jit_stats.arena_resets == 0 && rv64_jit_native_cache_epoch == initial_epoch);

    begin_case(REJECT_FIRST_LOAD);
    assert(rv64_jit_compile_block(RESET_VECTOR, FIXTURE_LOAD_COUNT) == NULL);
    rv64_jit_block_t *negative = rv64_jit_cache_slot(RESET_VECTOR);
    const rv64_jit_emitted_site_stats_t no_sites = {0};
    assert(attempts == 1 && emitted_loads == 1 && injected_failures == 1);
    assert(negative->valid && negative->entry == NULL && negative->generation == 0);
    assert(negative->source_len == RISCV_BASE_INSN_BYTES);
    assert(rv64_jit_stats.blocks_compiled == 0 && rv64_jit_stats.blocks_unsupported == 1);
    assert(memcmp(&rv64_jit_stats.emitted_sites, &no_sites, sizeof(no_sites)) == 0);
    assert(rv64_jit_write_may_touch_source_chunk(RESET_VECTOR, RISCV_BASE_INSN_BYTES));

    /* A rewritten unsupported instruction must lose its negative cache marker. */
    paddr_write(RESET_VECTOR, RISCV_BASE_INSN_BYTES, UINT32_C(0x00000013)); /* ADDI x0,x0,0 */
    assert(!negative->valid);
    assert(!rv64_jit_write_may_touch_source_chunk(RESET_VECTOR, RISCV_BASE_INSN_BYTES));
    assert(rv64_jit_stats.invalidated_blocks == 1);
    assert(rv64_jit_native_cache_epoch == initial_epoch + 1);

    begin_case(REJECT_SECOND_LOAD);
    check_published(rv64_jit_compile_block(RESET_VECTOR, FIXTURE_LOAD_COUNT), 1);
    assert(attempts == 1 && emitted_loads == 2 && injected_failures == 1);
    assert(rv64_jit_stats.arena_resets == 0 && rv64_jit_native_cache_epoch == initial_epoch);

    begin_case(EXHAUST_ONCE);
    check_published(rv64_jit_compile_block(RESET_VECTOR, FIXTURE_LOAD_COUNT), FIXTURE_LOAD_COUNT);
    assert(attempts == 2 && emitted_loads == 6 && injected_failures == 1);
    assert(rv64_jit_stats.arena_resets == 1 && rv64_jit_native_cache_epoch == initial_epoch + 1);
    assert(rv64_jit_stats.unsupported_by_opcode[RISCV_OPCODE_LOAD] == 0);
    assert(memcmp(&rv64_jit_stats.emitted_sites, &complete_sites, sizeof(complete_sites)) == 0);

    begin_case(EXHAUST_BOTH_ATTEMPTS);
    assert(rv64_jit_compile_block(RESET_VECTOR, FIXTURE_LOAD_COUNT) == NULL);
    assert(attempts == 2 && emitted_loads == 4 && injected_failures == 2);
    assert(rv64_jit_stats.arena_resets == 1 && rv64_jit_native_cache_epoch == initial_epoch + 1);
    assert(rv64_jit_stats.blocks_compiled == 0 && rv64_jit_stats.blocks_unsupported == 0);
    assert(rv64_jit_stats.unsupported_by_opcode[RISCV_OPCODE_LOAD] == 0);
    assert(memcmp(&rv64_jit_stats.emitted_sites, &no_sites, sizeof(no_sites)) == 0);
    assert(!rv64_jit_cache_slot(RESET_VECTOR)->valid && rv64_jit_code_used == 0);
    assert(!rv64_jit_write_may_touch_source_chunk(RESET_VECTOR, RISCV_BASE_INSN_BYTES));

    /* A later attempt is still eligible, and gets the next publication identity. */
    begin_case(EMIT_NORMALLY);
    check_published(rv64_jit_compile_block(RESET_VECTOR, FIXTURE_LOAD_COUNT), FIXTURE_LOAD_COUNT);
    puts("RV64 JIT compilation lifecycle: normal, negative invalidation, prefix, one retry, bounded failure and recovery passed");
    return 0;
}
