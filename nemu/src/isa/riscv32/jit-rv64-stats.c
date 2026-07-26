#include <generated/autoconf.h>

#ifdef CONFIG_RV64

#include "jit-rv64-internal.h"

#if RV64_JIT_STATS

/*
 * RV64 JIT statistics report.
 *
 * Keep presentation here rather than in the dispatcher.  This file deliberately
 * separates four kinds of counter which are easy to confuse when reading a
 * profile:
 *
 *   - "run time" counts dispatcher, helper, or generated-code events;
 *   - "compilation" counts completed blocks and compile-stop outcomes;
 *   - "emitted sites" counts instructions or guards produced by the compiler;
 *   - "maintenance" counts cache validation and invalidation work.
 *
 * An emitted load site can execute many times, so it must never be compared
 * directly with a run-time helper-load count.  The section headings and labels
 * below make that distinction explicit.
 */

typedef struct
{
    const char *key;
    const char *description;
} rv64_jit_reason_name_t;

/*
 * A report total may add several individually valid uint64_t counters.  Keep
 * those intermediate totals in 128 bits so the presentation layer does not wrap
 * before any source counter has wrapped.  A 128-bit unsigned value needs at most
 * 39 decimal digits plus its terminating null byte.
 */
typedef unsigned __int128 rv64_jit_wide_count_t;
#define RV64_JIT_WIDE_COUNT_BUFFER_SIZE 40u

static const rv64_jit_reason_name_t
    jit_block_end_reason_names[RV64_JIT_BLOCK_END_COUNT] = {
        [RV64_JIT_BLOCK_END_BUDGET] = {
            "budget", "instruction budget or trace limit"},
        [RV64_JIT_BLOCK_END_JUMP] = {
            "jump", "JAL or JALR ended the native region"},
        [RV64_JIT_BLOCK_END_CHAINED_LOOP] = {
            "chained-loop", "a native backedge ended the region"},
        [RV64_JIT_BLOCK_END_SOURCE_BOUNDARY] = {
            "source-boundary", "fetch/source metadata could not be extended"},
        [RV64_JIT_BLOCK_END_UNSUPPORTED_AFTER_PREFIX] = {
            "unsupported-after-prefix", "the next instruction needs fallback"},
};

static const rv64_jit_reason_name_t
    jit_side_exit_reason_names[RV64_JIT_SIDE_EXIT_COUNT] = {
        [RV64_JIT_SIDE_EXIT_LOAD_GUARD] = {
            "load-guard", "load alignment, range, or translation guard"},
        [RV64_JIT_SIDE_EXIT_STORE_GUARD] = {
            "store-guard", "store alignment, range, or translation guard"},
        [RV64_JIT_SIDE_EXIT_STORE_SOURCE] = {
            "store-source", "store may modify compiled source bytes"},
        [RV64_JIT_SIDE_EXIT_PAGED_STORE_HELPER] = {
            "paged-store-helper", "translated store completed through a helper"},
        [RV64_JIT_SIDE_EXIT_BRANCH_TAKEN] = {
            "branch-taken", "taken branch returned to the dispatcher"},
        [RV64_JIT_SIDE_EXIT_CHAINED_OVER_BUDGET] = {
            "chained-over-budget", "another native loop lap exceeded its budget"},
        [RV64_JIT_SIDE_EXIT_JALR_MISALIGNED] = {
            "jalr-misaligned", "JALR target failed the IALIGN check"},
};

/* Multiply in 128 bits so long-running profiles cannot overflow before division. */
static uint64_t jit_scaled_ratio(rv64_jit_wide_count_t numerator,
                                 rv64_jit_wide_count_t denominator,
                                 uint64_t scale)
{
    if (denominator == 0)
    {
        return 0;
    }

    /*
     * Report inputs are sums of at most 128 uint64_t counters and the largest
     * scale is 10,000.  Their product therefore remains well inside 128 bits.
     */
    const rv64_jit_wide_count_t scaled =
        numerator * scale + denominator / 2u;
    return (uint64_t)(scaled / denominator);
}

/* Format one wide aggregate without narrowing it back to uint64_t. */
static void jit_format_wide_count(
    char buffer[RV64_JIT_WIDE_COUNT_BUFFER_SIZE],
    rv64_jit_wide_count_t value)
{
    char reversed[RV64_JIT_WIDE_COUNT_BUFFER_SIZE - 1u];
    size_t digit_count = 0;

    /*
     * Build the number from least-significant digit to most-significant digit,
     * then reverse it into the caller's buffer.  The do/while also prints zero
     * as one visible digit rather than an empty string.
     */
    do
    {
        Assert(digit_count < sizeof(reversed),
               "jit: wide statistics count exceeded its decimal buffer");
        reversed[digit_count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0);

    for (size_t i = 0; i < digit_count; i++)
    {
        buffer[i] = reversed[digit_count - i - 1u];
    }

    buffer[digit_count] = '\0';
}

/* Print a visible boundary between groups of counters with different meanings. */
static void jit_log_section(const char *title)
{
    Log("jit: -- %s --", title);
}

/* Print one aligned integer counter with a short unit. */
static void jit_log_count(const char *label, uint64_t value, const char *unit)
{
    Log("jit:   %-38s = %12" PRIu64 " %s", label, value, unit);
}

/* Print an aggregate which may be wider than any one underlying counter. */
static void jit_log_wide_count(const char *label, rv64_jit_wide_count_t value,
                               const char *unit)
{
    char value_text[RV64_JIT_WIDE_COUNT_BUFFER_SIZE];
    jit_format_wide_count(value_text, value);
    Log("jit:   %-38s = %12s %s", label, value_text, unit);
}

/* Print a percentage, or explain why no percentage exists yet. */
static void jit_log_percentage(const char *label, rv64_jit_wide_count_t part,
                               rv64_jit_wide_count_t total)
{
    if (total == 0)
    {
        Log("jit:   %-38s =          n/a (no samples)", label);
        return;
    }

    const uint64_t percent_x100 = jit_scaled_ratio(part, total, 10000u);
    char part_text[RV64_JIT_WIDE_COUNT_BUFFER_SIZE];
    char total_text[RV64_JIT_WIDE_COUNT_BUFFER_SIZE];
    jit_format_wide_count(part_text, part);
    jit_format_wide_count(total_text, total);
    Log("jit:   %-38s = %6" PRIu64 ".%02" PRIu64
        "%%  (%s / %s)",
        label, percent_x100 / 100u, percent_x100 % 100u,
        part_text, total_text);
}

/* Print an average, or explain that its denominator has no observations. */
static void jit_log_average(const char *label, uint64_t total,
                            uint64_t sample_count, const char *unit)
{
    if (sample_count == 0)
    {
        Log("jit:   %-38s =          n/a (no samples)", label);
        return;
    }

    const uint64_t average_x100 =
        jit_scaled_ratio(total, sample_count, 100u);
    Log("jit:   %-38s = %9" PRIu64 ".%02" PRIu64 " %s",
        label, average_x100 / 100u, average_x100 % 100u, unit);
}

/* Return the standard major-opcode name used by the unprivileged ISA manual. */
static const char *jit_opcode_name(uint32_t opcode)
{
    switch (opcode)
    {
    case 0x03:
        return "LOAD";
    case 0x07:
        return "LOAD-FP";
    case 0x0f:
        return "MISC-MEM";
    case 0x13:
        return "OP-IMM";
    case 0x17:
        return "AUIPC";
    case 0x1b:
        return "OP-IMM-32";
    case 0x23:
        return "STORE";
    case 0x27:
        return "STORE-FP";
    case 0x2f:
        return "AMO";
    case 0x33:
        return "OP";
    case 0x37:
        return "LUI";
    case 0x3b:
        return "OP-32";
    case 0x43:
        return "MADD";
    case 0x47:
        return "MSUB";
    case 0x4b:
        return "NMSUB";
    case 0x4f:
        return "NMADD";
    case 0x53:
        return "OP-FP";
    case 0x57:
        return "OP-V";
    case 0x63:
        return "BRANCH";
    case 0x67:
        return "JALR";
    case 0x6f:
        return "JAL";
    case 0x73:
        return "SYSTEM";
    default:
        return "reserved, custom, or non-32-bit";
    }
}

/* Sum one fixed-size distribution without hiding the individual categories. */
static rv64_jit_wide_count_t jit_sum_counts(const uint64_t *counts,
                                            uint32_t count)
{
    rv64_jit_wide_count_t total = 0;

    for (uint32_t i = 0; i < count; i++)
    {
        total += counts[i];
    }

    return total;
}

/* Count the populated categories in a distribution for the legacy summary. */
static uint32_t jit_count_nonzero(const uint64_t *counts, uint32_t count)
{
    uint32_t populated = 0;

    for (uint32_t i = 0; i < count; i++)
    {
        populated += counts[i] != 0 ? 1u : 0u;
    }

    return populated;
}

/* Dispatcher-facing summary: all values in this section are run-time events. */
static void jit_dump_dispatch_stats(void)
{
    /*
     * A matching block which is longer than the remaining instruction budget is
     * deliberately rejected before either counter changes.  Hits plus misses are
     * therefore classified, usable outcomes rather than every raw cache probe.
     */
    const rv64_jit_wide_count_t classified_cache_probes =
        (rv64_jit_wide_count_t)rv64_jit_stats.cache_hits +
        rv64_jit_stats.cache_misses;

    jit_log_section("Run time: dispatch and native execution");
    jit_log_count("CPU-loop execution requests",
                  rv64_jit_stats.exec_requests, "requests");
    jit_log_wide_count("Classified block-cache probes",
                       classified_cache_probes, "probes");
    jit_log_percentage("Block-cache hit rate (incl. negative)",
                       rv64_jit_stats.cache_hits, classified_cache_probes);

    /*
     * Keep the long-standing field names on one machine-friendly line.  Several
     * local and external scripts parse these substrings, while the rows above
     * provide the clearer human interpretation.
     */
    if (classified_cache_probes == 0)
    {
        Log("jit:   exec requests = %" PRIu64
            ", cache hits = %" PRIu64 ", misses = %" PRIu64
            ", hit rate = n/a",
            rv64_jit_stats.exec_requests, rv64_jit_stats.cache_hits,
            rv64_jit_stats.cache_misses);
    }
    else
    {
        const uint64_t cache_hit_pct =
            jit_scaled_ratio(rv64_jit_stats.cache_hits,
                             classified_cache_probes, 10000u);
        Log("jit:   exec requests = %" PRIu64
            ", cache hits = %" PRIu64 ", misses = %" PRIu64
            ", hit rate = %" PRIu64 ".%02" PRIu64 "%%",
            rv64_jit_stats.exec_requests, rv64_jit_stats.cache_hits,
            rv64_jit_stats.cache_misses,
            cache_hit_pct / 100u, cache_hit_pct % 100u);
    }

    jit_log_count("Native entries with progress",
                  rv64_jit_stats.blocks_executed, "entries");
    jit_log_count("Guest instructions retired",
                  rv64_jit_stats.executed_insns, "instructions");
    jit_log_average("Guest instructions per native entry",
                    rv64_jit_stats.executed_insns,
                    rv64_jit_stats.blocks_executed, "instructions");
    jit_log_count("Unsupported-cache hits",
                  rv64_jit_stats.unsupported_hits, "hits");
    if (rv64_jit_stats.blocks_executed == 0)
    {
        Log("jit:   executed blocks = %" PRIu64
            ", JIT instructions = %" PRIu64
            ", avg executed block = n/a, unsupported hits = %" PRIu64,
            rv64_jit_stats.blocks_executed,
            rv64_jit_stats.executed_insns,
            rv64_jit_stats.unsupported_hits);
    }
    else
    {
        const uint64_t average_x100 =
            jit_scaled_ratio(rv64_jit_stats.executed_insns,
                             rv64_jit_stats.blocks_executed, 100u);
        Log("jit:   executed blocks = %" PRIu64
            ", JIT instructions = %" PRIu64
            ", avg executed block = %" PRIu64 ".%02" PRIu64
            " insn, unsupported hits = %" PRIu64,
            rv64_jit_stats.blocks_executed,
            rv64_jit_stats.executed_insns,
            average_x100 / 100u, average_x100 % 100u,
            rv64_jit_stats.unsupported_hits);
    }

    Log("jit:   zero side exits = %" PRIu64,
        rv64_jit_stats.zero_side_exits);
}

/* Compile-stop reasons are normal compilation outcomes, not all fallbacks. */
static void jit_dump_block_end_stats(void)
{
    const rv64_jit_wide_count_t block_end_total =
        jit_sum_counts(rv64_jit_stats.block_end_by_reason,
                       RV64_JIT_BLOCK_END_COUNT);

    jit_log_wide_count("Compiled block endings", block_end_total, "blocks");

    for (uint32_t reason = 0; reason < RV64_JIT_BLOCK_END_COUNT; reason++)
    {
        const uint64_t count = rv64_jit_stats.block_end_by_reason[reason];

        if (count != 0)
        {
            Log("jit:     block end %s = %" PRIu64 " (%s)",
                jit_block_end_reason_names[reason].key, count,
                jit_block_end_reason_names[reason].description);
        }
    }
}

/* Compilation summary: block outcomes and emitted sites, never executions. */
static void jit_dump_compilation_stats(void)
{
    jit_log_section("Compilation: blocks and emitted sites");
    jit_log_count("Blocks compiled", rv64_jit_stats.blocks_compiled, "blocks");
    jit_log_count("Blocks cached as unsupported",
                  rv64_jit_stats.blocks_unsupported, "blocks");
    jit_log_count("Guest instruction sites compiled",
                  rv64_jit_stats.compiled_insns, "sites");
    jit_log_average("Guest instruction sites per block",
                    rv64_jit_stats.compiled_insns,
                    rv64_jit_stats.blocks_compiled, "sites");

    /* Retain the old aggregate keys without making them the human-facing labels. */
    if (rv64_jit_stats.blocks_compiled == 0)
    {
        Log("jit:   compiled blocks = %" PRIu64
            ", unsupported blocks = %" PRIu64
            ", avg compiled length = n/a",
            rv64_jit_stats.blocks_compiled,
            rv64_jit_stats.blocks_unsupported);
    }
    else
    {
        const uint64_t average_x100 =
            jit_scaled_ratio(rv64_jit_stats.compiled_insns,
                             rv64_jit_stats.blocks_compiled, 100u);
        Log("jit:   compiled blocks = %" PRIu64
            ", unsupported blocks = %" PRIu64
            ", avg compiled length = %" PRIu64 ".%02" PRIu64 " insn",
            rv64_jit_stats.blocks_compiled,
            rv64_jit_stats.blocks_unsupported,
            average_x100 / 100u, average_x100 % 100u);
    }

    Log("jit:   translated blocks = %" PRIu64
        " (blocks fetched through Sv39)",
        rv64_jit_stats.translated_blocks);
    Log("jit:   translated cross-page blocks = %" PRIu64,
        rv64_jit_stats.translated_cross_page_blocks);
    Log("jit:   segmented source blocks = %" PRIu64,
        rv64_jit_stats.segmented_source_blocks);
    Log("jit:   trace blocks = %" PRIu64 ", trace instructions = %" PRIu64,
        rv64_jit_stats.trace_blocks, rv64_jit_stats.trace_insns);

    Log("jit:   emitted guest sites: native loads = %" PRIu64
        ", native stores = %" PRIu64,
        rv64_jit_stats.native_loads, rv64_jit_stats.native_stores);
    Log("jit:   emitted guest sites: native jumps = %" PRIu64
        ", native M ops = %" PRIu64,
        rv64_jit_stats.native_jumps, rv64_jit_stats.native_m_ops);
    Log("jit:   emitted Sv39 sites: native paged loads = %" PRIu64
        ", native paged stores = %" PRIu64,
        rv64_jit_stats.native_paged_loads,
        rv64_jit_stats.native_paged_stores);
    Log("jit:   native store continuations = %" PRIu64 " emitted sites",
        rv64_jit_stats.native_store_continuations);
    Log("jit:   inline paged loads = %" PRIu64
        ", inline paged stores = %" PRIu64 " emitted sites",
        rv64_jit_stats.inline_paged_loads,
        rv64_jit_stats.inline_paged_stores);
    Log("jit:   reg cache spills = %" PRIu64 " emitted spill sequences",
        rv64_jit_stats.reg_cache_spills);

    jit_dump_block_end_stats();
}

/* Memory summary: these counters measure helper and generated-code activity. */
static void jit_dump_memory_stats(void)
{
    const rv64_jit_wide_count_t data_tlb_lookups =
        (rv64_jit_wide_count_t)rv64_jit_stats.data_tlb_hits +
        rv64_jit_stats.data_tlb_misses;

    jit_log_section("Run time: memory and Sv39 data translation");
    jit_log_wide_count("Data-TLB lookups", data_tlb_lookups, "lookups");
    jit_log_percentage("Data-TLB hit rate",
                       rv64_jit_stats.data_tlb_hits, data_tlb_lookups);
    Log("jit:   data TLB hits = %" PRIu64 ", misses = %" PRIu64,
        rv64_jit_stats.data_tlb_hits, rv64_jit_stats.data_tlb_misses);
    Log("jit:   data TLB fills = %" PRIu64,
        rv64_jit_stats.data_tlb_fills);
    Log("jit:   data TLB flushes = %" PRIu64,
        rv64_jit_stats.data_tlb_flushes);
    Log("jit:   data TLB page-table flushes = %" PRIu64,
        rv64_jit_stats.data_tlb_page_table_flushes);
    Log("jit:   data TLB direct loads = %" PRIu64
        ", direct stores = %" PRIu64,
        rv64_jit_stats.data_tlb_direct_loads,
        rv64_jit_stats.data_tlb_direct_stores);
    Log("jit:   inline paged load hits = %" PRIu64
        ", inline paged store hits = %" PRIu64 " successful accesses",
        rv64_jit_stats.inline_paged_load_hits,
        rv64_jit_stats.inline_paged_store_hits);
    Log("jit:   helper loads = %" PRIu64 ", helper stores = %" PRIu64,
        rv64_jit_stats.helper_load_count,
        rv64_jit_stats.helper_store_count);
}

/* Linking and validation counters are updated by running generated code or C. */
static void jit_dump_link_and_validation_stats(void)
{
    const rv64_jit_wide_count_t direct_link_attempts =
        (rv64_jit_wide_count_t)rv64_jit_stats.direct_link_taken_count +
        rv64_jit_stats.direct_link_miss_count;

    jit_log_section("Run time: direct links and validation");
    jit_log_percentage("Direct-link success rate",
                       rv64_jit_stats.direct_link_taken_count,
                       direct_link_attempts);
    Log("jit:   direct links taken = %" PRIu64 ", misses = %" PRIu64,
        rv64_jit_stats.direct_link_taken_count,
        rv64_jit_stats.direct_link_miss_count);
    Log("jit:   direct branch links taken = %" PRIu64,
        rv64_jit_stats.direct_branch_link_taken_count);
    Log("jit:   direct guarded links taken = %" PRIu64,
        rv64_jit_stats.direct_guarded_link_taken_count);
    Log("jit:   ifetch generation fast hits = %" PRIu64
        ", revalidations = %" PRIu64 ", bumps = %" PRIu64,
        rv64_jit_stats.ifetch_generation_fast_hits,
        rv64_jit_stats.ifetch_generation_revalidations,
        rv64_jit_stats.ifetch_generation_bumps);
}

/* Maintenance work is kept separate from hot-path execution counts. */
static void jit_dump_invalidation_stats(void)
{
    jit_log_section("Maintenance: cache invalidation");
    Log("jit:   invalidation requests = %" PRIu64
        ", invalidated blocks = %" PRIu64 ", arena resets = %" PRIu64,
        rv64_jit_stats.invalidation_requests,
        rv64_jit_stats.invalidated_blocks,
        rv64_jit_stats.arena_resets);
    Log("jit:   source reverse invalidations = %" PRIu64
        ", full scans = %" PRIu64,
        rv64_jit_stats.source_reverse_invalidations,
        rv64_jit_stats.source_full_invalidation_scans);
}

/* Print only non-zero fallback categories so the diagnostic list stays short. */
static void jit_dump_fallback_stats(void)
{
    const rv64_jit_wide_count_t unsupported_total =
        jit_sum_counts(rv64_jit_stats.unsupported_by_opcode,
                       RV64_OPCODE_MASK + 1u);
    const uint32_t unsupported_distinct =
        jit_count_nonzero(rv64_jit_stats.unsupported_by_opcode,
                          RV64_OPCODE_MASK + 1u);
    const rv64_jit_wide_count_t side_exit_total =
        jit_sum_counts(rv64_jit_stats.side_exit_by_reason,
                       RV64_JIT_SIDE_EXIT_COUNT);

    jit_log_section("Fallback details: non-zero categories only");
    jit_log_wide_count("Unsupported instruction encounters",
                       unsupported_total, "encounters");

    /* Preserve the old total/distinct keys for consumers of the previous report. */
    char unsupported_total_text[RV64_JIT_WIDE_COUNT_BUFFER_SIZE];
    jit_format_wide_count(unsupported_total_text, unsupported_total);
    Log("jit:   unsupported opcodes total = %s, distinct = %" PRIu32,
        unsupported_total_text, unsupported_distinct);
    for (uint32_t opcode = 0; opcode <= RV64_OPCODE_MASK; opcode++)
    {
        const uint64_t count = rv64_jit_stats.unsupported_by_opcode[opcode];

        if (count != 0)
        {
            Log("jit:     unsupported opcode 0x%02x = %" PRIu64 " (%s)",
                opcode, count, jit_opcode_name(opcode));
        }
    }

    jit_log_wide_count("Run-time side exits", side_exit_total, "exits");

    for (uint32_t reason = 0; reason < RV64_JIT_SIDE_EXIT_COUNT; reason++)
    {
        const uint64_t count = rv64_jit_stats.side_exit_by_reason[reason];

        if (count != 0)
        {
            Log("jit:     side exit %s = %" PRIu64 " (%s)",
                jit_side_exit_reason_names[reason].key, count,
                jit_side_exit_reason_names[reason].description);
        }
    }
}

/* Print the human-oriented report after the core has applied run-time gates. */
void rv64_jit_dump_stats_report(void)
{
    Log("jit: ============================================================");
    Log("jit: RV64 JIT statistics");
    Log("jit: compilation counts are static; run-time sections count executed events");
    Log("jit: ============================================================");

    jit_dump_dispatch_stats();
    jit_dump_compilation_stats();
    jit_dump_memory_stats();
    jit_dump_link_and_validation_stats();
    jit_dump_invalidation_stats();
    jit_dump_fallback_stats();

    Log("jit: ============================================================");
}

#endif /* RV64_JIT_STATS */
#endif /* CONFIG_RV64 */
