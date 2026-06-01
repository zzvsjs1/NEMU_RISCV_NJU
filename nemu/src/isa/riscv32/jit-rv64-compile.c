#include <generated/autoconf.h>

#ifdef CONFIG_RV64

#include "jit-rv64-internal.h"

/*
 * RV64 JIT compile layer: RISC-V instruction scanning, block construction,
 * unsupported-instruction fallback and publication of compiled blocks.
 */
/* Return true for opcodes that can appear inside a native chained loop body. */
static bool jit_instr_can_chain_body(uint32_t instr)
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

/* Cheaply pre-scan whether this block has a branch back to its own start. */
static bool jit_block_has_chainable_backedge(vaddr_t pc, uint32_t max_insns,
                                             bool first_translated)
{
    vaddr_t cur_pc = pc;
    uint32_t count = 0;

    while (count < max_insns && count < RV64_JIT_TRACE_MAX_INSNS)
    {
        paddr_t cur_paddr = 0;
        bool cur_translated = false;

        if (!rv64_jit_translate_ifetch_ex(cur_pc, &cur_paddr, &cur_translated) ||
            !in_pmem(cur_paddr) ||
            cur_translated != first_translated)
        {
            return false;
        }

        const uint32_t instr = (uint32_t)vaddr_ifetch(cur_pc, RV64_INSN_SIZE);
        const uint32_t opcode = instr & RV64_OPCODE_MASK;

        if (!jit_instr_can_chain_body(instr))
        {
            return false;
        }

        if (opcode == RV64_OPCODE_BRANCH && cur_pc + imm_b(instr) == pc)
        {
            return true;
        }

        cur_pc += RV64_INSN_SIZE;
        count++;
    }

    return false;
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
 *   6. Publish the block metadata only after code emission, source copying and
 *      reverse invalidation links are all complete.
 */
/* Compile one straight-line block starting at the current guest PC. */
rv64_jit_block_t *rv64_jit_compile_block(vaddr_t pc, uint32_t max_insns)
{
    if (!rv64_jit_code_init() || max_insns == 0)
    {
        return NULL;
    }

    if (rv64_jit_code_used + RV64_JIT_BLOCK_CODE_HEADROOM > RV64_JIT_CODE_SIZE)
    {
        rv64_jit_arena_reset();
    }

    rv64_jit_code_used = rv64_jit_align_up(rv64_jit_code_used, RV64_JIT_CODE_ALIGN);

    paddr_t first_paddr = 0;
    bool first_translated = false;
    if (!rv64_jit_translate_ifetch_ex(pc, &first_paddr, &first_translated) ||
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
        return NULL;
    }

    const bool chain_safe_start =
        jit_block_has_chainable_backedge(pc, max_insns, first_translated);
    const bool loop_count_needed = true;
    const uint8_t *block_start_native = w.cur;
    vaddr_t cur_pc = pc;
    uint32_t count = 0;
    rv64_jit_source_builder_t source = {0};
    rv64_jit_ifetch_ref_builder_t ifetch_refs = {0};
    bool chain_safe = chain_safe_start;
    bool uses_data_state = false;
    rv64_jit_block_end_reason_t block_end_reason = RV64_JIT_BLOCK_END_BUDGET;

    while (count < max_insns && count < RV64_JIT_TRACE_MAX_INSNS)
    {
        /*
         * Re-translate every guest instruction, even inside one block. This keeps
         * the block metadata honest across page boundaries and avoids assuming that
         * adjacent virtual PCs are adjacent physical bytes.
         */
        paddr_t cur_paddr = 0;
        bool cur_translated = false;
        rv64_jit_ifetch_ref_builder_t ifetch_refs_start = ifetch_refs;

        if (!rv64_jit_translate_ifetch_collect(cur_pc, &cur_paddr, &cur_translated,
                                          &ifetch_refs) ||
            !in_pmem(cur_paddr) ||
            cur_translated != first_translated)
        {
            ifetch_refs = ifetch_refs_start;
            block_end_reason = RV64_JIT_BLOCK_END_SOURCE_BOUNDARY;
            break;
        }

        const uint32_t instr = (uint32_t)vaddr_ifetch(cur_pc, RV64_INSN_SIZE);
        const uint32_t opcode = instr & RV64_OPCODE_MASK;
        uint8_t *instr_start = w.cur;
        rv64_jit_reg_cache_t regs_start = regs;
        rv64_jit_source_builder_t source_start = source;
        bool end_block = false;

        if (!rv64_jit_source_builder_append(&source, cur_paddr, RV64_INSN_SIZE))
        {
            ifetch_refs = ifetch_refs_start;
            block_end_reason = RV64_JIT_BLOCK_END_SOURCE_BOUNDARY;
            break;
        }

        if (opcode == RV64_OPCODE_JAL ||
            opcode == RV64_OPCODE_JALR)
        {
            if (!rv64_jit_emit_jump_instr(&w, &regs, instr, cur_pc, count,
                                 loop_count_needed, uses_data_state))
            {
                w.cur = instr_start;
                rv64_jit_reg_cache_restore(&regs, &regs_start);
                source = source_start;
                ifetch_refs = ifetch_refs_start;
                rv64_jit_stat_unsupported_opcode(instr);
                block_end_reason = RV64_JIT_BLOCK_END_UNSUPPORTED_AFTER_PREFIX;
                break;
            }
            block_end_reason = RV64_JIT_BLOCK_END_JUMP;
            end_block = true;
        }
        else if (opcode == RV64_OPCODE_LOAD)
        {
            /*
             * A guarded load may side-exit with zero completed instructions
             * when it is the first block instruction and the runtime address is
             * unsafe.  The dispatcher treats that as a miss-like fallback and
             * lets the interpreter execute the load.
             */
            if (!rv64_jit_emit_load_instr(&w, &regs, instr, cur_pc, count, loop_count_needed))
            {
                w.cur = instr_start;
                rv64_jit_reg_cache_restore(&regs, &regs_start);
                source = source_start;
                ifetch_refs = ifetch_refs_start;
                rv64_jit_stat_unsupported_opcode(instr);
                block_end_reason = RV64_JIT_BLOCK_END_UNSUPPORTED_AFTER_PREFIX;
                break;
            }
            if ((cpu.csr.satp >> RV64_JIT_SATP_MODE_SHIFT) != 0)
            {
                uses_data_state = true;
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
            if (!rv64_jit_emit_store_instr(&w, &regs, instr, cur_pc, cur_pc + RV64_INSN_SIZE,
                                  count, loop_count_needed))
            {
                w.cur = instr_start;
                rv64_jit_reg_cache_restore(&regs, &regs_start);
                source = source_start;
                ifetch_refs = ifetch_refs_start;
                rv64_jit_stat_unsupported_opcode(instr);
                block_end_reason = RV64_JIT_BLOCK_END_UNSUPPORTED_AFTER_PREFIX;
                break;
            }
            if ((cpu.csr.satp >> RV64_JIT_SATP_MODE_SHIFT) != 0)
            {
                uses_data_state = true;
            }
        }
        else if (opcode == RV64_OPCODE_BRANCH)
        {
            bool branch_chained = false;

            if (!rv64_jit_emit_branch(&w, &regs, instr, cur_pc, pc, block_start_native,
                             chain_safe, &branch_chained, count + 1u,
                             uses_data_state))
            {
                w.cur = instr_start;
                rv64_jit_reg_cache_restore(&regs, &regs_start);
                source = source_start;
                ifetch_refs = ifetch_refs_start;
                rv64_jit_stat_unsupported_opcode(instr);
                block_end_reason = RV64_JIT_BLOCK_END_UNSUPPORTED_AFTER_PREFIX;
                break;
            }

            if (branch_chained)
            {
                block_end_reason = RV64_JIT_BLOCK_END_CHAINED_LOOP;
                end_block = true;
            }
        }
        else if (!rv64_jit_emit_instr(&w, &regs, instr, cur_pc, count + 1u))
        {
            w.cur = instr_start;
            rv64_jit_reg_cache_restore(&regs, &regs_start);
            source = source_start;
            ifetch_refs = ifetch_refs_start;
            rv64_jit_stat_unsupported_opcode(instr);
            block_end_reason = RV64_JIT_BLOCK_END_UNSUPPORTED_AFTER_PREFIX;
            break;
        }

        cur_pc += RV64_INSN_SIZE;
        count++;

        /*
         * A chained back-edge is both a taken-loop fast path and the natural end
         * of this native block.  Its fall-through path returns below with
         * `rv64_jit_loop_extra + count`, while taken laps jump back to
         * `block_start_native` without re-running the prologue.
         */
        if (end_block)
        {
            break;
        }
    }

    if (count == 0)
    {
        rv64_jit_mark_unsupported(pc, first_paddr, first_translated);
        return NULL;
    }

    if (!(rv64_jit_direct_link_enabled()
              ? rv64_jit_emit_direct_link_exit(&w, &regs, cur_pc, count, uses_data_state, NULL)
              : rv64_jit_emit_plain_block_exit(&w, &regs, cur_pc, count)))
    {
        return NULL;
    }

    __builtin___clear_cache((char *)w.start, (char *)w.cur);

    rv64_jit_block_t *block = rv64_jit_cache_slot(pc);
    rv64_jit_block_discard(block);
    *block = (rv64_jit_block_t){
        .valid = true,
        .translated = first_translated,
        .uses_data_state = uses_data_state,
        .pc = pc,
        .satp = cpu.csr.satp,
        .ifetch_state = rv64_jit_ifetch_state(),
        .data_state = rv64_jit_data_tlb_state(MEM_TYPE_READ),
        .ifetch_generation = rv64_jit_ifetch_generation,
        .paddr_start = first_paddr,
        .source_len = source.source_len,
        .source_segment_count = source.segment_count,
        .ifetch_pt_page_count = first_translated ? ifetch_refs.count : 0,
        .insn_count = count,
        .entry = (rv64_jit_entry_t)w.start,
        .body_entry = (rv64_jit_entry_t)block_start_native,
    };
    memcpy(block->source_segments, source.segments, sizeof(source.segments));
    memcpy(block->ifetch_pt_pages, ifetch_refs.pages,
           block->ifetch_pt_page_count * sizeof(block->ifetch_pt_pages[0]));
    rv64_jit_ifetch_refs_ref(block);
    rv64_jit_source_chunks_ref(block);
    rv64_jit_source_reverse_map_add(block);

    rv64_jit_code_used = (size_t)(w.cur - rv64_jit_code);
    JIT_STAT_INC(blocks_compiled);
    rv64_jit_stat_block_end(block_end_reason);
    if (first_translated)
    {
        JIT_STAT_INC(translated_blocks);
        if (((pc ^ (cur_pc - RV64_INSN_SIZE)) & ~(vaddr_t)PAGE_MASK) != 0)
        {
            JIT_STAT_INC(translated_cross_page_blocks);
        }
    }
    if (source.segment_count > 1u)
    {
        JIT_STAT_INC(segmented_source_blocks);
    }
    if (count > RV64_JIT_BLOCK_MAX_INSNS)
    {
        JIT_STAT_INC(trace_blocks);
        JIT_STAT_ADD(trace_insns, count);
    }
    JIT_STAT_ADD(compiled_insns, count);
    return block;
}

#endif /* CONFIG_RV64 */
