#ifndef __RISCV32_JIT_INTERNAL_H__
#define __RISCV32_JIT_INTERNAL_H__

#include <generated/autoconf.h>

#ifndef CONFIG_RV64

#include <isa-jit.h>
#include <isa.h>
#include <cpu/difftest.h>
#include <memory/host.h>
#include <memory/paddr.h>
#include <memory/vaddr.h>
#include <utils.h>
#include "local-include/reg.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/*
 * Private declarations shared by the normal RV32 JIT compilation units.
 * Keep architectural constants and generated-code data layouts centralised so
 * moving implementation ownership cannot silently change the native ABI.
 */
#if defined(__x86_64__) && defined(CONFIG_RV32_JIT) && \
    defined(CONFIG_TARGET_NATIVE_ELF) && !defined(CONFIG_TRACE) && \
    !defined(CONFIG_DIFFTEST) && !defined(CONFIG_WATCHPOINT) && \
    !defined(CONFIG_MTRACE) && !defined(CONFIG_FTRACE)
#define RV32_JIT_ENABLED 1
#else
#define RV32_JIT_ENABLED 0
#endif

#define RV32_JIT_BLOCK_MAX_INSNS 64u
/*
 * Match the CPU device polling interval so one isa_jit_exec() call can consume
 * many short cached blocks before returning.  The cap is still bounded and
 * device_update() remains time-gated to TIMER_HZ, so this removes avoidable
 * dispatcher churn without letting native code run without limits.
 */
#define RV32_JIT_BATCH_MAX_INSNS 65536u
#define RV32_JIT_CACHE_SIZE 262144u
#define RV32_JIT_CODE_SIZE (256u * 1024u * 1024u)
#define RV32_JIT_CODE_ALIGN 16u
/*
 * Keep enough spare arena space for one worst-case block before compiling.  A
 * 64-instruction block can contain guarded memory operations with cold helper
 * exits, so the old 4 KiB margin was too tight near the end of the arena.
 */
#define RV32_JIT_BLOCK_CODE_HEADROOM (32u * 1024u)
/*
 * Store continuation needs to know whether a write can touch translated source
 * bytes.  A whole 4 KiB page is too coarse for small AM images, where .text,
 * .rodata, .data and .bss can share one page.  Track source ownership in
 * 128-byte chunks instead: this is still cheap, but it avoids forcing normal
 * data stores in the same page as code through the helper-and-exit path.
 */
#define RV32_JIT_SOURCE_CHUNK_SHIFT 7u
#define RV32_JIT_SOURCE_CHUNK_SIZE (1u << RV32_JIT_SOURCE_CHUNK_SHIFT)
#define RV32_JIT_SOURCE_CHUNK_MASK (RV32_JIT_SOURCE_CHUNK_SIZE - 1u)
#define RV32_JIT_PMEM_CHUNK_COUNT \
    (((size_t)CONFIG_MSIZE + (size_t)RV32_JIT_SOURCE_CHUNK_SIZE - 1u) / \
     (size_t)RV32_JIT_SOURCE_CHUNK_SIZE)
/*
 * Preserve the concise JIT paging vocabulary while taking every architectural
 * SATP and PTE value from the common ISA definitions.
 */
#define RV32_JIT_SATP_MODE_MASK RISCV32_SATP_MODE_MASK
#define RV32_JIT_SATP_PPN_MASK RISCV32_SATP_PPN_MASK
#define RV32_JIT_PTE_V RISCV_PTE_V
#define RV32_JIT_PTE_R RISCV_PTE_R
#define RV32_JIT_PTE_W RISCV_PTE_W
#define RV32_JIT_PTE_X RISCV_PTE_X
#define RV32_JIT_TLB_SIZE 128u
#define RV32_JIT_PMEM_PAGE_COUNT \
    (((size_t)CONFIG_MSIZE + (size_t)PAGE_SIZE - 1u) / (size_t)PAGE_SIZE)
#ifdef CONFIG_RV32_JIT_STATS
#define RV32_JIT_STATS 1
#else
#define RV32_JIT_STATS 0
#endif

typedef uint32_t (*rv32_jit_entry_t)(void);

typedef struct
{
    /* satp separates address spaces that reuse the same virtual page number. */
    uint32_t satp;
    /* Virtual page number, excluding the 12-bit page offset. */
    uint32_t vpn;
    /* Cached R/W/X permission bits from the Sv32 leaf PTE. */
    uint32_t perm;
    /* Physical base address of the translated 4 KiB page. */
    paddr_t pg_paddr;
    /* Level-0 page-table page used by the walk; stores here may stale the entry. */
    paddr_t pt_page;
    bool valid;
    /*
     * Keep each entry at 32 bytes so generated x86 can compute
     * `&jit_tlb[index]` with one shift instead of an imul.
     */
    uint8_t pad[11];
} rv32_jit_tlb_entry_t;

typedef char rv32_jit_tlb_entry_size_must_be_32[sizeof(rv32_jit_tlb_entry_t) == 32 ? 1 : -1];
typedef char rv32_jit_pmem_mapping_must_be_page_aligned[((CONFIG_MBASE | CONFIG_MSIZE) & PAGE_MASK) == 0 ? 1 : -1];

typedef struct
{
    /* First byte of the native code being emitted for the current block. */
    uint8_t *start;
    /* Next free byte in the code arena; every emit helper advances this pointer. */
    uint8_t *cur;
    /* One-past-last byte available to this writer; emit helpers fail at this bound. */
    uint8_t *end;
} rv32_jit_writer_t;

typedef struct
{
    /* True when this cache slot contains either native code or an unsupported marker. */
    bool valid;
    /* Guest virtual PC that starts this translated block. */
    vaddr_t pc;
    /* Address-space tag; Sv32 can map the same virtual PC to different PMEM bytes. */
    word_t satp;
    /* Physical address of the first guest instruction byte translated by this slot. */
    paddr_t paddr_start;
    /* Number of contiguous source bytes covered by this block, normally 4 * insns. */
    uint32_t source_len;
    /* Guest instruction count completed when `entry` returns normally. */
    uint32_t insn_count;
    /*
     * Native function pointer. NULL with valid == true is a negative cache marker:
     * the instruction is unsupported by this JIT and should use the interpreter.
     */
    rv32_jit_entry_t entry;
} rv32_jit_block_t;


#if RV32_JIT_STATS
typedef struct
{
    /* Number of times the CPU loop asked the JIT to execute at least one block. */
    uint64_t exec_requests;
    /* Cache lookup found a valid slot for the current PC/satp tag. */
    uint64_t cache_hits;
    /* Cache lookup missed and the compiler had to try building a block. */
    uint64_t cache_misses;
    /* Valid negative-cache slots that redirected execution to the interpreter. */
    uint64_t unsupported_hits;
    /* Native blocks that were actually entered and returned normally. */
    uint64_t blocks_executed;
    /* Guest instruction count reported by executed native blocks. */
    uint64_t executed_insns;

    /* Number of block compilation attempts. */
    uint64_t compile_requests;
    /* Number of successful native blocks published to the cache. */
    uint64_t blocks_compiled;
    /* Guest instruction count represented by successfully compiled blocks. */
    uint64_t compiled_insns;
    /* Number of negative-cache markers created for unsupported instructions. */
    uint64_t blocks_unsupported;
    /* Unsupported markers whose first instruction has an FP major opcode. */
    uint64_t fp_blocks_unsupported;
    /* Native integer prefixes that stop immediately before an FP instruction. */
    uint64_t native_prefixes_before_fp;
    /* Configured FP instructions emitted as calls to the shared SoftFloat executor. */
    uint64_t fp_helper_sites;
    /* FP helper calls made from generated native code. */
    uint64_t fp_helper_calls;
    /* Non-memory FP helper calls that completed and resumed the native block. */
    uint64_t fp_helper_continuations;
    /* FP helper calls that raised an architectural trap and left the native block. */
    uint64_t fp_helper_trap_exits;
    /* Successful FP loads and stores that conservatively ended the native block. */
    uint64_t fp_helper_memory_exits;
    /* Number of times the executable arena was cleared and reused. */
    uint64_t arena_resets;

    /* Number of physical invalidation requests from stores, DMA, or full flushes. */
    uint64_t invalidation_requests;
    /* Invalidation requests skipped because no source chunk refcount was present. */
    uint64_t invalidation_page_skips;
    /* Cached native blocks discarded because their source bytes overlapped a write. */
    uint64_t invalidated_blocks;

    /* Helper load calls made from native code. */
    uint64_t helper_loads;
    /* Helper loads that could still use proven ordinary PMEM access. */
    uint64_t helper_load_direct;
    /* Helper loads that delegated to the full virtual-memory/device path. */
    uint64_t helper_load_slow;
    /* Helper store calls made from native code. */
    uint64_t helper_stores;
    /* Helper stores that wrote proven ordinary PMEM directly. */
    uint64_t helper_store_direct;
    /* Helper stores that delegated to the full virtual-memory/device path. */
    uint64_t helper_store_slow;
    /* Complex RV32M operation helper calls not emitted directly in native code. */
    uint64_t helper_complex_ops;
} rv32_jit_stats_t;

#define JIT_STAT_INC(field) \
    do \
    { \
        rv32_jit_stats.field++; \
    } while (0)

#define JIT_STAT_ADD(field, value) \
    do \
    { \
        rv32_jit_stats.field += (value); \
    } while (0)
#else
#define JIT_STAT_INC(field) \
    do \
    { \
    } while (0)
#define JIT_STAT_ADD(field, value) \
    do \
    { \
    } while (0)
#endif

/* Return inclusive bit range [hi:lo] from a 32-bit instruction or value. */
static inline uint32_t bits(uint32_t value, int hi, int lo)
{
    return (value >> lo) & ((1u << (hi - lo + 1)) - 1u);
}

/*
 * Sign-extend a right-aligned immediate field. The xor/subtract form is a
 * branch-free way to move the unsigned field into the signed RV32 value range.
 */
static inline int32_t sext(uint32_t value, unsigned width)
{
    const uint32_t sign = 1u << (width - 1u);
    return (int32_t)((value ^ sign) - sign);
}

/* Decode an I-type immediate, used by loads, OP-IMM, JALR, and CSR forms. */
static inline int32_t imm_i(uint32_t instr)
{
    return sext(bits(instr, 31, 20), 12);
}

/* Decode an S-type store immediate from its split high/low instruction fields. */
static inline int32_t imm_s(uint32_t instr)
{
    return sext(bits(instr, 11, 7) | (bits(instr, 31, 25) << 5), 12);
}

/* Decode a B-type branch byte offset; bit 0 is implicit because branches align. */
static inline int32_t imm_b(uint32_t instr)
{
    const uint32_t imm = (bits(instr, 11, 8) << 1) | (bits(instr, 30, 25) << 5) | (bits(instr, 7, 7) << 11) | (bits(instr, 31, 31) << 12);
    return sext(imm, 13);
}

/* Decode a U-type immediate; it already occupies instruction bits [31:12]. */
static inline uint32_t imm_u(uint32_t instr)
{
    return instr & 0xfffff000u;
}

/* Decode a J-type jump byte offset from its shuffled instruction fields. */
static inline int32_t imm_j(uint32_t instr)
{
    const uint32_t imm = (bits(instr, 19, 12) << 12) | (bits(instr, 20, 20) << 11) | (bits(instr, 30, 21) << 1) | (bits(instr, 31, 31) << 20);
    return sext(imm, 21);
}

typedef struct
{
    /* Displacement of the slow-path jump emitted when satp is not Bare mode. */
    uint8_t *satp_slow_disp;
    /* Displacement of the slow-path jump emitted when the PMEM range check fails. */
    uint8_t *range_slow_disp;
} rv32_jit_pmem_guard_patch_t;

typedef struct
{
    /*
     * Paged-load guards have several independent reasons to give up: missing TLB
     * entry, different address space, permission miss, cross-page access, or an
     * unexpected PMEM range.  Keep the branch displacement list compact so the
     * caller can patch every conservative fallback to the same helper path.
     */
    uint8_t *slow_disps[8];
    uint32_t count;
} rv32_jit_tlb_load_patch_t;

typedef enum
{
    /* Callee-saved host register used as a guest-register cache slot. */
    RV32_JIT_HREG_RBX = 0,
    /* Callee-saved host register used as a guest-register cache slot. */
    RV32_JIT_HREG_R12,
    /* Callee-saved host register used as a guest-register cache slot. */
    RV32_JIT_HREG_R13,
    /* Callee-saved host register used as a guest-register cache slot. */
    RV32_JIT_HREG_R14,
    /* Callee-saved host register used as a guest-register cache slot. */
    RV32_JIT_HREG_R15,
    /* Number of host registers reserved for guest-register caching. */
    RV32_JIT_HREG_COUNT,
} rv32_jit_hreg_t;

typedef struct
{
    /* This slot currently represents `guest_reg`; false means it can be reused. */
    bool valid;
    /* The host register contains a real value; false means it is only reserved. */
    bool loaded;
    /* The host value differs from cpu.gpr[guest_reg] and must be flushed on exit. */
    bool dirty;
    /* Guest architectural register index stored in this slot. */
    uint32_t guest_reg;
    /* Monotonic use age for simple least-recently-used replacement. */
    uint32_t age;
    /* Which callee-saved host register backs this slot. */
    rv32_jit_hreg_t hreg;
} rv32_jit_reg_slot_t;

typedef struct
{
    /* Fixed set of host-register cache slots available within one native block. */
    rv32_jit_reg_slot_t slots[RV32_JIT_HREG_COUNT];
    /* Next age number assigned when a slot is touched. */
    uint32_t next_age;
    /* True once r9 has been loaded with `jit_source_chunk_refs` in this block. */
    bool source_refs_loaded;
} rv32_jit_reg_cache_t;

extern rv32_jit_block_t rv32_jit_cache[RV32_JIT_CACHE_SIZE];
extern rv32_jit_tlb_entry_t rv32_jit_tlb[RV32_JIT_TLB_SIZE];
extern uint16_t rv32_jit_tlb_pt_page_refs[RV32_JIT_PMEM_PAGE_COUNT];
extern uint16_t rv32_jit_source_chunk_refs[RV32_JIT_PMEM_CHUNK_COUNT];
extern uint8_t *rv32_jit_code;
extern size_t rv32_jit_code_used;
#if RV32_JIT_STATS
extern rv32_jit_stats_t rv32_jit_stats;
#endif
extern volatile uint32_t rv32_jit_entry_budget;
extern volatile uint32_t rv32_jit_loop_extra;

void rv32_jit_tlb_flush(void);
bool rv32_jit_write_may_touch_page_table(paddr_t addr, int len);
bool rv32_jit_write_may_touch_source_chunk(paddr_t addr, int len);
void rv32_jit_source_chunks_ref(paddr_t addr, uint32_t len);
void rv32_jit_source_chunks_unref(paddr_t addr, uint32_t len);

uint32_t rv32_jit_load_i8(vaddr_t addr);
uint32_t rv32_jit_load_i16(vaddr_t addr);
uint32_t rv32_jit_load_u32(vaddr_t addr);
uint32_t rv32_jit_load_u8(vaddr_t addr);
uint32_t rv32_jit_load_u16(vaddr_t addr);
void rv32_jit_store_u8(vaddr_t addr, uint32_t data);
void rv32_jit_store_u16(vaddr_t addr, uint32_t data);
void rv32_jit_store_u32(vaddr_t addr, uint32_t data);
uint32_t rv32_jit_store_u8_continue(vaddr_t addr, uint32_t data);
uint32_t rv32_jit_store_u16_continue(vaddr_t addr, uint32_t data);
uint32_t rv32_jit_store_u32_continue(vaddr_t addr, uint32_t data);

rv32_jit_block_t *rv32_jit_cache_slot(vaddr_t pc);
void rv32_jit_block_discard(rv32_jit_block_t *block);
bool rv32_jit_code_init(void);
void rv32_jit_arena_reset(void);
bool rv32_jit_translate_ifetch(vaddr_t pc, paddr_t *paddr);
bool rv32_jit_block_matches(const rv32_jit_block_t *block, vaddr_t pc);

void rv32_jit_reg_cache_init(rv32_jit_reg_cache_t *regs);
void rv32_jit_reg_cache_restore(
    rv32_jit_reg_cache_t *regs,
    const rv32_jit_reg_cache_t *snapshot);
bool rv32_jit_emit_prologue(rv32_jit_writer_t *writer);
bool rv32_jit_emit_block_exit(
    rv32_jit_writer_t *writer,
    rv32_jit_reg_cache_t *regs,
    vaddr_t next_pc,
    uint32_t completed_count,
    bool pc_already_set,
    bool return_loop_count);
bool rv32_jit_emit_load_store(
    rv32_jit_writer_t *writer,
    rv32_jit_reg_cache_t *regs,
    uint32_t instr,
    vaddr_t pc,
    uint32_t completed_count,
    bool loop_count_needed);
bool rv32_jit_emit_branch(
    rv32_jit_writer_t *writer,
    rv32_jit_reg_cache_t *regs,
    uint32_t instr,
    vaddr_t pc,
    vaddr_t block_start_pc,
    const uint8_t *block_start_native,
    bool loop_count_needed,
    bool chain_safe,
    bool *branch_chained,
    uint32_t completed_count);
bool rv32_jit_emit_control_flow(
    rv32_jit_writer_t *writer,
    rv32_jit_reg_cache_t *regs,
    uint32_t instr,
    vaddr_t pc,
    uint32_t completed_count);
bool rv32_jit_emit_alu(
    rv32_jit_writer_t *writer,
    rv32_jit_reg_cache_t *regs,
    uint32_t instr,
    vaddr_t pc);
#ifdef CONFIG_RISCV_FPU
bool rv32_jit_emit_fpu(
    rv32_jit_writer_t *writer,
    rv32_jit_reg_cache_t *regs,
    uint32_t instr,
    vaddr_t pc,
    uint32_t completed_count,
    bool *ends_block);
#endif
bool rv32_jit_instr_can_chain_body(uint32_t instr);

rv32_jit_block_t *rv32_jit_compile_block(vaddr_t pc, uint32_t max_insns);
void rv32_jit_dump_stats_report(void);

#endif /* !CONFIG_RV64 */

#endif /* __RISCV32_JIT_INTERNAL_H__ */
