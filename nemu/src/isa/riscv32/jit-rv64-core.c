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
/*
 * Incoming mutable links are owned by cache-slot identity, not by one target
 * generation. A collision or invalidation can therefore unpatch the current
 * owner while leaving its source records waiting for a later matching target.
 */
static rv64_jit_link_t *rv64_jit_link_slot_heads[RV64_JIT_CACHE_SIZE];
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
uint64_t rv64_jit_native_cache_epoch = 1;
/*
 * A PIC way uses this identity to distinguish successive owners of the same
 * direct-map slot. Never reset it with the executable arena: an old sidecar
 * must not mistake recycled storage for its former target publication.
 */
static uint64_t rv64_jit_next_block_generation = 1;
#if RV64_JIT_ENABLED
static bool rv64_jit_disabled = false;
#endif
static bool rv64_jit_env_disable = false;
static bool rv64_jit_env_disable_direct_link = false;
static bool rv64_jit_env_disable_return_link = false;
static bool rv64_jit_env_disable_fp_gpr_effects = false;
static bool rv64_jit_env_perf_map = false;
static bool rv64_jit_stats_enabled = false;
static bool rv64_jit_runtime_options_ready = false;
/* Current native-entry instruction budget, used by in-block chained loops. */
volatile uint32_t rv64_jit_entry_budget = 0;
/* Extra guest instructions completed by earlier chained loop laps. */
volatile uint32_t rv64_jit_loop_extra = 0;
/*
 * A completed helper may need the outer CPU loop to observe a newly raised
 * interrupt or emulator-state transition before another native block runs.
 */
volatile bool rv64_jit_cpu_boundary_requested = false;

/* Dynamic PIC refill shares the ordinary target-slot link lifecycle below. */
static void jit_link_unpatch(rv64_jit_link_t *link);
static void jit_link_try_patch(rv64_jit_link_t *link, rv64_jit_block_t *target);
static void jit_link_remove_from_slot(rv64_jit_link_t *link);
static void jit_link_add_to_slot(rv64_jit_link_t *link, uint32_t slot_index);

/*
 * Permanently return one churn-heavy source to its guarded two-way PIC.
 * Both selectors must become guarded before either reverse link is detached;
 * from then on exact slot generations, rather than target-owned lists, protect
 * the data-only publications against invalidation and cache-slot reuse.
 */
static void jit_indirect_pic_downgrade(rv64_jit_indirect_pic_t *pic)
{
    Assert(pic != NULL && !pic->guarded_only, "jit: invalid RV64 indirect PIC downgrade");

    for (uint32_t i = 0; i < RV64_JIT_INDIRECT_PIC_WAYS; i++)
    {
        rv64_jit_link_t *link = &pic->links[i];

        Assert(link->dynamic && link->source != NULL && link->source->valid, "jit: invalid RV64 PIC link during downgrade");
        jit_link_unpatch(link);
    }

    /* Both selectors are guarded before either target list loses ownership. */
    for (uint32_t i = 0; i < RV64_JIT_INDIRECT_PIC_WAYS; i++)
    {
        rv64_jit_link_t *link = &pic->links[i];

        Assert(!link->patched, "jit: RV64 PIC selector survived downgrade unpatching");
        if (link->target_slot_index != UINT32_MAX)
        {
            jit_link_remove_from_slot(link);
        }
        link->target_generation = 0;
        link->patch_eligible = false;
    }

    pic->guarded_only = 1;
    JIT_STAT_INC(indirect_pic_patch_downgrades[pic->kind]);
}

/* Allocate one process-unique, non-zero native block publication identity. */
uint64_t rv64_jit_allocate_block_generation(void)
{
    Assert(rv64_jit_next_block_generation != UINT64_MAX, "jit: RV64 block generation space exhausted");
    return rv64_jit_next_block_generation++;
}

/* Publish one cold authoritative lookup result into a two-way indirect PIC. */
rv64_jit_entry_t rv64_jit_indirect_pic_refill(rv64_jit_indirect_pic_t *pic, vaddr_t target_pc, rv64_jit_block_t *target_slot)
{
    Assert(pic != NULL, "jit: missing RV64 indirect PIC sidecar");
    Assert(pic->kind < RV64_JIT_INDIRECT_PIC_KIND_COUNT, "jit: invalid RV64 indirect PIC kind %u", pic->kind);
    Assert(target_slot != NULL && target_slot->valid && target_slot->generation != 0 && target_slot->pc == target_pc && !target_slot->translated &&
               !target_slot->uses_data_state && target_slot->body_entry != NULL,
           "jit: unsafe RV64 indirect PIC refill target");

    uint32_t victim = RV64_JIT_INDIRECT_PIC_WAYS;

    /* Refresh a stale publication in place rather than creating duplicate tags. */
    for (uint32_t i = 0; i < RV64_JIT_INDIRECT_PIC_WAYS; i++)
    {
        if (pic->ways[i].target_pc == target_pc && pic->ways[i].target_slot == target_slot)
        {
            victim = i;
            break;
        }
    }

    if (victim == RV64_JIT_INDIRECT_PIC_WAYS)
    {
        victim = pic->next_victim;
        Assert(victim < RV64_JIT_INDIRECT_PIC_WAYS, "jit: invalid RV64 indirect PIC victim %u", victim);
        pic->next_victim = (uint8_t)((victim + 1u) % RV64_JIT_INDIRECT_PIC_WAYS);

        if (pic->ways[victim].target_generation != 0)
        {
            JIT_STAT_INC(indirect_pic_replacements[pic->kind]);

            if (!pic->guarded_only)
            {
                Assert(pic->patch_replacement_count < RV64_JIT_INDIRECT_PIC_PATCH_REPLACEMENT_LIMIT, "jit: RV64 PIC replacement counter overflow");
                pic->patch_replacement_count++;

                if (pic->patch_replacement_count == RV64_JIT_INDIRECT_PIC_PATCH_REPLACEMENT_LIMIT)
                {
                    jit_indirect_pic_downgrade(pic);
                }
            }
        }
    }

    rv64_jit_indirect_pic_entry_t *entry = &pic->ways[victim];
    rv64_jit_link_t *link = &pic->links[victim];

    Assert(link->dynamic && link->source != NULL && link->source->valid && link->pic_kind == pic->kind && link->pic_way == victim,
           "jit: invalid RV64 dynamic PIC link ownership");

    /*
     * Make the old direct destination unreachable before changing either its
     * tag or target-slot list ownership. A detached guarded selector remains
     * safe while the new publication metadata is prepared.
     */
    jit_link_unpatch(link);
    if (link->target_slot_index != UINT32_MAX)
    {
        jit_link_remove_from_slot(link);
    }

    /*
     * The current JIT executes on one vCPU thread. Still publish the generation
     * marker last so a future synchronised implementation has one clear field
     * to turn into an acquire/release protocol.
     */
    entry->target_generation = 0;
    entry->target_slot = target_slot;
    entry->target_pc = target_pc;

    if (pic->guarded_only)
    {
        Assert(!link->patched && !link->patch_eligible && link->target_slot_index == UINT32_MAX && link->target_generation == 0,
               "jit: downgraded RV64 PIC retained a direct edge");
        entry->target_generation = target_slot->generation;
        JIT_STAT_INC(indirect_pic_fills[pic->kind]);
        return target_slot->body_entry;
    }

    link->target_pc = target_pc;
    link->target_satp = target_slot->satp;
    link->target_ifetch_state = target_slot->ifetch_state;
    link->target_generation = target_slot->generation;
    jit_link_add_to_slot(link, (uint32_t)(target_slot - rv64_jit_cache));

    entry->target_generation = target_slot->generation;
    jit_link_try_patch(link, target_slot);
    Assert(link->patched, "jit: authoritative RV64 PIC refill did not patch its edge");
    JIT_STAT_INC(indirect_pic_fills[pic->kind]);
    return target_slot->body_entry;
}

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
    return value != NULL && value[0] != '\0' && !(value[0] == '0' && value[1] == '\0');
}

/* Cache runtime switches once so dispatch does not call getenv() repeatedly. */
static void rv64_jit_init_runtime_options(void)
{
    if (!rv64_jit_runtime_options_ready)
    {
        rv64_jit_env_disable = jit_env_flag_enabled("NEMU_DISABLE_JIT");
        rv64_jit_env_disable_direct_link = jit_env_flag_enabled("NEMU_DISABLE_RV64_JIT_DIRECT_LINK");
        rv64_jit_env_disable_return_link = jit_env_flag_enabled("NEMU_DISABLE_RV64_JIT_RETURN_LINK");
        rv64_jit_env_disable_fp_gpr_effects = jit_env_flag_enabled("NEMU_DISABLE_RV64_JIT_FP_GPR_EFFECTS");
        rv64_jit_env_perf_map = jit_env_flag_enabled("NEMU_JIT_PERFMAP");
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

/*
 * Return links are a separately measurable subset of direct links.  Disabling
 * all direct links necessarily disables them too, while their narrower switch
 * leaves known-target JAL, branch, and fall-through links unchanged.
 */
bool rv64_jit_return_link_enabled(void)
{
    rv64_jit_init_runtime_options();
    return !rv64_jit_env_disable_direct_link && !rv64_jit_env_disable_return_link;
}

/*
 * Keep an exact same-binary control for the FP helper cache policy.  This is
 * consulted only while compiling guest code, so the environment switch adds
 * no branch to a generated hot path.
 */
bool rv64_jit_fp_gpr_effects_enabled(void)
{
    rv64_jit_init_runtime_options();
    return !rv64_jit_env_disable_fp_gpr_effects;
}

/* Hash one fetch context and guest PC into the direct-mapped cache. */
static uint32_t jit_hash_context(vaddr_t pc, word_t satp, uint32_t ifetch_state)
{
    /*
     * `pc >> 2` drops fixed 4-byte instruction-alignment zeros. `satp >> 12`
     * mixes the PPN/ASID-like high bits with the raw CSR value.  Include the
     * fetch privilege so M/S/U entries for the same PC do not evict each other.
     */
    return rv64_jit_cache_hash_context(pc, satp, ifetch_state);
}

/* Hash the current fetch context and guest PC into the direct-mapped cache. */
static uint32_t jit_hash(vaddr_t pc, word_t satp)
{
    return jit_hash_context(pc, satp, rv64_jit_ifetch_state());
}

/* Return the cache slot for a PC under an already-known fetch context. */
rv64_jit_block_t *rv64_jit_cache_slot_context(vaddr_t pc, word_t satp, uint32_t ifetch_state)
{
    return &rv64_jit_cache[jit_hash_context(pc, satp, ifetch_state)];
}

/* Return the direct-mapped block-cache slot for the current PC. */
rv64_jit_block_t *rv64_jit_cache_slot(vaddr_t pc)
{
    return &rv64_jit_cache[jit_hash(pc, cpu.csr.satp)];
}

/*
 * Rewrite one emitted x86 rel32 displacement while arena code is quiescent.
 * The current JIT has one execution thread: patching happens in a C helper
 * reached from that thread, so no host can concurrently fetch this possibly
 * unaligned displacement. A future multi-vCPU JIT must replace this protocol
 * with an atomic patch site or a stop-the-world rendezvous.
 */
static void jit_link_patch_rel32(uint8_t *disp, const uint8_t *target)
{
    Assert(disp != NULL && target != NULL, "jit: invalid RV64 direct-link patch");

    const int64_t rel = target - (disp + sizeof(int32_t));
    Assert(rel >= INT32_MIN && rel <= INT32_MAX, "jit: RV64 direct-link target is out of rel32 range");
    const int32_t rel32 = (int32_t)rel;
    memcpy(disp, &rel32, sizeof(rel32));
    __builtin___clear_cache((char *)disp, (char *)(disp + sizeof(rel32)));
}

/* Restore one mutable source selector before its target can be discarded. */
static void jit_link_unpatch(rv64_jit_link_t *link)
{
    if (!link->patched)
    {
        return;
    }

    /*
     * Redirect the selector first. The old target displacement may retain a
     * stale address because it is unreachable once this write is visible.
     */
    jit_link_patch_rel32(link->selector_disp, link->guarded_path);
    link->patched = false;

    if (link->dynamic)
    {
        Assert(link->pic_kind < RV64_JIT_INDIRECT_PIC_KIND_COUNT, "jit: invalid RV64 PIC unlink kind %u", link->pic_kind);
        JIT_STAT_INC(indirect_pic_patch_unlinks[link->pic_kind]);
    }
    else
    {
        JIT_STAT_INC(direct_link_patch_unlinks);
    }
}

/* Resolve one waiting source only when the slot holds its exact safe target. */
static void jit_link_try_patch(rv64_jit_link_t *link, rv64_jit_block_t *target)
{
    if (link->patched || !link->patch_eligible || link->source == NULL || !link->source->valid || !target->valid || target->entry == NULL ||
        target->chain_entry == NULL || target->translated || target->uses_data_state || target->pc != link->target_pc ||
        target->satp != link->target_satp || target->ifetch_state != link->target_ifetch_state)
    {
        return;
    }

    if (link->dynamic && (link->target_generation == 0 || link->target_generation != target->generation))
    {
        return;
    }

    const uint8_t *chain_entry = (const uint8_t *)(uintptr_t)target->chain_entry;

    if (link->target_disp != NULL)
    {
        Assert(link->patched_path != NULL, "jit: RV64 link thunk has no patched entry");
        /*
         * Publish the thunk's destination before making the thunk reachable
         * from its selector.
         */
        jit_link_patch_rel32(link->target_disp, chain_entry);
        jit_link_patch_rel32(link->selector_disp, link->patched_path);
    }
    else
    {
        jit_link_patch_rel32(link->selector_disp, chain_entry);
    }

    link->patched = true;

    if (link->dynamic)
    {
        Assert(link->pic_kind < RV64_JIT_INDIRECT_PIC_KIND_COUNT, "jit: invalid RV64 PIC patch kind %u", link->pic_kind);
        JIT_STAT_INC(indirect_pic_patch_resolutions[link->pic_kind]);
    }
    else
    {
        JIT_STAT_INC(direct_link_patch_resolutions);
    }
}

/* Remove one source record from the persistent list for its target slot. */
static void jit_link_remove_from_slot(rv64_jit_link_t *link)
{
    Assert(link->target_slot_index < RV64_JIT_CACHE_SIZE, "jit: invalid RV64 direct-link target slot");

    if (link->slot_prev != NULL)
    {
        link->slot_prev->slot_next = link->slot_next;
    }
    else
    {
        Assert(rv64_jit_link_slot_heads[link->target_slot_index] == link, "jit: RV64 direct-link slot head mismatch");
        rv64_jit_link_slot_heads[link->target_slot_index] = link->slot_next;
    }

    if (link->slot_next != NULL)
    {
        link->slot_next->slot_prev = link->slot_prev;
    }

    link->slot_prev = NULL;
    link->slot_next = NULL;
    link->target_slot_index = UINT32_MAX;
}

/* Attach one detached source record to the persistent list for a target slot. */
static void jit_link_add_to_slot(rv64_jit_link_t *link, uint32_t slot_index)
{
    Assert(link != NULL && slot_index < RV64_JIT_CACHE_SIZE && link->target_slot_index == UINT32_MAX && link->slot_prev == NULL &&
               link->slot_next == NULL,
           "jit: invalid RV64 direct-link attachment");

    link->target_slot_index = slot_index;
    link->slot_next = rv64_jit_link_slot_heads[slot_index];

    if (link->slot_next != NULL)
    {
        link->slot_next->slot_prev = link;
    }

    rv64_jit_link_slot_heads[slot_index] = link;
}

/* Register every persistent edge owned by one newly published source block. */
void rv64_jit_links_source_published(rv64_jit_block_t *block)
{
    Assert(block != NULL && block->valid, "jit: publishing links for an invalid RV64 source block");

    for (uint32_t i = 0; i < block->outgoing_link_count; i++)
    {
        rv64_jit_link_t *link = &block->outgoing_links[i];
        const uint32_t slot_index = rv64_jit_cache_hash_context(link->target_pc, link->target_satp, link->target_ifetch_state);

        link->source = block;
        jit_link_add_to_slot(link, slot_index);
        jit_link_try_patch(link, &rv64_jit_cache[slot_index]);
    }

    if (block->indirect_pic != NULL)
    {
        for (uint32_t i = 0; i < RV64_JIT_INDIRECT_PIC_WAYS; i++)
        {
            rv64_jit_link_t *link = &block->indirect_pic->links[i];
            Assert(link->dynamic && link->source == NULL && link->target_slot_index == UINT32_MAX, "jit: invalid unpublished RV64 PIC link");
            link->source = block;
        }
    }
}

/* Resolve all waiting incoming edges after an exact target is published. */
void rv64_jit_links_target_published(rv64_jit_block_t *block)
{
    Assert(block >= rv64_jit_cache && block < rv64_jit_cache + RV64_JIT_CACHE_SIZE, "jit: RV64 target block is outside the cache");

    if (!block->valid)
    {
        return;
    }

    const uint32_t slot_index = (uint32_t)(block - rv64_jit_cache);

    for (rv64_jit_link_t *link = rv64_jit_link_slot_heads[slot_index]; link != NULL; link = link->slot_next)
    {
        jit_link_try_patch(link, block);
    }
}

/* Disconnect both incoming target users and outgoing source-owned records. */
void rv64_jit_links_block_discard(rv64_jit_block_t *block)
{
    Assert(block >= rv64_jit_cache && block < rv64_jit_cache + RV64_JIT_CACHE_SIZE, "jit: discarded RV64 block is outside the cache");

    const uint32_t slot_index = (uint32_t)(block - rv64_jit_cache);

    if (block->valid)
    {
        for (rv64_jit_link_t *link = rv64_jit_link_slot_heads[slot_index]; link != NULL;)
        {
            /* Detaching the current record rewrites its next pointer. */
            rv64_jit_link_t *next = link->slot_next;

            if (link->target_pc == block->pc && link->target_satp == block->satp && link->target_ifetch_state == block->ifetch_state)
            {
                jit_link_unpatch(link);

                /*
                 * A dynamic edge is tied to this exact publication
                 * generation. It cannot resolve against the replacement
                 * block, so remove it rather than retaining a permanently
                 * stale node on the target slot's incoming list. The owning
                 * PIC remains safe on its guarded path and an authoritative
                 * refill will attach the record to the new generation.
                 */
                if (link->dynamic)
                {
                    Assert(link->pic_kind < RV64_JIT_INDIRECT_PIC_KIND_COUNT, "jit: invalid RV64 PIC detach kind %u", link->pic_kind);
                    jit_link_remove_from_slot(link);
                    link->target_generation = 0;
                    JIT_STAT_INC(indirect_pic_target_detaches[link->pic_kind]);
                }
            }

            link = next;
        }
    }

    for (uint32_t i = 0; i < block->outgoing_link_count; i++)
    {
        rv64_jit_link_t *link = &block->outgoing_links[i];

        if (link->source != block)
        {
            continue;
        }

        jit_link_unpatch(link);
        jit_link_remove_from_slot(link);
        link->source = NULL;
    }

    if (block->indirect_pic != NULL)
    {
        for (uint32_t i = 0; i < RV64_JIT_INDIRECT_PIC_WAYS; i++)
        {
            rv64_jit_link_t *link = &block->indirect_pic->links[i];

            if (link->source != block)
            {
                continue;
            }

            jit_link_unpatch(link);
            if (link->target_slot_index != UINT32_MAX)
            {
                Assert(link->pic_kind < RV64_JIT_INDIRECT_PIC_KIND_COUNT, "jit: invalid RV64 PIC source-detach kind %u", link->pic_kind);
                jit_link_remove_from_slot(link);
                JIT_STAT_INC(indirect_pic_source_detaches[link->pic_kind]);
            }
            link->source = NULL;
            link->target_generation = 0;
        }
    }
}

/* Forget every arena-owned list node before the whole native arena is reused. */
void rv64_jit_links_reset(void)
{
    memset(rv64_jit_link_slot_heads, 0, sizeof(rv64_jit_link_slot_heads));
}

/* Clear every published block when arena or broad machine state changes. */
static void jit_cache_clear(void)
{
    rv64_jit_links_reset();
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

    void *mem = mmap(NULL, RV64_JIT_CODE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (mem == MAP_FAILED)
    {
        rv64_jit_disabled = true;
        Log("jit: mmap failed, disable RISC-V64 JIT");
        return false;
    }

    rv64_jit_code = (uint8_t *)mem;
    rv64_jit_code_used = 0;
    rv64_jit_source_reverse_map_init();
    isa_jit_invalidation_active = true;
    rv64_jit_init_runtime_options();
    rv64_jit_perf_map_init(rv64_jit_env_perf_map);
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
    rv64_jit_perf_map_reset();
    rv64_jit_code_used = 0;
    rv64_jit_native_cache_epoch++;
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
     * Only a page-table page referenced by a live translated block can make
     * that block's virtual-to-physical source mapping stale. Ordinary data
     * writes rely on the independent exact source-byte invalidation below and
     * no longer force every translated block through revalidation.
     */
    if (rv64_jit_write_may_touch_ifetch_page_table(addr, len))
    {
        rv64_jit_ifetch_generation_bump();
    }
    else
    {
        JIT_STAT_INC(ifetch_generation_avoided_bumps);
    }

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
        bool invalidated_source = false;

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

                if (block->valid && rv64_jit_block_source_overlaps(block, addr, len))
                {
                    rv64_jit_block_discard(block);
                    JIT_STAT_INC(invalidated_blocks);
                    invalidated_source = true;
                }

                node = next;
            }
        }

        if (invalidated_source)
        {
            rv64_jit_native_cache_epoch++;
        }

        return;
    }

    bool invalidated_source = false;

    JIT_STAT_INC(source_full_invalidation_scans);

    for (size_t i = 0; i < RV64_JIT_CACHE_SIZE; i++)
    {
        rv64_jit_block_t *block = &rv64_jit_cache[i];

        if (block->valid && rv64_jit_block_source_overlaps(block, addr, len))
        {
            rv64_jit_block_discard(block);
            JIT_STAT_INC(invalidated_blocks);
            invalidated_source = true;
        }
    }

    if (invalidated_source)
    {
        rv64_jit_native_cache_epoch++;
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

    uint32_t batch_budget = remaining > RV64_JIT_BATCH_MAX_INSNS ? RV64_JIT_BATCH_MAX_INSNS : (uint32_t)remaining;

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
        rv64_jit_cpu_boundary_requested = false;
        const uint32_t ran = block->entry();
        const bool cpu_boundary_requested = rv64_jit_cpu_boundary_requested;
        rv64_jit_cpu_boundary_requested = false;

        if (ran == 0)
        {
            JIT_STAT_INC(zero_side_exits);
            break;
        }

        Assert(ran <= remaining_budget, "jit: invalid RV64 executed count %u", ran);
        JIT_STAT_INC(blocks_executed);
        JIT_STAT_ADD(executed_insns, ran);
        total += ran;

        if (cpu_boundary_requested)
        {
            JIT_STAT_INC(cpu_boundary_breaks);
            break;
        }
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
