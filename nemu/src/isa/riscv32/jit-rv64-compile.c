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
    case RISCV_FP_OPCODE_LOAD:
    case RISCV_FP_OPCODE_STORE:
    case RISCV_FP_OPCODE_FMADD:
    case RISCV_FP_OPCODE_FMSUB:
    case RISCV_FP_OPCODE_FNMSUB:
    case RISCV_FP_OPCODE_FNMADD:
    case RISCV_FP_OPCODE_OP:
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
    const uint32_t opcode = instr & RISCV_OPCODE_MASK;

    switch (opcode)
    {
    case RISCV_OPCODE_LOAD:
    case RISCV_OPCODE_STORE:
    case RISCV_OPCODE_OP_IMM:
    case RISCV_OPCODE_OP_IMM_32:
    case RISCV_OPCODE_OP:
    case RISCV_OPCODE_OP_32:
    case RISCV_OPCODE_AUIPC:
    case RISCV_OPCODE_LUI:
    case RISCV_OPCODE_BRANCH:
        return true;
#ifdef CONFIG_RISCV_FPU
    case RISCV_FP_OPCODE_LOAD:
    case RISCV_FP_OPCODE_STORE:
        return rv64_jit_decode_fp_memory(instr) != RV64_JIT_FP_MEMORY_INVALID;
    case RISCV_FP_OPCODE_OP:
        return rv64_jit_decode_fp_exact(instr) != RV64_JIT_FP_EXACT_INVALID;
#endif
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
    if (reg != RISCV_GPR_ZERO)
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
    const uint32_t opcode = instr & RISCV_OPCODE_MASK;

    switch (opcode)
    {
    case RISCV_OPCODE_OP_IMM:
    case RISCV_OPCODE_OP_IMM_32:
        jit_stable_loop_add_reg(reg_mask, rv64_instr_rd(instr));
        jit_stable_loop_add_reg(reg_mask, rv64_instr_rs1(instr));
        return true;
    case RISCV_OPCODE_OP:
    case RISCV_OPCODE_OP_32:
        /*
         * Every full-width M funct3 is defined. OP-32 reserves funct3 1-3, so
         * reject those encodings before expanding the loop register cache.
         * All valid M members are now helper-free and leave R8 available for
         * the seventh loop-carried guest register.
         */
        if (rv64_instr_funct7(instr) == RV64_FUNCT7_MULDIV && opcode == RISCV_OPCODE_OP_32)
        {
            switch (rv64_instr_funct3(instr))
            {
            case RV64_M_FUNCT3_MUL:  /* MULW */
            case RV64_M_FUNCT3_DIV:  /* DIVW */
            case RV64_M_FUNCT3_DIVU: /* DIVUW */
            case RV64_M_FUNCT3_REM:  /* REMW */
            case RV64_M_FUNCT3_REMU: /* REMUW */
                break;
            default:
                return false;
            }
        }

        jit_stable_loop_add_reg(reg_mask, rv64_instr_rd(instr));
        jit_stable_loop_add_reg(reg_mask, rv64_instr_rs1(instr));
        jit_stable_loop_add_reg(reg_mask, rv64_instr_rs2(instr));
        return true;
    case RISCV_OPCODE_LUI:
    case RISCV_OPCODE_AUIPC:
        jit_stable_loop_add_reg(reg_mask, rv64_instr_rd(instr));
        return true;
    case RISCV_OPCODE_BRANCH:
        jit_stable_loop_add_reg(reg_mask, rv64_instr_rs1(instr));
        jit_stable_loop_add_reg(reg_mask, rv64_instr_rs2(instr));
        return true;
#ifdef CONFIG_RISCV_FPU
    case RISCV_FP_OPCODE_LOAD:
    case RISCV_FP_OPCODE_STORE:
        /*
         * FP memory can form a native self-backedge, but its guards reserve R8
         * as scratch. Reject the seven-slot stable mapping while allowing the
         * ordinary six-slot loop cache selected by the completed pre-scan.
         */
        return false;
    case RISCV_FP_OPCODE_OP:
        switch (rv64_jit_decode_fp_exact(instr))
        {
        case RV64_JIT_FP_EXACT_FMV_W_X:
        case RV64_JIT_FP_EXACT_FMV_D_X:
            jit_stable_loop_add_reg(reg_mask, rv64_instr_rs1(instr));
            return true;
        case RV64_JIT_FP_EXACT_FMV_X_W:
        case RV64_JIT_FP_EXACT_FMV_X_D:
        case RV64_JIT_FP_EXACT_FCLASS_S:
        case RV64_JIT_FP_EXACT_FCLASS_D:
            jit_stable_loop_add_reg(reg_mask, rv64_instr_rd(instr));
            return true;
        case RV64_JIT_FP_EXACT_FSGNJ_S:
        case RV64_JIT_FP_EXACT_FSGNJN_S:
        case RV64_JIT_FP_EXACT_FSGNJX_S:
        case RV64_JIT_FP_EXACT_FSGNJ_D:
        case RV64_JIT_FP_EXACT_FSGNJN_D:
        case RV64_JIT_FP_EXACT_FSGNJX_D:
            return true;
        default:
            return false;
        }
#endif
    default:
        return false;
    }
}

/*
 * Cheaply pre-scan for a conditional branch back to the block head.  Ordinary
 * branches are followed along their fall-through path; only a self-backedge is
 * a candidate for native loop chaining.
 */
static rv64_jit_loop_scan_t jit_prescan_self_backedge(vaddr_t pc, uint32_t max_insns, bool uses_translated_ifetch)
{
    rv64_jit_loop_scan_t result = {0};
    vaddr_t candidate_pc = pc;
    uint32_t scanned_insn_count = 0;
    uint32_t stable_reg_mask = 0;
    bool stable_mapping_eligible = true;

    while (scanned_insn_count < max_insns && scanned_insn_count < RV64_JIT_TRACE_MAX_INSNS)
    {
        paddr_t candidate_paddr = 0;
        bool candidate_uses_translated_ifetch = false;

        if (!rv64_jit_translate_ifetch_ex(candidate_pc, &candidate_paddr, &candidate_uses_translated_ifetch) || !in_pmem(candidate_paddr) ||
            candidate_uses_translated_ifetch != uses_translated_ifetch)
        {
            return result;
        }

        const uint32_t instr = (uint32_t)vaddr_ifetch(candidate_pc, RISCV_BASE_INSN_BYTES);
        const uint32_t opcode = instr & RISCV_OPCODE_MASK;
        const bool is_self_backedge = opcode == RISCV_OPCODE_BRANCH && candidate_pc + imm_b(instr) == pc;

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
        if (stable_mapping_eligible && opcode == RISCV_OPCODE_BRANCH && !is_self_backedge)
        {
            stable_mapping_eligible = false;
        }

        if (stable_mapping_eligible && !jit_collect_stable_loop_regs(instr, &stable_reg_mask))
        {
            stable_mapping_eligible = false;
        }

        if (is_self_backedge)
        {
            result.found_self_backedge = true;
            result.loop_insn_count = scanned_insn_count + 1u;

            if (stable_mapping_eligible)
            {
                const uint32_t reg_count = (uint32_t)__builtin_popcount(stable_reg_mask);

                if (reg_count > 0 && reg_count <= RV64_JIT_HREG_COUNT)
                {
                    result.stable_reg_mask = stable_reg_mask;
                    result.stable_reg_count = reg_count;
                }
            }

            return result;
        }

        candidate_pc += RISCV_BASE_INSN_BYTES;
        scanned_insn_count++;
    }

    return result;
}

typedef struct
{
    uint32_t uses;
    uint32_t defs;
    uint32_t live_after;
} rv64_jit_gpr_liveness_t;

typedef struct
{
    rv64_jit_gpr_liveness_t insns[RV64_JIT_TRACE_MAX_INSNS];
    uint32_t count;
} rv64_jit_gpr_liveness_scan_t;

/* Return the bit for one cacheable architectural GPR, excluding constant x0. */
static uint32_t jit_gpr_mask_bit(uint32_t reg)
{
    return reg == RISCV_GPR_ZERO ? 0 : 1u << reg;
}

/*
 * Decode the integer-register effects needed only for victim selection.
 *
 * Unknown major opcodes end the window. Missing a sub-encoding can only make
 * the heuristic more conservative because dirty victims are never discarded;
 * the real emitter remains the authority for instruction support and traps.
 */
static bool jit_decode_gpr_use_def(uint32_t instr, uint32_t *uses, uint32_t *defs, bool *terminal)
{
    const uint32_t opcode = instr & RISCV_OPCODE_MASK;
    const uint32_t rd_bit = jit_gpr_mask_bit(rv64_instr_rd(instr));
    const uint32_t rs1_bit = jit_gpr_mask_bit(rv64_instr_rs1(instr));
    const uint32_t rs2_bit = jit_gpr_mask_bit(rv64_instr_rs2(instr));

    *uses = 0;
    *defs = 0;
    *terminal = false;

    switch (opcode)
    {
    case RISCV_OPCODE_OP_IMM:
    case RISCV_OPCODE_OP_IMM_32:
        *uses = rs1_bit;
        *defs = rd_bit;
        return true;
    case RISCV_OPCODE_OP:
    case RISCV_OPCODE_OP_32:
        *uses = rs1_bit | rs2_bit;
        *defs = rd_bit;
        return true;
    case RISCV_OPCODE_LOAD:
        *uses = rs1_bit;
        *defs = rd_bit;
        return true;
    case RISCV_OPCODE_STORE:
        *uses = rs1_bit | rs2_bit;
        return true;
    case RISCV_OPCODE_BRANCH:
        *uses = rs1_bit | rs2_bit;
        return true;
    case RISCV_OPCODE_JAL:
        *defs = rd_bit;
        *terminal = true;
        return true;
    case RISCV_OPCODE_JALR:
        *uses = rs1_bit;
        *defs = rd_bit;
        *terminal = true;
        return true;
    case RISCV_OPCODE_LUI:
    case RISCV_OPCODE_AUIPC:
        *defs = rd_bit;
        return true;
#ifdef CONFIG_RISCV_FPU
    case RISCV_FP_OPCODE_LOAD:
    case RISCV_FP_OPCODE_STORE:
        /* FP stores encode an FPR, not a GPR, in the rs2 field. */
        *uses = rs1_bit;
        return true;
    case RISCV_FP_OPCODE_FMADD:
    case RISCV_FP_OPCODE_FMSUB:
    case RISCV_FP_OPCODE_FNMSUB:
    case RISCV_FP_OPCODE_FNMADD:
        return true;
    case RISCV_FP_OPCODE_OP:
        switch (rv64_jit_decode_fp_exact(instr))
        {
        case RV64_JIT_FP_EXACT_FMV_W_X:
        case RV64_JIT_FP_EXACT_FMV_D_X:
            *uses = rs1_bit;
            break;
        case RV64_JIT_FP_EXACT_FMV_X_W:
        case RV64_JIT_FP_EXACT_FMV_X_D:
        case RV64_JIT_FP_EXACT_FCLASS_S:
        case RV64_JIT_FP_EXACT_FCLASS_D:
            *defs = rd_bit;
            break;
        default:
            /*
             * Other FP operations use FPRs or run through a helper which reads
             * flushed CPU state, so no emitted GPR slot pointer is retained.
             */
            break;
        }
        return true;
#endif
    default:
        return false;
    }
}

/*
 * Build a bounded fall-through liveness window with the same safe fetch proof
 * as compilation. This is a compile-time hint only: source metadata and the
 * real emitter still decide the published block prefix.
 */
static rv64_jit_gpr_liveness_scan_t jit_prescan_gpr_liveness(vaddr_t pc, uint32_t max_insns, bool uses_translated_ifetch)
{
    rv64_jit_gpr_liveness_scan_t scan = {0};
    vaddr_t candidate_pc = pc;

    while (scan.count < max_insns && scan.count < RV64_JIT_TRACE_MAX_INSNS)
    {
        paddr_t candidate_paddr = 0;
        bool candidate_uses_translated_ifetch = false;

        if (!rv64_jit_translate_ifetch_ex(candidate_pc, &candidate_paddr, &candidate_uses_translated_ifetch) || !in_pmem(candidate_paddr) ||
            candidate_uses_translated_ifetch != uses_translated_ifetch)
        {
            break;
        }

        const uint32_t instr = (uint32_t)vaddr_ifetch(candidate_pc, RISCV_BASE_INSN_BYTES);
        rv64_jit_gpr_liveness_t *item = &scan.insns[scan.count];
        bool terminal = false;

        if (!jit_decode_gpr_use_def(instr, &item->uses, &item->defs, &terminal))
        {
            break;
        }

        scan.count++;

        if (terminal)
        {
            break;
        }

        candidate_pc += RISCV_BASE_INSN_BYTES;
    }

    uint32_t live = 0;

    for (uint32_t i = scan.count; i-- > 0;)
    {
        rv64_jit_gpr_liveness_t *item = &scan.insns[i];
        item->live_after = live;
        live = (live & ~item->defs) | item->uses;
    }

    return scan;
}

typedef struct
{
    rv64_jit_emitter_checkpoint_t emitter;
    rv64_jit_source_builder_t source;
    rv64_jit_ifetch_ref_builder_t ifetch_refs;
} rv64_jit_compile_snapshot_t;

typedef enum
{
    /* Budget, fallback, and a chained branch still need a fall-through exit. */
    RV64_JIT_COMPILE_NEEDS_FALLTHROUGH_EXIT,
    /* JAL and JALR emit every possible return/link path themselves. */
    RV64_JIT_COMPILE_HAS_TERMINAL_EXIT,
} rv64_jit_compile_exit_state_t;

/* Describe whether the block pre-scan proved a native self-backedge. */
typedef enum
{
    JIT_BACKEDGE_NONE,
    JIT_BACKEDGE_ORDINARY,
    JIT_BACKEDGE_STABLE,
} jit_backedge_mode_t;

/* Immutable block-level inputs needed only while selecting one emitter. */
typedef struct
{
    vaddr_t start_pc;
    const uint8_t *loop_body_entry;
    jit_backedge_mode_t backedge_mode;
} jit_block_emit_plan_t;

/* Values which change for each guest instruction sent to the dispatcher. */
typedef struct
{
    uint32_t instr;
    vaddr_t pc;
    uint32_t completed_before;
    bool source_uses_data_translation_state;
} jit_instruction_emit_request_t;

/* Successful stop kinds retain their distinct block publication semantics. */
typedef enum
{
    JIT_INSTRUCTION_EMIT_FAILED = 0,
    JIT_INSTRUCTION_EMIT_CONTINUE,
    JIT_INSTRUCTION_EMIT_STOP_FP_MEMORY,
    JIT_INSTRUCTION_EMIT_STOP_JUMP,
    JIT_INSTRUCTION_EMIT_STOP_CHAINED_LOOP,
} jit_instruction_emit_kind_t;

/* Instruction-local effects which later direct links must guard. */
typedef enum
{
    JIT_INSTRUCTION_EFFECT_NONE = 0,
    JIT_INSTRUCTION_EFFECT_DATA_TRANSLATION = 1u << 0,
} jit_instruction_emit_effect_t;

typedef struct
{
    jit_instruction_emit_kind_t kind;
    uint32_t effects;
} jit_instruction_emit_outcome_t;

/*
 * An instruction contributes to the block only after its native bytes and
 * source metadata are known to be usable.  If an emitter rejects a sub-case
 * after writing some bytes, roll back to the state from before this instruction,
 * including the ifetch refs collected for it, and record why this block stopped.
 * A negative cache entry is published later only when no instruction was kept.
 */
static bool jit_handle_emit_failure(rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs, rv64_jit_source_builder_t *source,
                                    rv64_jit_ifetch_ref_builder_t *ifetch_refs, rv64_jit_mmio_route_builder_t *mmio_routes,
                                    const rv64_jit_compile_snapshot_t *snapshot, uint32_t instr, rv64_jit_block_end_reason_t *block_end_reason,
                                    bool *arena_overflowed)
{
    rv64_jit_emitter_checkpoint_restore(&snapshot->emitter, w, regs, mmio_routes);
    *source = snapshot->source;
    *ifetch_refs = snapshot->ifetch_refs;

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
 * Materialise mutable direct-link records after all executable bytes and other
 * sidecars. Build records point into the just-emitted native region; their
 * persistent counterparts additionally acquire cache-slot list ownership when
 * the block is published.
 */
static bool jit_finalise_links(rv64_jit_writer_t *w, rv64_jit_link_builder_t *links, rv64_jit_link_t **records)
{
    Assert(links != NULL, "jit: missing RV64 direct-link builder");
    Assert(records != NULL, "jit: missing RV64 direct-link sidecar result");
    Assert(links->state.count <= RV64_JIT_BLOCK_MAX_LINKS, "jit: too many RV64 direct-link records");
    *records = NULL;

    if (links->state.count == 0)
    {
        return true;
    }

    const uintptr_t aligned = rv64_jit_align_up((uintptr_t)w->cur, _Alignof(rv64_jit_link_t));
    const size_t padding = (size_t)(aligned - (uintptr_t)w->cur);
    const size_t record_bytes = (size_t)links->state.count * sizeof(rv64_jit_link_t);
    const size_t available = (size_t)(w->end - w->cur);

    if (padding > available || record_bytes > available - padding)
    {
        w->overflowed = true;
        return false;
    }

    memset(w->cur, 0, padding);
    w->cur += padding;
    rv64_jit_link_t *persistent = (rv64_jit_link_t *)w->cur;
    memset(persistent, 0, record_bytes);

    for (uint32_t i = 0; i < links->state.count; i++)
    {
        const rv64_jit_link_build_record_t *build = &links->records[i];
        persistent[i] = (rv64_jit_link_t){
            .selector_disp = build->selector_disp,
            .target_disp = build->target_disp,
            .guarded_path = build->guarded_path,
            .patched_path = build->patched_path,
            .target_pc = build->target_pc,
            .target_satp = build->target_satp,
            .target_ifetch_state = build->target_ifetch_state,
            .target_slot_index = UINT32_MAX,
            .patch_eligible = build->patch_eligible,
        };
    }

    w->cur += record_bytes;
    *records = persistent;
    return true;
}

/* Record compile-time counters after publication has definitely succeeded. */
static void jit_record_compilation_stats(const rv64_jit_publish_info_t *info, rv64_jit_block_end_reason_t end_reason)
{
    JIT_STAT_INC(blocks_compiled);
    rv64_jit_stat_block_end(end_reason);

    if (info->uses_translated_ifetch)
    {
        JIT_STAT_INC(translated_blocks);
        const vaddr_t last_pc = info->next_pc - RISCV_BASE_INSN_BYTES;

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
        JIT_STAT_ADD(stable_loop_preloaded_regs, info->stable_loop_reg_count);
    }

    JIT_STAT_ADD(compiled_insns, info->compiled_insn_count);
}

static jit_instruction_emit_outcome_t jit_make_instruction_emit_outcome(jit_instruction_emit_kind_t kind, uint32_t effects)
{
    return (jit_instruction_emit_outcome_t){.kind = kind, .effects = effects};
}

/*
 * Select and invoke the emitter for one guest instruction.
 *
 * This function deliberately owns only opcode selection. The caller still
 * owns transactional rollback, source metadata, block-level state, instruction
 * counting and publication. A failed emitter is therefore returned unchanged;
 * the caller inspects the writer's attempt-wide overflow flag only after it has
 * restored the instruction checkpoint.
 */
static jit_instruction_emit_outcome_t jit_emit_instruction(rv64_jit_writer_t *w, rv64_jit_reg_cache_t *regs,
                                                           rv64_jit_mmio_route_builder_t *mmio_routes, const jit_block_emit_plan_t *plan,
                                                           const jit_instruction_emit_request_t *request)
{
    const uint32_t opcode = request->instr & RISCV_OPCODE_MASK;

#ifdef CONFIG_RISCV_FPU
    if (jit_opcode_is_fp(opcode))
    {
        bool fp_ends_block = false;

        if (!rv64_jit_emit_fp_instr(w, regs, request->instr, request->pc, request->completed_before, &fp_ends_block))
        {
            return jit_make_instruction_emit_outcome(JIT_INSTRUCTION_EMIT_FAILED, JIT_INSTRUCTION_EFFECT_NONE);
        }

        const bool uses_data_translation = rv64_jit_decode_fp_memory(request->instr) != RV64_JIT_FP_MEMORY_INVALID &&
                                           (cpu.csr.satp >> RISCV64_SATP_MODE_SHIFT) != RISCV_SATP_MODE_BARE;

        return jit_make_instruction_emit_outcome(fp_ends_block ? JIT_INSTRUCTION_EMIT_STOP_FP_MEMORY : JIT_INSTRUCTION_EMIT_CONTINUE,
                                                 uses_data_translation ? JIT_INSTRUCTION_EFFECT_DATA_TRANSLATION : JIT_INSTRUCTION_EFFECT_NONE);
    }
#endif

    if (opcode == RISCV_OPCODE_JAL || opcode == RISCV_OPCODE_JALR)
    {
        if (!rv64_jit_emit_jump_instr(w, regs, request->instr, request->pc, request->completed_before, request->source_uses_data_translation_state))
        {
            return jit_make_instruction_emit_outcome(JIT_INSTRUCTION_EMIT_FAILED, JIT_INSTRUCTION_EFFECT_NONE);
        }

        return jit_make_instruction_emit_outcome(JIT_INSTRUCTION_EMIT_STOP_JUMP, JIT_INSTRUCTION_EFFECT_NONE);
    }

    if (opcode == RISCV_OPCODE_LOAD)
    {
        /*
         * A guarded load may side-exit with zero completed instructions when
         * it is first in the block. The dispatcher then lets the interpreter
         * execute the unsafe access.
         */
        if (!rv64_jit_emit_load_instr(w, regs, request->instr, request->pc, request->completed_before, mmio_routes))
        {
            return jit_make_instruction_emit_outcome(JIT_INSTRUCTION_EMIT_FAILED, JIT_INSTRUCTION_EFFECT_NONE);
        }

        return jit_make_instruction_emit_outcome(JIT_INSTRUCTION_EMIT_CONTINUE, (cpu.csr.satp >> RISCV64_SATP_MODE_SHIFT) != RISCV_SATP_MODE_BARE
                                                                                    ? JIT_INSTRUCTION_EFFECT_DATA_TRANSLATION
                                                                                    : JIT_INSTRUCTION_EFFECT_NONE);
    }

    if (opcode == RISCV_OPCODE_STORE)
    {
        /*
         * Safe PMEM stores continue natively. Faulting, MMIO, source-writing
         * and page-table-sensitive cases retain their established side exits.
         */
        if (!rv64_jit_emit_store_instr(w, regs, request->instr, request->pc, request->pc + RISCV_BASE_INSN_BYTES,
                                      request->completed_before, mmio_routes))
        {
            return jit_make_instruction_emit_outcome(JIT_INSTRUCTION_EMIT_FAILED, JIT_INSTRUCTION_EFFECT_NONE);
        }

        return jit_make_instruction_emit_outcome(JIT_INSTRUCTION_EMIT_CONTINUE, (cpu.csr.satp >> RISCV64_SATP_MODE_SHIFT) != RISCV_SATP_MODE_BARE
                                                                                    ? JIT_INSTRUCTION_EFFECT_DATA_TRANSLATION
                                                                                    : JIT_INSTRUCTION_EFFECT_NONE);
    }

    if (opcode == RISCV_OPCODE_BRANCH)
    {
        const bool can_chain = plan->backedge_mode != JIT_BACKEDGE_NONE;
        const bool stable = plan->backedge_mode == JIT_BACKEDGE_STABLE;
        bool emitted_native_backedge = false;

        if (!rv64_jit_emit_branch(w, regs, request->instr, request->pc, plan->start_pc, plan->loop_body_entry, can_chain, stable,
                                  &emitted_native_backedge, request->completed_before + 1u, request->source_uses_data_translation_state))
        {
            return jit_make_instruction_emit_outcome(JIT_INSTRUCTION_EMIT_FAILED, JIT_INSTRUCTION_EFFECT_NONE);
        }

        return jit_make_instruction_emit_outcome(emitted_native_backedge ? JIT_INSTRUCTION_EMIT_STOP_CHAINED_LOOP : JIT_INSTRUCTION_EMIT_CONTINUE,
                                                 JIT_INSTRUCTION_EFFECT_NONE);
    }

    if (!rv64_jit_emit_instr(w, regs, request->instr, request->pc, request->completed_before + 1u))
    {
        return jit_make_instruction_emit_outcome(JIT_INSTRUCTION_EMIT_FAILED, JIT_INSTRUCTION_EFFECT_NONE);
    }

    return jit_make_instruction_emit_outcome(JIT_INSTRUCTION_EMIT_CONTINUE, JIT_INSTRUCTION_EFFECT_NONE);
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
static rv64_jit_block_t *jit_compile_block_once(vaddr_t pc, uint32_t max_insns, bool *arena_overflowed)
{
    *arena_overflowed = false;

    if (!rv64_jit_code_init() || max_insns == 0)
    {
        return NULL;
    }

#if RV64_JIT_STATS
    rv64_jit_emitter_checkpoint_validate_once();
#endif

    if (rv64_jit_code_used + RV64_JIT_BLOCK_CODE_HEADROOM + RV64_JIT_MMIO_ROUTE_MAX_ALLOCATION + RV64_JIT_INDIRECT_JUMP_CACHE_MAX_ALLOCATION +
            RV64_JIT_INDIRECT_PIC_MAX_ALLOCATION + _Alignof(rv64_jit_link_t) - 1u + RV64_JIT_BLOCK_MAX_LINKS * sizeof(rv64_jit_link_t) >
        RV64_JIT_CODE_SIZE)
    {
        rv64_jit_arena_reset();
    }

    rv64_jit_code_used = rv64_jit_align_up(rv64_jit_code_used, RV64_JIT_CODE_ALIGN);

    paddr_t first_paddr = 0;
    bool uses_translated_ifetch = false;

    if (!rv64_jit_translate_ifetch_ex(pc, &first_paddr, &uses_translated_ifetch) || !in_pmem(first_paddr))
    {
        return NULL;
    }

    /*
     * Initialise the complete transactional state so future frontiers start
     * safely at zero. Every consumed payload record is still fully assigned
     * when emitted, avoiding a large per-block stack clear.
     */
    rv64_jit_link_builder_t links;
    links.state = (rv64_jit_link_builder_state_t){0};
    rv64_jit_indirect_pic_builder_t indirect_pic = {0};
    rv64_jit_indirect_jump_cache_builder_t indirect_jump_cache = {0};
    rv64_jit_writer_t w = {
        .start = rv64_jit_code + rv64_jit_code_used,
        .cur = rv64_jit_code + rv64_jit_code_used,
        .end = rv64_jit_code + RV64_JIT_CODE_SIZE,
        .links = &links,
        .indirect_pic = &indirect_pic,
        .indirect_jump_cache = &indirect_jump_cache,
    };
    rv64_jit_reg_cache_t regs;
    rv64_jit_reg_cache_init(&regs);
    rv64_jit_mmio_route_builder_t mmio_routes = {0};
    rv64_jit_emitter_checkpoint_t attempt_checkpoint;
    rv64_jit_emitter_checkpoint_capture(&attempt_checkpoint, &w, &regs, &mmio_routes);

    if (!rv64_jit_emit_prologue(&w))
    {
        *arena_overflowed = w.overflowed;
        rv64_jit_emitter_checkpoint_restore(&attempt_checkpoint, &w, &regs, &mmio_routes);
        return NULL;
    }

    const rv64_jit_loop_scan_t loop_scan = jit_prescan_self_backedge(pc, max_insns, uses_translated_ifetch);
    const rv64_jit_gpr_liveness_scan_t liveness_scan = jit_prescan_gpr_liveness(pc, max_insns, uses_translated_ifetch);
    const uint8_t *native_body_entry = w.cur;
    const uint8_t *loop_body_entry = native_body_entry;

    if (loop_scan.stable_reg_count != 0)
    {
        if (!rv64_jit_prepare_stable_loop_regs(&w, &regs, loop_scan.stable_reg_mask, loop_scan.loop_insn_count))
        {
            *arena_overflowed = w.overflowed;
            rv64_jit_emitter_checkpoint_restore(&attempt_checkpoint, &w, &regs, &mmio_routes);
            return NULL;
        }

        /*
         * Incoming direct links use native_body_entry and therefore execute
         * these preloads. Only the proven self-backedge skips them.
         */
        loop_body_entry = w.cur;
    }

    jit_backedge_mode_t backedge_mode = JIT_BACKEDGE_NONE;

    if (loop_scan.found_self_backedge)
    {
        backedge_mode = loop_scan.stable_reg_count != 0 ? JIT_BACKEDGE_STABLE : JIT_BACKEDGE_ORDINARY;
    }

    const jit_block_emit_plan_t emit_plan = {
        .start_pc = pc,
        .loop_body_entry = loop_body_entry,
        .backedge_mode = backedge_mode,
    };

    vaddr_t guest_pc = pc;
    uint32_t compiled_insn_count = 0;
    rv64_jit_source_builder_t source = {0};
    rv64_jit_ifetch_ref_builder_t ifetch_refs = {0};
    bool needs_data_translation_guard = false;
    bool used_stable_loop = false;
    rv64_jit_compile_exit_state_t exit_state = RV64_JIT_COMPILE_NEEDS_FALLTHROUGH_EXIT;
    rv64_jit_block_end_reason_t block_end_reason = RV64_JIT_BLOCK_END_BUDGET;

    while (compiled_insn_count < max_insns && compiled_insn_count < RV64_JIT_TRACE_MAX_INSNS)
    {
        /*
         * Re-translate every guest instruction, even inside one block. This keeps
         * the block metadata honest across page boundaries and avoids assuming that
         * adjacent virtual PCs are adjacent physical bytes.
         */
        paddr_t instruction_paddr = 0;
        bool instruction_uses_translated_ifetch = false;
        rv64_jit_ifetch_ref_builder_t ifetch_refs_start = ifetch_refs;

        if (!rv64_jit_translate_ifetch_collect(guest_pc, &instruction_paddr, &instruction_uses_translated_ifetch, &ifetch_refs) ||
            !in_pmem(instruction_paddr) || instruction_uses_translated_ifetch != uses_translated_ifetch)
        {
            ifetch_refs = ifetch_refs_start;
            block_end_reason = RV64_JIT_BLOCK_END_SOURCE_BOUNDARY;
            break;
        }

        const uint32_t instr = (uint32_t)vaddr_ifetch(guest_pc, RISCV_BASE_INSN_BYTES);
        uint32_t current_uses = 0;
        uint32_t current_defs = 0;
        bool current_terminal = false;

        if (compiled_insn_count < liveness_scan.count)
        {
            current_uses = liveness_scan.insns[compiled_insn_count].uses;
            rv64_jit_reg_cache_set_liveness(&regs, current_uses, liveness_scan.insns[compiled_insn_count].live_after);
        }
        else
        {
            /*
             * A conservative scan boundary can precede the real emitter's
             * exact boundary. Pin this instruction's operands but disable
             * future-use preference for the remaining prefix.
             */
            (void)jit_decode_gpr_use_def(instr, &current_uses, &current_defs, &current_terminal);
            rv64_jit_reg_cache_set_liveness(&regs, current_uses, UINT32_MAX);
        }

        rv64_jit_compile_snapshot_t instr_snapshot = {
            .source = source,
            .ifetch_refs = ifetch_refs_start,
        };
        rv64_jit_emitter_checkpoint_capture(&instr_snapshot.emitter, &w, &regs, &mmio_routes);

        if (!rv64_jit_source_builder_append(&source, instruction_paddr, RISCV_BASE_INSN_BYTES))
        {
            ifetch_refs = ifetch_refs_start;
            block_end_reason = RV64_JIT_BLOCK_END_SOURCE_BOUNDARY;
            break;
        }

        const jit_instruction_emit_request_t emit_request = {
            .instr = instr,
            .pc = guest_pc,
            .completed_before = compiled_insn_count,
            .source_uses_data_translation_state = needs_data_translation_guard,
        };
        const jit_instruction_emit_outcome_t emit_outcome = jit_emit_instruction(&w, &regs, &mmio_routes, &emit_plan, &emit_request);

        if (emit_outcome.kind == JIT_INSTRUCTION_EMIT_FAILED)
        {
            if (!jit_handle_emit_failure(&w, &regs, &source, &ifetch_refs, &mmio_routes, &instr_snapshot, instr, &block_end_reason, arena_overflowed))
            {
                rv64_jit_emitter_checkpoint_restore(&attempt_checkpoint, &w, &regs, &mmio_routes);
                return NULL;
            }
            break;
        }

        if ((emit_outcome.effects & JIT_INSTRUCTION_EFFECT_DATA_TRANSLATION) != 0)
        {
            /*
             * MPRV, SUM, and MXR affect explicit data accesses, whereas
             * instruction fetch uses its own privilege state. Direct links
             * must therefore guard data-translation state separately.
             */
            needs_data_translation_guard = true;
        }

        switch (emit_outcome.kind)
        {
        case JIT_INSTRUCTION_EMIT_CONTINUE:
            break;
        case JIT_INSTRUCTION_EMIT_STOP_FP_MEMORY:
            block_end_reason = RV64_JIT_BLOCK_END_FP_MEMORY;
            exit_state = RV64_JIT_COMPILE_HAS_TERMINAL_EXIT;
            break;
        case JIT_INSTRUCTION_EMIT_STOP_JUMP:
            block_end_reason = RV64_JIT_BLOCK_END_JUMP;
            exit_state = RV64_JIT_COMPILE_HAS_TERMINAL_EXIT;
            break;
        case JIT_INSTRUCTION_EMIT_STOP_CHAINED_LOOP:
            block_end_reason = RV64_JIT_BLOCK_END_CHAINED_LOOP;
            used_stable_loop = emit_plan.backedge_mode == JIT_BACKEDGE_STABLE;
            break;
        case JIT_INSTRUCTION_EMIT_FAILED:
        default:
            Assert(false, "jit: invalid RV64 instruction emit outcome %u", (unsigned)emit_outcome.kind);
        }

        guest_pc += RISCV_BASE_INSN_BYTES;
        compiled_insn_count++;

        /*
         * A chained back-edge is both a taken-loop fast path and the natural end
         * of this native block.  Its fall-through path returns below with
         * `rv64_jit_loop_extra + compiled_insn_count`, while taken laps jump
         * back to `loop_body_entry` without re-running the prologue or stable
         * register and counter preloads.
         */
        if (emit_outcome.kind != JIT_INSTRUCTION_EMIT_CONTINUE)
        {
            break;
        }
    }

    if (compiled_insn_count == 0)
    {
        if (block_end_reason == RV64_JIT_BLOCK_END_UNSUPPORTED_AFTER_PREFIX)
        {
            rv64_jit_mark_unsupported(pc, first_paddr, uses_translated_ifetch);
        }

        rv64_jit_emitter_checkpoint_restore(&attempt_checkpoint, &w, &regs, &mmio_routes);
        return NULL;
    }

    /*
     * JAL/JALR emit terminal paths as part of the instruction.  All other stop
     * conditions still need one fall-through exit.  Keeping this state explicit
     * avoids unreachable duplicate exits and prevents dead-byte emission from
     * rejecting an otherwise valid jump block.
     */
    if (exit_state == RV64_JIT_COMPILE_NEEDS_FALLTHROUGH_EXIT)
    {
        bool emitted_exit;

        if (rv64_jit_direct_link_enabled())
        {
            emitted_exit = rv64_jit_emit_direct_link_exit(&w, &regs, guest_pc, compiled_insn_count, needs_data_translation_guard, NULL);
        }
        else
        {
            emitted_exit = rv64_jit_emit_plain_block_exit(&w, &regs, guest_pc, compiled_insn_count);
        }

        if (!emitted_exit)
        {
            *arena_overflowed = w.overflowed;
            rv64_jit_emitter_checkpoint_restore(&attempt_checkpoint, &w, &regs, &mmio_routes);
            return NULL;
        }
    }

    /*
     * All ordinary paths are terminal now. A context-simple target can append
     * the alternate entry used by an incoming patched edge; translated or
     * data-sensitive targets retain only their complete guarded entry path.
     * The alternate entry performs the destination budget check without
     * another prologue and then jumps backwards to native_body_entry.
     */
    const uint8_t *chain_entry = NULL;

    if (!uses_translated_ifetch && !needs_data_translation_guard &&
        !rv64_jit_emit_chain_entry(&w, pc, compiled_insn_count, native_body_entry, &chain_entry))
    {
        *arena_overflowed = w.overflowed;
        rv64_jit_emitter_checkpoint_restore(&attempt_checkpoint, &w, &regs, &mmio_routes);
        return NULL;
    }

    /*
     * Keep profiler attribution and instruction-cache synchronisation bounded
     * to native bytes, then append mutable sidecars for this generation.
     */
    const uint8_t *native_code_end = w.cur;

    Assert(!(indirect_pic.state.used && indirect_jump_cache.state.used), "jit: one RV64 block selected both indirect cache kinds");

    if (!rv64_jit_finalise_mmio_routes(&w, &mmio_routes))
    {
        *arena_overflowed = w.overflowed;
        rv64_jit_emitter_checkpoint_restore(&attempt_checkpoint, &w, &regs, &mmio_routes);
        return NULL;
    }

    if (!rv64_jit_finalise_indirect_jump_cache(&w, &indirect_jump_cache))
    {
        *arena_overflowed = w.overflowed;
        rv64_jit_emitter_checkpoint_restore(&attempt_checkpoint, &w, &regs, &mmio_routes);
        return NULL;
    }

    rv64_jit_indirect_pic_t *indirect_pic_sidecar = NULL;

    if (!rv64_jit_finalise_indirect_pic(&w, &indirect_pic, &indirect_pic_sidecar))
    {
        *arena_overflowed = w.overflowed;
        rv64_jit_emitter_checkpoint_restore(&attempt_checkpoint, &w, &regs, &mmio_routes);
        return NULL;
    }

    rv64_jit_link_t *link_records = NULL;

    if (!jit_finalise_links(&w, &links, &link_records))
    {
        *arena_overflowed = w.overflowed;
        rv64_jit_emitter_checkpoint_restore(&attempt_checkpoint, &w, &regs, &mmio_routes);
        return NULL;
    }

    const rv64_jit_publish_info_t publish_info = {
        .start_pc = pc,
        .next_pc = guest_pc,
        .first_paddr = first_paddr,
        .uses_translated_ifetch = uses_translated_ifetch,
        .needs_data_translation_guard = needs_data_translation_guard,
        .compiled_insn_count = compiled_insn_count,
        .stable_loop_reg_count = used_stable_loop ? loop_scan.stable_reg_count : 0,
        .native_body_entry = native_body_entry,
        .chain_entry = chain_entry,
        .native_code_end = native_code_end,
        .allocation_end = w.cur,
        .link_records = link_records,
        .link_count = links.state.count,
        .indirect_pic = indirect_pic_sidecar,
        .source = &source,
        .ifetch_refs = &ifetch_refs,
    };

    rv64_jit_block_t *block = rv64_jit_publish_compiled_block(&w, &publish_info);
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
    rv64_jit_block_t *block = jit_compile_block_once(pc, max_insns, &arena_overflowed);

    if (block != NULL || !arena_overflowed)
    {
        return block;
    }

    rv64_jit_arena_reset();
    arena_overflowed = false;
    return jit_compile_block_once(pc, max_insns, &arena_overflowed);
}

#endif /* CONFIG_RV64 */
