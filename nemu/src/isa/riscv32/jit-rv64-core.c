#include <generated/autoconf.h>

#ifdef CONFIG_RV64

#include "jit-rv64-internal.h"

#if RV64_JIT_ENABLED
#include <sys/mman.h>
#include <unistd.h>
#endif

/*
 * RV64 JIT core: public hooks, runtime gates, global state, cache ownership,
 * executable arena management, invalidation entry points and statistics.
 *
 * RISC-V64 x86-64 JIT design.
 *
 * This JIT accelerates a conservative subset of the RV64 direct interpreter.
 * The interpreter remains the architectural reference: cases that are hard to
 * prove safe in native code fall back before an instruction can partially
 * commit. Native blocks return exact retired-instruction counts so cpu_exec()
 * keeps device polling and budget accounting unchanged.
 */

rv64_jit_block_t rv64_jit_cache[RV64_JIT_CACHE_SIZE];
rv64_jit_data_tlb_entry_t rv64_jit_data_tlb[RV64_JIT_DATA_TLB_SIZE];
uint16_t rv64_jit_data_tlb_pt_page_refs[RV64_JIT_PMEM_PAGE_COUNT];
uint32_t rv64_jit_ifetch_pt_page_refs[RV64_JIT_PMEM_PAGE_COUNT];
uint32_t rv64_jit_source_chunk_refs[RV64_JIT_PMEM_CHUNK_COUNT];
uint32_t rv64_jit_source_chunk_heads[RV64_JIT_PMEM_CHUNK_COUNT];
rv64_jit_source_link_t rv64_jit_source_links[RV64_JIT_SOURCE_LINK_COUNT];
uint32_t rv64_jit_source_link_free_head = RV64_JIT_SOURCE_LINK_NULL;
uint8_t *rv64_jit_code = NULL;
size_t rv64_jit_code_used = 0;
rv64_jit_stats_t rv64_jit_stats;
uint64_t rv64_jit_ifetch_generation = 1;
#if RV64_JIT_ENABLED
static bool rv64_jit_disabled = false;
#endif
static bool rv64_jit_env_disable = false;
static bool rv64_jit_env_disable_direct_link = false;
static bool rv64_jit_stats_enabled = false;
static bool rv64_jit_runtime_options_ready = false;
/* Current native-entry instruction budget, used by in-block chained loops. */
volatile uint32_t rv64_jit_entry_budget = 0;
/* Extra guest instructions completed by earlier chained loop laps. */
volatile uint32_t rv64_jit_loop_extra = 0;

/*
 * Public write-side guard. It becomes true after the native arena exists, so
 * PMEM writers know when exact physical invalidation may be needed.
 */
bool isa_jit_invalidation_active = false;

/* Record why one candidate instruction could not be emitted by this JIT. */
void rv64_jit_stat_unsupported_opcode(uint32_t instr)
{
    const uint32_t opcode = instr & RV64_OPCODE_MASK;
    JIT_STAT_INC(unsupported_by_opcode[opcode]);
#if !RV64_JIT_STATS
    (void)opcode;
#endif
}

/* Record the reason a compiled native block stopped growing. */
void rv64_jit_stat_block_end(rv64_jit_block_end_reason_t reason)
{
    JIT_STAT_INC(block_end_by_reason[reason]);
#if !RV64_JIT_STATS
    (void)reason;
#endif
}

/* Read simple environment flags: unset, empty, and exactly "0" mean false. */
static bool jit_env_flag_enabled(const char *name)
{
    const char *value = getenv(name);
    return value != NULL && value[0] != '\0' &&
           !(value[0] == '0' && value[1] == '\0');
}

/* Cache runtime switches once so dispatch does not call getenv() repeatedly. */
static void rv64_jit_init_runtime_options(void)
{
    if (!rv64_jit_runtime_options_ready)
    {
        rv64_jit_env_disable = jit_env_flag_enabled("NEMU_DISABLE_JIT");
        rv64_jit_env_disable_direct_link =
            jit_env_flag_enabled("NEMU_DISABLE_RV64_JIT_DIRECT_LINK");
        rv64_jit_stats_enabled = jit_env_flag_enabled("NEMU_JIT_STATS");
        rv64_jit_runtime_options_ready = true;
    }
}

/* Return whether runtime configuration has disabled this binary's RV64 JIT. */
static bool rv64_jit_runtime_disabled(void)
{
    rv64_jit_init_runtime_options();
    return rv64_jit_env_disable;
}

/* Return whether cross-block direct links should be emitted for this process. */
bool rv64_jit_direct_link_enabled(void)
{
    rv64_jit_init_runtime_options();
    return !rv64_jit_env_disable_direct_link;
}

/* Hash one fetch context and guest PC into the direct-mapped cache. */
static uint32_t jit_hash_context(vaddr_t pc, word_t satp, uint32_t ifetch_state)
{
    /*
     * `pc >> 2` drops fixed 4-byte instruction-alignment zeros. `satp >> 12`
     * mixes the PPN/ASID-like high bits with the raw CSR value.  Include the
     * fetch privilege so M/S/U entries for the same PC do not evict each other.
     */
    return (uint32_t)(((pc >> 2) ^ satp ^ (satp >> 12) ^ ifetch_state) &
                      (RV64_JIT_CACHE_SIZE - 1u));
}

/* Hash the current fetch context and guest PC into the direct-mapped cache. */
static uint32_t jit_hash(vaddr_t pc, word_t satp)
{
    return jit_hash_context(pc, satp, rv64_jit_ifetch_state());
}

/* Return the cache slot for a PC under an already-known fetch context. */
rv64_jit_block_t *rv64_jit_cache_slot_context(vaddr_t pc, word_t satp,
                                                uint32_t ifetch_state)
{
    return &rv64_jit_cache[jit_hash_context(pc, satp, ifetch_state)];
}

/* Return the direct-mapped block-cache slot for the current PC. */
rv64_jit_block_t *rv64_jit_cache_slot(vaddr_t pc)
{
    return &rv64_jit_cache[jit_hash(pc, cpu.csr.satp)];
}

/* Clear every published block when arena or broad machine state changes. */
static void jit_cache_clear(void)
{
    memset(rv64_jit_cache, 0, sizeof(rv64_jit_cache));
    memset(rv64_jit_source_chunk_refs, 0, sizeof(rv64_jit_source_chunk_refs));
    memset(rv64_jit_ifetch_pt_page_refs, 0, sizeof(rv64_jit_ifetch_pt_page_refs));
    rv64_jit_source_reverse_map_reset();
}

/* Allocate executable memory for generated x86-64 blocks. */
bool rv64_jit_code_init(void)
{
    if (rv64_jit_code != NULL)
    {
        return true;
    }

#if RV64_JIT_ENABLED
    if (rv64_jit_disabled)
    {
        return false;
    }

    void *mem = mmap(NULL, RV64_JIT_CODE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (mem == MAP_FAILED)
    {
        rv64_jit_disabled = true;
        Log("jit: mmap failed, disable RISC-V64 JIT");
        return false;
    }

    rv64_jit_code = (uint8_t *)mem;
    rv64_jit_code_used = 0;
    rv64_jit_source_reverse_map_reset();
    isa_jit_invalidation_active = true;
    Log("jit: RISC-V64 native code arena = %zu bytes", (size_t)RV64_JIT_CODE_SIZE);
    return true;
#else
    return false;
#endif
}

/* Reuse the executable arena after discarding every old code pointer. */
void rv64_jit_arena_reset(void)
{
    jit_cache_clear();
    rv64_jit_code_used = 0;
    JIT_STAT_INC(arena_resets);
}

/* Report whether native RV64 JIT execution can be attempted in this run. */
bool isa_jit_available(void)
{
    return RV64_JIT_ENABLED && !rv64_jit_runtime_disabled();
}

/* Drop all cached native blocks and reset private RV64 JIT state. */
void isa_jit_flush_all(void)
{
    if (rv64_jit_code != NULL)
    {
        rv64_jit_arena_reset();
    }
    rv64_jit_data_tlb_flush();
}

/* Flush only the JIT's local data translations after SFENCE.VMA. */
void isa_jit_flush_data_tlb(void)
{
    rv64_jit_data_tlb_flush();
    rv64_jit_ifetch_generation_bump();
}

/* Invalidate native blocks whose physical source bytes overlap a PMEM write. */
void isa_jit_invalidate_paddr(paddr_t addr, int len)
{
    JIT_STAT_INC(invalidation_requests);

    if (len <= 0 || rv64_jit_code == NULL)
    {
        return;
    }

    /*
     * This conservative generation bump covers page-table remaps for translated
     * instruction fetches.  Exact source-byte invalidation below still discards
     * native blocks whose physical code bytes changed.
     */
    rv64_jit_ifetch_generation_bump();

    if (rv64_jit_write_may_touch_data_tlb_page_table(addr, len))
    {
        JIT_STAT_INC(data_tlb_page_table_flushes);
        rv64_jit_data_tlb_flush();
    }

    if (!rv64_jit_write_may_touch_source_chunk(addr, len))
    {
        return;
    }

    size_t first = 0;
    size_t last = 0;

    if (rv64_jit_source_chunk_range(addr, (uint32_t)len, &first, &last))
    {
        JIT_STAT_INC(source_reverse_invalidations);

        for (size_t chunk = first; chunk <= last; chunk++)
        {
            uint32_t node = rv64_jit_source_chunk_heads[chunk];

            while (node != RV64_JIT_SOURCE_LINK_NULL)
            {
                /*
                 * Discarding a block removes its reverse-map links, including
                 * the current node.  Save `next` before the discard so this
                 * traversal can continue safely.
                 */
                const uint32_t next = rv64_jit_source_links[node].next;
                rv64_jit_block_t *block = &rv64_jit_cache[rv64_jit_source_links[node].block_index];

                if (block->valid &&
                    rv64_jit_block_source_overlaps(block, addr, len))
                {
                    rv64_jit_block_discard(block);
                    JIT_STAT_INC(invalidated_blocks);
                }

                node = next;
            }
        }

        return;
    }

    JIT_STAT_INC(source_full_invalidation_scans);
    for (size_t i = 0; i < RV64_JIT_CACHE_SIZE; i++)
    {
        rv64_jit_block_t *block = &rv64_jit_cache[i];

        if (block->valid &&
            rv64_jit_block_source_overlaps(block, addr, len))
        {
            rv64_jit_block_discard(block);
            JIT_STAT_INC(invalidated_blocks);
        }
    }
}

/*
 * Execute cached or newly compiled native RV64 blocks within the given budgets.
 *
 * This is the only entry point used by the generic CPU loop.  It first clamps
 * work to both the remaining instruction budget and the device-polling budget.
 * Each iteration then tries a direct cache hit, recompiles on a miss, or stops
 * cleanly on an unsupported negative entry.  A native function returning zero is
 * treated as a side exit that made no forward progress, so the interpreter can
 * execute the current instruction and report the precise trap or helper effect.
 *
 * The tiny loop ABI uses `rv64_jit_entry_budget` and `rv64_jit_loop_extra` so generated
 * chained loops can stay native while still returning exact retired counts.
 */
/* Execute cached or newly compiled native RV64 blocks within the given budgets. */
bool isa_jit_exec(uint64_t remaining, uint32_t device_budget, uint32_t *executed)
{
    *executed = 0;

    if (remaining == 0 || device_budget == 0 || !isa_jit_available())
    {
        return false;
    }

    JIT_STAT_INC(exec_requests);

    uint32_t batch_budget = remaining > RV64_JIT_BATCH_MAX_INSNS
                                ? RV64_JIT_BATCH_MAX_INSNS
                                : (uint32_t)remaining;

    if (batch_budget > device_budget)
    {
        batch_budget = device_budget;
    }

    uint32_t total = 0;

    while (total < batch_budget)
    {
        uint32_t remaining_budget = batch_budget - total;
        uint32_t block_budget = remaining_budget;

        if (block_budget > RV64_JIT_TRACE_MAX_INSNS)
        {
            block_budget = RV64_JIT_TRACE_MAX_INSNS;
        }

        rv64_jit_block_t *block = rv64_jit_cache_slot(cpu.pc);

        if (rv64_jit_block_matches(block, cpu.pc))
        {
            if (block->entry != NULL && block->insn_count > block_budget)
            {
                break;
            }
            JIT_STAT_INC(cache_hits);
        }
        else
        {
            JIT_STAT_INC(cache_misses);
            block = rv64_jit_compile_block(cpu.pc, block_budget);
        }

        if (block == NULL || !block->valid || block->entry == NULL)
        {
            if (block != NULL && block->valid && block->entry == NULL)
            {
                JIT_STAT_INC(unsupported_hits);
            }
            break;
        }

        /*
         * Chained loops use these two globals as a tiny ABI between cpu_exec()
         * and generated code. `rv64_jit_entry_budget` is the maximum work this entry
         * may retire; `rv64_jit_loop_extra` starts at zero and accumulates completed
         * native loop laps before the final block exit returns the total.
         */
        rv64_jit_entry_budget = remaining_budget;
        rv64_jit_loop_extra = 0;
        const uint32_t ran = block->entry();
        if (ran == 0)
        {
            JIT_STAT_INC(zero_side_exits);
            break;
        }

        Assert(ran <= remaining_budget,
               "jit: invalid RV64 executed count %u", ran);
        JIT_STAT_INC(blocks_executed);
        JIT_STAT_ADD(executed_insns, ran);
        total += ran;
    }

    *executed = total;
    return total > 0;
}

/* Gate and print the optional RV64 JIT report at the end of execution. */
void isa_jit_dump_stats(void)
{
    rv64_jit_init_runtime_options();

    if (rv64_jit_runtime_disabled())
    {
        Log("jit: disabled by NEMU_DISABLE_JIT=1");
        return;
    }

#if RV64_JIT_STATS
    if (!rv64_jit_stats_enabled || !RV64_JIT_ENABLED)
    {
        return;
    }

    rv64_jit_dump_stats_report();
#else
    if (rv64_jit_stats_enabled)
    {
        Log("jit: stats requested, but this binary was built without RV64_JIT_STATS=1");
    }
#endif
}
#endif /* CONFIG_RV64 */
