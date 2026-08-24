#include <generated/autoconf.h>

#ifndef CONFIG_RV64

#include "jit-rv32-internal.h"

/*
 * RV32 JIT compile layer: fetch validation, negative-cache policy, loop
 * pre-scan, transactional block construction, and publication.
 */

/* Round `value` up to the next `align` boundary; align is a power of two here. */
static size_t jit_align_up(size_t value, size_t align)
{
    return (value + align - 1u) & ~(align - 1u);
}

/* Return true for RV32 instructions that terminate a straight-line block. */
static bool jit_instr_is_control_flow(uint32_t instr)
{
    const uint32_t opcode = instr & RISCV_OPCODE_MASK;
    return opcode == RISCV_OPCODE_BRANCH || opcode == RISCV_OPCODE_JAL || opcode == RISCV_OPCODE_JALR;
}

/*
 * Translate an instruction-fetch virtual PC to the physical source byte address.
 *
 * Blocks are invalidated by physical PMEM writes, so every cache entry records
 * the physical bytes that backed its translated guest instructions.
 */
bool rv32_jit_translate_ifetch(vaddr_t pc, paddr_t *paddr)
{
    const int mmu = isa_mmu_check(pc, 4, MEM_TYPE_IFETCH);

    if (mmu == MMU_DIRECT)
    {
        *paddr = (paddr_t)pc;
        return true;
    }

    if (mmu == MMU_TRANSLATE)
    {
        const paddr_t ret = isa_mmu_translate(pc, 4, MEM_TYPE_IFETCH);

        if ((ret & (paddr_t)PAGE_MASK) == MEM_RET_OK)
        {
            *paddr = (ret & ~(paddr_t)PAGE_MASK) | (paddr_t)(pc & PAGE_MASK);
            return true;
        }
    }

    return false;
}

/* Check whether a cache slot is still valid for the current PC, satp, and mapping. */
bool rv32_jit_block_matches(const rv32_jit_block_t *block, vaddr_t pc)
{
    /*
     * Cheap tag checks come first. Unsupported markers also pass this test when
     * their PC and satp still match; the caller will see entry == NULL and fall
     * back without trying to execute native code.
     */

    if (!block->valid || block->pc != pc || block->satp != cpu.csr.satp)
    {
        return false;
    }

    /*
     * satp alone is not enough in paged mode: a guest can rewrite page tables so
     * the same virtual PCs point at different physical source bytes. Re-translate
     * every virtual page touched by this block before trusting cached native code.
     * Most blocks fit in one page, while hot loops near a page boundary still keep
     * their native block if the recorded physical source bytes are unchanged.
     */

    if ((cpu.csr.satp & RV32_JIT_SATP_MODE_MASK) != RISCV_SATP_MODE_BARE)
    {
        uint32_t offset = 0;

        while (offset < block->source_len)
        {
            const vaddr_t check_pc = pc + (vaddr_t)offset;
            paddr_t now = 0;

            /*
             * A translation failure is treated as a cache miss. That keeps the JIT out
             * of cases where the normal interpreter path needs to raise or report the
             * underlying memory problem.
             */
            if (!rv32_jit_translate_ifetch(check_pc, &now) || now != block->paddr_start + (paddr_t)offset)
            {
                return false;
            }

            const uint32_t page_left = PAGE_SIZE - (uint32_t)(check_pc & PAGE_MASK);
            const uint32_t remaining = block->source_len - offset;
            offset += page_left < remaining ? page_left : remaining;
        }
    }

    return true;
}

/*
 * Floating-point loads, stores, fused operations, and ordinary OP-FP use seven
 * distinct major opcodes. Keep this classifier available even without optional
 * statistics because configured FP instructions are lowered through one shared
 * SoftFloat helper.
 */
static bool __attribute__((unused)) jit_opcode_is_fp(uint32_t opcode)
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

/* Publish a negative cache entry for an instruction this JIT cannot translate. */
static void jit_mark_unsupported(vaddr_t pc, paddr_t paddr, uint32_t source_len, uint32_t instr)
{
    JIT_STAT_INC(blocks_unsupported);
#if RV32_JIT_STATS
    if (jit_opcode_is_fp(instr & RISCV_OPCODE_MASK))
    {
        JIT_STAT_INC(fp_blocks_unsupported);
    }
#else
    (void)instr;
#endif

    /*
     * Negative cache entries still include satp and the first physical source
     * address so paged-mode lookups can reject them after a remap. They do not
     * own source-chunk refs because no native code can become stale; at worst, a
     * later overwrite keeps this PC on the interpreter path until normal slot
     * eviction or a full flush gives compilation another chance.
     *
     * The trade-off is deliberate: unsupported code is correctness-neutral because
     * it falls back immediately, while source-refcounting it would make every data
     * write near that instruction pay invalidation cost for no executable block.
     */
    rv32_jit_block_t *block = rv32_jit_cache_slot(pc);
    rv32_jit_block_discard(block);
    *block = (rv32_jit_block_t){
        .valid = true,
        .pc = pc,
        .satp = cpu.csr.satp,
        .paddr_start = paddr,
        .source_len = source_len,
        .insn_count = 0,
        .entry = NULL,
    };
}

/* Cheaply pre-scan whether this block can use loop-aware branch exits. */
static bool jit_block_has_chainable_backedge(vaddr_t pc, uint32_t max_insns, paddr_t first_paddr)
{
    vaddr_t cur_pc = pc;
    uint32_t count = 0;
    uint32_t source_len = 0;

    while (count < max_insns && count < RV32_JIT_BLOCK_MAX_INSNS)
    {
        paddr_t cur_paddr = 0;

        if (!rv32_jit_translate_ifetch(cur_pc, &cur_paddr) || !in_pmem(cur_paddr) || cur_paddr != first_paddr + (paddr_t)source_len)
        {
            return false;
        }

        const uint32_t instr = vaddr_ifetch(cur_pc, RISCV_BASE_INSN_BYTES);
        const uint32_t opcode = instr & RISCV_OPCODE_MASK;

        if (!rv32_jit_instr_can_chain_body(instr))
        {
            return false;
        }

        if (opcode == RISCV_OPCODE_BRANCH && cur_pc + imm_b(instr) == pc)
        {
            return true;
        }

        cur_pc += 4;
        source_len += 4;
        count++;
    }

    return false;
}

/*
 * Compile a native block starting at `pc`.
 *
 * The block is limited by the caller's execution budget, the maximum native
 * block length, unsupported instructions, control flow, and physical source
 * contiguity. It returns the published cache entry on success, or NULL when the
 * interpreter should execute the current instruction.
 */
rv32_jit_block_t *rv32_jit_compile_block(vaddr_t pc, uint32_t max_insns)
{
    JIT_STAT_INC(compile_requests);

    if (!rv32_jit_code_init() || max_insns == 0)
    {
        return NULL;
    }

    if (rv32_jit_code_used + RV32_JIT_BLOCK_CODE_HEADROOM > RV32_JIT_CODE_SIZE)
    {
        rv32_jit_arena_reset();
    }

    rv32_jit_code_used = jit_align_up(rv32_jit_code_used, RV32_JIT_CODE_ALIGN);

    paddr_t first_paddr = 0;

    if (!rv32_jit_translate_ifetch(pc, &first_paddr) || !in_pmem(first_paddr))
    {
        return NULL;
    }

    rv32_jit_writer_t w = {
        .start = rv32_jit_code + rv32_jit_code_used,
        .cur = rv32_jit_code + rv32_jit_code_used,
        .end = rv32_jit_code + RV32_JIT_CODE_SIZE,
    };
    rv32_jit_reg_cache_t regs;
    rv32_jit_reg_cache_init(&regs);

    if (!rv32_jit_emit_prologue(&w))
    {
        return NULL;
    }

    const bool loop_count_needed = jit_block_has_chainable_backedge(pc, max_insns, first_paddr);
    const uint8_t *block_start_native = w.cur;
    vaddr_t cur_pc = pc;
    uint32_t count = 0;
    uint32_t source_len = 0;
    uint32_t unsupported_instr = 0;
    bool block_sets_pc = false;
    bool chain_safe = loop_count_needed;
    bool chained_loop = false;
#if RV32_JIT_STATS
    bool stopped_before_fp = false;
    uint32_t emitted_fp_helper_sites = 0;
#endif

    while (count < max_insns && count < RV32_JIT_BLOCK_MAX_INSNS)
    {
        /*
         * Re-translate every guest instruction, even inside one block. This keeps
         * the block metadata honest across page boundaries and avoids assuming that
         * adjacent virtual PCs are adjacent physical bytes.
         */
        paddr_t cur_paddr = 0;

        if (!rv32_jit_translate_ifetch(cur_pc, &cur_paddr) || !in_pmem(cur_paddr))
        {
            break;
        }

        /*
         * Source invalidation records one physical byte range. Stop if virtual
         * aliases make the next guest instruction non-contiguous in PMEM.
         */

        if (cur_paddr != first_paddr + (paddr_t)source_len)
        {
            break;
        }

        const uint32_t instr = vaddr_ifetch(cur_pc, RISCV_BASE_INSN_BYTES);
        uint8_t *instr_start = w.cur;
        /*
         * Native bytes and compile-time register-cache metadata describe the same
         * partial instruction, so both must roll back together if emission fails.
         */
        rv32_jit_reg_cache_t regs_start = regs;
        bool end_block = false;
        const uint32_t opcode = instr & RISCV_OPCODE_MASK;

#ifdef CONFIG_RISCV_FPU
        if (jit_opcode_is_fp(opcode))
        {
            if (!rv32_jit_emit_fpu(&w, &regs, instr, cur_pc, count + 1u, &end_block))
            {
                w.cur = instr_start;
                rv32_jit_reg_cache_restore(&regs, &regs_start);
                unsupported_instr = instr;
#if RV32_JIT_STATS
                stopped_before_fp = true;
#endif
                break;
            }

            /*
             * A terminal FP memory helper has already published cpu.pc for
             * either normal completion or a trap. Prevent the common epilogue
             * from replacing that result with a compile-time sequential PC.
             */
            block_sets_pc = end_block;
#if RV32_JIT_STATS
            emitted_fp_helper_sites++;
#endif
        }
        else
#endif
            if (opcode == RISCV_OPCODE_BRANCH)
        {
            bool branch_chained = false;

            if (!rv32_jit_emit_branch(&w, &regs, instr, cur_pc, pc, block_start_native, loop_count_needed, chain_safe, &branch_chained, count + 1u))
            {
                w.cur = instr_start;
                rv32_jit_reg_cache_restore(&regs, &regs_start);
                unsupported_instr = instr;
                break;
            }

            if (branch_chained)
            {
                chained_loop = true;
                end_block = true;
            }
        }
        else if (jit_instr_is_control_flow(instr))
        {
            if (!rv32_jit_emit_control_flow(&w, &regs, instr, cur_pc, count + 1u))
            {
                w.cur = instr_start;
                rv32_jit_reg_cache_restore(&regs, &regs_start);
                unsupported_instr = instr;
                break;
            }

            block_sets_pc = true;
            end_block = true;
        }
        else if (!rv32_jit_emit_alu(&w, &regs, instr, cur_pc))
        {
            w.cur = instr_start;
            rv32_jit_reg_cache_restore(&regs, &regs_start);

            if (!rv32_jit_emit_load_store(&w, &regs, instr, cur_pc, count + 1u, loop_count_needed))
            {
                /*
                 * Emitters may fail after writing a prefix of an x86 instruction. Roll
                 * back to the last complete native instruction before falling back.
                 */
                w.cur = instr_start;
                rv32_jit_reg_cache_restore(&regs, &regs_start);
                unsupported_instr = instr;
#if RV32_JIT_STATS
                stopped_before_fp = jit_opcode_is_fp(opcode);
#endif
                break;
            }
        }

        cur_pc += 4;
        source_len += 4;
        count++;

        if (end_block)
        {
            break;
        }
    }

    if (count == 0)
    {
        jit_mark_unsupported(pc, first_paddr, RISCV_BASE_INSN_BYTES, unsupported_instr);
        return NULL;
    }

    if (!rv32_jit_emit_block_exit(&w, &regs, cur_pc, count, block_sets_pc, chained_loop))
    {
        return NULL;
    }

    __builtin___clear_cache((char *)w.start, (char *)w.cur);

    /*
     * Publish the block only after the instruction cache has been synchronised
     * and the old slot's source refs have been released. From this point onward,
     * writes to the recorded PMEM chunks must be able to find this block.
     */
    rv32_jit_block_t *block = rv32_jit_cache_slot(pc);
    rv32_jit_block_discard(block);
    rv32_jit_source_chunks_ref(first_paddr, source_len);
    JIT_STAT_INC(blocks_compiled);
    JIT_STAT_ADD(compiled_insns, count);
#if RV32_JIT_STATS
    if (stopped_before_fp)
    {
        JIT_STAT_INC(native_prefixes_before_fp);
    }
#endif
    *block = (rv32_jit_block_t){
        .valid = true,
        .pc = pc,
        .satp = cpu.csr.satp,
        .paddr_start = first_paddr,
        .source_len = source_len,
        .insn_count = count,
        .entry = (rv32_jit_entry_t)w.start,
    };

#if RV32_JIT_STATS
    /*
     * Commit emitted FP sites only after the complete block is published. A
     * failed final epilogue therefore cannot leak sites for discarded bytes.
     */
    JIT_STAT_ADD(fp_helper_sites, emitted_fp_helper_sites);
#endif

    rv32_jit_code_used = (size_t)(w.cur - rv32_jit_code);
    return block;
}

#endif /* !CONFIG_RV64 */
