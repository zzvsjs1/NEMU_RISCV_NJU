/*
 * Host-only report fixture. Including the implementation keeps private report
 * metadata testable without exporting it to the JIT or parsing C source text.
 * This process never emits or executes native code: its synthetic statistics
 * object cannot become a live generated-code counter target.
 */
#include "../../nemu/src/isa/riscv32/jit-rv64-internal.h"

#include <assert.h>
#include <stdio.h>

/* Compare the report payload; source locations and terminal colours are not
 * statistics output. Retain the real format strings and C format checking. */
#undef Log
#define Log(format, ...) printf(format "\n", ##__VA_ARGS__)
#undef Assert
#define Assert(condition, ...) assert(condition)

#include "../../nemu/src/isa/riscv32/jit-rv64-stats.c"

rv64_jit_stats_t rv64_jit_stats;

/* This member list is independent of report metadata. Distinct seeded values
 * reveal swapped mappings across primary counters, including fields which only
 * occur in compound legacy lines. New storage must be deliberately added here. */
static const struct
{
    const char *identity;
    uint64_t *value;
    rv64_jit_metric_category_t category;
} expected_scalar_counters[] = {
    {"exec_requests", &rv64_jit_stats.exec_requests,
     RV64_JIT_METRIC_RUN_TIME},
    {"cache_hits", &rv64_jit_stats.cache_hits,
     RV64_JIT_METRIC_RUN_TIME},
    {"cache_misses", &rv64_jit_stats.cache_misses,
     RV64_JIT_METRIC_RUN_TIME},
    {"unsupported_hits", &rv64_jit_stats.unsupported_hits,
     RV64_JIT_METRIC_RUN_TIME},
    {"blocks_executed", &rv64_jit_stats.blocks_executed,
     RV64_JIT_METRIC_RUN_TIME},
    {"executed_insns", &rv64_jit_stats.executed_insns,
     RV64_JIT_METRIC_RUN_TIME},
    {"zero_side_exits", &rv64_jit_stats.zero_side_exits,
     RV64_JIT_METRIC_RUN_TIME},
    {"cpu_boundary_breaks", &rv64_jit_stats.cpu_boundary_breaks,
     RV64_JIT_METRIC_RUN_TIME},
    {"blocks_compiled", &rv64_jit_stats.blocks_compiled,
     RV64_JIT_METRIC_COMPILATION},
    {"blocks_unsupported", &rv64_jit_stats.blocks_unsupported,
     RV64_JIT_METRIC_COMPILATION},
    {"compiled_insns", &rv64_jit_stats.compiled_insns,
     RV64_JIT_METRIC_COMPILATION},
    {"translated_blocks", &rv64_jit_stats.translated_blocks,
     RV64_JIT_METRIC_COMPILATION},
    {"translated_cross_page_blocks", &rv64_jit_stats.translated_cross_page_blocks,
     RV64_JIT_METRIC_COMPILATION},
    {"segmented_source_blocks", &rv64_jit_stats.segmented_source_blocks,
     RV64_JIT_METRIC_COMPILATION},
    {"trace_blocks", &rv64_jit_stats.trace_blocks,
     RV64_JIT_METRIC_COMPILATION},
    {"trace_insns", &rv64_jit_stats.trace_insns,
     RV64_JIT_METRIC_COMPILATION},
    {"stable_loop_blocks", &rv64_jit_stats.stable_loop_blocks,
     RV64_JIT_METRIC_COMPILATION},
    {"stable_loop_preloaded_regs", &rv64_jit_stats.stable_loop_preloaded_regs,
     RV64_JIT_METRIC_COMPILATION},
    {"data_tlb_hits", &rv64_jit_stats.data_tlb_hits,
     RV64_JIT_METRIC_RUN_TIME},
    {"data_tlb_misses", &rv64_jit_stats.data_tlb_misses,
     RV64_JIT_METRIC_RUN_TIME},
    {"data_tlb_fills", &rv64_jit_stats.data_tlb_fills,
     RV64_JIT_METRIC_RUN_TIME},
    {"data_tlb_flushes", &rv64_jit_stats.data_tlb_flushes,
     RV64_JIT_METRIC_MAINTENANCE},
    {"data_tlb_page_table_flushes", &rv64_jit_stats.data_tlb_page_table_flushes,
     RV64_JIT_METRIC_MAINTENANCE},
    {"data_tlb_direct_loads", &rv64_jit_stats.data_tlb_direct_loads,
     RV64_JIT_METRIC_RUN_TIME},
    {"data_tlb_direct_stores", &rv64_jit_stats.data_tlb_direct_stores,
     RV64_JIT_METRIC_RUN_TIME},
    {"inline_paged_load_hits", &rv64_jit_stats.inline_paged_load_hits,
     RV64_JIT_METRIC_RUN_TIME},
    {"inline_paged_store_hits", &rv64_jit_stats.inline_paged_store_hits,
     RV64_JIT_METRIC_RUN_TIME},
    {"inline_direct_mmio_load_hits", &rv64_jit_stats.inline_direct_mmio_load_hits,
     RV64_JIT_METRIC_RUN_TIME},
    {"inline_direct_mmio_store_hits", &rv64_jit_stats.inline_direct_mmio_store_hits,
     RV64_JIT_METRIC_RUN_TIME},
    {"direct_mmio_load_route_hits", &rv64_jit_stats.direct_mmio_load_route_hits,
     RV64_JIT_METRIC_RUN_TIME},
    {"direct_mmio_load_route_misses", &rv64_jit_stats.direct_mmio_load_route_misses,
     RV64_JIT_METRIC_RUN_TIME},
    {"direct_mmio_load_route_fills", &rv64_jit_stats.direct_mmio_load_route_fills,
     RV64_JIT_METRIC_RUN_TIME},
    {"direct_mmio_store_route_hits", &rv64_jit_stats.direct_mmio_store_route_hits,
     RV64_JIT_METRIC_RUN_TIME},
    {"direct_mmio_store_route_misses", &rv64_jit_stats.direct_mmio_store_route_misses,
     RV64_JIT_METRIC_RUN_TIME},
    {"direct_mmio_store_route_fills", &rv64_jit_stats.direct_mmio_store_route_fills,
     RV64_JIT_METRIC_RUN_TIME},
    {"helper_load_count", &rv64_jit_stats.helper_load_count,
     RV64_JIT_METRIC_RUN_TIME},
    {"helper_store_count", &rv64_jit_stats.helper_store_count,
     RV64_JIT_METRIC_RUN_TIME},
    {"paged_store_helper_continuations", &rv64_jit_stats.paged_store_helper_continuations,
     RV64_JIT_METRIC_RUN_TIME},
    {"bare_mmio_load_calls", &rv64_jit_stats.bare_mmio_load_calls,
     RV64_JIT_METRIC_RUN_TIME},
    {"bare_mmio_store_calls", &rv64_jit_stats.bare_mmio_store_calls,
     RV64_JIT_METRIC_RUN_TIME},
    {"bare_mmio_store_continuations", &rv64_jit_stats.bare_mmio_store_continuations,
     RV64_JIT_METRIC_RUN_TIME},
    {"bare_mmio_store_boundary_exits", &rv64_jit_stats.bare_mmio_store_boundary_exits,
     RV64_JIT_METRIC_RUN_TIME},
    {"bare_mmio_store_invalidation_exits", &rv64_jit_stats.bare_mmio_store_invalidation_exits,
     RV64_JIT_METRIC_RUN_TIME},
    {"fp_helper_calls", &rv64_jit_stats.fp_helper_calls,
     RV64_JIT_METRIC_RUN_TIME},
    {"fp_helper_continuations", &rv64_jit_stats.fp_helper_continuations,
     RV64_JIT_METRIC_RUN_TIME},
    {"fp_helper_trap_exits", &rv64_jit_stats.fp_helper_trap_exits,
     RV64_JIT_METRIC_RUN_TIME},
    {"fp_helper_memory_exits", &rv64_jit_stats.fp_helper_memory_exits,
     RV64_JIT_METRIC_RUN_TIME},
    {"direct_link_taken_count", &rv64_jit_stats.direct_link_taken_count,
     RV64_JIT_METRIC_RUN_TIME},
    {"direct_link_miss_count", &rv64_jit_stats.direct_link_miss_count,
     RV64_JIT_METRIC_RUN_TIME},
    {"direct_branch_link_taken_count", &rv64_jit_stats.direct_branch_link_taken_count,
     RV64_JIT_METRIC_RUN_TIME},
    {"direct_guarded_link_taken_count", &rv64_jit_stats.direct_guarded_link_taken_count,
     RV64_JIT_METRIC_RUN_TIME},
    {"patched_direct_link_taken_count", &rv64_jit_stats.patched_direct_link_taken_count,
     RV64_JIT_METRIC_RUN_TIME},
    {"direct_link_patch_resolutions", &rv64_jit_stats.direct_link_patch_resolutions,
     RV64_JIT_METRIC_MAINTENANCE},
    {"direct_link_patch_unlinks", &rv64_jit_stats.direct_link_patch_unlinks,
     RV64_JIT_METRIC_MAINTENANCE},
    {"direct_return_link_taken_count", &rv64_jit_stats.direct_return_link_taken_count,
     RV64_JIT_METRIC_RUN_TIME},
    {"direct_return_link_miss_count", &rv64_jit_stats.direct_return_link_miss_count,
     RV64_JIT_METRIC_RUN_TIME},
    {"direct_jalr_link_taken_count", &rv64_jit_stats.direct_jalr_link_taken_count,
     RV64_JIT_METRIC_RUN_TIME},
    {"direct_jalr_link_miss_count", &rv64_jit_stats.direct_jalr_link_miss_count,
     RV64_JIT_METRIC_RUN_TIME},
    {"indirect_jump_cache_hits", &rv64_jit_stats.indirect_jump_cache_hits,
     RV64_JIT_METRIC_RUN_TIME},
    {"indirect_jump_cache_misses", &rv64_jit_stats.indirect_jump_cache_misses,
     RV64_JIT_METRIC_RUN_TIME},
    {"indirect_jump_cache_fills", &rv64_jit_stats.indirect_jump_cache_fills,
     RV64_JIT_METRIC_RUN_TIME},
    {"indirect_jump_cache_replacements", &rv64_jit_stats.indirect_jump_cache_replacements,
     RV64_JIT_METRIC_RUN_TIME},
    {"indirect_jump_cache_stale_rejections", &rv64_jit_stats.indirect_jump_cache_stale_rejections,
     RV64_JIT_METRIC_RUN_TIME},
    {"indirect_jump_cache_budget_rejections", &rv64_jit_stats.indirect_jump_cache_budget_rejections,
     RV64_JIT_METRIC_RUN_TIME},
    {"ifetch_generation_fast_hits", &rv64_jit_stats.ifetch_generation_fast_hits,
     RV64_JIT_METRIC_MAINTENANCE},
    {"ifetch_generation_revalidations", &rv64_jit_stats.ifetch_generation_revalidations,
     RV64_JIT_METRIC_MAINTENANCE},
    {"ifetch_generation_bumps", &rv64_jit_stats.ifetch_generation_bumps,
     RV64_JIT_METRIC_MAINTENANCE},
    {"ifetch_generation_avoided_bumps", &rv64_jit_stats.ifetch_generation_avoided_bumps,
     RV64_JIT_METRIC_MAINTENANCE},
    {"source_reverse_invalidations", &rv64_jit_stats.source_reverse_invalidations,
     RV64_JIT_METRIC_MAINTENANCE},
    {"source_full_invalidation_scans", &rv64_jit_stats.source_full_invalidation_scans,
     RV64_JIT_METRIC_MAINTENANCE},
    {"source_link_sequential_allocations", &rv64_jit_stats.source_link_sequential_allocations,
     RV64_JIT_METRIC_MAINTENANCE},
    {"source_link_recycled_allocations", &rv64_jit_stats.source_link_recycled_allocations,
     RV64_JIT_METRIC_MAINTENANCE},
    {"invalidation_requests", &rv64_jit_stats.invalidation_requests,
     RV64_JIT_METRIC_MAINTENANCE},
    {"invalidated_blocks", &rv64_jit_stats.invalidated_blocks,
     RV64_JIT_METRIC_MAINTENANCE},
    {"arena_resets", &rv64_jit_stats.arena_resets,
     RV64_JIT_METRIC_MAINTENANCE},
    {"emitted_sites.native_loads", &rv64_jit_stats.emitted_sites.native_loads,
     RV64_JIT_METRIC_EMITTED_SITES},
    {"emitted_sites.native_stores", &rv64_jit_stats.emitted_sites.native_stores,
     RV64_JIT_METRIC_EMITTED_SITES},
    {"emitted_sites.native_jumps", &rv64_jit_stats.emitted_sites.native_jumps,
     RV64_JIT_METRIC_EMITTED_SITES},
    {"emitted_sites.native_m_ops", &rv64_jit_stats.emitted_sites.native_m_ops,
     RV64_JIT_METRIC_EMITTED_SITES},
    {"emitted_sites.native_fp_exact_sites", &rv64_jit_stats.emitted_sites.native_fp_exact_sites,
     RV64_JIT_METRIC_EMITTED_SITES},
    {"emitted_sites.native_fp_memory_sites", &rv64_jit_stats.emitted_sites.native_fp_memory_sites,
     RV64_JIT_METRIC_EMITTED_SITES},
    {"emitted_sites.indirect_pic_sites", &rv64_jit_stats.emitted_sites.indirect_pic_sites,
     RV64_JIT_METRIC_EMITTED_SITES},
    {"emitted_sites.indirect_jump_cache_sites", &rv64_jit_stats.emitted_sites.indirect_jump_cache_sites,
     RV64_JIT_METRIC_EMITTED_SITES},
    {"emitted_sites.reg_cache_spills", &rv64_jit_stats.emitted_sites.reg_cache_spills,
     RV64_JIT_METRIC_EMITTED_SITES},
    {"emitted_sites.reg_cache_dead_victims", &rv64_jit_stats.emitted_sites.reg_cache_dead_victims,
     RV64_JIT_METRIC_EMITTED_SITES},
    {"emitted_sites.reg_cache_live_lru_avoided", &rv64_jit_stats.emitted_sites.reg_cache_live_lru_avoided,
     RV64_JIT_METRIC_EMITTED_SITES},
    {"emitted_sites.native_store_continuations", &rv64_jit_stats.emitted_sites.native_store_continuations,
     RV64_JIT_METRIC_EMITTED_SITES},
    {"emitted_sites.native_paged_loads", &rv64_jit_stats.emitted_sites.native_paged_loads,
     RV64_JIT_METRIC_EMITTED_SITES},
    {"emitted_sites.native_paged_stores", &rv64_jit_stats.emitted_sites.native_paged_stores,
     RV64_JIT_METRIC_EMITTED_SITES},
    {"emitted_sites.inline_paged_loads", &rv64_jit_stats.emitted_sites.inline_paged_loads,
     RV64_JIT_METRIC_EMITTED_SITES},
    {"emitted_sites.inline_paged_stores", &rv64_jit_stats.emitted_sites.inline_paged_stores,
     RV64_JIT_METRIC_EMITTED_SITES},
    {"emitted_sites.direct_mmio_load_sites", &rv64_jit_stats.emitted_sites.direct_mmio_load_sites,
     RV64_JIT_METRIC_EMITTED_SITES},
    {"emitted_sites.direct_mmio_store_sites", &rv64_jit_stats.emitted_sites.direct_mmio_store_sites,
     RV64_JIT_METRIC_EMITTED_SITES},
    {"emitted_sites.fp_helper_sites", &rv64_jit_stats.emitted_sites.fp_helper_sites,
     RV64_JIT_METRIC_EMITTED_SITES},
    {"emitted_sites.fp_helper_gpr_effect_sites", &rv64_jit_stats.emitted_sites.fp_helper_gpr_effect_sites,
     RV64_JIT_METRIC_EMITTED_SITES},
    {"emitted_sites.fp_helper_gpr_mappings_preserved", &rv64_jit_stats.emitted_sites.fp_helper_gpr_mappings_preserved,
     RV64_JIT_METRIC_EMITTED_SITES},
    {"emitted_sites.fp_helper_gpr_selective_invalidations", &rv64_jit_stats.emitted_sites.fp_helper_gpr_selective_invalidations,
     RV64_JIT_METRIC_EMITTED_SITES},
    {"emitted_sites.fp_helper_gpr_input_flushes", &rv64_jit_stats.emitted_sites.fp_helper_gpr_input_flushes,
     RV64_JIT_METRIC_EMITTED_SITES},
    {"emitted_sites.fp_helper_gpr_dirty_mappings_preserved", &rv64_jit_stats.emitted_sites.fp_helper_gpr_dirty_mappings_preserved,
     RV64_JIT_METRIC_EMITTED_SITES},
    {"emitted_sites.fp_helper_gpr_trap_stores", &rv64_jit_stats.emitted_sites.fp_helper_gpr_trap_stores,
     RV64_JIT_METRIC_EMITTED_SITES},
};

/* Keep the independently specified key order and array ownership visible.
 * The reporter may share event metadata between kinds, but neither index may
 * be exchanged or accidentally used as a byte offset. */
static const struct
{
    const char *identity;
    const char *key;
    const uint64_t *values;
    rv64_jit_metric_category_t category;
    const char *unit;
} expected_pic_counters[] = {
    {"indirect_pic_hits", "hits", rv64_jit_stats.indirect_pic_hits, RV64_JIT_METRIC_RUN_TIME, "hits"},
    {"indirect_pic_secondary_hits", "secondary_hits", rv64_jit_stats.indirect_pic_secondary_hits, RV64_JIT_METRIC_RUN_TIME, "hits"},
    {"indirect_pic_misses", "misses", rv64_jit_stats.indirect_pic_misses, RV64_JIT_METRIC_RUN_TIME, "misses"},
    {"indirect_pic_fills", "fills", rv64_jit_stats.indirect_pic_fills, RV64_JIT_METRIC_RUN_TIME, "fills"},
    {"indirect_pic_replacements", "replacements", rv64_jit_stats.indirect_pic_replacements, RV64_JIT_METRIC_RUN_TIME, "replacements"},
    {"indirect_pic_stale_rejections", "stale_rejections", rv64_jit_stats.indirect_pic_stale_rejections, RV64_JIT_METRIC_RUN_TIME, "rejections"},
    {"indirect_pic_budget_rejections", "budget_rejections", rv64_jit_stats.indirect_pic_budget_rejections, RV64_JIT_METRIC_RUN_TIME, "rejections"},
    {"indirect_pic_patch_resolutions", "patch_resolutions", rv64_jit_stats.indirect_pic_patch_resolutions, RV64_JIT_METRIC_MAINTENANCE, "patches"},
    {"indirect_pic_patch_unlinks", "patch_unlinks", rv64_jit_stats.indirect_pic_patch_unlinks, RV64_JIT_METRIC_MAINTENANCE, "unlinks"},
    {"indirect_pic_source_detaches", "source_detaches", rv64_jit_stats.indirect_pic_source_detaches, RV64_JIT_METRIC_MAINTENANCE, "detaches"},
    {"indirect_pic_target_detaches", "target_detaches", rv64_jit_stats.indirect_pic_target_detaches, RV64_JIT_METRIC_MAINTENANCE, "detaches"},
    {"indirect_pic_patched_entries", "patched_entries", rv64_jit_stats.indirect_pic_patched_entries, RV64_JIT_METRIC_RUN_TIME, "entries"},
    {"indirect_pic_patch_downgrades", "patch_downgrades", rv64_jit_stats.indirect_pic_patch_downgrades, RV64_JIT_METRIC_MAINTENANCE, "downgrades"},
};

/* Every counter is uint64_t, including the existing enum-indexed arrays. This
 * coverage assertion catches newly added storage without inspecting C source or
 * aliasing the statistics object as an invented contiguous array. */
_Static_assert(sizeof(rv64_jit_stats_t) ==
                   (ARRLEN(expected_scalar_counters) + ARRLEN(expected_pic_counters) * RV64_JIT_INDIRECT_PIC_KIND_COUNT +
                    RISCV_OPCODE_MASK + 1u + RV64_JIT_BLOCK_END_COUNT + RV64_JIT_SIDE_EXIT_COUNT + RV64_JIT_M_OP_COUNT +
                    RV64_JIT_FP_EXACT_OP_COUNT + RV64_JIT_FP_MEMORY_OP_COUNT) * sizeof(uint64_t),
               "statistics storage needs complete metadata or bounded-distribution coverage");

static void check_metric_description(const rv64_jit_counter_metric_t *metric)
{
    assert(metric->identity != NULL && metric->identity[0] != '\0');
    assert(metric->label != NULL && metric->label[0] != '\0');
    assert(metric->unit != NULL && metric->unit[0] != '\0');
    assert(metric->recording_boundary != NULL && metric->recording_boundary[0] != '\0');
    assert(metric->value != NULL);
}

static void check_scalar_metadata(void)
{
    const struct
    {
        const rv64_jit_counter_metric_t *metrics;
        size_t count;
    } groups[] = {
        {jit_primary_counter_metrics, ARRLEN(jit_primary_counter_metrics)},
        {jit_indirect_jump_cache_metrics, ARRLEN(jit_indirect_jump_cache_metrics)},
    };

    _Static_assert(ARRLEN(jit_primary_counter_metrics) + ARRLEN(jit_indirect_jump_cache_metrics) == ARRLEN(expected_scalar_counters),
                   "scalar metric coverage changed");

    for (size_t expected = 0; expected < ARRLEN(expected_scalar_counters); expected++)
    {
        size_t matches = 0;

        for (size_t group = 0; group < ARRLEN(groups); group++)
        {
            for (size_t i = 0; i < groups[group].count; i++)
            {
                const rv64_jit_counter_metric_t *metric = &groups[group].metrics[i];
                check_metric_description(metric);

                if (strcmp(metric->identity, expected_scalar_counters[expected].identity) == 0)
                {
                    matches++;
                    assert(metric->value == expected_scalar_counters[expected].value);
                    assert(metric->category == expected_scalar_counters[expected].category);
                }
            }
        }

        assert(matches == 1);

        for (size_t prior = 0; prior < expected; prior++)
        {
            assert(expected_scalar_counters[prior].value != expected_scalar_counters[expected].value);
            assert(strcmp(expected_scalar_counters[prior].identity, expected_scalar_counters[expected].identity) != 0);
        }
    }

    /* The pilot already covers its seven exports. These are the only other
     * exported scalar fields; an accidental non-null key changes the API. */
    for (size_t i = 0; i < ARRLEN(jit_primary_counter_metrics); i++)
    {
        const rv64_jit_counter_metric_t *metric = &jit_primary_counter_metrics[i];

        if (metric->value == &rv64_jit_stats.direct_jalr_link_taken_count)
        {
            assert(metric->key != NULL && strcmp(metric->key, "direct_jalr_link.taken") == 0);
        }
        else if (metric->value == &rv64_jit_stats.direct_jalr_link_miss_count)
        {
            assert(metric->key != NULL && strcmp(metric->key, "direct_jalr_link.misses") == 0);
        }
        else
        {
            assert(metric->key == NULL);
        }
    }
}

static void check_pic_metadata(void)
{
    const rv64_jit_reason_name_t expected_kinds[] = {
        [RV64_JIT_INDIRECT_PIC_RETURN] = {"return", "return"},
        [RV64_JIT_INDIRECT_PIC_JALR] = {"jalr", "JALR"},
    };

    _Static_assert(ARRLEN(expected_pic_counters) == RV64_JIT_PIC_METRIC_COUNT, "PIC metric coverage changed");
    _Static_assert(ARRLEN(expected_kinds) == RV64_JIT_INDIRECT_PIC_KIND_COUNT, "PIC kind coverage changed");

    for (size_t kind = 0; kind < ARRLEN(expected_kinds); kind++)
    {
        assert(jit_indirect_pic_kind_names[kind].key != NULL && expected_kinds[kind].key != NULL);
        assert(jit_indirect_pic_kind_names[kind].description != NULL && expected_kinds[kind].description != NULL);
        assert(strcmp(jit_indirect_pic_kind_names[kind].key, expected_kinds[kind].key) == 0);
        assert(strcmp(jit_indirect_pic_kind_names[kind].description, expected_kinds[kind].description) == 0);
    }

    for (size_t i = 0; i < ARRLEN(expected_pic_counters); i++)
    {
        const rv64_jit_counter_metric_t *metric = &jit_indirect_pic_metrics[i];
        check_metric_description(metric);
        assert(strcmp(metric->identity, expected_pic_counters[i].identity) == 0);
        assert(metric->key != NULL && strcmp(metric->key, expected_pic_counters[i].key) == 0);
        assert(metric->category == expected_pic_counters[i].category);
        assert(strcmp(metric->unit, expected_pic_counters[i].unit) == 0);

        for (uint32_t kind = 0; kind < RV64_JIT_INDIRECT_PIC_KIND_COUNT; kind++)
        {
            assert(&metric->value[kind] == &expected_pic_counters[i].values[kind]);
        }

        for (size_t prior = 0; prior < i; prior++)
        {
            assert(strcmp(metric->key, jit_indirect_pic_metrics[prior].key) != 0);
            assert(metric->value != jit_indirect_pic_metrics[prior].value);
        }
    }
}

static void check_jump_cache_metadata(void)
{
    /* This independent list binds each public key to a named storage member.
     * A descriptor accidentally duplicated, omitted or attached to the wrong
     * field must fail even when a zero-filled report would look plausible. */
    const struct
    {
        const char *key;
        const uint64_t *value;
        rv64_jit_metric_category_t category;
        const char *unit;
    } expected[] = {
        {"indirect_jump_cache.sites", &rv64_jit_stats.emitted_sites.indirect_jump_cache_sites, RV64_JIT_METRIC_EMITTED_SITES, "sites"},
        {"indirect_jump_cache.hits", &rv64_jit_stats.indirect_jump_cache_hits, RV64_JIT_METRIC_RUN_TIME, "hits"},
        {"indirect_jump_cache.misses", &rv64_jit_stats.indirect_jump_cache_misses, RV64_JIT_METRIC_RUN_TIME, "misses"},
        {"indirect_jump_cache.fills", &rv64_jit_stats.indirect_jump_cache_fills, RV64_JIT_METRIC_RUN_TIME, "fills"},
        {"indirect_jump_cache.replacements", &rv64_jit_stats.indirect_jump_cache_replacements, RV64_JIT_METRIC_RUN_TIME, "replacements"},
        {"indirect_jump_cache.stale_rejections", &rv64_jit_stats.indirect_jump_cache_stale_rejections, RV64_JIT_METRIC_RUN_TIME, "rejections"},
        {"indirect_jump_cache.budget_rejections", &rv64_jit_stats.indirect_jump_cache_budget_rejections, RV64_JIT_METRIC_RUN_TIME, "rejections"},
    };

    _Static_assert(ARRLEN(expected) == RV64_JIT_JUMP_CACHE_METRIC_COUNT, "jump-cache metric coverage changed");
    _Static_assert(ARRLEN(jit_indirect_jump_cache_metrics) == ARRLEN(expected), "jump-cache table size changed");

    for (size_t i = 0; i < ARRLEN(expected); i++)
    {
        const rv64_jit_counter_metric_t *metric = &jit_indirect_jump_cache_metrics[i];
        assert(metric->key != NULL && strcmp(metric->key, expected[i].key) == 0);
        assert(metric->value == expected[i].value);
        assert(metric->category == expected[i].category);
        assert(metric->unit != NULL && strcmp(metric->unit, expected[i].unit) == 0);
        assert(metric->label != NULL && metric->label[0] != '\0');
        assert(metric->recording_boundary != NULL && metric->recording_boundary[0] != '\0');

        for (size_t j = 0; j < i; j++)
        {
            assert(strcmp(metric->key, jit_indirect_jump_cache_metrics[j].key) != 0);
            assert(metric->value != jit_indirect_jump_cache_metrics[j].value);
        }
    }
}

static void check_report_helpers(void)
{
    char count[RV64_JIT_WIDE_COUNT_BUFFER_SIZE];

    jit_format_wide_count(count, 0);
    assert(strcmp(count, "0") == 0);
    jit_format_wide_count(count, (rv64_jit_wide_count_t)UINT64_MAX * 2u);
    assert(strcmp(count, "36893488147419103230") == 0);
    assert(jit_scaled_ratio(0, 0, 10000u) == 0);
    assert(jit_scaled_ratio(1, 8, 100u) == 13u);
    assert(jit_scaled_ratio(UINT64_MAX, (rv64_jit_wide_count_t)UINT64_MAX * 2u, 10000u) == 5000u);

    /* Every bounded distribution must have a visible name, even when the
     * current guest does not execute that operation or encounter that reason. */
    for (uint32_t i = 0; i < RV64_JIT_BLOCK_END_COUNT; i++)
    {
        assert(jit_block_end_reason_names[i].key != NULL && jit_block_end_reason_names[i].key[0] != '\0');
        assert(jit_block_end_reason_names[i].description != NULL && jit_block_end_reason_names[i].description[0] != '\0');
    }

    for (uint32_t i = 0; i < RV64_JIT_SIDE_EXIT_COUNT; i++)
    {
        assert(jit_side_exit_reason_names[i].key != NULL && jit_side_exit_reason_names[i].key[0] != '\0');
        assert(jit_side_exit_reason_names[i].description != NULL && jit_side_exit_reason_names[i].description[0] != '\0');
    }

    for (uint32_t i = 0; i < RV64_JIT_M_OP_COUNT; i++)
    {
        assert(jit_m_operation_names[i] != NULL && jit_m_operation_names[i][0] != '\0');
    }

    for (uint32_t i = 0; i < RV64_JIT_FP_EXACT_OP_COUNT; i++)
    {
        assert(jit_fp_exact_operation_names[i] != NULL && jit_fp_exact_operation_names[i][0] != '\0');
    }

    for (uint32_t i = 0; i < RV64_JIT_FP_MEMORY_OP_COUNT; i++)
    {
        assert(jit_fp_memory_operation_names[i] != NULL && jit_fp_memory_operation_names[i][0] != '\0');
    }
}

static void seed_report(const char *scenario)
{
    if (strcmp(scenario, "zero") == 0)
    {
        return;
    }

    const bool maximum = strcmp(scenario, "max") == 0;
    assert(maximum || strcmp(scenario, "distinct") == 0);

    for (size_t i = 0; i < ARRLEN(expected_scalar_counters); i++)
    {
        *expected_scalar_counters[i].value = maximum ? UINT64_MAX : 101u + i;
    }

    /* Each PIC kind and event gets its own value, independent of the production
     * descriptor table. This distinguishes both transposed events and kinds. */
    for (uint32_t kind = 0; kind < RV64_JIT_INDIRECT_PIC_KIND_COUNT; kind++)
    {
        rv64_jit_stats.indirect_pic_hits[kind] = maximum ? UINT64_MAX : 301u + kind;
        rv64_jit_stats.indirect_pic_secondary_hits[kind] = maximum ? UINT64_MAX : 303u + kind;
        rv64_jit_stats.indirect_pic_misses[kind] = maximum ? UINT64_MAX : 305u + kind;
        rv64_jit_stats.indirect_pic_fills[kind] = maximum ? UINT64_MAX : 307u + kind;
        rv64_jit_stats.indirect_pic_replacements[kind] = maximum ? UINT64_MAX : 309u + kind;
        rv64_jit_stats.indirect_pic_stale_rejections[kind] = maximum ? UINT64_MAX : 311u + kind;
        rv64_jit_stats.indirect_pic_budget_rejections[kind] = maximum ? UINT64_MAX : 313u + kind;
        rv64_jit_stats.indirect_pic_patch_resolutions[kind] = maximum ? UINT64_MAX : 315u + kind;
        rv64_jit_stats.indirect_pic_patch_unlinks[kind] = maximum ? UINT64_MAX : 317u + kind;
        rv64_jit_stats.indirect_pic_source_detaches[kind] = maximum ? UINT64_MAX : 319u + kind;
        rv64_jit_stats.indirect_pic_target_detaches[kind] = maximum ? UINT64_MAX : 321u + kind;
        rv64_jit_stats.indirect_pic_patched_entries[kind] = maximum ? UINT64_MAX : 323u + kind;
        rv64_jit_stats.indirect_pic_patch_downgrades[kind] = maximum ? UINT64_MAX : 325u + kind;
    }

    /* Distinct values expose swapped fields; maximum values expose narrowing
     * or signed formatting. Assign members explicitly, independently of the
     * production metadata mapping which this fixture is intended to check. */
    rv64_jit_stats.emitted_sites.indirect_jump_cache_sites = maximum ? UINT64_MAX : 11u;
    rv64_jit_stats.indirect_jump_cache_hits = maximum ? UINT64_MAX : 13u;
    rv64_jit_stats.indirect_jump_cache_misses = maximum ? UINT64_MAX : 17u;
    rv64_jit_stats.indirect_jump_cache_fills = maximum ? UINT64_MAX : 19u;
    rv64_jit_stats.indirect_jump_cache_replacements = maximum ? UINT64_MAX : 23u;
    rv64_jit_stats.indirect_jump_cache_stale_rejections = maximum ? UINT64_MAX : 29u;
    rv64_jit_stats.indirect_jump_cache_budget_rejections = maximum ? UINT64_MAX : 31u;

    rv64_jit_stats.blocks_compiled = maximum ? UINT64_MAX : 8u;
    rv64_jit_stats.compiled_insns = maximum ? UINT64_MAX : 1u;
    rv64_jit_stats.direct_link_taken_count = maximum ? UINT64_MAX : 1u;
    rv64_jit_stats.direct_link_miss_count = maximum ? UINT64_MAX : 7u;

    for (uint32_t i = 0; i < RV64_JIT_BLOCK_END_COUNT; i++)
    {
        rv64_jit_stats.block_end_by_reason[i] = maximum ? UINT64_MAX - i : i + 1u;
    }

    for (uint32_t i = 0; i < RV64_JIT_SIDE_EXIT_COUNT; i++)
    {
        rv64_jit_stats.side_exit_by_reason[i] = maximum ? UINT64_MAX - i : i + 1u;
    }

    for (uint32_t i = 0; i <= RISCV_OPCODE_MASK; i++)
    {
        rv64_jit_stats.unsupported_by_opcode[i] = maximum ? UINT64_MAX - i : i + 1u;
    }

    for (uint32_t i = 0; i < RV64_JIT_M_OP_COUNT; i++)
    {
        rv64_jit_stats.native_m_executions[i] = maximum ? UINT64_MAX - i : i + 1u;
    }

    for (uint32_t i = 0; i < RV64_JIT_FP_EXACT_OP_COUNT; i++)
    {
        rv64_jit_stats.native_fp_exact_executions[i] = maximum ? UINT64_MAX - i : i + 1u;
    }

    for (uint32_t i = 0; i < RV64_JIT_FP_MEMORY_OP_COUNT; i++)
    {
        rv64_jit_stats.native_fp_memory_executions[i] = maximum ? UINT64_MAX - i : i + 1u;
    }
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    check_jump_cache_metadata();
    check_scalar_metadata();
    check_pic_metadata();
    check_report_helpers();
    seed_report(argv[1]);

    /* The outer NEMU_JIT_STATS gate belongs to the CPU integration checks.
     * Here exercise the real report's narrower, exactly-"1" machine gate. */
    if (strcmp(argv[2], "unset") == 0)
    {
        assert(unsetenv("NEMU_RV64_JIT_STATS_KV") == 0);
    }
    else
    {
        assert(setenv("NEMU_RV64_JIT_STATS_KV", argv[2], 1) == 0);
    }

    rv64_jit_dump_stats_report();
    return 0;
}
