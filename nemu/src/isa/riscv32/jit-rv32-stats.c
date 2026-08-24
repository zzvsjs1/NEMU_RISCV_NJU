#include <generated/autoconf.h>

#ifndef CONFIG_RV64

#include "jit-rv32-internal.h"

/*
 * RV32 JIT statistics presentation. Counters remain owned by core and are
 * updated at their existing run-time or compilation sites.
 */

#if RV32_JIT_STATS
/* Compute a rounded fixed-point ratio with two decimal digits. */
static uint64_t jit_ratio_x100(uint64_t numerator, uint64_t denominator)
{
    if (denominator == 0)
    {
        return 0;
    }

    return (numerator * 100u + denominator / 2u) / denominator;
}

/* Compute a rounded fixed-point percentage with two decimal digits. */
static uint64_t jit_percent_x100(uint64_t numerator, uint64_t denominator)
{
    if (denominator == 0)
    {
        return 0;
    }

    return (numerator * 10000u + denominator / 2u) / denominator;
}
#endif

void rv32_jit_dump_stats_report(void)
{
#if RV32_JIT_STATS
    const uint64_t cache_total = rv32_jit_stats.cache_hits + rv32_jit_stats.cache_misses;
    const uint64_t cache_hit_pct = jit_percent_x100(rv32_jit_stats.cache_hits, cache_total);
    const uint64_t avg_compile_len = jit_ratio_x100(rv32_jit_stats.compiled_insns, rv32_jit_stats.blocks_compiled);
    const uint64_t avg_exec_len = jit_ratio_x100(rv32_jit_stats.executed_insns, rv32_jit_stats.blocks_executed);
    const uint64_t load_direct_pct = jit_percent_x100(rv32_jit_stats.helper_load_direct, rv32_jit_stats.helper_loads);
    const uint64_t store_direct_pct = jit_percent_x100(rv32_jit_stats.helper_store_direct, rv32_jit_stats.helper_stores);

    Log("jit: exec requests = %" PRIu64 ", cache hits = %" PRIu64 ", misses = %" PRIu64 ", hit rate = %" PRIu64 ".%02" PRIu64 "%%",
        rv32_jit_stats.exec_requests, rv32_jit_stats.cache_hits, rv32_jit_stats.cache_misses, cache_hit_pct / 100u, cache_hit_pct % 100u);
    Log("jit: compiled blocks = %" PRIu64 ", unsupported blocks = %" PRIu64 ", unsupported FP blocks = %" PRIu64
        ", native prefixes before FP = %" PRIu64 ", avg compiled length = %" PRIu64 ".%02" PRIu64 " insn",
        rv32_jit_stats.blocks_compiled, rv32_jit_stats.blocks_unsupported, rv32_jit_stats.fp_blocks_unsupported,
        rv32_jit_stats.native_prefixes_before_fp, avg_compile_len / 100u, avg_compile_len % 100u);
    Log("jit: executed blocks = %" PRIu64 ", JIT instructions = %" PRIu64 ", avg executed block = %" PRIu64 ".%02" PRIu64 " insn"
        ", unsupported hits = %" PRIu64,
        rv32_jit_stats.blocks_executed, rv32_jit_stats.executed_insns, avg_exec_len / 100u, avg_exec_len % 100u, rv32_jit_stats.unsupported_hits);
    Log("jit: helper loads = %" PRIu64 " (%" PRIu64 ".%02" PRIu64 "%% direct PMEM), stores = %" PRIu64 " (%" PRIu64 ".%02" PRIu64
        "%% direct PMEM), complex ops = %" PRIu64,
        rv32_jit_stats.helper_loads, load_direct_pct / 100u, load_direct_pct % 100u, rv32_jit_stats.helper_stores, store_direct_pct / 100u,
        store_direct_pct % 100u, rv32_jit_stats.helper_complex_ops);
    Log("jit: FP helper sites = %" PRIu64 ", calls = %" PRIu64 ", continuations = %" PRIu64 ", trap exits = %" PRIu64 ", memory exits = %" PRIu64,
        rv32_jit_stats.fp_helper_sites, rv32_jit_stats.fp_helper_calls, rv32_jit_stats.fp_helper_continuations, rv32_jit_stats.fp_helper_trap_exits,
        rv32_jit_stats.fp_helper_memory_exits);
    Log("jit: invalidation requests = %" PRIu64 ", page-filter skips = %" PRIu64 ", invalidated blocks = %" PRIu64 ", arena resets = %" PRIu64,
        rv32_jit_stats.invalidation_requests, rv32_jit_stats.invalidation_page_skips, rv32_jit_stats.invalidated_blocks, rv32_jit_stats.arena_resets);
#endif
}

#endif /* !CONFIG_RV64 */
