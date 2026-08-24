#include <generated/autoconf.h>

#ifndef CONFIG_RV64

#include "jit-rv32-internal.h"

#if RV32_JIT_ENABLED
#include <sys/mman.h>
#include <unistd.h>
#endif

/*
 * RV32 JIT core: shared state, runtime gates, cache and arena ownership,
 * physical invalidation, public hooks, and bounded native dispatch.
 */

/* Direct-mapped block cache indexed by a hash of guest PC and satp. */
rv32_jit_block_t rv32_jit_cache[RV32_JIT_CACHE_SIZE];
/* Small translated-PMEM cache used by JIT memory helpers in Sv32 mode. */
rv32_jit_tlb_entry_t rv32_jit_tlb[RV32_JIT_TLB_SIZE];
/*
 * Refcount PMEM pages that are currently used as page-table pages by cached JIT
 * TLB entries.  Stores can then test one indexed counter instead of scanning all
 * TLB entries, which matters because FCEUX performs huge numbers of stores.
 */
uint16_t rv32_jit_tlb_pt_page_refs[RV32_JIT_PMEM_PAGE_COUNT];
/*
 * Refcount per 128-byte PMEM source chunk. A non-zero value means at least one
 * native block was compiled from bytes in that chunk, so stores there may need
 * exact cache invalidation.
 */
uint16_t rv32_jit_source_chunk_refs[RV32_JIT_PMEM_CHUNK_COUNT];
/* Executable arena allocated with mmap(); emitted blocks live here. */
uint8_t *rv32_jit_code = NULL;
/* Number of bytes already used in `rv32_jit_code`, rounded up before each block. */
size_t rv32_jit_code_used = 0;
#if RV32_JIT_ENABLED
/* Sticky flag set when executable memory allocation fails. */
static bool jit_disabled = false;
#endif
#if RV32_JIT_STATS
rv32_jit_stats_t rv32_jit_stats;
#endif

/* Cached value of the runtime `NEMU_DISABLE_JIT` environment switch. */
static bool jit_env_disable = false;
/* True after runtime environment switches have been read once. */
static bool jit_runtime_options_ready = false;
/* Cached value of the runtime `NEMU_JIT_STATS` environment switch. */
static bool jit_stats_enabled = false;
/* Current native-entry instruction budget, used by in-block chained loops. */
volatile uint32_t rv32_jit_entry_budget = 0;
/* Extra instructions completed by chained loop laps before the final exit. */
volatile uint32_t rv32_jit_loop_extra = 0;
/* Public guard used by fast PMEM stores before calling invalidation hooks. */
bool isa_jit_invalidation_active = false;

/*
 * Read a simple boolean environment flag.
 *
 * Empty, missing, and exactly "0" mean false; any other non-empty value means
 * true. This keeps runtime switches easy to use from shell commands.
 */
static bool jit_env_flag_enabled(const char *name)
{
#if RV32_JIT_ENABLED
    const char *value = getenv(name);
    return value != NULL && value[0] != '\0' && !(value[0] == '0' && value[1] == '\0');
#else
    (void)name;
    return false;
#endif
}

/* Cache runtime environment switches once so hot dispatch does not call getenv(). */
static void jit_init_runtime_options(void)
{
    if (!jit_runtime_options_ready)
    {
        jit_env_disable = jit_env_flag_enabled("NEMU_DISABLE_JIT");
        jit_stats_enabled = jit_env_flag_enabled("NEMU_JIT_STATS");
        jit_runtime_options_ready = true;
    }
}

/* Report whether `NEMU_DISABLE_JIT` disabled native execution for this run. */
static bool jit_runtime_disabled(void)
{
    jit_init_runtime_options();
    return jit_env_disable;
}

/* Hash guest PC and address-space tag into the direct-mapped block cache. */
static uint32_t jit_hash(vaddr_t pc, word_t satp)
{
    return ((pc >> 2) ^ satp ^ (satp >> 12)) & (RV32_JIT_CACHE_SIZE - 1u);
}

/* Return the direct-mapped cache slot for the current PC and CPU satp tag. */
rv32_jit_block_t *rv32_jit_cache_slot(vaddr_t pc)
{
    return &rv32_jit_cache[jit_hash(pc, cpu.csr.satp)];
}

/* Clear every block cache slot and reset all source-chunk refcounts together. */
static void jit_cache_clear(void)
{
    memset(rv32_jit_cache, 0, sizeof(rv32_jit_cache));
    memset(rv32_jit_source_chunk_refs, 0, sizeof(rv32_jit_source_chunk_refs));
}

/*
 * Allocate the executable code arena on first use.
 *
 * The arena is RWX because this compact JIT emits bytes directly and then calls
 * them. If allocation fails, the sticky disabled flag avoids repeated mmap
 * attempts and execution falls back to the interpreter.
 */
bool rv32_jit_code_init(void)
{
#if RV32_JIT_ENABLED
    if (jit_disabled)
    {
        return false;
    }

    if (rv32_jit_code != NULL)
    {
        return true;
    }

    void *mem = mmap(NULL, RV32_JIT_CODE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (mem == MAP_FAILED)
    {
        jit_disabled = true;
        Log("jit: mmap failed, disable RISC-V32 JIT");
        return false;
    }

    rv32_jit_code = mem;
    rv32_jit_code_used = 0;
    isa_jit_invalidation_active = true;
    jit_cache_clear();
    Log("jit: RISC-V32 x86-64 code cache enabled, size = %u bytes", RV32_JIT_CODE_SIZE);
    return true;
#else
    return false;
#endif
}

/* Reuse the code arena from byte zero and forget all cached native blocks. */
void rv32_jit_arena_reset(void)
{
    JIT_STAT_INC(arena_resets);
    rv32_jit_code_used = 0;
    jit_cache_clear();
}

/* Public hook: report whether native RISC-V32 JIT execution can be attempted. */
bool isa_jit_available(void)
{
    return RV32_JIT_ENABLED && !jit_runtime_disabled();
}

/* Public hook: discard all native blocks after broad CPU or address-space change. */
void isa_jit_flush_all(void)
{
    /*
     * A full flush drops every piece of JIT-owned state: native code, source refs,
     * and local Sv32 translations.  Snapshot restore is the clearest example: PMEM
     * and CSRs may both change while old (pc, satp) tags still look plausible.
     */

    if (rv32_jit_code != NULL)
    {
        rv32_jit_arena_reset();
    }

    rv32_jit_tlb_flush();
}

/* Public hook: discard only the RV32 JIT's private Sv32 data translations. */
void isa_jit_flush_data_tlb(void)
{
    rv32_jit_tlb_flush();
}

/* Public hook: react to PMEM writes that can stale native code or JIT translations. */
void isa_jit_invalidate_paddr(paddr_t addr, int len)
{
    JIT_STAT_INC(invalidation_requests);

    /*
     * Physical writes are the common point shared by interpreter stores, JIT
     * helper stores, interpreter stores, and devices.  Two independent JIT caches
     * can become stale here:
     *
     *   1. native blocks translated from overwritten instruction bytes;
     *   2. local Sv32 translations whose root or level-0 PTE page was modified.
     *
     * The source-code check below uses the half-open interval [addr, addr + len).
     */

    if (len <= 0 || rv32_jit_code == NULL)
    {
        return;
    }

    if (rv32_jit_write_may_touch_page_table(addr, len))
    {
        rv32_jit_tlb_flush();
    }

    if (!rv32_jit_write_may_touch_source_chunk(addr, len))
    {
        JIT_STAT_INC(invalidation_page_skips);
        return;
    }

    const paddr_t end = addr + (paddr_t)len;

    for (size_t i = 0; i < RV32_JIT_CACHE_SIZE; i++)
    {
        rv32_jit_block_t *block = &rv32_jit_cache[i];

        if (!block->valid)
        {
            continue;
        }

        const paddr_t block_end = block->paddr_start + block->source_len;

        if (addr < block_end && end > block->paddr_start)
        {
            JIT_STAT_INC(invalidated_blocks);
            rv32_jit_block_discard(block);
        }
    }
}

/*
 * Public hook: execute cached or newly compiled native blocks.
 *
 * `remaining` is the CPU loop's instruction budget and `device_budget` is the
 * maximum number of instructions before the next device update. The function
 * writes the actual completed count to `*executed` and returns true only when at
 * least one guest instruction ran in native code.
 */
bool isa_jit_exec(uint64_t remaining, uint32_t device_budget, uint32_t *executed)
{
    *executed = 0;

    if (remaining == 0 || device_budget == 0)
    {
        return false;
    }

    /*
     * cpu_exec() already asks isa_jit_available() before entering its hot loop.
     * Keep the repeated block-dispatch path cheap, but still handle direct calls
     * before initialisation.
     */

    if (rv32_jit_code == NULL && !isa_jit_available())
    {
        return false;
    }

    JIT_STAT_INC(exec_requests);

    uint32_t batch_budget = remaining > RV32_JIT_BATCH_MAX_INSNS ? RV32_JIT_BATCH_MAX_INSNS : (uint32_t)remaining;

    if (batch_budget > device_budget)
    {
        batch_budget = device_budget;
    }

    uint32_t total = 0;

    while (total < batch_budget)
    {
        /*
         * Each native block reports how many guest instructions it completed. The
         * dispatcher uses that count, rather than assuming a fixed block length, so
         * helper exits and control-flow terminators keep device timing bounded.
         */
        uint32_t remaining_budget = batch_budget - total;
        uint32_t block_budget = remaining_budget;

        if (block_budget > RV32_JIT_BLOCK_MAX_INSNS)
        {
            block_budget = RV32_JIT_BLOCK_MAX_INSNS;
        }

        rv32_jit_block_t *block = rv32_jit_cache_slot(cpu.pc);

        if (rv32_jit_block_matches(block, cpu.pc))
        {
            /*
             * A valid longer block is useful cache state. If the current batch budget
             * cannot run it, return to cpu_exec() rather than replacing it with a
             * shorter budget-limited variant that would hurt later hot executions.
             */

            if (block->entry != NULL && block->insn_count > block_budget)
            {
                break;
            }

            JIT_STAT_INC(cache_hits);
        }
        else
        {
            JIT_STAT_INC(cache_misses);
            block = rv32_jit_compile_block(cpu.pc, block_budget);
        }

        if (block == NULL || !block->valid || block->entry == NULL)
        {
            if (block != NULL && block->valid && block->entry == NULL)
            {
                JIT_STAT_INC(unsupported_hits);
            }
            break;
        }

        rv32_jit_entry_budget = remaining_budget;
        rv32_jit_loop_extra = 0;
        const uint32_t ran = block->entry();
        Assert(ran > 0 && ran <= remaining_budget, "jit: invalid executed count %u", ran);
        JIT_STAT_INC(blocks_executed);
        JIT_STAT_ADD(executed_insns, ran);
        total += ran;
    }

    *executed = total;
    return total > 0;
}

/* Public hook: print optional JIT statistics at the end of execution. */
void isa_jit_dump_stats(void)
{
    jit_init_runtime_options();

    if (jit_runtime_disabled())
    {
        Log("jit: disabled by NEMU_DISABLE_JIT=1");
        return;
    }

#if RV32_JIT_STATS
    if (!jit_stats_enabled || !RV32_JIT_ENABLED || (rv32_jit_code == NULL && rv32_jit_stats.exec_requests == 0))
    {
        return;
    }

    rv32_jit_dump_stats_report();
#else
    if (jit_stats_enabled)
    {
        Log("jit: stats requested, but this binary was built without RV32_JIT_STATS=1");
    }
#endif
}

#endif /* !CONFIG_RV64 */
