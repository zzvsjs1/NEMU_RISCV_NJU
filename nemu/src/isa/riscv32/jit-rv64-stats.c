#include <generated/autoconf.h>

#ifdef CONFIG_RV64

#include "jit-rv64-internal.h"

#include <stdlib.h>
#include <string.h>

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

/*
 * Reporting metadata refers directly to persistent named counters. These
 * pointers are read only by the reporter; generated increments continue to use
 * the original storage members at their original execution boundaries.
 *
 * Identity names the storage member independently of the output interface. A
 * null key deliberately leaves a scalar out of the machine report. Compound
 * legacy lines remain explicit: adding metadata never silently extends them.
 */
typedef enum
{
    RV64_JIT_METRIC_RUN_TIME,
    RV64_JIT_METRIC_COMPILATION,
    RV64_JIT_METRIC_EMITTED_SITES,
    RV64_JIT_METRIC_MAINTENANCE,
} rv64_jit_metric_category_t;

typedef enum
{
    RV64_JIT_JUMP_CACHE_SITES,
    RV64_JIT_JUMP_CACHE_HITS,
    RV64_JIT_JUMP_CACHE_MISSES,
    RV64_JIT_JUMP_CACHE_FILLS,
    RV64_JIT_JUMP_CACHE_REPLACEMENTS,
    RV64_JIT_JUMP_CACHE_STALE_REJECTIONS,
    RV64_JIT_JUMP_CACHE_BUDGET_REJECTIONS,
    RV64_JIT_JUMP_CACHE_METRIC_COUNT,
} rv64_jit_jump_cache_metric_t;

typedef struct
{
    const char *identity;
    const char *key;
    const char *label;
    const char *unit;
    rv64_jit_metric_category_t category;
    const uint64_t *value;
    const char *recording_boundary;
} rv64_jit_counter_metric_t;

static const rv64_jit_counter_metric_t jit_indirect_jump_cache_metrics[RV64_JIT_JUMP_CACHE_METRIC_COUNT] = {
    [RV64_JIT_JUMP_CACHE_SITES] = {
        .identity = "emitted_sites.indirect_jump_cache_sites",
        .key = "indirect_jump_cache.sites",
        .label = "Indirect jump-cache sites",
        .unit = "sites",
        .category = RV64_JIT_METRIC_EMITTED_SITES,
        .value = &rv64_jit_stats.emitted_sites.indirect_jump_cache_sites,
        .recording_boundary = "Sidecar selection during emission; rolled back with any abandoned instruction or compilation attempt",
    },
    [RV64_JIT_JUMP_CACHE_HITS] = {
        .identity = "indirect_jump_cache_hits",
        .key = "indirect_jump_cache.hits",
        .label = "Indirect jump-cache hits",
        .unit = "hits",
        .category = RV64_JIT_METRIC_RUN_TIME,
        .value = &rv64_jit_stats.indirect_jump_cache_hits,
        .recording_boundary = "Generated probe accepts the target identity, publication generation and whole-target instruction budget",
    },
    [RV64_JIT_JUMP_CACHE_MISSES] = {
        .identity = "indirect_jump_cache_misses",
        .key = "indirect_jump_cache.misses",
        .label = "Indirect jump-cache misses",
        .unit = "misses",
        .category = RV64_JIT_METRIC_RUN_TIME,
        .value = &rv64_jit_stats.indirect_jump_cache_misses,
        .recording_boundary = "Generated empty, tag-mismatch or stale probe edge reaches authoritative lookup; excludes budget rejection",
    },
    [RV64_JIT_JUMP_CACHE_FILLS] = {
        .identity = "indirect_jump_cache_fills",
        .key = "indirect_jump_cache.fills",
        .label = "Indirect jump-cache fills",
        .unit = "fills",
        .category = RV64_JIT_METRIC_RUN_TIME,
        .value = &rv64_jit_stats.indirect_jump_cache_fills,
        .recording_boundary = "Generated authoritative success installs the target slot, before publishing its non-zero generation certificate",
    },
    [RV64_JIT_JUMP_CACHE_REPLACEMENTS] = {
        .identity = "indirect_jump_cache_replacements",
        .key = "indirect_jump_cache.replacements",
        .label = "Indirect jump-cache replacements",
        .unit = "replacements",
        .category = RV64_JIT_METRIC_RUN_TIME,
        .value = &rv64_jit_stats.indirect_jump_cache_replacements,
        .recording_boundary = "Generated authoritative refill observes an occupied entry before overwriting it, including the same stale target",
    },
    [RV64_JIT_JUMP_CACHE_STALE_REJECTIONS] = {
        .identity = "indirect_jump_cache_stale_rejections",
        .key = "indirect_jump_cache.stale_rejections",
        .label = "Indirect jump-cache stale rejections",
        .unit = "rejections",
        .category = RV64_JIT_METRIC_RUN_TIME,
        .value = &rv64_jit_stats.indirect_jump_cache_stale_rejections,
        .recording_boundary = "Generated tag-matching probe rejects an invalid target or a zero or changed publication generation; also a cache miss",
    },
    [RV64_JIT_JUMP_CACHE_BUDGET_REJECTIONS] = {
        .identity = "indirect_jump_cache_budget_rejections",
        .key = "indirect_jump_cache.budget_rejections",
        .label = "Indirect jump-cache budget rejections",
        .unit = "rejections",
        .category = RV64_JIT_METRIC_RUN_TIME,
        .value = &rv64_jit_stats.indirect_jump_cache_budget_rejections,
        .recording_boundary = "Generated valid probe exceeds the whole-target budget and bypasses authoritative lookup and the cache-miss increment",
    },
};

/* Ordinary scalar definitions. Only the two established JALR keys are
 * exported here; legacy human rows do not imply a new machine interface. */
static const rv64_jit_counter_metric_t jit_primary_counter_metrics[] = {
    {
        .identity = "exec_requests", .key = NULL,
        .label = "CPU-loop execution requests", .unit = "requests",
        .category = RV64_JIT_METRIC_RUN_TIME, .value = &rv64_jit_stats.exec_requests,
        .recording_boundary = "Dispatcher accepts an execution request, before iterating over its bounded native entries",
    },
    {
        .identity = "cache_hits", .key = NULL,
        .label = "Block-cache hits", .unit = "hits",
        .category = RV64_JIT_METRIC_RUN_TIME, .value = &rv64_jit_stats.cache_hits,
        .recording_boundary = "Dispatcher accepts a matching cache slot after rejecting native blocks longer than the available budget",
    },
    {
        .identity = "cache_misses", .key = NULL,
        .label = "Block-cache misses", .unit = "misses",
        .category = RV64_JIT_METRIC_RUN_TIME, .value = &rv64_jit_stats.cache_misses,
        .recording_boundary = "Dispatcher finds no matching cache slot, before attempting compilation",
    },
    {
        .identity = "unsupported_hits", .key = NULL,
        .label = "Unsupported-cache hits", .unit = "hits",
        .category = RV64_JIT_METRIC_RUN_TIME, .value = &rv64_jit_stats.unsupported_hits,
        .recording_boundary = "Dispatcher observes a valid slot with no native entry after lookup or compilation",
    },
    {
        .identity = "blocks_executed", .key = NULL,
        .label = "Native entries with progress", .unit = "entries",
        .category = RV64_JIT_METRIC_RUN_TIME, .value = &rv64_jit_stats.blocks_executed,
        .recording_boundary = "Dispatcher receives non-zero progress from one native entry, including any chained execution",
    },
    {
        .identity = "executed_insns", .key = NULL,
        .label = "Guest instructions retired", .unit = "instructions",
        .category = RV64_JIT_METRIC_RUN_TIME, .value = &rv64_jit_stats.executed_insns,
        .recording_boundary = "Dispatcher adds the native entry's non-zero returned progress, preserving its existing CPU-loop accounting",
    },
    {
        .identity = "zero_side_exits", .key = NULL,
        .label = "zero side exits", .unit = "exits",
        .category = RV64_JIT_METRIC_RUN_TIME, .value = &rv64_jit_stats.zero_side_exits,
        .recording_boundary = "Dispatcher receives zero progress from a native entry and leaves the execution batch",
    },
    {
        .identity = "cpu_boundary_breaks", .key = NULL,
        .label = "CPU boundary breaks", .unit = "breaks",
        .category = RV64_JIT_METRIC_RUN_TIME, .value = &rv64_jit_stats.cpu_boundary_breaks,
        .recording_boundary = "Dispatcher receives non-zero native progress with a pending CPU boundary request and ends the batch",
    },
    {
        .identity = "blocks_compiled", .key = NULL,
        .label = "Blocks compiled", .unit = "blocks",
        .category = RV64_JIT_METRIC_COMPILATION, .value = &rv64_jit_stats.blocks_compiled,
        .recording_boundary = "Compiler records a successfully published native block after code, dependencies and links are published",
    },
    {
        .identity = "blocks_unsupported", .key = NULL,
        .label = "Blocks cached as unsupported", .unit = "blocks",
        .category = RV64_JIT_METRIC_COMPILATION, .value = &rv64_jit_stats.blocks_unsupported,
        .recording_boundary = "Negative-publication attempt begins, before translated-source revalidation can reject the entry",
    },
    {
        .identity = "compiled_insns", .key = NULL,
        .label = "Guest instruction sites compiled", .unit = "sites",
        .category = RV64_JIT_METRIC_COMPILATION, .value = &rv64_jit_stats.compiled_insns,
        .recording_boundary = "Compiler adds the published block's guest instruction count after publication succeeds",
    },
    {
        .identity = "translated_blocks", .key = NULL,
        .label = "translated blocks", .unit = "blocks",
        .category = RV64_JIT_METRIC_COMPILATION, .value = &rv64_jit_stats.translated_blocks,
        .recording_boundary = "Published native block used translated instruction fetch",
    },
    {
        .identity = "translated_cross_page_blocks", .key = NULL,
        .label = "translated cross-page blocks", .unit = "blocks",
        .category = RV64_JIT_METRIC_COMPILATION, .value = &rv64_jit_stats.translated_cross_page_blocks,
        .recording_boundary = "Published translated block's start and final instruction PCs lie on different virtual pages",
    },
    {
        .identity = "segmented_source_blocks", .key = NULL,
        .label = "segmented source blocks", .unit = "blocks",
        .category = RV64_JIT_METRIC_COMPILATION, .value = &rv64_jit_stats.segmented_source_blocks,
        .recording_boundary = "Published native block has more than one physical source segment",
    },
    {
        .identity = "trace_blocks", .key = NULL,
        .label = "Trace blocks", .unit = "blocks",
        .category = RV64_JIT_METRIC_COMPILATION, .value = &rv64_jit_stats.trace_blocks,
        .recording_boundary = "Published native block's instruction count exceeds the normal block limit",
    },
    {
        .identity = "trace_insns", .key = NULL,
        .label = "Trace instructions", .unit = "instructions",
        .category = RV64_JIT_METRIC_COMPILATION, .value = &rv64_jit_stats.trace_insns,
        .recording_boundary = "Compiler adds all instruction sites in a published block longer than the normal block limit",
    },
    {
        .identity = "stable_loop_blocks", .key = NULL,
        .label = "Stable register loops", .unit = "blocks",
        .category = RV64_JIT_METRIC_COMPILATION, .value = &rv64_jit_stats.stable_loop_blocks,
        .recording_boundary = "Published native block has a non-zero stable-loop preload register count",
    },
    {
        .identity = "stable_loop_preloaded_regs", .key = NULL,
        .label = "Stable-loop preloaded registers", .unit = "registers",
        .category = RV64_JIT_METRIC_COMPILATION, .value = &rv64_jit_stats.stable_loop_preloaded_regs,
        .recording_boundary = "Compiler adds the stable-loop preload register count of a successfully published block",
    },
    {
        .identity = "data_tlb_hits", .key = NULL,
        .label = "Data-TLB hits", .unit = "hits",
        .category = RV64_JIT_METRIC_RUN_TIME, .value = &rv64_jit_stats.data_tlb_hits,
        .recording_boundary = "C translation accepts cached PMEM, or a generated inline page guard accepts PMEM before its memory operation",
    },
    {
        .identity = "data_tlb_misses", .key = NULL,
        .label = "Data-TLB misses", .unit = "misses",
        .category = RV64_JIT_METRIC_RUN_TIME, .value = &rv64_jit_stats.data_tlb_misses,
        .recording_boundary = "C translation rejects the cached entry and begins a page-table walk; earlier eligibility failures are excluded",
    },
    {
        .identity = "data_tlb_fills", .key = NULL,
        .label = "data TLB fills", .unit = "fills",
        .category = RV64_JIT_METRIC_RUN_TIME, .value = &rv64_jit_stats.data_tlb_fills,
        .recording_boundary = "C page-table walk publishes a valid PMEM translation and its page-table references",
    },
    {
        .identity = "data_tlb_flushes", .key = NULL,
        .label = "data TLB flushes", .unit = "flushes",
        .category = RV64_JIT_METRIC_MAINTENANCE, .value = &rv64_jit_stats.data_tlb_flushes,
        .recording_boundary = "Data-TLB flush finishes clearing entries and page-table dependency references",
    },
    {
        .identity = "data_tlb_page_table_flushes", .key = NULL,
        .label = "data TLB page-table flushes", .unit = "flushes",
        .category = RV64_JIT_METRIC_MAINTENANCE, .value = &rv64_jit_stats.data_tlb_page_table_flushes,
        .recording_boundary = "PMEM invalidation detects a live data-TLB page-table dependency, immediately before flushing translations",
    },
    {
        .identity = "data_tlb_direct_loads", .key = NULL,
        .label = "Data-TLB direct loads", .unit = "loads",
        .category = RV64_JIT_METRIC_RUN_TIME, .value = &rv64_jit_stats.data_tlb_direct_loads,
        .recording_boundary = "Load helper obtains a PMEM translation, immediately before host memory is read",
    },
    {
        .identity = "data_tlb_direct_stores", .key = NULL,
        .label = "Data-TLB direct stores", .unit = "stores",
        .category = RV64_JIT_METRIC_RUN_TIME, .value = &rv64_jit_stats.data_tlb_direct_stores,
        .recording_boundary = "Store helper obtains a PMEM translation, before performing the physical write and continuation decision",
    },
    {
        .identity = "inline_paged_load_hits", .key = NULL,
        .label = "Inline paged load hits", .unit = "accesses",
        .category = RV64_JIT_METRIC_RUN_TIME, .value = &rv64_jit_stats.inline_paged_load_hits,
        .recording_boundary = "Generated inline data-TLB guards accept the PMEM address, immediately before the native load",
    },
    {
        .identity = "inline_paged_store_hits", .key = NULL,
        .label = "Inline paged store hits", .unit = "accesses",
        .category = RV64_JIT_METRIC_RUN_TIME, .value = &rv64_jit_stats.inline_paged_store_hits,
        .recording_boundary = "Generated inline data-TLB and source/dependency guards accept the PMEM address, immediately before the native store",
    },
    {
        .identity = "inline_direct_mmio_load_hits", .key = NULL,
        .label = "inline direct MMIO load hits", .unit = "hits",
        .category = RV64_JIT_METRIC_RUN_TIME, .value = &rv64_jit_stats.inline_direct_mmio_load_hits,
        .recording_boundary = "Generated direct-MMIO load path accepts a warm route or a cold eligible callback-free map",
    },
    {
        .identity = "inline_direct_mmio_store_hits", .key = NULL,
        .label = "inline direct MMIO store hits", .unit = "hits",
        .category = RV64_JIT_METRIC_RUN_TIME, .value = &rv64_jit_stats.inline_direct_mmio_store_hits,
        .recording_boundary = "Generated direct-MMIO store finishes a warm-route or cold-map native write",
    },
    {
        .identity = "direct_mmio_load_route_hits", .key = NULL,
        .label = "Direct MMIO load route hits", .unit = "hits",
        .category = RV64_JIT_METRIC_RUN_TIME, .value = &rv64_jit_stats.direct_mmio_load_route_hits,
        .recording_boundary = "Generated direct-MMIO load accepts a warm route and completes its direct load",
    },
    {
        .identity = "direct_mmio_load_route_misses", .key = NULL,
        .label = "Direct MMIO load route misses", .unit = "misses",
        .category = RV64_JIT_METRIC_RUN_TIME, .value = &rv64_jit_stats.direct_mmio_load_route_misses,
        .recording_boundary = "Generated load route tag mismatches before the PMEM proof; includes accesses which subsequently take PMEM",
    },
    {
        .identity = "direct_mmio_load_route_fills", .key = NULL,
        .label = "Direct MMIO load route fills", .unit = "fills",
        .category = RV64_JIT_METRIC_RUN_TIME, .value = &rv64_jit_stats.direct_mmio_load_route_fills,
        .recording_boundary = "Generated cold-map load publishes the selected map into its reusable route",
    },
    {
        .identity = "direct_mmio_store_route_hits", .key = NULL,
        .label = "Direct MMIO store route hits", .unit = "hits",
        .category = RV64_JIT_METRIC_RUN_TIME, .value = &rv64_jit_stats.direct_mmio_store_route_hits,
        .recording_boundary = "Generated direct-MMIO store accepts a warm route and completes its direct write",
    },
    {
        .identity = "direct_mmio_store_route_misses", .key = NULL,
        .label = "Direct MMIO store route misses", .unit = "misses",
        .category = RV64_JIT_METRIC_RUN_TIME, .value = &rv64_jit_stats.direct_mmio_store_route_misses,
        .recording_boundary = "Generated store route tag mismatches before the PMEM proof; includes accesses which subsequently take PMEM",
    },
    {
        .identity = "direct_mmio_store_route_fills", .key = NULL,
        .label = "Direct MMIO store route fills", .unit = "fills",
        .category = RV64_JIT_METRIC_RUN_TIME, .value = &rv64_jit_stats.direct_mmio_store_route_fills,
        .recording_boundary = "Generated cold-map store publishes the selected map into its reusable route",
    },
    {
        .identity = "helper_load_count", .key = NULL,
        .label = "Helper loads", .unit = "calls",
        .category = RV64_JIT_METRIC_RUN_TIME, .value = &rv64_jit_stats.helper_load_count,
        .recording_boundary = "Paged or Bare load helper starts an access, before translation or physical access can fail",
    },
    {
        .identity = "helper_store_count", .key = NULL,
        .label = "Helper stores", .unit = "calls",
        .category = RV64_JIT_METRIC_RUN_TIME, .value = &rv64_jit_stats.helper_store_count,
        .recording_boundary = "Paged or Bare store helper starts an access, before translation or physical access can fail",
    },
    {
        .identity = "paged_store_helper_continuations", .key = NULL,
        .label = "paged store helper continuations", .unit = "continuations",
        .category = RV64_JIT_METRIC_RUN_TIME, .value = &rv64_jit_stats.paged_store_helper_continuations,
        .recording_boundary = "Paged store helper completes a direct PMEM write and accepts native continuation",
    },
    {
        .identity = "bare_mmio_load_calls", .key = NULL,
        .label = "bare MMIO load calls", .unit = "calls",
        .category = RV64_JIT_METRIC_RUN_TIME, .value = &rv64_jit_stats.bare_mmio_load_calls,
        .recording_boundary = "Bare physical-load helper enters, immediately before the authoritative physical access",
    },
    {
        .identity = "bare_mmio_store_calls", .key = NULL,
        .label = "Bare MMIO store calls", .unit = "calls",
        .category = RV64_JIT_METRIC_RUN_TIME, .value = &rv64_jit_stats.bare_mmio_store_calls,
        .recording_boundary = "Bare physical-store helper enters, before map inspection and the authoritative physical write",
    },
    {
        .identity = "bare_mmio_store_continuations", .key = NULL,
        .label = "Bare MMIO store continuations", .unit = "continuations",
        .category = RV64_JIT_METRIC_RUN_TIME, .value = &rv64_jit_stats.bare_mmio_store_continuations,
        .recording_boundary = "Bare physical-store helper accepts continuation after epoch and CPU boundary checks",
    },
    {
        .identity = "bare_mmio_store_boundary_exits", .key = NULL,
        .label = "Bare MMIO store boundary exits", .unit = "exits",
        .category = RV64_JIT_METRIC_RUN_TIME, .value = &rv64_jit_stats.bare_mmio_store_boundary_exits,
        .recording_boundary = "Bare physical-store helper rejects continuation for a CPU boundary after checking the native-cache epoch",
    },
    {
        .identity = "bare_mmio_store_invalidation_exits", .key = NULL,
        .label = "Bare MMIO store invalidation exits", .unit = "exits",
        .category = RV64_JIT_METRIC_RUN_TIME, .value = &rv64_jit_stats.bare_mmio_store_invalidation_exits,
        .recording_boundary = "Bare physical-store helper rejects continuation because the physical write changed the native-cache epoch",
    },
    {
        .identity = "fp_helper_calls", .key = NULL,
        .label = "FP helper calls", .unit = "calls",
        .category = RV64_JIT_METRIC_RUN_TIME, .value = &rv64_jit_stats.fp_helper_calls,
        .recording_boundary = "FP bridge calls riscv_fpu_jit_exec, before inspecting the helper's returned progress",
    },
    {
        .identity = "fp_helper_continuations", .key = NULL,
        .label = "FP helper continuations", .unit = "continuations",
        .category = RV64_JIT_METRIC_RUN_TIME, .value = &rv64_jit_stats.fp_helper_continuations,
        .recording_boundary = "Generated non-memory FP helper success path accepts native continuation",
    },
    {
        .identity = "fp_helper_trap_exits", .key = NULL,
        .label = "FP helper trap exits", .unit = "exits",
        .category = RV64_JIT_METRIC_RUN_TIME, .value = &rv64_jit_stats.fp_helper_trap_exits,
        .recording_boundary = "FP bridge observes zero progress returned by riscv_fpu_jit_exec; preserves the existing trap-count meaning",
    },
    {
        .identity = "fp_helper_memory_exits", .key = NULL,
        .label = "FP helper memory exits", .unit = "exits",
        .category = RV64_JIT_METRIC_RUN_TIME, .value = &rv64_jit_stats.fp_helper_memory_exits,
        .recording_boundary = "Generated FP memory helper success path returns progress to the dispatcher",
    },
    {
        .identity = "direct_link_taken_count", .key = NULL,
        .label = "Direct links taken", .unit = "links",
        .category = RV64_JIT_METRIC_RUN_TIME, .value = &rv64_jit_stats.direct_link_taken_count,
        .recording_boundary = "Generated direct or indirect link accepts its target and budget before entering the native target",
    },
    {
        .identity = "direct_link_miss_count", .key = NULL,
        .label = "Direct link misses", .unit = "misses",
        .category = RV64_JIT_METRIC_RUN_TIME, .value = &rv64_jit_stats.direct_link_miss_count,
        .recording_boundary = "Generated link failure path returns to the dispatcher after target, context or budget rejection",
    },
    {
        .identity = "direct_branch_link_taken_count", .key = NULL,
        .label = "direct branch links taken", .unit = "links",
        .category = RV64_JIT_METRIC_RUN_TIME, .value = &rv64_jit_stats.direct_branch_link_taken_count,
        .recording_boundary = "Generated taken-branch link passes target guards and budget checks",
    },
    {
        .identity = "direct_guarded_link_taken_count", .key = NULL,
        .label = "direct guarded links taken", .unit = "links",
        .category = RV64_JIT_METRIC_RUN_TIME, .value = &rv64_jit_stats.direct_guarded_link_taken_count,
        .recording_boundary = "Generated accepted link targets a block using translated fetch or data-state validation",
    },
    {
        .identity = "patched_direct_link_taken_count", .key = NULL,
        .label = "Patched direct links taken", .unit = "links",
        .category = RV64_JIT_METRIC_RUN_TIME, .value = &rv64_jit_stats.patched_direct_link_taken_count,
        .recording_boundary = "Generated patched-entry veneer accepts a non-dynamic direct link's whole-target budget",
    },
    {
        .identity = "direct_link_patch_resolutions", .key = NULL,
        .label = "Direct link patch resolutions", .unit = "patches",
        .category = RV64_JIT_METRIC_MAINTENANCE, .value = &rv64_jit_stats.direct_link_patch_resolutions,
        .recording_boundary = "C patch publication completes for a non-dynamic direct link",
    },
    {
        .identity = "direct_link_patch_unlinks", .key = NULL,
        .label = "Direct link patch unlinks", .unit = "unlinks",
        .category = RV64_JIT_METRIC_MAINTENANCE, .value = &rv64_jit_stats.direct_link_patch_unlinks,
        .recording_boundary = "C restores the guarded selector of a previously patched non-dynamic direct link",
    },
    {
        .identity = "direct_return_link_taken_count", .key = NULL,
        .label = "Direct return links taken", .unit = "links",
        .category = RV64_JIT_METRIC_RUN_TIME, .value = &rv64_jit_stats.direct_return_link_taken_count,
        .recording_boundary = "Generated canonical-return link accepts its target and budget, through guarded or patched entry",
    },
    {
        .identity = "direct_return_link_miss_count", .key = NULL,
        .label = "Direct return link misses", .unit = "misses",
        .category = RV64_JIT_METRIC_RUN_TIME, .value = &rv64_jit_stats.direct_return_link_miss_count,
        .recording_boundary = "Generated canonical-return link returns to the dispatcher after failed target or budget checks",
    },
    {
        .identity = "direct_jalr_link_taken_count", .key = "direct_jalr_link.taken",
        .label = "Direct JALR links taken", .unit = "links",
        .category = RV64_JIT_METRIC_RUN_TIME, .value = &rv64_jit_stats.direct_jalr_link_taken_count,
        .recording_boundary = "Generated non-return JALR link accepts its target and budget, through guarded or patched entry",
    },
    {
        .identity = "direct_jalr_link_miss_count", .key = "direct_jalr_link.misses",
        .label = "Direct JALR link misses", .unit = "misses",
        .category = RV64_JIT_METRIC_RUN_TIME, .value = &rv64_jit_stats.direct_jalr_link_miss_count,
        .recording_boundary = "Generated non-return JALR link returns to the dispatcher after failed target or budget checks",
    },
    {
        .identity = "ifetch_generation_fast_hits", .key = NULL,
        .label = "Ifetch generation fast hits", .unit = "hits",
        .category = RV64_JIT_METRIC_MAINTENANCE, .value = &rv64_jit_stats.ifetch_generation_fast_hits,
        .recording_boundary = "Block matching accepts a translated block whose recorded fetch generation still matches",
    },
    {
        .identity = "ifetch_generation_revalidations", .key = NULL,
        .label = "Ifetch generation revalidations", .unit = "revalidations",
        .category = RV64_JIT_METRIC_MAINTENANCE, .value = &rv64_jit_stats.ifetch_generation_revalidations,
        .recording_boundary = "Block matching starts translated-source revalidation after a fetch-generation mismatch",
    },
    {
        .identity = "ifetch_generation_bumps", .key = NULL,
        .label = "Ifetch generation bumps", .unit = "bumps",
        .category = RV64_JIT_METRIC_MAINTENANCE, .value = &rv64_jit_stats.ifetch_generation_bumps,
        .recording_boundary = "Fetch-generation bump advances its independent generation counter",
    },
    {
        .identity = "ifetch_generation_avoided_bumps", .key = NULL,
        .label = "unrelated ifetch writes avoided", .unit = "writes",
        .category = RV64_JIT_METRIC_MAINTENANCE, .value = &rv64_jit_stats.ifetch_generation_avoided_bumps,
        .recording_boundary = "A positive-length PMEM invalidation with an allocated arena misses all live fetch page-table dependencies",
    },
    {
        .identity = "source_reverse_invalidations", .key = NULL,
        .label = "Source reverse invalidations", .unit = "traversals",
        .category = RV64_JIT_METRIC_MAINTENANCE, .value = &rv64_jit_stats.source_reverse_invalidations,
        .recording_boundary = "Potential source write selects exact source reverse-map traversal, even if no block is ultimately discarded",
    },
    {
        .identity = "source_full_invalidation_scans", .key = NULL,
        .label = "Source full invalidation scans", .unit = "scans",
        .category = RV64_JIT_METRIC_MAINTENANCE, .value = &rv64_jit_stats.source_full_invalidation_scans,
        .recording_boundary = "Potential source write cannot use a chunk range and begins the fallback full-cache scan",
    },
    {
        .identity = "source_link_sequential_allocations", .key = NULL,
        .label = "Source-link sequential allocations", .unit = "nodes",
        .category = RV64_JIT_METRIC_MAINTENANCE, .value = &rv64_jit_stats.source_link_sequential_allocations,
        .recording_boundary = "Reverse-map allocation consumes a fresh non-sentinel node from the sequential pool",
    },
    {
        .identity = "source_link_recycled_allocations", .key = NULL,
        .label = "Source-link recycled allocations", .unit = "nodes",
        .category = RV64_JIT_METRIC_MAINTENANCE, .value = &rv64_jit_stats.source_link_recycled_allocations,
        .recording_boundary = "Reverse-map allocation takes a node from the recycled free list",
    },
    {
        .identity = "invalidation_requests", .key = NULL,
        .label = "Invalidation requests", .unit = "requests",
        .category = RV64_JIT_METRIC_MAINTENANCE, .value = &rv64_jit_stats.invalidation_requests,
        .recording_boundary = "Physical invalidation function is entered, including calls rejected by length or absent-arena guards",
    },
    {
        .identity = "invalidated_blocks", .key = NULL,
        .label = "Invalidated blocks", .unit = "blocks",
        .category = RV64_JIT_METRIC_MAINTENANCE, .value = &rv64_jit_stats.invalidated_blocks,
        .recording_boundary = "Physical-write invalidation discards a valid block whose exact source overlaps the write",
    },
    {
        .identity = "arena_resets", .key = NULL,
        .label = "Arena resets", .unit = "resets",
        .category = RV64_JIT_METRIC_MAINTENANCE, .value = &rv64_jit_stats.arena_resets,
        .recording_boundary = "Arena reset completes discarding blocks, resetting allocation and advancing the native-cache epoch",
    },
    {
        .identity = "emitted_sites.native_loads", .key = NULL,
        .label = "Native load sites", .unit = "sites",
        .category = RV64_JIT_METRIC_EMITTED_SITES, .value = &rv64_jit_stats.emitted_sites.native_loads,
        .recording_boundary = "Integer native load emission succeeds; restored with an abandoned instruction or compilation attempt",
    },
    {
        .identity = "emitted_sites.native_stores", .key = NULL,
        .label = "Native store sites", .unit = "sites",
        .category = RV64_JIT_METRIC_EMITTED_SITES, .value = &rv64_jit_stats.emitted_sites.native_stores,
        .recording_boundary = "Integer native store emission succeeds; restored with an abandoned instruction or compilation attempt",
    },
    {
        .identity = "emitted_sites.native_jumps", .key = NULL,
        .label = "Native jump sites", .unit = "sites",
        .category = RV64_JIT_METRIC_EMITTED_SITES, .value = &rv64_jit_stats.emitted_sites.native_jumps,
        .recording_boundary = "JAL or JALR emission succeeds; restored with an abandoned instruction or compilation attempt",
    },
    {
        .identity = "emitted_sites.native_m_ops", .key = NULL,
        .label = "Native M sites", .unit = "sites",
        .category = RV64_JIT_METRIC_EMITTED_SITES, .value = &rv64_jit_stats.emitted_sites.native_m_ops,
        .recording_boundary = "Native M lowering succeeds; restored with an abandoned instruction or compilation attempt",
    },
    {
        .identity = "emitted_sites.native_fp_exact_sites", .key = NULL,
        .label = "Native exact FP sites", .unit = "sites",
        .category = RV64_JIT_METRIC_EMITTED_SITES, .value = &rv64_jit_stats.emitted_sites.native_fp_exact_sites,
        .recording_boundary = "Exact FP instruction lowering succeeds; restored with an abandoned instruction or compilation attempt",
    },
    {
        .identity = "emitted_sites.native_fp_memory_sites", .key = NULL,
        .label = "Native FP memory sites", .unit = "sites",
        .category = RV64_JIT_METRIC_EMITTED_SITES, .value = &rv64_jit_stats.emitted_sites.native_fp_memory_sites,
        .recording_boundary = "Native FP memory instruction lowering succeeds; restored with an abandoned instruction or compilation attempt",
    },
    {
        .identity = "emitted_sites.indirect_pic_sites", .key = NULL,
        .label = "emitted indirect PIC sites", .unit = "sites",
        .category = RV64_JIT_METRIC_EMITTED_SITES, .value = &rv64_jit_stats.emitted_sites.indirect_pic_sites,
        .recording_boundary = "Indirect sidecar selection allocates a PIC; restored with an abandoned instruction or compilation attempt",
    },
    {
        .identity = "emitted_sites.reg_cache_spills", .key = NULL,
        .label = "reg cache spills", .unit = "sequences",
        .category = RV64_JIT_METRIC_EMITTED_SITES, .value = &rv64_jit_stats.emitted_sites.reg_cache_spills,
        .recording_boundary = "Register allocation successfully emits a loaded dirty victim spill; restored with abandoned emitted code",
    },
    {
        .identity = "emitted_sites.reg_cache_dead_victims", .key = NULL,
        .label = "Register-cache dead victims", .unit = "victims",
        .category = RV64_JIT_METRIC_EMITTED_SITES, .value = &rv64_jit_stats.emitted_sites.reg_cache_dead_victims,
        .recording_boundary = "Register-cache replacement selects a dead loaded mapping; restored with abandoned emitted code",
    },
    {
        .identity = "emitted_sites.reg_cache_live_lru_avoided", .key = NULL,
        .label = "Register-cache live LRU avoided", .unit = "victims",
        .category = RV64_JIT_METRIC_EMITTED_SITES, .value = &rv64_jit_stats.emitted_sites.reg_cache_live_lru_avoided,
        .recording_boundary = "Dead-victim selection avoids a different live LRU victim; restored with abandoned emitted code",
    },
    {
        .identity = "emitted_sites.native_store_continuations", .key = NULL,
        .label = "native store continuations", .unit = "sites",
        .category = RV64_JIT_METRIC_EMITTED_SITES, .value = &rv64_jit_stats.emitted_sites.native_store_continuations,
        .recording_boundary = "Bare native-store lowering emits its continuing path; restored with abandoned emitted code",
    },
    {
        .identity = "emitted_sites.native_paged_loads", .key = NULL,
        .label = "Native paged load sites", .unit = "sites",
        .category = RV64_JIT_METRIC_EMITTED_SITES, .value = &rv64_jit_stats.emitted_sites.native_paged_loads,
        .recording_boundary = "Integer or FP paged-load lowering succeeds; restored with abandoned emitted code",
    },
    {
        .identity = "emitted_sites.native_paged_stores", .key = NULL,
        .label = "Native paged store sites", .unit = "sites",
        .category = RV64_JIT_METRIC_EMITTED_SITES, .value = &rv64_jit_stats.emitted_sites.native_paged_stores,
        .recording_boundary = "Integer or FP paged-store lowering succeeds; restored with abandoned emitted code",
    },
    {
        .identity = "emitted_sites.inline_paged_loads", .key = NULL,
        .label = "Inline paged load sites", .unit = "sites",
        .category = RV64_JIT_METRIC_EMITTED_SITES, .value = &rv64_jit_stats.emitted_sites.inline_paged_loads,
        .recording_boundary = "Integer or FP inline data-TLB load lowering succeeds; restored with abandoned emitted code",
    },
    {
        .identity = "emitted_sites.inline_paged_stores", .key = NULL,
        .label = "Inline paged store sites", .unit = "sites",
        .category = RV64_JIT_METRIC_EMITTED_SITES, .value = &rv64_jit_stats.emitted_sites.inline_paged_stores,
        .recording_boundary = "Integer or FP inline data-TLB store lowering succeeds; restored with abandoned emitted code",
    },
    {
        .identity = "emitted_sites.direct_mmio_load_sites", .key = NULL,
        .label = "inline direct MMIO load sites", .unit = "sites",
        .category = RV64_JIT_METRIC_EMITTED_SITES, .value = &rv64_jit_stats.emitted_sites.direct_mmio_load_sites,
        .recording_boundary = "Bare native-load lowering includes a direct-MMIO probe; restored with abandoned emitted code",
    },
    {
        .identity = "emitted_sites.direct_mmio_store_sites", .key = NULL,
        .label = "inline direct MMIO store sites", .unit = "sites",
        .category = RV64_JIT_METRIC_EMITTED_SITES, .value = &rv64_jit_stats.emitted_sites.direct_mmio_store_sites,
        .recording_boundary = "Bare native-store lowering includes a direct-MMIO probe; restored with abandoned emitted code",
    },
    {
        .identity = "emitted_sites.fp_helper_sites", .key = NULL,
        .label = "FP helper sites", .unit = "sites",
        .category = RV64_JIT_METRIC_EMITTED_SITES, .value = &rv64_jit_stats.emitted_sites.fp_helper_sites,
        .recording_boundary = "General FP helper or FP memory slow-path lowering succeeds; restored with abandoned emitted code",
    },
    {
        .identity = "emitted_sites.fp_helper_gpr_effect_sites", .key = NULL,
        .label = "FP helper classified GPR sites", .unit = "sites",
        .category = RV64_JIT_METRIC_EMITTED_SITES, .value = &rv64_jit_stats.emitted_sites.fp_helper_gpr_effect_sites,
        .recording_boundary = "Selective helper effects are applied to continuing register metadata; restored with abandoned emitted code",
    },
    {
        .identity = "emitted_sites.fp_helper_gpr_mappings_preserved", .key = NULL,
        .label = "FP helper preserved GPR mappings", .unit = "mappings",
        .category = RV64_JIT_METRIC_EMITTED_SITES, .value = &rv64_jit_stats.emitted_sites.fp_helper_gpr_mappings_preserved,
        .recording_boundary = "Selective helper effect adds loaded mappings remaining after output invalidation; restored with abandoned code",
    },
    {
        .identity = "emitted_sites.fp_helper_gpr_selective_invalidations", .key = NULL,
        .label = "FP helper selective GPR invalidations", .unit = "mappings",
        .category = RV64_JIT_METRIC_EMITTED_SITES, .value = &rv64_jit_stats.emitted_sites.fp_helper_gpr_selective_invalidations,
        .recording_boundary = "Selective helper effect adds loaded mappings discarded for written GPRs; restored with abandoned emitted code",
    },
    {
        .identity = "emitted_sites.fp_helper_gpr_input_flushes", .key = NULL,
        .label = "FP helper GPR input flushes", .unit = "sequences",
        .category = RV64_JIT_METRIC_EMITTED_SITES, .value = &rv64_jit_stats.emitted_sites.fp_helper_gpr_input_flushes,
        .recording_boundary = "Selective helper effect adds the input flush sequences emitted for this helper; restored with abandoned code",
    },
    {
        .identity = "emitted_sites.fp_helper_gpr_dirty_mappings_preserved", .key = NULL,
        .label = "FP helper preserved dirty GPR mappings", .unit = "mappings",
        .category = RV64_JIT_METRIC_EMITTED_SITES, .value = &rv64_jit_stats.emitted_sites.fp_helper_gpr_dirty_mappings_preserved,
        .recording_boundary = "Selective helper effect adds dirty mappings remaining after output invalidation; restored with abandoned code",
    },
    {
        .identity = "emitted_sites.fp_helper_gpr_trap_stores", .key = NULL,
        .label = "FP helper GPR trap stores", .unit = "sequences",
        .category = RV64_JIT_METRIC_EMITTED_SITES, .value = &rv64_jit_stats.emitted_sites.fp_helper_gpr_trap_stores,
        .recording_boundary = "Selective helper effect adds emitted trap-arm stores of deferred GPR values; restored with abandoned code",
    },
};

/* PIC values remain indexed by the existing architectural-purpose kind
 * enum. Keys below are suffixes within that bounded family, not format strings;
 * the two explicit kind names supply the unchanged public key prefixes. */
typedef enum
{
    RV64_JIT_PIC_METRIC_HITS,
    RV64_JIT_PIC_METRIC_SECONDARY_HITS,
    RV64_JIT_PIC_METRIC_MISSES,
    RV64_JIT_PIC_METRIC_FILLS,
    RV64_JIT_PIC_METRIC_REPLACEMENTS,
    RV64_JIT_PIC_METRIC_STALE_REJECTIONS,
    RV64_JIT_PIC_METRIC_BUDGET_REJECTIONS,
    RV64_JIT_PIC_METRIC_PATCH_RESOLUTIONS,
    RV64_JIT_PIC_METRIC_PATCH_UNLINKS,
    RV64_JIT_PIC_METRIC_SOURCE_DETACHES,
    RV64_JIT_PIC_METRIC_TARGET_DETACHES,
    RV64_JIT_PIC_METRIC_PATCHED_ENTRIES,
    RV64_JIT_PIC_METRIC_PATCH_DOWNGRADES,
    RV64_JIT_PIC_METRIC_COUNT,
} rv64_jit_pic_metric_t;

static const rv64_jit_counter_metric_t jit_indirect_pic_metrics[RV64_JIT_PIC_METRIC_COUNT] = {
    [RV64_JIT_PIC_METRIC_HITS] = {
        .identity = "indirect_pic_hits", .key = "hits",
        .label = "PIC hits", .unit = "hits",
        .category = RV64_JIT_METRIC_RUN_TIME, .value = rv64_jit_stats.indirect_pic_hits,
        .recording_boundary = "Generated guarded or patched PIC entry accepts a target and whole-target budget",
    },
    [RV64_JIT_PIC_METRIC_SECONDARY_HITS] = {
        .identity = "indirect_pic_secondary_hits", .key = "secondary_hits",
        .label = "PIC secondary hits", .unit = "hits",
        .category = RV64_JIT_METRIC_RUN_TIME, .value = rv64_jit_stats.indirect_pic_secondary_hits,
        .recording_boundary = "Generated accepted PIC entry selects its second way; also included in PIC hits",
    },
    [RV64_JIT_PIC_METRIC_MISSES] = {
        .identity = "indirect_pic_misses", .key = "misses",
        .label = "PIC misses", .unit = "misses",
        .category = RV64_JIT_METRIC_RUN_TIME, .value = rv64_jit_stats.indirect_pic_misses,
        .recording_boundary = "Generated PIC probe reaches authoritative lookup after empty, tag-mismatch or stale rejection; "
                              "excludes budget rejection",
    },
    [RV64_JIT_PIC_METRIC_FILLS] = {
        .identity = "indirect_pic_fills", .key = "fills",
        .label = "PIC fills", .unit = "fills",
        .category = RV64_JIT_METRIC_RUN_TIME, .value = rv64_jit_stats.indirect_pic_fills,
        .recording_boundary = "C authoritative refill finishes publishing a guarded or patched PIC target",
    },
    [RV64_JIT_PIC_METRIC_REPLACEMENTS] = {
        .identity = "indirect_pic_replacements", .key = "replacements",
        .label = "PIC replacements", .unit = "replacements",
        .category = RV64_JIT_METRIC_RUN_TIME, .value = rv64_jit_stats.indirect_pic_replacements,
        .recording_boundary = "C refill selects an occupied round-robin victim; an in-place refresh of the same target is excluded",
    },
    [RV64_JIT_PIC_METRIC_STALE_REJECTIONS] = {
        .identity = "indirect_pic_stale_rejections", .key = "stale_rejections",
        .label = "PIC stale rejections", .unit = "rejections",
        .category = RV64_JIT_METRIC_RUN_TIME, .value = rv64_jit_stats.indirect_pic_stale_rejections,
        .recording_boundary = "Generated tag-matching guarded probe rejects target validity or publication identity; also included in PIC misses",
    },
    [RV64_JIT_PIC_METRIC_BUDGET_REJECTIONS] = {
        .identity = "indirect_pic_budget_rejections", .key = "budget_rejections",
        .label = "PIC budget rejections", .unit = "rejections",
        .category = RV64_JIT_METRIC_RUN_TIME, .value = rv64_jit_stats.indirect_pic_budget_rejections,
        .recording_boundary = "Generated guarded or patched PIC entry rejects the whole-target budget without counting a PIC miss",
    },
    [RV64_JIT_PIC_METRIC_PATCH_RESOLUTIONS] = {
        .identity = "indirect_pic_patch_resolutions", .key = "patch_resolutions",
        .label = "PIC patch resolutions", .unit = "patches",
        .category = RV64_JIT_METRIC_MAINTENANCE, .value = rv64_jit_stats.indirect_pic_patch_resolutions,
        .recording_boundary = "C patch publication completes for a dynamic PIC link",
    },
    [RV64_JIT_PIC_METRIC_PATCH_UNLINKS] = {
        .identity = "indirect_pic_patch_unlinks", .key = "patch_unlinks",
        .label = "PIC patch unlinks", .unit = "unlinks",
        .category = RV64_JIT_METRIC_MAINTENANCE, .value = rv64_jit_stats.indirect_pic_patch_unlinks,
        .recording_boundary = "C restores the guarded selector of a previously patched dynamic PIC link",
    },
    [RV64_JIT_PIC_METRIC_SOURCE_DETACHES] = {
        .identity = "indirect_pic_source_detaches", .key = "source_detaches",
        .label = "PIC source detaches", .unit = "detaches",
        .category = RV64_JIT_METRIC_MAINTENANCE, .value = rv64_jit_stats.indirect_pic_source_detaches,
        .recording_boundary = "Source teardown removes a dynamic PIC link still attached to a target-slot list",
    },
    [RV64_JIT_PIC_METRIC_TARGET_DETACHES] = {
        .identity = "indirect_pic_target_detaches", .key = "target_detaches",
        .label = "PIC target detaches", .unit = "detaches",
        .category = RV64_JIT_METRIC_MAINTENANCE, .value = rv64_jit_stats.indirect_pic_target_detaches,
        .recording_boundary = "Target-slot teardown removes a dynamic PIC link from its target-owned list",
    },
    [RV64_JIT_PIC_METRIC_PATCHED_ENTRIES] = {
        .identity = "indirect_pic_patched_entries", .key = "patched_entries",
        .label = "PIC patched entries", .unit = "entries",
        .category = RV64_JIT_METRIC_RUN_TIME, .value = rv64_jit_stats.indirect_pic_patched_entries,
        .recording_boundary = "Generated patched PIC veneer accepts the target's whole-block budget; also included in PIC hits",
    },
    [RV64_JIT_PIC_METRIC_PATCH_DOWNGRADES] = {
        .identity = "indirect_pic_patch_downgrades", .key = "patch_downgrades",
        .label = "PIC patch downgrades", .unit = "downgrades",
        .category = RV64_JIT_METRIC_MAINTENANCE, .value = rv64_jit_stats.indirect_pic_patch_downgrades,
        .recording_boundary = "C replacement-churn handling finishes unpatching both ways and switches the PIC to guarded-only operation",
    },
};

typedef struct
{
    const char *key;
    const char *description;
} rv64_jit_reason_name_t;

static const rv64_jit_reason_name_t jit_indirect_pic_kind_names[RV64_JIT_INDIRECT_PIC_KIND_COUNT] = {
    [RV64_JIT_INDIRECT_PIC_RETURN] = {"return", "return"},
    [RV64_JIT_INDIRECT_PIC_JALR] = {"jalr", "JALR"},
};

/*
 * A report total may add several individually valid uint64_t counters.  Keep
 * those intermediate totals in 128 bits so the presentation layer does not wrap
 * before any source counter has wrapped.  A 128-bit unsigned value needs at most
 * 39 decimal digits plus its terminating null byte.
 */
typedef unsigned __int128 rv64_jit_wide_count_t;
#define RV64_JIT_WIDE_COUNT_BUFFER_SIZE 40u

static const rv64_jit_reason_name_t jit_block_end_reason_names[RV64_JIT_BLOCK_END_COUNT] = {
    [RV64_JIT_BLOCK_END_BUDGET] = {"budget", "instruction budget or trace limit"},
    [RV64_JIT_BLOCK_END_JUMP] = {"jump", "JAL or JALR ended the native region"},
    [RV64_JIT_BLOCK_END_CHAINED_LOOP] = {"chained-loop", "a native backedge ended the region"},
    [RV64_JIT_BLOCK_END_FP_MEMORY] = {"fp-memory", "an FP memory helper ended the region"},
    [RV64_JIT_BLOCK_END_SOURCE_BOUNDARY] = {"source-boundary", "fetch/source metadata could not be extended"},
    [RV64_JIT_BLOCK_END_UNSUPPORTED_AFTER_PREFIX] = {"unsupported-after-prefix", "the next instruction needs fallback"},
};

static const rv64_jit_reason_name_t jit_side_exit_reason_names[RV64_JIT_SIDE_EXIT_COUNT] = {
    [RV64_JIT_SIDE_EXIT_LOAD_GUARD] = {"load-guard", "load alignment, range, or translation guard"},
    [RV64_JIT_SIDE_EXIT_STORE_GUARD] = {"store-guard", "store alignment, range, or translation guard"},
    [RV64_JIT_SIDE_EXIT_STORE_SOURCE] = {"store-source", "store may modify compiled source or a translation dependency"},
    [RV64_JIT_SIDE_EXIT_STORE_HELPER] = {"store-helper", "bare store completed through a helper"},
    [RV64_JIT_SIDE_EXIT_PAGED_STORE_HELPER] = {"paged-store-helper", "translated store completed through a helper"},
    [RV64_JIT_SIDE_EXIT_BRANCH_TAKEN] = {"branch-taken", "taken branch returned to the dispatcher"},
    [RV64_JIT_SIDE_EXIT_CHAINED_OVER_BUDGET] = {"chained-over-budget", "another native loop lap exceeded its budget"},
    [RV64_JIT_SIDE_EXIT_JALR_MISALIGNED] = {"jalr-misaligned", "JALR target failed the IALIGN check"},
    [RV64_JIT_SIDE_EXIT_FP_FS_OFF] = {"fp-fs-off", "native exact or memory FP instruction observed FS=Off"},
};

static const char *const jit_m_operation_names[RV64_JIT_M_OP_COUNT] = {
    [RV64_JIT_M_OP_MUL] = "MUL",     [RV64_JIT_M_OP_MULH] = "MULH", [RV64_JIT_M_OP_MULHSU] = "MULHSU", [RV64_JIT_M_OP_MULHU] = "MULHU",
    [RV64_JIT_M_OP_DIV] = "DIV",     [RV64_JIT_M_OP_DIVU] = "DIVU", [RV64_JIT_M_OP_REM] = "REM",       [RV64_JIT_M_OP_REMU] = "REMU",
    [RV64_JIT_M_OP_MULW] = "MULW",   [RV64_JIT_M_OP_DIVW] = "DIVW", [RV64_JIT_M_OP_DIVUW] = "DIVUW",   [RV64_JIT_M_OP_REMW] = "REMW",
    [RV64_JIT_M_OP_REMUW] = "REMUW",
};

static const char *const jit_fp_exact_operation_names[RV64_JIT_FP_EXACT_OP_COUNT] = {
    [RV64_JIT_FP_EXACT_FMV_X_W] = "FMV.X.W",   [RV64_JIT_FP_EXACT_FMV_W_X] = "FMV.W.X",   [RV64_JIT_FP_EXACT_FMV_X_D] = "FMV.X.D",
    [RV64_JIT_FP_EXACT_FMV_D_X] = "FMV.D.X",   [RV64_JIT_FP_EXACT_FSGNJ_S] = "FSGNJ.S",   [RV64_JIT_FP_EXACT_FSGNJN_S] = "FSGNJN.S",
    [RV64_JIT_FP_EXACT_FSGNJX_S] = "FSGNJX.S", [RV64_JIT_FP_EXACT_FSGNJ_D] = "FSGNJ.D",   [RV64_JIT_FP_EXACT_FSGNJN_D] = "FSGNJN.D",
    [RV64_JIT_FP_EXACT_FSGNJX_D] = "FSGNJX.D", [RV64_JIT_FP_EXACT_FCLASS_S] = "FCLASS.S", [RV64_JIT_FP_EXACT_FCLASS_D] = "FCLASS.D",
};

static const char *const jit_fp_memory_operation_names[RV64_JIT_FP_MEMORY_OP_COUNT] = {
    [RV64_JIT_FP_MEMORY_FLW] = "FLW",
    [RV64_JIT_FP_MEMORY_FLD] = "FLD",
    [RV64_JIT_FP_MEMORY_FSW] = "FSW",
    [RV64_JIT_FP_MEMORY_FSD] = "FSD",
};

/* Multiply in 128 bits so long-running profiles cannot overflow before division. */
static uint64_t jit_scaled_ratio(rv64_jit_wide_count_t numerator, rv64_jit_wide_count_t denominator, uint64_t scale)
{
    if (denominator == 0)
    {
        return 0;
    }

    /*
     * Report inputs are sums of at most 128 uint64_t counters and the largest
     * scale is 10,000.  Their product therefore remains well inside 128 bits.
     */
    const rv64_jit_wide_count_t scaled = numerator * scale + denominator / 2u;
    return (uint64_t)(scaled / denominator);
}

/* Format one wide aggregate without narrowing it back to uint64_t. */
static void jit_format_wide_count(char buffer[RV64_JIT_WIDE_COUNT_BUFFER_SIZE], rv64_jit_wide_count_t value)
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
        Assert(digit_count < sizeof(reversed), "jit: wide statistics count exceeded its decimal buffer");
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

/* Resolve a report row by its named storage member. This cold lookup avoids
 * a second enum for every scalar and detects a forgotten metadata definition. */
static const rv64_jit_counter_metric_t *jit_primary_counter_metric(const uint64_t *value)
{
    for (size_t i = 0; i < ARRLEN(jit_primary_counter_metrics); i++)
    {
        if (jit_primary_counter_metrics[i].value == value)
        {
            return &jit_primary_counter_metrics[i];
        }
    }

    Assert(false, "jit: primary statistics row has no metadata");
    return NULL;
}

/* Repetitive aligned rows share their label, value and unit with metadata. */
static void jit_log_primary_count(const uint64_t *value)
{
    const rv64_jit_counter_metric_t *metric = jit_primary_counter_metric(value);
    jit_log_count(metric->label, *metric->value, metric->unit);
}

/* A plain legacy row has a fixed shape; retain its old optional suffix at the
 * call site. Compound rows and derived arithmetic keep explicit formatters. */
static void jit_log_primary_legacy_count(const uint64_t *value, const char *suffix)
{
    const rv64_jit_counter_metric_t *metric = jit_primary_counter_metric(value);
    Log("jit:   %s = %" PRIu64 "%s", metric->label, *metric->value, suffix);
}

/* Print an aggregate which may be wider than any one underlying counter. */
static void jit_log_wide_count(const char *label, rv64_jit_wide_count_t value, const char *unit)
{
    char value_text[RV64_JIT_WIDE_COUNT_BUFFER_SIZE];
    jit_format_wide_count(value_text, value);
    Log("jit:   %-38s = %12s %s", label, value_text, unit);
}

/* Print a percentage, or explain why no percentage exists yet. */
static void jit_log_percentage(const char *label, rv64_jit_wide_count_t part, rv64_jit_wide_count_t total)
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
    Log("jit:   %-38s = %6" PRIu64 ".%02" PRIu64 "%%  (%s / %s)", label, percent_x100 / 100u, percent_x100 % 100u, part_text, total_text);
}

/* Print an average, or explain that its denominator has no observations. */
static void jit_log_average(const char *label, uint64_t total, uint64_t sample_count, const char *unit)
{
    if (sample_count == 0)
    {
        Log("jit:   %-38s =          n/a (no samples)", label);
        return;
    }

    const uint64_t average_x100 = jit_scaled_ratio(total, sample_count, 100u);
    Log("jit:   %-38s = %9" PRIu64 ".%02" PRIu64 " %s", label, average_x100 / 100u, average_x100 % 100u, unit);
}

/* Return the standard major-opcode name used by the unprivileged ISA manual. */
static const char *jit_opcode_name(uint32_t opcode)
{
    switch (opcode)
    {
    case RISCV_OPCODE_LOAD:
        return "LOAD";
    case RISCV_FP_OPCODE_LOAD:
        return "LOAD-FP";
    case RISCV_OPCODE_MISC_MEM:
        return "MISC-MEM";
    case RISCV_OPCODE_OP_IMM:
        return "OP-IMM";
    case RISCV_OPCODE_AUIPC:
        return "AUIPC";
    case RISCV_OPCODE_OP_IMM_32:
        return "OP-IMM-32";
    case RISCV_OPCODE_STORE:
        return "STORE";
    case RISCV_FP_OPCODE_STORE:
        return "STORE-FP";
    case RISCV_OPCODE_AMO:
        return "AMO";
    case RISCV_OPCODE_OP:
        return "OP";
    case RISCV_OPCODE_LUI:
        return "LUI";
    case RISCV_OPCODE_OP_32:
        return "OP-32";
    case RISCV_FP_OPCODE_FMADD:
        return "MADD";
    case RISCV_FP_OPCODE_FMSUB:
        return "MSUB";
    case RISCV_FP_OPCODE_FNMSUB:
        return "NMSUB";
    case RISCV_FP_OPCODE_FNMADD:
        return "NMADD";
    case RISCV_FP_OPCODE_OP:
        return "OP-FP";
    case RISCV_OPCODE_OP_V:
        return "OP-V";
    case RISCV_OPCODE_BRANCH:
        return "BRANCH";
    case RISCV_OPCODE_JALR:
        return "JALR";
    case RISCV_OPCODE_JAL:
        return "JAL";
    case RISCV_OPCODE_SYSTEM:
        return "SYSTEM";
    case RISCV_OPCODE_OP_VE:
        return "OP-VE";
    default:
        return "reserved, custom, or non-32-bit";
    }
}

/* Sum one fixed-size distribution without hiding the individual categories. */
static rv64_jit_wide_count_t jit_sum_counts(const uint64_t *counts, uint32_t count)
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
    const rv64_jit_wide_count_t classified_cache_probes = (rv64_jit_wide_count_t)rv64_jit_stats.cache_hits + rv64_jit_stats.cache_misses;

    jit_log_section("Run time: dispatch and native execution");
    jit_log_primary_count(&rv64_jit_stats.exec_requests);
    jit_log_wide_count("Classified block-cache probes", classified_cache_probes, "probes");
    jit_log_percentage("Block-cache hit rate (incl. negative)", rv64_jit_stats.cache_hits, classified_cache_probes);

    /*
     * Keep the long-standing field names on one machine-friendly line.  Several
     * local and external scripts parse these substrings, while the rows above
     * provide the clearer human interpretation.
     */
    if (classified_cache_probes == 0)
    {
        Log("jit:   exec requests = %" PRIu64 ", cache hits = %" PRIu64 ", misses = %" PRIu64 ", hit rate = n/a", rv64_jit_stats.exec_requests,
            rv64_jit_stats.cache_hits, rv64_jit_stats.cache_misses);
    }
    else
    {
        const uint64_t cache_hit_pct = jit_scaled_ratio(rv64_jit_stats.cache_hits, classified_cache_probes, 10000u);
        Log("jit:   exec requests = %" PRIu64 ", cache hits = %" PRIu64 ", misses = %" PRIu64 ", hit rate = %" PRIu64 ".%02" PRIu64 "%%",
            rv64_jit_stats.exec_requests, rv64_jit_stats.cache_hits, rv64_jit_stats.cache_misses, cache_hit_pct / 100u, cache_hit_pct % 100u);
    }

    jit_log_primary_count(&rv64_jit_stats.blocks_executed);
    jit_log_primary_count(&rv64_jit_stats.executed_insns);
    jit_log_average("Guest instructions per native entry", rv64_jit_stats.executed_insns, rv64_jit_stats.blocks_executed, "instructions");
    jit_log_primary_count(&rv64_jit_stats.unsupported_hits);
    if (rv64_jit_stats.blocks_executed == 0)
    {
        Log("jit:   executed blocks = %" PRIu64 ", JIT instructions = %" PRIu64 ", avg executed block = n/a, unsupported hits = %" PRIu64,
            rv64_jit_stats.blocks_executed, rv64_jit_stats.executed_insns, rv64_jit_stats.unsupported_hits);
    }
    else
    {
        const uint64_t average_x100 = jit_scaled_ratio(rv64_jit_stats.executed_insns, rv64_jit_stats.blocks_executed, 100u);
        Log("jit:   executed blocks = %" PRIu64 ", JIT instructions = %" PRIu64 ", avg executed block = %" PRIu64 ".%02" PRIu64
            " insn, unsupported hits = %" PRIu64,
            rv64_jit_stats.blocks_executed, rv64_jit_stats.executed_insns, average_x100 / 100u, average_x100 % 100u, rv64_jit_stats.unsupported_hits);
    }

    jit_log_primary_legacy_count(&rv64_jit_stats.zero_side_exits, "");
    jit_log_primary_legacy_count(&rv64_jit_stats.cpu_boundary_breaks, "");
}

/* Report actual generated M operations separately from emitted M sites. */
static void jit_dump_native_m_execution_stats(void)
{
    jit_log_section("Run time: native RV64M execution");

    for (uint32_t op = 0; op < RV64_JIT_M_OP_COUNT; op++)
    {
        Log("jit: native M %s executions = %" PRIu64, jit_m_operation_names[op], rv64_jit_stats.native_m_executions[op]);
    }
}

/* Report exact FP bit operations which completed without entering C. */
static void jit_dump_native_fp_exact_execution_stats(void)
{
    jit_log_section("Run time: native exact floating-point execution");

    for (uint32_t op = 0; op < RV64_JIT_FP_EXACT_OP_COUNT; op++)
    {
        Log("jit: native exact FP %s executions = %" PRIu64, jit_fp_exact_operation_names[op], rv64_jit_stats.native_fp_exact_executions[op]);
    }
}

/* Report FP memory operations which completed through a guarded native path. */
static void jit_dump_native_fp_memory_execution_stats(void)
{
    jit_log_section("Run time: native floating-point memory execution");

    for (uint32_t op = 0; op < RV64_JIT_FP_MEMORY_OP_COUNT; op++)
    {
        Log("jit: native FP memory %s executions = %" PRIu64, jit_fp_memory_operation_names[op], rv64_jit_stats.native_fp_memory_executions[op]);
    }
}

/* Compile-stop reasons are normal compilation outcomes, not all fallbacks. */
static void jit_dump_block_end_stats(void)
{
    const rv64_jit_wide_count_t block_end_total = jit_sum_counts(rv64_jit_stats.block_end_by_reason, RV64_JIT_BLOCK_END_COUNT);

    jit_log_wide_count("Compiled block endings", block_end_total, "blocks");

    for (uint32_t reason = 0; reason < RV64_JIT_BLOCK_END_COUNT; reason++)
    {
        const uint64_t count = rv64_jit_stats.block_end_by_reason[reason];

        if (count != 0)
        {
            Log("jit:     block end %s = %" PRIu64 " (%s)", jit_block_end_reason_names[reason].key, count,
                jit_block_end_reason_names[reason].description);
        }
    }
}

/* Compilation summary: block outcomes and emitted sites, never executions. */
static void jit_dump_compilation_stats(void)
{
    jit_log_section("Compilation: blocks and emitted sites");
    jit_log_primary_count(&rv64_jit_stats.blocks_compiled);
    jit_log_primary_count(&rv64_jit_stats.blocks_unsupported);
    jit_log_primary_count(&rv64_jit_stats.compiled_insns);
    jit_log_average("Guest instruction sites per block", rv64_jit_stats.compiled_insns, rv64_jit_stats.blocks_compiled, "sites");

    /* Retain the old aggregate keys without making them the human-facing labels. */
    if (rv64_jit_stats.blocks_compiled == 0)
    {
        Log("jit:   compiled blocks = %" PRIu64 ", unsupported blocks = %" PRIu64 ", avg compiled length = n/a", rv64_jit_stats.blocks_compiled,
            rv64_jit_stats.blocks_unsupported);
    }
    else
    {
        const uint64_t average_x100 = jit_scaled_ratio(rv64_jit_stats.compiled_insns, rv64_jit_stats.blocks_compiled, 100u);
        Log("jit:   compiled blocks = %" PRIu64 ", unsupported blocks = %" PRIu64 ", avg compiled length = %" PRIu64 ".%02" PRIu64 " insn",
            rv64_jit_stats.blocks_compiled, rv64_jit_stats.blocks_unsupported, average_x100 / 100u, average_x100 % 100u);
    }

    jit_log_primary_legacy_count(&rv64_jit_stats.translated_blocks, " (blocks fetched through Sv39)");
    jit_log_primary_legacy_count(&rv64_jit_stats.translated_cross_page_blocks, "");
    jit_log_primary_legacy_count(&rv64_jit_stats.segmented_source_blocks, "");
    Log("jit:   trace blocks = %" PRIu64 ", trace instructions = %" PRIu64, rv64_jit_stats.trace_blocks, rv64_jit_stats.trace_insns);

    Log("jit:   emitted guest sites: native loads = %" PRIu64 ", native stores = %" PRIu64, rv64_jit_stats.emitted_sites.native_loads,
        rv64_jit_stats.emitted_sites.native_stores);
    Log("jit:   emitted guest sites: native jumps = %" PRIu64 ", native M ops = %" PRIu64 ", native exact FP ops = %" PRIu64
        ", native FP memory ops = %" PRIu64,
        rv64_jit_stats.emitted_sites.native_jumps, rv64_jit_stats.emitted_sites.native_m_ops, rv64_jit_stats.emitted_sites.native_fp_exact_sites,
        rv64_jit_stats.emitted_sites.native_fp_memory_sites);
    jit_log_primary_legacy_count(&rv64_jit_stats.emitted_sites.indirect_pic_sites, "");
    Log("jit:   emitted Sv39 sites: native paged loads = %" PRIu64 ", native paged stores = %" PRIu64,
        rv64_jit_stats.emitted_sites.native_paged_loads, rv64_jit_stats.emitted_sites.native_paged_stores);
    jit_log_primary_legacy_count(&rv64_jit_stats.emitted_sites.native_store_continuations, " emitted sites");
    Log("jit:   inline paged loads = %" PRIu64 ", inline paged stores = %" PRIu64 " emitted sites", rv64_jit_stats.emitted_sites.inline_paged_loads,
        rv64_jit_stats.emitted_sites.inline_paged_stores);
    jit_log_primary_legacy_count(&rv64_jit_stats.emitted_sites.direct_mmio_load_sites, " emitted sites");
    jit_log_primary_legacy_count(&rv64_jit_stats.emitted_sites.direct_mmio_store_sites, " emitted sites");
    jit_log_primary_legacy_count(&rv64_jit_stats.emitted_sites.reg_cache_spills, " emitted spill sequences");
    Log("jit:   reg cache liveness: dead victims = %" PRIu64 ", live LRU avoided = %" PRIu64, rv64_jit_stats.emitted_sites.reg_cache_dead_victims,
        rv64_jit_stats.emitted_sites.reg_cache_live_lru_avoided);
    Log("jit:   stable register loops = %" PRIu64 ", preloaded registers = %" PRIu64, rv64_jit_stats.stable_loop_blocks,
        rv64_jit_stats.stable_loop_preloaded_regs);

    jit_dump_block_end_stats();
}

/* Keep the shared RV32/RV64 FP summary stable for correctness-gate parsing. */
static void jit_dump_fp_helper_stats(void)
{
    jit_log_section("Floating-point helper sites and execution");
    Log("jit: FP helper sites = %" PRIu64 ", calls = %" PRIu64 ", continuations = %" PRIu64 ", trap exits = %" PRIu64 ", memory exits = %" PRIu64,
        rv64_jit_stats.emitted_sites.fp_helper_sites, rv64_jit_stats.fp_helper_calls, rv64_jit_stats.fp_helper_continuations,
        rv64_jit_stats.fp_helper_trap_exits, rv64_jit_stats.fp_helper_memory_exits);
    Log("jit:   FP helper GPR effects: classified sites = %" PRIu64 ", preserved mappings = %" PRIu64 ", selective invalidations = %" PRIu64
        ", input flushes = %" PRIu64 ", dirty mappings preserved = %" PRIu64 ", trap stores = %" PRIu64,
        rv64_jit_stats.emitted_sites.fp_helper_gpr_effect_sites, rv64_jit_stats.emitted_sites.fp_helper_gpr_mappings_preserved,
        rv64_jit_stats.emitted_sites.fp_helper_gpr_selective_invalidations, rv64_jit_stats.emitted_sites.fp_helper_gpr_input_flushes,
        rv64_jit_stats.emitted_sites.fp_helper_gpr_dirty_mappings_preserved, rv64_jit_stats.emitted_sites.fp_helper_gpr_trap_stores);
}

/* Memory summary: these counters measure helper and generated-code activity. */
static void jit_dump_memory_stats(void)
{
    const rv64_jit_wide_count_t data_tlb_lookups = (rv64_jit_wide_count_t)rv64_jit_stats.data_tlb_hits + rv64_jit_stats.data_tlb_misses;

    jit_log_section("Run time: memory and Sv39 data translation");
    jit_log_wide_count("Data-TLB lookups", data_tlb_lookups, "lookups");
    jit_log_percentage("Data-TLB hit rate", rv64_jit_stats.data_tlb_hits, data_tlb_lookups);
    Log("jit:   data TLB hits = %" PRIu64 ", misses = %" PRIu64, rv64_jit_stats.data_tlb_hits, rv64_jit_stats.data_tlb_misses);
    jit_log_primary_legacy_count(&rv64_jit_stats.data_tlb_fills, "");
    jit_log_primary_legacy_count(&rv64_jit_stats.data_tlb_flushes, "");
    jit_log_primary_legacy_count(&rv64_jit_stats.data_tlb_page_table_flushes, "");
    Log("jit:   data TLB direct loads = %" PRIu64 ", direct stores = %" PRIu64, rv64_jit_stats.data_tlb_direct_loads,
        rv64_jit_stats.data_tlb_direct_stores);
    Log("jit:   inline paged load hits = %" PRIu64 ", inline paged store hits = %" PRIu64 " successful accesses",
        rv64_jit_stats.inline_paged_load_hits, rv64_jit_stats.inline_paged_store_hits);
    jit_log_primary_legacy_count(&rv64_jit_stats.inline_direct_mmio_load_hits, "");
    jit_log_primary_legacy_count(&rv64_jit_stats.inline_direct_mmio_store_hits, "");
    Log("jit:   direct MMIO load routes: warm hits = %" PRIu64 ", misses = %" PRIu64 ", fills = %" PRIu64, rv64_jit_stats.direct_mmio_load_route_hits,
        rv64_jit_stats.direct_mmio_load_route_misses, rv64_jit_stats.direct_mmio_load_route_fills);
    Log("jit:   direct MMIO store routes: warm hits = %" PRIu64 ", misses = %" PRIu64 ", fills = %" PRIu64,
        rv64_jit_stats.direct_mmio_store_route_hits, rv64_jit_stats.direct_mmio_store_route_misses, rv64_jit_stats.direct_mmio_store_route_fills);
    Log("jit:   helper loads = %" PRIu64 ", helper stores = %" PRIu64, rv64_jit_stats.helper_load_count, rv64_jit_stats.helper_store_count);
    jit_log_primary_legacy_count(&rv64_jit_stats.paged_store_helper_continuations, "");
    jit_log_primary_legacy_count(&rv64_jit_stats.bare_mmio_load_calls, "");
    Log("jit:   bare MMIO store calls = %" PRIu64 ", continuations = %" PRIu64 ", boundary exits = %" PRIu64 ", invalidation exits = %" PRIu64,
        rv64_jit_stats.bare_mmio_store_calls, rv64_jit_stats.bare_mmio_store_continuations, rv64_jit_stats.bare_mmio_store_boundary_exits,
        rv64_jit_stats.bare_mmio_store_invalidation_exits);
}

/*
 * This compound line is an existing human-output contract. Keep its wording,
 * ordering and spacing explicit while sharing the table's field mapping with
 * the machine output; a new metric does not automatically extend this line.
 */
static void jit_dump_indirect_jump_cache_legacy_stats(void)
{
    Log("jit:   indirect jump cache sites = %" PRIu64 ", hits = %" PRIu64 ", misses = %" PRIu64 ", fills = %" PRIu64 ", replacements = %" PRIu64
        ", stale rejections = %" PRIu64 ", budget rejects = %" PRIu64,
        *jit_indirect_jump_cache_metrics[RV64_JIT_JUMP_CACHE_SITES].value,
        *jit_indirect_jump_cache_metrics[RV64_JIT_JUMP_CACHE_HITS].value,
        *jit_indirect_jump_cache_metrics[RV64_JIT_JUMP_CACHE_MISSES].value,
        *jit_indirect_jump_cache_metrics[RV64_JIT_JUMP_CACHE_FILLS].value,
        *jit_indirect_jump_cache_metrics[RV64_JIT_JUMP_CACHE_REPLACEMENTS].value,
        *jit_indirect_jump_cache_metrics[RV64_JIT_JUMP_CACHE_STALE_REJECTIONS].value,
        *jit_indirect_jump_cache_metrics[RV64_JIT_JUMP_CACHE_BUDGET_REJECTIONS].value);
}

/* Preserve both compound PIC lines, including the legacy spelling of JALR.
 * The same enum-indexed field mapping feeds the machine report below. */
static void jit_dump_indirect_pic_legacy_stats(void)
{
    for (uint32_t i = 0; i < RV64_JIT_INDIRECT_PIC_KIND_COUNT; i++)
    {
        Log("jit:   direct %s PIC hits = %" PRIu64 ", secondary hits = %" PRIu64 ", misses = %" PRIu64 ", fills = %" PRIu64
            ", replacements = %" PRIu64 ", stale rejections = %" PRIu64 ", budget rejects = %" PRIu64,
            jit_indirect_pic_kind_names[i].description,
            jit_indirect_pic_metrics[RV64_JIT_PIC_METRIC_HITS].value[i],
            jit_indirect_pic_metrics[RV64_JIT_PIC_METRIC_SECONDARY_HITS].value[i],
            jit_indirect_pic_metrics[RV64_JIT_PIC_METRIC_MISSES].value[i],
            jit_indirect_pic_metrics[RV64_JIT_PIC_METRIC_FILLS].value[i],
            jit_indirect_pic_metrics[RV64_JIT_PIC_METRIC_REPLACEMENTS].value[i],
            jit_indirect_pic_metrics[RV64_JIT_PIC_METRIC_STALE_REJECTIONS].value[i],
            jit_indirect_pic_metrics[RV64_JIT_PIC_METRIC_BUDGET_REJECTIONS].value[i]);
        Log("jit:   direct %s PIC patches = %" PRIu64 ", unlinks = %" PRIu64 ", source detaches = %" PRIu64 ", target detaches = %" PRIu64
            ", patched entries = %" PRIu64 ", churn downgrades = %" PRIu64,
            jit_indirect_pic_kind_names[i].description,
            jit_indirect_pic_metrics[RV64_JIT_PIC_METRIC_PATCH_RESOLUTIONS].value[i],
            jit_indirect_pic_metrics[RV64_JIT_PIC_METRIC_PATCH_UNLINKS].value[i],
            jit_indirect_pic_metrics[RV64_JIT_PIC_METRIC_SOURCE_DETACHES].value[i],
            jit_indirect_pic_metrics[RV64_JIT_PIC_METRIC_TARGET_DETACHES].value[i],
            jit_indirect_pic_metrics[RV64_JIT_PIC_METRIC_PATCHED_ENTRIES].value[i],
            jit_indirect_pic_metrics[RV64_JIT_PIC_METRIC_PATCH_DOWNGRADES].value[i]);
    }
}


/* Linking and validation counters are updated by running generated code or C. */
static void jit_dump_link_and_validation_stats(void)
{
    const rv64_jit_wide_count_t direct_link_attempts =
        (rv64_jit_wide_count_t)rv64_jit_stats.direct_link_taken_count + rv64_jit_stats.direct_link_miss_count;
    const rv64_jit_wide_count_t direct_return_link_attempts =
        (rv64_jit_wide_count_t)rv64_jit_stats.direct_return_link_taken_count + rv64_jit_stats.direct_return_link_miss_count;
    const rv64_jit_wide_count_t direct_jalr_link_attempts =
        (rv64_jit_wide_count_t)rv64_jit_stats.direct_jalr_link_taken_count + rv64_jit_stats.direct_jalr_link_miss_count;

    jit_log_section("Run time: direct links and validation");
    jit_log_percentage("Direct-link success rate", rv64_jit_stats.direct_link_taken_count, direct_link_attempts);
    Log("jit:   direct links taken = %" PRIu64 ", misses = %" PRIu64, rv64_jit_stats.direct_link_taken_count, rv64_jit_stats.direct_link_miss_count);
    jit_log_primary_legacy_count(&rv64_jit_stats.direct_branch_link_taken_count, "");
    jit_log_primary_legacy_count(&rv64_jit_stats.direct_guarded_link_taken_count, "");
    Log("jit:   patched direct links taken = %" PRIu64 ", resolutions = %" PRIu64 ", unlinks = %" PRIu64,
        rv64_jit_stats.patched_direct_link_taken_count, rv64_jit_stats.direct_link_patch_resolutions, rv64_jit_stats.direct_link_patch_unlinks);
    jit_log_percentage("Direct return-link success rate", rv64_jit_stats.direct_return_link_taken_count, direct_return_link_attempts);
    Log("jit:   direct return links taken = %" PRIu64 ", misses = %" PRIu64, rv64_jit_stats.direct_return_link_taken_count,
        rv64_jit_stats.direct_return_link_miss_count);
    jit_log_percentage("Direct JALR-link success rate", rv64_jit_stats.direct_jalr_link_taken_count, direct_jalr_link_attempts);
    Log("jit:   direct JALR links taken = %" PRIu64 ", misses = %" PRIu64, rv64_jit_stats.direct_jalr_link_taken_count,
        rv64_jit_stats.direct_jalr_link_miss_count);
    jit_dump_indirect_jump_cache_legacy_stats();

    jit_dump_indirect_pic_legacy_stats();

    Log("jit:   ifetch generation fast hits = %" PRIu64 ", revalidations = %" PRIu64 ", bumps = %" PRIu64, rv64_jit_stats.ifetch_generation_fast_hits,
        rv64_jit_stats.ifetch_generation_revalidations, rv64_jit_stats.ifetch_generation_bumps);
    jit_log_primary_legacy_count(&rv64_jit_stats.ifetch_generation_avoided_bumps, "");
}

/*
 * Keep machine-consumed counters separate from the human report.  The latter
 * is deliberately descriptive and may change wording or field order; this
 * opt-in channel is a small, stable interface for correctness checks.
 */
static bool jit_machine_stats_requested(void)
{
    const char *value = getenv("NEMU_RV64_JIT_STATS_KV");
    return value != NULL && strcmp(value, "1") == 0;
}

/* Null keys document deliberate non-export. No reporting descriptor can add
 * instrumentation, move a counter, or change the lifetime of native targets. */
static void jit_dump_machine_counter_group(const rv64_jit_counter_metric_t *metrics, size_t count)
{
    for (size_t i = 0; i < count; i++)
    {
        const rv64_jit_counter_metric_t *metric = &metrics[i];

        if (metric->key != NULL)
        {
            Log("jit-kv: %s=%" PRIu64, metric->key, *metric->value);
        }
    }
}

static void jit_dump_machine_link_stats(void)
{
    jit_dump_machine_counter_group(jit_indirect_jump_cache_metrics, ARRLEN(jit_indirect_jump_cache_metrics));

    for (uint32_t kind = 0; kind < RV64_JIT_INDIRECT_PIC_KIND_COUNT; kind++)
    {
        for (size_t i = 0; i < ARRLEN(jit_indirect_pic_metrics); i++)
        {
            const rv64_jit_counter_metric_t *metric = &jit_indirect_pic_metrics[i];
            Log("jit-kv: indirect_pic.%s.%s=%" PRIu64, jit_indirect_pic_kind_names[kind].key, metric->key, metric->value[kind]);
        }
    }

    jit_dump_machine_counter_group(jit_primary_counter_metrics, ARRLEN(jit_primary_counter_metrics));
}

/* Maintenance work is kept separate from hot-path execution counts. */
static void jit_dump_invalidation_stats(void)
{
    jit_log_section("Maintenance: cache invalidation");
    Log("jit:   invalidation requests = %" PRIu64 ", invalidated blocks = %" PRIu64 ", arena resets = %" PRIu64, rv64_jit_stats.invalidation_requests,
        rv64_jit_stats.invalidated_blocks, rv64_jit_stats.arena_resets);
    Log("jit:   source reverse invalidations = %" PRIu64 ", full scans = %" PRIu64, rv64_jit_stats.source_reverse_invalidations,
        rv64_jit_stats.source_full_invalidation_scans);
    Log("jit:   source reverse-map nodes: sequential allocations = %" PRIu64 ", recycled allocations = %" PRIu64 ", usable capacity = %u",
        rv64_jit_stats.source_link_sequential_allocations, rv64_jit_stats.source_link_recycled_allocations,
        (unsigned)(RV64_JIT_SOURCE_LINK_COUNT - 1u));
}

/* Print only non-zero fallback categories so the diagnostic list stays short. */
static void jit_dump_fallback_stats(void)
{
    const rv64_jit_wide_count_t unsupported_total = jit_sum_counts(rv64_jit_stats.unsupported_by_opcode, RISCV_OPCODE_MASK + 1u);
    const uint32_t unsupported_distinct = jit_count_nonzero(rv64_jit_stats.unsupported_by_opcode, RISCV_OPCODE_MASK + 1u);
    const rv64_jit_wide_count_t side_exit_total = jit_sum_counts(rv64_jit_stats.side_exit_by_reason, RV64_JIT_SIDE_EXIT_COUNT);

    jit_log_section("Fallback details: non-zero categories only");
    jit_log_wide_count("Unsupported instruction encounters", unsupported_total, "encounters");

    /* Preserve the old total/distinct keys for consumers of the previous report. */
    char unsupported_total_text[RV64_JIT_WIDE_COUNT_BUFFER_SIZE];
    jit_format_wide_count(unsupported_total_text, unsupported_total);
    Log("jit:   unsupported opcodes total = %s, distinct = %" PRIu32, unsupported_total_text, unsupported_distinct);
    for (uint32_t opcode = 0; opcode <= RISCV_OPCODE_MASK; opcode++)
    {
        const uint64_t count = rv64_jit_stats.unsupported_by_opcode[opcode];

        if (count != 0)
        {
            Log("jit:     unsupported opcode 0x%02x = %" PRIu64 " (%s)", opcode, count, jit_opcode_name(opcode));
        }
    }

    jit_log_wide_count("Run-time side exits", side_exit_total, "exits");

    for (uint32_t reason = 0; reason < RV64_JIT_SIDE_EXIT_COUNT; reason++)
    {
        const uint64_t count = rv64_jit_stats.side_exit_by_reason[reason];

        if (count != 0)
        {
            Log("jit:     side exit %s = %" PRIu64 " (%s)", jit_side_exit_reason_names[reason].key, count,
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
    jit_dump_native_m_execution_stats();
    jit_dump_native_fp_exact_execution_stats();
    jit_dump_native_fp_memory_execution_stats();
    jit_dump_compilation_stats();
    jit_dump_fp_helper_stats();
    jit_dump_memory_stats();
    jit_dump_link_and_validation_stats();
    jit_dump_invalidation_stats();
    jit_dump_fallback_stats();

    if (jit_machine_stats_requested())
    {
        jit_dump_machine_link_stats();
    }

    Log("jit: ============================================================");
}

#endif /* RV64_JIT_STATS */
#endif /* CONFIG_RV64 */
