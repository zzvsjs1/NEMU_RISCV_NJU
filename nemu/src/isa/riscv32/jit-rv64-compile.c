#include <generated/autoconf.h>

#ifdef CONFIG_RV64

#include "jit-rv64-internal.h"

/*
 * RV64 JIT compile layer: RISC-V instruction scanning, block construction,
 * unsupported-instruction fallback and publication of compiled blocks.
 */
#ifdef CONFIG_RISCV_FPU
/* Recognise every scalar F/D major opcode handled by the shared executor. */
static bool jit_opcode_is_fp(uint32_t opcode)
{
    switch (opcode)
    {
    case RV64_FP_OPCODE_LOAD:
    case RV64_FP_OPCODE_STORE:
    case RV64_FP_OPCODE_FMADD:
    case RV64_FP_OPCODE_FMSUB:
    case RV64_FP_OPCODE_FNMSUB:
    case RV64_FP_OPCODE_FNMADD:
    case RV64_FP_OPCODE_OP:
        return true;
    default:
        return false;
    }
}
#endif

/*
 * Return whether an opcode class is worth following during the cheap loop
 * pre-scan.  This is only a hint: the real emitter still validates funct fields,
 * reserved encodings, memory guards, and output space instruction by instruction.
 */
static bool jit_opcode_may_be_in_loop_prescan(uint32_t instr)
{
    const uint32_t opcode = instr & RV64_OPCODE_MASK;

    switch (opcode)
    {
    case RV64_OPCODE_LOAD:
    case RV64_OPCODE_STORE:
    case RV64_OPCODE_OP_IMM:
    case RV64_OPCODE_OP_IMM_32:
    case RV64_OPCODE_OP:
    case RV64_OPCODE_OP_32:
    case RV64_OPCODE_AUIPC:
    case RV64_OPCODE_LUI:
    case RV64_OPCODE_BRANCH:
        return true;
    default:
        return false;
    }
}

typedef struct
{
    bool found_self_backedge;
    uint32_t stable_reg_mask;
    uint32_t stable_reg_count;
    uint32_t loop_insn_count;
} rv64_jit_loop_scan_t;

/* Add one non-zero architectural GPR to the stable-loop preload set. */
static void jit_stable_loop_add_reg(uint32_t *reg_mask, uint32_t reg)
{
    if (reg != RV64_GPR_ZERO)
    {
        *reg_mask |= 1u << reg;
    }
}

/*
 * Collect operands only for instructions that cannot call a helper or use R8
 * as a memory-emission scratch register.
 */
static bool jit_collect_stable_loop_regs(uint32_t instr, uint32_t *reg_mask)
{
    const uint32_t opcode = instr & RV64_OPCODE_MASK;

    switch (opcode)
    {
    case RV64_OPCODE_OP_IMM:
    case RV64_OPCODE_OP_IMM_32:
        jit_stable_loop_add_reg(reg_mask, rv64_instr_rd(instr));
        jit_stable_loop_add_reg(reg_mask, rv64_instr_rs1(instr));
        return true;
    case RV64_OPCODE_OP:
    case RV64_OPCODE_OP_32:
        /*
         * Some RV64M operations still call the pure C arithmetic helper. Keep
         * the complete M class on the ordinary six-slot mapping until every
         * member has a native lowering.
         */
        if (rv64_instr_funct7(instr) == RV64_FUNCT7_MULDIV)
        {
            return false;
        }

        jit_stable_loop_add_reg(reg_mask, rv64_instr_rd(instr));
        jit_stable_loop_add_reg(reg_mask, rv64_instr_rs1(instr));
        jit_stable_loop_add_reg(reg_mask, rv64_instr_rs2(instr));
        return true;
    case RV64_OPCODE_LUI:
    case RV64_OPCODE_AUIPC:
        jit_stable_loop_add_reg(reg_mask, rv64_instr_rd(instr));
        return true;
    case RV64_OPCODE_BRANCH:
        jit_stable_loop_add_reg(reg_mask, rv64_instr_rs1(instr));
        jit_stable_loop_add_reg(reg_mask, rv64_instr_rs2(instr));
        return true;
    default:
        return false;
    }
}

/*
 * Cheaply pre-scan for a conditional branch back to the block head.  Ordinary
 * branches are followed along their fall-through path; only a self-backedge is
 * a candidate for native loop chaining.
 */
static rv64_jit_loop_scan_t
jit_prescan_self_backedge(vaddr_t pc, uint32_t max_insns,
                          bool uses_translated_ifetch)
{
    rv64_jit_loop_scan_t result = {0};
    vaddr_t candidate_pc = pc;
    uint32_t scanned_insn_count = 0;
    uint32_t stable_reg_mask = 0;
    bool stable_mapping_eligible = true;

    while (scanned_insn_count < max_insns &&
           scanned_insn_count < RV64_JIT_TRACE_MAX_INSNS)
    {
        paddr_t candidate_paddr = 0;
        bool candidate_uses_translated_ifetch = false;

        if (!rv64_jit_translate_ifetch_ex(
                candidate_pc, &candidate_paddr,
                &candidate_uses_translated_ifetch) ||
            !in_pmem(candidate_paddr) ||
            candidate_uses_translated_ifetch != uses_translated_ifetch)
        {
            return result;
        }

        const uint32_t instr =
            (uint32_t)vaddr_ifetch(candidate_pc, RV64_INSN_SIZE);
        const uint32_t opcode = instr & RV64_OPCODE_MASK;
        const bool is_self_backedge =
            opcode == RV64_OPCODE_BRANCH &&
            candidate_pc + imm_b(instr) == pc;

        if (!jit_opcode_may_be_in_loop_prescan(instr))
        {
            return result;
        }

        /*
         * An earlier conditional branch can leave after several native laps.
         * Its exit code was emitted before later loop writes became dirty, so
         * neither architectural values nor the cached retired count are safe
         * there. Stable mapping is therefore limited to one terminal backedge.
         */
        if (stable_mapping_eligible &&
            opcode == RV64_OPCODE_BRANCH && !is_self_backedge)
        {
            stable_mapping_eligible = false;
        }

        if (stable_mapping_eligible &&
            !jit_collect_stable_loop_regs(instr, &stable_reg_mask))
        {
            stable_mapping_eligible = false;
        }

        if (is_self_backedge)
        {
            result.found_self_backedge = true;
            result.loop_insn_count = scanned_insn_count + 1u;

            if (stable_mapping_eligible)
            {
                const uint32_t reg_count =
                    (uint32_t)__builtin_popcount(stable_reg_mask);

                if (reg_count > 0 && reg_count <= RV64_JIT_HREG_COUNT)
                {
                    result.stable_reg_mask = stable_reg_mask;
                    result.stable_reg_count = reg_count;
                }
            }

            return result;
        }

        candidate_pc += RV64_INSN_SIZE;
        scanned_insn_count++;
    }

    return result;
}

/*
 * Counters in this snapshot describe native sites emitted while compiling.
 * They are transactional for the same reason as native bytes and register-cache
 * metadata: bytes which are rolled back or never published must not appear in
 * the final profile as usable emitted sites.
 */
#if RV64_JIT_STATS
typedef struct
{
    uint64_t native_loads;
    uint64_t native_stores;
    uint64_t native_jumps;
    uint64_t native_m_ops;
    uint64_t reg_cache_spills;
    uint64_t native_store_continuations;
    uint64_t native_paged_loads;
    uint64_t native_paged_stores;
    uint64_t inline_paged_loads;
    uint64_t inline_paged_stores;
    uint64_t fp_helper_sites;
} rv64_jit_emitted_site_stats_t;

static rv64_jit_emitted_site_stats_t jit_snapshot_emitted_site_stats(void)
{
    return (rv64_jit_emitted_site_stats_t){
        .native_loads = rv64_jit_stats.native_loads,
        .native_stores = rv64_jit_stats.native_stores,
        .native_jumps = rv64_jit_stats.native_jumps,
        .native_m_ops = rv64_jit_stats.native_m_ops,
        .reg_cache_spills = rv64_jit_stats.reg_cache_spills,
        .native_store_continuations = rv64_jit_stats.native_store_continuations,
        .native_paged_loads = rv64_jit_stats.native_paged_loads,
        .native_paged_stores = rv64_jit_stats.native_paged_stores,
        .inline_paged_loads = rv64_jit_stats.inline_paged_loads,
        .inline_paged_stores = rv64_jit_stats.inline_paged_stores,
        .fp_helper_sites = rv64_jit_stats.fp_helper_sites,
    };
}

static void jit_restore_emitted_site_stats(
    const rv64_jit_emitted_site_stats_t *snapshot)
{
    rv64_jit_stats.native_loads = snapshot->native_loads;
    rv64_jit_stats.native_stores = snapshot->native_stores;
    rv64_jit_stats.native_jumps = snapshot->native_jumps;
    rv64_jit_stats.native_m_ops = snapshot->native_m_ops;
    rv64_jit_stats.reg_cache_spills = snapshot->reg_cache_spills;
    rv64_jit_stats.native_store_continuations =
        snapshot->native_store_continuations;
    rv64_jit_stats.native_paged_loads = snapshot->native_paged_loads;
    rv64_jit_stats.native_paged_stores = snapshot->native_paged_stores;
    rv64_jit_stats.inline_paged_loads = snapshot->inline_paged_loads;
    rv64_jit_stats.inline_paged_stores = snapshot->inline_paged_stores;
    rv64_jit_stats.fp_helper_sites = snapshot->fp_helper_sites;
}
#else
/* Keep call sites uniform; the optimiser removes this one-byte no-op snapshot. */
typedef struct
{
    uint8_t unused;
} rv64_jit_emitted_site_stats_t;

static rv64_jit_emitted_site_stats_t jit_snapshot_emitted_site_stats(void)
{
    return (rv64_jit_emitted_site_stats_t){0};
}

static void jit_restore_emitted_site_stats(
    const rv64_jit_emitted_site_stats_t *snapshot)
{
    (void)snapshot;
}
#endif

typedef struct
{
    uint8_t *writer_cur;
    rv64_jit_reg_cache_t regs;
    rv64_jit_source_builder_t source;
    rv64_jit_ifetch_ref_builder_t ifetch_refs;
    rv64_jit_emitted_site_stats_t emitted_site_stats;
} rv64_jit_compile_snapshot_t;

typedef enum
{
    /* Budget, fallback, and a chained branch still need a fall-through exit. */
    RV64_JIT_COMPILE_NEEDS_FALLTHROUGH_EXIT,
    /* JAL and JALR emit every possible return/link path themselves. */
    RV64_JIT_COMPILE_HAS_TERMINAL_EXIT,
} rv64_jit_compile_exit_state_t;

/* Everything needed to publish one fully emitted native region. */
typedef struct
{
    vaddr_t start_pc;
    vaddr_t next_pc;
    paddr_t first_paddr;
    bool uses_translated_ifetch;
    bool needs_data_translation_guard;
    uint32_t compiled_insn_count;
    uint32_t stable_loop_reg_count;
    const uint8_t *native_body_entry;
    const rv64_jit_source_builder_t *source;
    const rv64_jit_ifetch_ref_builder_t *ifetch_refs;
} rv64_jit_publish_info_t;

/*
 * An instruction contributes to the block only after its native bytes and
 * source metadata are known to be usable.  If an emitter rejects a sub-case
 * after writing some bytes, roll back to the state from before this instruction,
 * including the ifetch refs collected for it, and record why this block stopped.
 * A negative cache entry is published later only when no instruction was kept.
 */
static bool jit_handle_emit_failure(rv64_jit_writer_t *w,
                                    rv64_jit_reg_cache_t *regs,
                                    rv64_jit_source_builder_t *source,
                                    rv64_jit_ifetch_ref_builder_t *ifetch_refs,
                                    const rv64_jit_compile_snapshot_t *snapshot,
                                    uint32_t instr,
                                    rv64_jit_block_end_reason_t *block_end_reason,
                                    bool *arena_overflowed)
{
    w->cur = snapshot->writer_cur;
    rv64_jit_reg_cache_restore(regs, &snapshot->regs);
    *source = snapshot->source;
    *ifetch_refs = snapshot->ifetch_refs;
    jit_restore_emitted_site_stats(&snapshot->emitted_site_stats);

    /*
     * A full writer is a compiler resource failure, not an unsupported RISC-V
     * instruction.  Abort this compilation without publishing a negative cache
     * entry; a later arena reset may provide enough space for the same opcode.
     */
    if (w->overflowed)
    {
        *arena_overflowed = true;
        w->overflowed = false;
        return false;
    }

    rv64_jit_stat_unsupported_opcode(instr);
    *block_end_reason = RV64_JIT_BLOCK_END_UNSUPPORTED_AFTER_PREFIX;
    return true;
}

/*
 * Publish generated code only after all bytes and dependency metadata exist.
 * The slot remains invalid while it is populated: synchronise the native
 * instruction cache, discard the previous owner, copy metadata, install
 * dependency references, and set `valid` last.  Finally advance arena usage so
 * a later compilation cannot overwrite the new block.
 */
static rv64_jit_block_t *jit_publish_compiled_block(
    rv64_jit_writer_t *w, const rv64_jit_publish_info_t *info)
{
    __builtin___clear_cache((char *)w->start, (char *)w->cur);

    rv64_jit_block_t *block = rv64_jit_cache_slot(info->start_pc);
    rv64_jit_block_discard(block);
    *block = (rv64_jit_block_t){
        .valid = false,
        .translated = info->uses_translated_ifetch,
        .uses_data_state = info->needs_data_translation_guard,
        .pc = info->start_pc,
        .satp = cpu.csr.satp,
        .ifetch_state = rv64_jit_ifetch_state(),
        .data_state = rv64_jit_data_tlb_state(MEM_TYPE_READ),
        .ifetch_generation = rv64_jit_ifetch_generation,
        .paddr_start = info->first_paddr,
        .source_len = info->source->source_len,
        .source_segment_count = info->source->segment_count,
        .ifetch_pt_page_count = info->uses_translated_ifetch
                                    ? info->ifetch_refs->count
                                    : 0,
        .insn_count = info->compiled_insn_count,
        .entry = (rv64_jit_entry_t)w->start,
        .body_entry = (rv64_jit_entry_t)info->native_body_entry,
    };
    memcpy(block->source_segments, info->source->segments,
           sizeof(info->source->segments));
    memcpy(block->ifetch_pt_pages, info->ifetch_refs->pages,
           block->ifetch_pt_page_count * sizeof(block->ifetch_pt_pages[0]));
    rv64_jit_ifetch_refs_ref(block);
    rv64_jit_source_chunks_ref(block);
    rv64_jit_source_reverse_map_add(block);
    block->valid = true;

    rv64_jit_code_used = (size_t)(w->cur - rv64_jit_code);
    return block;
}

/* Record compile-time counters after publication has definitely succeeded. */
static void jit_record_compilation_stats(const rv64_jit_publish_info_t *info,
                                         rv64_jit_block_end_reason_t end_reason)
{
    JIT_STAT_INC(blocks_compiled);
    rv64_jit_stat_block_end(end_reason);

    if (info->uses_translated_ifetch)
    {
        JIT_STAT_INC(translated_blocks);
        const vaddr_t last_pc = info->next_pc - RV64_INSN_SIZE;

        if (((info->start_pc ^ last_pc) & ~(vaddr_t)PAGE_MASK) != 0)
        {
            JIT_STAT_INC(translated_cross_page_blocks);
        }
    }

    if (info->source->segment_count > 1u)
    {
        JIT_STAT_INC(segmented_source_blocks);
    }

    if (info->compiled_insn_count > RV64_JIT_BLOCK_MAX_INSNS)
    {
        JIT_STAT_INC(trace_blocks);
        JIT_STAT_ADD(trace_insns, info->compiled_insn_count);
    }

    if (info->stable_loop_reg_count != 0)
    {
        JIT_STAT_INC(stable_loop_blocks);
        JIT_STAT_ADD(stable_loop_preloaded_regs,
                     info->stable_loop_reg_count);
    }

    JIT_STAT_ADD(compiled_insns, info->compiled_insn_count);
}

/*
 * Compile one native region starting at the current guest PC.
 *
 * The compile pipeline is intentionally linear:
 *   1. Allocate aligned arena space and translate the first fetch.
 *   2. Emit the function prologue and initialise the register cache.
 *   3. Walk guest instructions until budget, trace limit, unsupported opcode,
 *      source-boundary change or terminating control flow.
 *   4. For each instruction, record physical source bytes and ifetch page-table
 *      refs before emitting bytes that can observe that instruction.
 *   5. Emit either a normal block exit, a guarded direct link, a side exit or a
 *      chained-loop backedge.
 *   6. Publish the block only after code emission, source/dependency metadata,
 *      and reverse invalidation links are all complete.
 */
/*
 * Attempt to compile one bounded native region without retrying resource
 * failures.  `arena_overflowed` distinguishes writer exhaustion from normal
 * interpreter fallback so the public wrapper can perform one controlled arena
 * reset without ever creating an unsupported-instruction cache entry.
 */
static rv64_jit_block_t *jit_compile_block_once(vaddr_t pc,
                                                uint32_t max_insns,
                                                bool *arena_overflowed)
{
    *arena_overflowed = false;

    if (!rv64_jit_code_init() || max_insns == 0)
    {
        return NULL;
    }

    const rv64_jit_emitted_site_stats_t attempt_site_stats =
        jit_snapshot_emitted_site_stats();

    if (rv64_jit_code_used + RV64_JIT_BLOCK_CODE_HEADROOM > RV64_JIT_CODE_SIZE)
    {
        rv64_jit_arena_reset();
    }

    rv64_jit_code_used = rv64_jit_align_up(rv64_jit_code_used, RV64_JIT_CODE_ALIGN);

    paddr_t first_paddr = 0;
    bool uses_translated_ifetch = false;

    if (!rv64_jit_translate_ifetch_ex(pc, &first_paddr,
                                      &uses_translated_ifetch) ||
        !in_pmem(first_paddr))
    {
        return NULL;
    }

    rv64_jit_writer_t w = {
        .start = rv64_jit_code + rv64_jit_code_used,
        .cur = rv64_jit_code + rv64_jit_code_used,
        .end = rv64_jit_code + RV64_JIT_CODE_SIZE,
    };
    rv64_jit_reg_cache_t regs;
    rv64_jit_reg_cache_init(&regs);

    if (!rv64_jit_emit_prologue(&w))
    {
        *arena_overflowed = w.overflowed;
        jit_restore_emitted_site_stats(&attempt_site_stats);
        return NULL;
    }

    const rv64_jit_loop_scan_t loop_scan =
        jit_prescan_self_backedge(pc, max_insns,
                                  uses_translated_ifetch);
    const uint8_t *native_body_entry = w.cur;
    const uint8_t *loop_body_entry = native_body_entry;

    if (loop_scan.stable_reg_count != 0)
    {
        if (!rv64_jit_prepare_stable_loop_regs(
                &w, &regs, loop_scan.stable_reg_mask,
                loop_scan.loop_insn_count))
        {
            *arena_overflowed = w.overflowed;
            jit_restore_emitted_site_stats(&attempt_site_stats);
            return NULL;
        }

        /*
         * Incoming direct links use native_body_entry and therefore execute
         * these preloads. Only the proven self-backedge skips them.
         */
        loop_body_entry = w.cur;
    }

    vaddr_t guest_pc = pc;
    uint32_t compiled_insn_count = 0;
    rv64_jit_source_builder_t source = {0};
    rv64_jit_ifetch_ref_builder_t ifetch_refs = {0};
    bool needs_data_translation_guard = false;
    bool used_stable_loop = false;
    rv64_jit_compile_exit_state_t exit_state =
        RV64_JIT_COMPILE_NEEDS_FALLTHROUGH_EXIT;
    rv64_jit_block_end_reason_t block_end_reason = RV64_JIT_BLOCK_END_BUDGET;

    while (compiled_insn_count < max_insns &&
           compiled_insn_count < RV64_JIT_TRACE_MAX_INSNS)
    {
        /*
         * Re-translate every guest instruction, even inside one block. This keeps
         * the block metadata honest across page boundaries and avoids assuming that
         * adjacent virtual PCs are adjacent physical bytes.
         */
        paddr_t instruction_paddr = 0;
        bool instruction_uses_translated_ifetch = false;
        rv64_jit_ifetch_ref_builder_t ifetch_refs_start = ifetch_refs;

        if (!rv64_jit_translate_ifetch_collect(
                guest_pc, &instruction_paddr,
                &instruction_uses_translated_ifetch, &ifetch_refs) ||
            !in_pmem(instruction_paddr) ||
            instruction_uses_translated_ifetch != uses_translated_ifetch)
        {
            ifetch_refs = ifetch_refs_start;
            block_end_reason = RV64_JIT_BLOCK_END_SOURCE_BOUNDARY;
            break;
        }

        const uint32_t instr =
            (uint32_t)vaddr_ifetch(guest_pc, RV64_INSN_SIZE);
        const uint32_t opcode = instr & RV64_OPCODE_MASK;
        const rv64_jit_compile_snapshot_t instr_snapshot = {
            .writer_cur = w.cur,
            .regs = regs,
            .source = source,
            .ifetch_refs = ifetch_refs_start,
            .emitted_site_stats = jit_snapshot_emitted_site_stats(),
        };
        bool stop_after_instruction = false;

        if (!rv64_jit_source_builder_append(&source, instruction_paddr,
                                            RV64_INSN_SIZE))
        {
            ifetch_refs = ifetch_refs_start;
            block_end_reason = RV64_JIT_BLOCK_END_SOURCE_BOUNDARY;
            break;
        }

#ifdef CONFIG_RISCV_FPU
        if (jit_opcode_is_fp(opcode))
        {
            bool fp_ends_block = false;

            if (!rv64_jit_emit_fp_instr(&w, &regs, instr, guest_pc, compiled_insn_count, &fp_ends_block))
            {
                if (!jit_handle_emit_failure(&w, &regs, &source, &ifetch_refs, &instr_snapshot, instr, &block_end_reason, arena_overflowed))
                {
                    jit_restore_emitted_site_stats(&attempt_site_stats);
                    return NULL;
                }
                break;
            }

            if (fp_ends_block)
            {
                block_end_reason = RV64_JIT_BLOCK_END_FP_MEMORY;
                exit_state = RV64_JIT_COMPILE_HAS_TERMINAL_EXIT;
                stop_after_instruction = true;
            }
        }
        else
#endif
            if (opcode == RV64_OPCODE_JAL || opcode == RV64_OPCODE_JALR)
        {
            if (!rv64_jit_emit_jump_instr(
                    &w, &regs, instr, guest_pc, compiled_insn_count,
                    needs_data_translation_guard))
            {
                if (!jit_handle_emit_failure(
                        &w, &regs, &source, &ifetch_refs, &instr_snapshot,
                        instr, &block_end_reason, arena_overflowed))
                {
                    jit_restore_emitted_site_stats(&attempt_site_stats);
                    return NULL;
                }
                break;
            }

            block_end_reason = RV64_JIT_BLOCK_END_JUMP;
            exit_state = RV64_JIT_COMPILE_HAS_TERMINAL_EXIT;
            stop_after_instruction = true;
        }
        else if (opcode == RV64_OPCODE_LOAD)
        {
            /*
             * A guarded load may side-exit with zero completed instructions
             * when it is the first block instruction and the runtime address is
             * unsafe.  The dispatcher treats that as a miss-like fallback and
             * lets the interpreter execute the load.
             */
            if (!rv64_jit_emit_load_instr(&w, &regs, instr, guest_pc,
                                          compiled_insn_count))
            {
                if (!jit_handle_emit_failure(
                        &w, &regs, &source, &ifetch_refs, &instr_snapshot,
                        instr, &block_end_reason, arena_overflowed))
                {
                    jit_restore_emitted_site_stats(&attempt_site_stats);
                    return NULL;
                }
                break;
            }

            if ((cpu.csr.satp >> RV64_JIT_SATP_MODE_SHIFT) !=
                RV64_JIT_SATP_MODE_BARE)
            {
                /*
                 * MPRV, SUM, and MXR affect explicit data accesses, whereas
                 * instruction fetch uses its own privilege state.  Direct links
                 * must therefore guard this data-translation state separately.
                 */
                needs_data_translation_guard = true;
            }
        }
        else if (opcode == RV64_OPCODE_STORE)
        {
            /*
             * Safe PMEM data stores can continue in the native block.  Stores
             * that may fault, hit MMIO, or touch source bytes side-exit before
             * or immediately after the store so interpreter-visible ordering is
             * preserved.
             */
            if (!rv64_jit_emit_store_instr(
                    &w, &regs, instr, guest_pc,
                    guest_pc + RV64_INSN_SIZE, compiled_insn_count))
            {
                if (!jit_handle_emit_failure(
                        &w, &regs, &source, &ifetch_refs, &instr_snapshot,
                        instr, &block_end_reason, arena_overflowed))
                {
                    jit_restore_emitted_site_stats(&attempt_site_stats);
                    return NULL;
                }
                break;
            }

            if ((cpu.csr.satp >> RV64_JIT_SATP_MODE_SHIFT) !=
                RV64_JIT_SATP_MODE_BARE)
            {
                needs_data_translation_guard = true;
            }
        }
        else if (opcode == RV64_OPCODE_BRANCH)
        {
            bool emitted_native_backedge = false;

            if (!rv64_jit_emit_branch(
                    &w, &regs, instr, guest_pc, pc, loop_body_entry,
                    loop_scan.found_self_backedge,
                    loop_scan.stable_reg_count != 0,
                    &emitted_native_backedge,
                    compiled_insn_count + 1u,
                    needs_data_translation_guard))
            {
                if (!jit_handle_emit_failure(
                        &w, &regs, &source, &ifetch_refs, &instr_snapshot,
                        instr, &block_end_reason, arena_overflowed))
                {
                    jit_restore_emitted_site_stats(&attempt_site_stats);
                    return NULL;
                }
                break;
            }

            if (emitted_native_backedge)
            {
                block_end_reason = RV64_JIT_BLOCK_END_CHAINED_LOOP;
                used_stable_loop = loop_scan.stable_reg_count != 0;
                stop_after_instruction = true;
            }
        }
        else if (!rv64_jit_emit_instr(&w, &regs, instr, guest_pc,
                                      compiled_insn_count + 1u))
        {
            if (!jit_handle_emit_failure(
                    &w, &regs, &source, &ifetch_refs, &instr_snapshot,
                    instr, &block_end_reason, arena_overflowed))
            {
                jit_restore_emitted_site_stats(&attempt_site_stats);
                return NULL;
            }
            break;
        }

        guest_pc += RV64_INSN_SIZE;
        compiled_insn_count++;

        /*
         * A chained back-edge is both a taken-loop fast path and the natural end
         * of this native block.  Its fall-through path returns below with
         * `rv64_jit_loop_extra + compiled_insn_count`, while taken laps jump
         * back to `loop_body_entry` without re-running the prologue or stable
         * register and counter preloads.
         */
        if (stop_after_instruction)
        {
            break;
        }
    }

    if (compiled_insn_count == 0)
    {
        if (block_end_reason == RV64_JIT_BLOCK_END_UNSUPPORTED_AFTER_PREFIX)
        {
            rv64_jit_mark_unsupported(pc, first_paddr,
                                      uses_translated_ifetch);
        }

        jit_restore_emitted_site_stats(&attempt_site_stats);
        return NULL;
    }

    /*
     * JAL/JALR emit terminal paths as part of the instruction.  All other stop
     * conditions still need one fall-through exit.  Keeping this state explicit
     * avoids unreachable duplicate exits and prevents dead-byte emission from
     * rejecting an otherwise valid jump block.
     */
    if (exit_state == RV64_JIT_COMPILE_NEEDS_FALLTHROUGH_EXIT &&
        !(rv64_jit_direct_link_enabled()
              ? rv64_jit_emit_direct_link_exit(
                    &w, &regs, guest_pc, compiled_insn_count,
                    needs_data_translation_guard, NULL)
              : rv64_jit_emit_plain_block_exit(
                    &w, &regs, guest_pc, compiled_insn_count)))
    {
        *arena_overflowed = w.overflowed;
        jit_restore_emitted_site_stats(&attempt_site_stats);
        return NULL;
    }

    const rv64_jit_publish_info_t publish_info = {
        .start_pc = pc,
        .next_pc = guest_pc,
        .first_paddr = first_paddr,
        .uses_translated_ifetch = uses_translated_ifetch,
        .needs_data_translation_guard = needs_data_translation_guard,
        .compiled_insn_count = compiled_insn_count,
        .stable_loop_reg_count =
            used_stable_loop ? loop_scan.stable_reg_count : 0,
        .native_body_entry = native_body_entry,
        .source = &source,
        .ifetch_refs = &ifetch_refs,
    };

    rv64_jit_block_t *block =
        jit_publish_compiled_block(&w, &publish_info);
    jit_record_compilation_stats(&publish_info, block_end_reason);
    return block;
}

/*
 * Compile a block, recovering once if the current arena tail was too small.
 *
 * The conservative headroom check normally prevents writer exhaustion.  If an
 * unusually large expansion still reaches the arena end, discard the old arena
 * and retry from offset zero exactly once.  The bound is important: an emitter
 * bug or a future block larger than the complete arena must fall back to the
 * interpreter instead of entering an unbounded reset loop.
 */
rv64_jit_block_t *rv64_jit_compile_block(vaddr_t pc, uint32_t max_insns)
{
    bool arena_overflowed = false;
    rv64_jit_block_t *block =
        jit_compile_block_once(pc, max_insns, &arena_overflowed);

    if (block != NULL || !arena_overflowed)
    {
        return block;
    }

    rv64_jit_arena_reset();
    arena_overflowed = false;
    return jit_compile_block_once(pc, max_insns, &arena_overflowed);
}

#endif /* CONFIG_RV64 */
