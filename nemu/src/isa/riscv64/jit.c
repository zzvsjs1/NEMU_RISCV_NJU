#include <isa-jit.h>
#include <isa.h>
#include <memory/host.h>
#include <memory/paddr.h>
#include <memory/vaddr.h>
#include <utils.h>

#include <stddef.h>
#include <stdlib.h>

/*
 * Minimal RISC-V64 JIT bring-up.
 *
 * This first native emitter is deliberately small. It compiles RV64 integer ALU
 * instructions and conditional branches into x86-64 code, records the physical
 * source bytes used by each block, and lets the interpreter handle memory,
 * CSR/trap, fence, and other sensitive instructions. This gives a strict,
 * testable native-execution baseline before widening the fast path.
 */

#if defined(__x86_64__) && defined(CONFIG_RV64_JIT) && \
    defined(CONFIG_TARGET_NATIVE_ELF) && !defined(CONFIG_TRACE) && \
    !defined(CONFIG_DIFFTEST) && !defined(CONFIG_WATCHPOINT) && \
    !defined(CONFIG_MTRACE) && !defined(CONFIG_FTRACE)
#define RV64_JIT_ENABLED 1
#include <sys/mman.h>
#include <unistd.h>
#else
#define RV64_JIT_ENABLED 0
#endif

#ifdef CONFIG_RV64_JIT_STATS
#define RV64_JIT_STATS 1
#else
#define RV64_JIT_STATS 0
#endif

/* RISC-V base instructions are 4 bytes here; compressed `C` is not emitted yet. */
#define RV64_INSN_SIZE 4u
/* Seven low bits select the base RISC-V opcode. */
#define RV64_OPCODE_MASK 0x7fu
/* Branch targets must be 4-byte aligned while the JIT has no compressed path. */
#define RV64_BRANCH_ALIGN_MASK 0x3u

/* RISC-V opcodes used by this first native subset. */
#define RV64_OPCODE_LOAD 0x03u
#define RV64_OPCODE_OP_IMM 0x13u
#define RV64_OPCODE_AUIPC 0x17u
#define RV64_OPCODE_OP_IMM_32 0x1bu
#define RV64_OPCODE_STORE 0x23u
#define RV64_OPCODE_OP 0x33u
#define RV64_OPCODE_LUI 0x37u
#define RV64_OPCODE_OP_32 0x3bu
#define RV64_OPCODE_BRANCH 0x63u
#define RV64_OPCODE_JALR 0x67u
#define RV64_OPCODE_JAL 0x6fu

/* Compile at most 32 guest instructions so one block remains cheap to build. */
#define RV64_JIT_BLOCK_MAX_INSNS 32u
/* Match the CPU loop's device polling window; native code still returns bounded work. */
#define RV64_JIT_BATCH_MAX_INSNS 65536u
/* Power-of-two direct-mapped cache size, so `(size - 1)` is a valid index mask. */
#define RV64_JIT_CACHE_SIZE 65536u
/* 64 MiB keeps early RV64 experiments away from frequent arena resets. */
#define RV64_JIT_CODE_SIZE (64u * 1024u * 1024u)
/* 16-byte alignment is the normal x86-64 code-entry alignment. */
#define RV64_JIT_CODE_ALIGN 16u
/* Conservative per-block free-space check for the worst native byte expansion. */
#define RV64_JIT_BLOCK_CODE_HEADROOM (16u * 1024u)

typedef uint32_t (*rv64_jit_entry_t)(void);

typedef struct
{
    uint8_t *start;
    uint8_t *cur;
    uint8_t *end;
} rv64_jit_writer_t;

typedef struct
{
    bool valid;
    vaddr_t pc;
    word_t satp;
    paddr_t paddr_start;
    uint32_t source_len;
    uint32_t insn_count;
    rv64_jit_entry_t entry;
} rv64_jit_block_t;

typedef struct
{
    uint64_t exec_requests;
    uint64_t cache_hits;
    uint64_t cache_misses;
    uint64_t unsupported_hits;
    uint64_t blocks_compiled;
    uint64_t blocks_unsupported;
    uint64_t blocks_executed;
    uint64_t compiled_insns;
    uint64_t executed_insns;
    uint64_t native_loads;
    uint64_t native_stores;
    uint64_t native_jumps;
    uint64_t native_m_ops;
    uint64_t invalidation_requests;
    uint64_t invalidated_blocks;
    uint64_t arena_resets;
} rv64_jit_stats_t;

static rv64_jit_block_t jit_cache[RV64_JIT_CACHE_SIZE];
static uint8_t *jit_code = NULL;
static size_t jit_code_used = 0;
static rv64_jit_stats_t jit_stats;
#if RV64_JIT_ENABLED
static bool jit_disabled = false;
#endif
static bool jit_env_disable = false;
static bool jit_stats_enabled = false;
static bool jit_runtime_options_ready = false;
/* Current native-entry instruction budget, used by in-block chained loops. */
static volatile uint32_t jit_entry_budget = 0;
/* Extra guest instructions completed by earlier chained loop laps. */
static volatile uint32_t jit_loop_extra = 0;

/*
 * Public write-side guard. It becomes true after the native arena exists, so
 * PMEM writers know when exact physical invalidation may be needed.
 */
bool isa_jit_invalidation_active = false;

#if RV64_JIT_STATS
#define JIT_STAT_INC(field) \
    do \
    { \
        jit_stats.field++; \
    } while (0)
#define JIT_STAT_ADD(field, value) \
    do \
    { \
        jit_stats.field += (value); \
    } while (0)
#else
#define JIT_STAT_INC(field) \
    do \
    { \
    } while (0)
#define JIT_STAT_ADD(field, value) \
    do \
    { \
        (void)(value); \
    } while (0)
#endif

/* Extract an inclusive bit range from a 32-bit RISC-V instruction. */
static uint32_t bits(uint32_t value, int hi, int lo)
{
    /*
     * All current callers pass 0 <= lo <= hi < 32. The `(1u << width) - 1`
     * mask is therefore well-defined and keeps only the requested field.
     */
    return (value >> lo) & ((1u << (hi - lo + 1)) - 1u);
}

/* Sign-extend an instruction field whose sign bit is at width - 1. */
static int64_t sext(uint32_t value, unsigned width)
{
    /*
     * RV64 immediates in this file are at most 32 bits before extension. Shift
     * left until the field sign reaches bit 31, then rely on signed arithmetic
     * right shift to fill the high bits.
     */
    const uint32_t shift = 32u - width;
    return (int64_t)((int32_t)(value << shift) >> shift);
}

/* Decode the I-format immediate as a signed XLEN value. */
static int64_t imm_i(uint32_t instr)
{
    return sext(bits(instr, 31, 20), 12);
}

/* Decode the S-format store immediate as a signed XLEN value. */
static int64_t imm_s(uint32_t instr)
{
    const uint32_t imm = bits(instr, 11, 7) | (bits(instr, 31, 25) << 5);
    return sext(imm, 12);
}

/* Decode the B-format immediate, including the implicit low zero bit. */
static int64_t imm_b(uint32_t instr)
{
    /*
     * RISC-V scatters branch offsets as imm[12|10:5|4:1|11]. The low bit is
     * always zero because base ISA branches target halfword/word boundaries.
     */
    uint32_t imm = (bits(instr, 11, 8) << 1) |
                   (bits(instr, 30, 25) << 5) |
                   (bits(instr, 7, 7) << 11) |
                   (bits(instr, 31, 31) << 12);
    return sext(imm, 13);
}

/* Decode and sign-extend a U-format immediate for RV64 LUI/AUIPC. */
static int64_t imm_u_sext(uint32_t instr)
{
    /* `0xfffff000` keeps imm[31:12], the upper 20-bit U-type payload. */
    return (int64_t)(int32_t)(instr & 0xfffff000u);
}

/* Decode the J-format immediate, including the implicit low zero bit. */
static int64_t imm_j(uint32_t instr)
{
    /*
     * JAL scatters offsets as imm[20|10:1|11|19:12]. The low bit is implicit
     * zero, then the 21-bit value is sign-extended to XLEN.
     */
    uint32_t imm = (bits(instr, 30, 21) << 1) |
                   (bits(instr, 20, 20) << 11) |
                   (bits(instr, 19, 12) << 12) |
                   (bits(instr, 31, 31) << 20);
    return sext(imm, 21);
}

/* Return the byte offset of a guest GPR inside CPU_state. */
static uint32_t jit_gpr_offset(uint32_t reg)
{
    return (uint32_t)(offsetof(CPU_state, gpr) + reg * sizeof(cpu.gpr[0]));
}

/* Return the byte offset of the guest PC inside CPU_state. */
static uint32_t jit_pc_offset(void)
{
    return (uint32_t)offsetof(CPU_state, pc);
}

/* Read simple environment flags: unset, empty, and exactly "0" mean false. */
static bool jit_env_flag_enabled(const char *name)
{
    const char *value = getenv(name);
    return value != NULL && value[0] != '\0' &&
           !(value[0] == '0' && value[1] == '\0');
}

/* Cache runtime switches once so dispatch does not call getenv() repeatedly. */
static void jit_init_runtime_options(void)
{
    if (!jit_runtime_options_ready)
    {
        jit_env_disable = jit_env_flag_enabled("NEMU_DISABLE_JIT");
        jit_stats_enabled = jit_env_flag_enabled("NEMU_JIT_STATS");
        jit_runtime_options_ready = true;
    }
}

/* Return whether runtime configuration has disabled this binary's RV64 JIT. */
static bool jit_runtime_disabled(void)
{
    jit_init_runtime_options();
    return jit_env_disable;
}

/* Commit one already-guarded bare-mode PMEM store through the normal boundary. */
static void jit_store_pmem(paddr_t addr, uint32_t len, uint64_t data)
{
    /*
     * Keep paddr_write() in the store fast path. It owns MMIO dispatch, tracing
     * hooks and exact JIT source invalidation, so the native emitter does not
     * duplicate those correctness-sensitive policies.
     */
    paddr_write(addr, (int)len, (word_t)data);
}

/* Sign-extend one 32-bit W-form result to the RV64 register width. */
static uint64_t jit_sext32(uint32_t value)
{
    return (uint64_t)(int64_t)(int32_t)value;
}

/* Compute RV64M operations that are uncommon or awkward to emit inline. */
static uint64_t jit_m_result(uint64_t lhs, uint64_t rhs, uint32_t instr)
{
    const uint32_t opcode = instr & RV64_OPCODE_MASK;
    const uint32_t funct3 = bits(instr, 14, 12);
    const uint32_t key = (bits(instr, 31, 25) << 3) | funct3;

    if (opcode == RV64_OPCODE_OP)
    {
        switch (key)
        {
        case 0x009: /* MULH */
            return (uint64_t)(((__int128)(int64_t)lhs * (__int128)(int64_t)rhs) >> 64);
        case 0x00a: /* MULHSU */
            return (uint64_t)(((__int128)(int64_t)lhs * (__int128)(uint64_t)rhs) >> 64);
        case 0x00b: /* MULHU */
            return (uint64_t)(((__uint128_t)lhs * (__uint128_t)rhs) >> 64);
        case 0x00c: /* DIV */
            if (rhs == 0)
            {
                return UINT64_MAX;
            }
            if (lhs == (uint64_t)INT64_MIN && rhs == UINT64_MAX)
            {
                return lhs;
            }
            return (uint64_t)((int64_t)lhs / (int64_t)rhs);
        case 0x00d: /* DIVU */
            return rhs == 0 ? UINT64_MAX : lhs / rhs;
        case 0x00e: /* REM */
            if (rhs == 0)
            {
                return lhs;
            }
            if (lhs == (uint64_t)INT64_MIN && rhs == UINT64_MAX)
            {
                return 0;
            }
            return (uint64_t)((int64_t)lhs % (int64_t)rhs);
        case 0x00f: /* REMU */
            return rhs == 0 ? lhs : lhs % rhs;
        default:
            return 0;
        }
    }

    if (opcode == RV64_OPCODE_OP_32)
    {
        const int32_t lhs_s = (int32_t)lhs;
        const int32_t rhs_s = (int32_t)rhs;
        const uint32_t lhs_u = (uint32_t)lhs;
        const uint32_t rhs_u = (uint32_t)rhs;

        switch (key)
        {
        case 0x00c: /* DIVW */
            if (rhs_s == 0)
            {
                return UINT64_MAX;
            }
            if (lhs_s == INT32_MIN && rhs_s == -1)
            {
                return jit_sext32((uint32_t)lhs_s);
            }
            return jit_sext32((uint32_t)(lhs_s / rhs_s));
        case 0x00d: /* DIVUW */
            return rhs_u == 0 ? UINT64_MAX : jit_sext32(lhs_u / rhs_u);
        case 0x00e: /* REMW */
            if (rhs_s == 0)
            {
                return jit_sext32((uint32_t)lhs_s);
            }
            if (lhs_s == INT32_MIN && rhs_s == -1)
            {
                return 0;
            }
            return jit_sext32((uint32_t)(lhs_s % rhs_s));
        case 0x00f: /* REMUW */
            return rhs_u == 0 ? jit_sext32(lhs_u) : jit_sext32(lhs_u % rhs_u);
        default:
            return 0;
        }
    }

    return 0;
}

/* Round a code offset up to the next power-of-two alignment boundary. */
static size_t jit_align_up(size_t value, size_t align)
{
    return (value + align - 1u) & ~(align - 1u);
}

/* Return true when two half-open physical ranges overlap. */
static bool jit_ranges_overlap(paddr_t a, uint32_t a_len, paddr_t b, int b_len)
{
    if (a_len == 0 || b_len <= 0)
    {
        return false;
    }

    const paddr_t a_end = a + (paddr_t)a_len;
    const paddr_t b_end = b + (paddr_t)b_len;
    return a < b_end && b < a_end;
}

/* Hash the current address-space tag and guest PC into the direct-mapped cache. */
static uint32_t jit_hash(vaddr_t pc, word_t satp)
{
    /*
     * `pc >> 2` drops fixed 4-byte instruction-alignment zeros. `satp >> 12`
     * mixes the PPN/ASID-like high bits with the raw CSR value. The cache size
     * is a power of two, so `size - 1` is the direct-mapped slot mask.
     */
    return (uint32_t)(((pc >> 2) ^ satp ^ (satp >> 12)) & (RV64_JIT_CACHE_SIZE - 1u));
}

/* Return the direct-mapped block-cache slot for the current PC. */
static rv64_jit_block_t *jit_cache_slot(vaddr_t pc)
{
    return &jit_cache[jit_hash(pc, cpu.csr.satp)];
}

/* Clear every published block when arena or broad machine state changes. */
static void jit_cache_clear(void)
{
    memset(jit_cache, 0, sizeof(jit_cache));
}

/* Allocate executable memory for generated x86-64 blocks. */
static bool jit_code_init(void)
{
    if (jit_code != NULL)
    {
        return true;
    }

#if RV64_JIT_ENABLED
    if (jit_disabled)
    {
        return false;
    }

    void *mem = mmap(NULL, RV64_JIT_CODE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (mem == MAP_FAILED)
    {
        jit_disabled = true;
        Log("jit: mmap failed, disable RISC-V64 JIT");
        return false;
    }

    jit_code = (uint8_t *)mem;
    jit_code_used = 0;
    isa_jit_invalidation_active = true;
    Log("jit: RISC-V64 native code arena = %zu bytes", (size_t)RV64_JIT_CODE_SIZE);
    return true;
#else
    return false;
#endif
}

/* Reuse the executable arena after discarding every old code pointer. */
static void jit_arena_reset(void)
{
    jit_cache_clear();
    jit_code_used = 0;
    JIT_STAT_INC(arena_resets);
}

/* Emit one byte into the current native block. */
static bool emit_u8(rv64_jit_writer_t *w, uint8_t value)
{
    if (w->cur >= w->end)
    {
        return false;
    }

    *w->cur++ = value;
    return true;
}

/* Emit one little-endian 32-bit value into the current native block. */
static bool emit_u32(rv64_jit_writer_t *w, uint32_t value)
{
    for (size_t i = 0; i < sizeof(value); i++)
    {
        if (!emit_u8(w, (uint8_t)(value >> (i * 8))))
        {
            return false;
        }
    }

    return true;
}

/* Emit one little-endian 64-bit value into the current native block. */
static bool emit_u64(rv64_jit_writer_t *w, uint64_t value)
{
    for (size_t i = 0; i < sizeof(value); i++)
    {
        if (!emit_u8(w, (uint8_t)(value >> (i * 8))))
        {
            return false;
        }
    }

    return true;
}

/* Forward declaration: the prologue needs R10 before the grouped move helpers. */
static bool emit_movabs_r10(rv64_jit_writer_t *w, uint64_t value);

/* Emit `movabs r11, &cpu`, restoring the fixed CPU-state base register. */
static bool emit_load_cpu_base(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x49) && emit_u8(w, 0xbb) &&
           emit_u64(w, (uint64_t)(uintptr_t)&cpu);
}

/* Emit the common native-block prologue and load long-lived base registers. */
static bool emit_prologue(rv64_jit_writer_t *w)
{
    /*
     * System V enters a function with rsp % 16 == 8. Subtracting 8 gives helper
     * calls normal 16-byte alignment. R11 is caller-saved, so the generated code
     * can dedicate it to `CPU_state *` without saving it. R10 holds the host
     * pointer for guest physical CONFIG_MBASE, letting direct PMEM loads use
     * `[r10 + offset]` after a strict in-range guard.
     */
    return emit_u8(w, 0x48) && emit_u8(w, 0x83) &&
           emit_u8(w, 0xec) && emit_u8(w, 0x08) &&
           emit_load_cpu_base(w) &&
           emit_movabs_r10(w, (uint64_t)(uintptr_t)guest_to_host(CONFIG_MBASE));
}

/* Emit `add rsp, 8; ret`, restoring the caller's stack pointer. */
static bool emit_epilogue(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x83) &&
           emit_u8(w, 0xc4) && emit_u8(w, 0x08) &&
           emit_u8(w, 0xc3);
}

/* Emit a native return with a fixed completed guest-instruction count. */
static bool emit_return_count(rv64_jit_writer_t *w, uint32_t count)
{
    return emit_u8(w, 0xb8) && emit_u32(w, count) && emit_epilogue(w);
}

/* Emit a native return when EAX already holds the completed instruction count. */
static bool emit_return_eax(rv64_jit_writer_t *w)
{
    return emit_epilogue(w);
}

/* Emit `movabs rax, imm64`, used for full-width constants and helper targets. */
static bool emit_movabs_rax(rv64_jit_writer_t *w, uint64_t value)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0xb8) && emit_u64(w, value);
}

/* Emit `movabs rdx, imm64`, used for addresses of JIT loop counters. */
static bool emit_movabs_rdx(rv64_jit_writer_t *w, uint64_t value)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0xba) && emit_u64(w, value);
}

/* Emit `movabs rcx, imm64`, used for full-width PMEM range guards. */
static bool emit_movabs_rcx(rv64_jit_writer_t *w, uint64_t value)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0xb9) && emit_u64(w, value);
}

/* Emit `movabs r10, imm64`, the fixed host PMEM base for direct loads. */
static bool emit_movabs_r10(rv64_jit_writer_t *w, uint64_t value)
{
    return emit_u8(w, 0x49) && emit_u8(w, 0xba) && emit_u64(w, value);
}

/* Emit `mov eax, [rdx]`, loading one 32-bit JIT loop counter. */
static bool emit_mov_eax_m32_rdx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x8b) && emit_u8(w, 0x02);
}

/* Emit `mov [rdx], eax`, storing one 32-bit JIT loop counter. */
static bool emit_mov_m32_rdx_eax(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x89) && emit_u8(w, 0x02);
}

/* Emit `mov ecx, eax`, copying the loop count for the budget look-ahead. */
static bool emit_mov_ecx_eax(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x89) && emit_u8(w, 0xc1);
}

/* Emit `mov eax, ecx`, restoring a saved dynamic return count. */
static bool emit_mov_eax_ecx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x89) && emit_u8(w, 0xc8);
}

/* Emit `mov rcx, rax`, preserving a dynamic JALR target across link writes. */
static bool emit_mov_rcx_rax(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x89) && emit_u8(w, 0xc1);
}

/* Emit `mov rax, rcx`, restoring a dynamic JALR target. */
static bool emit_mov_rax_rcx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x89) && emit_u8(w, 0xc8);
}

/* Emit `mov rdx, rax`, copying a guest address for PMEM range checks. */
static bool emit_mov_rdx_rax(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x89) && emit_u8(w, 0xc2);
}

/* Emit `mov rdi, rdx`, preparing the first helper argument from a PMEM offset. */
static bool emit_mov_rdi_rdx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x89) && emit_u8(w, 0xd7);
}

/* Emit `mov rdi, rax`, preparing the first helper argument from a guest value. */
static bool emit_mov_rdi_rax(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x89) && emit_u8(w, 0xc7);
}

/* Emit `mov rsi, rdx`, preparing the second helper argument from a guest value. */
static bool emit_mov_rsi_rdx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x89) && emit_u8(w, 0xd6);
}

/* Emit `add eax, imm32`, used for completed-loop instruction accounting. */
static bool emit_add_eax_imm32(rv64_jit_writer_t *w, uint32_t imm)
{
    return emit_u8(w, 0x05) && emit_u32(w, imm);
}

/* Emit `add ecx, imm32`, used to test whether one more loop lap fits. */
static bool emit_add_ecx_imm32(rv64_jit_writer_t *w, uint32_t imm)
{
    return emit_u8(w, 0x81) && emit_u8(w, 0xc1) && emit_u32(w, imm);
}

/* Emit `sub rdx, rcx`, converting guest address to a PMEM byte offset. */
static bool emit_sub_rdx_rcx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x29) && emit_u8(w, 0xca);
}

/* Emit `add rdi, rcx`, converting a PMEM offset back to a physical address. */
static bool emit_add_rdi_rcx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x01) && emit_u8(w, 0xcf);
}

/* Emit `cmp rdx, rcx`, used by unsigned PMEM range guards. */
static bool emit_cmp_rdx_rcx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x39) && emit_u8(w, 0xca);
}

/* Emit `mov esi, imm32`, preparing the second helper argument. */
static bool emit_mov_esi_imm32(rv64_jit_writer_t *w, uint32_t imm)
{
    return emit_u8(w, 0xbe) && emit_u32(w, imm);
}

/* Emit `mov edx, imm32`, preparing the third helper argument. */
static bool emit_mov_edx_imm32(rv64_jit_writer_t *w, uint32_t imm)
{
    return emit_u8(w, 0xba) && emit_u32(w, imm);
}

/* Emit `cmp ecx, [rdx]`, comparing proposed work with the entry budget. */
static bool emit_cmp_ecx_m32_rdx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x3b) && emit_u8(w, 0x0a);
}

/* Return `jit_loop_extra + count` for exits from blocks with chained laps. */
static bool emit_return_loop_count(rv64_jit_writer_t *w, uint32_t count)
{
    return emit_movabs_rdx(w, (uint64_t)(uintptr_t)&jit_loop_extra) &&
           emit_mov_eax_m32_rdx(w) &&
           emit_add_eax_imm32(w, count) &&
           emit_return_eax(w);
}

/* Emit a 32-bit zeroing idiom for RAX. */
static bool emit_zero_rax(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x31) && emit_u8(w, 0xc0);
}

/* Emit `test al, imm8`, checking low address alignment bits. */
static bool emit_test_al_imm8(rv64_jit_writer_t *w, uint8_t mask)
{
    return emit_u8(w, 0xa8) && emit_u8(w, mask);
}

/* Load one guest register into RAX, treating x0 as the constant zero. */
static bool emit_load_gpr_rax(rv64_jit_writer_t *w, uint32_t reg)
{
    if (reg == 0)
    {
        return emit_zero_rax(w);
    }

    /* `49 8b 83 disp32` is `mov rax, [r11 + disp32]`. */
    return emit_u8(w, 0x49) && emit_u8(w, 0x8b) &&
           emit_u8(w, 0x83) && emit_u32(w, jit_gpr_offset(reg));
}

/* Load one guest register into RCX, treating x0 as the constant zero. */
static bool emit_load_gpr_rcx(rv64_jit_writer_t *w, uint32_t reg)
{
    if (reg == 0)
    {
        return emit_u8(w, 0x31) && emit_u8(w, 0xc9);
    }

    /* `49 8b 8b disp32` is `mov rcx, [r11 + disp32]`. */
    return emit_u8(w, 0x49) && emit_u8(w, 0x8b) &&
           emit_u8(w, 0x8b) && emit_u32(w, jit_gpr_offset(reg));
}

/* Load one guest register into RDX, treating x0 as the constant zero. */
static bool emit_load_gpr_rdx(rv64_jit_writer_t *w, uint32_t reg)
{
    if (reg == 0)
    {
        return emit_u8(w, 0x31) && emit_u8(w, 0xd2);
    }

    /* `49 8b 93 disp32` is `mov rdx, [r11 + disp32]`. */
    return emit_u8(w, 0x49) && emit_u8(w, 0x8b) &&
           emit_u8(w, 0x93) && emit_u32(w, jit_gpr_offset(reg));
}

/* Store RAX into a guest register, ignoring writes to x0. */
static bool emit_store_rax_gpr(rv64_jit_writer_t *w, uint32_t reg)
{
    if (reg == 0)
    {
        return true;
    }

    /* `49 89 83 disp32` is `mov [r11 + disp32], rax`. */
    return emit_u8(w, 0x49) && emit_u8(w, 0x89) &&
           emit_u8(w, 0x83) && emit_u32(w, jit_gpr_offset(reg));
}

/* Store an immediate guest PC by materialising it in RAX first. */
static bool emit_store_pc_imm(rv64_jit_writer_t *w, vaddr_t pc)
{
    return emit_movabs_rax(w, pc) &&
           /* `49 89 83 disp32` stores RAX into `cpu.pc` through R11. */
           emit_u8(w, 0x49) && emit_u8(w, 0x89) &&
           emit_u8(w, 0x83) && emit_u32(w, jit_pc_offset());
}

/* Store a dynamic guest PC already held in RAX. */
static bool emit_store_rax_pc(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x49) && emit_u8(w, 0x89) &&
           emit_u8(w, 0x83) && emit_u32(w, jit_pc_offset());
}

/* Emit `add rax, imm32`, whose immediate is sign-extended by x86-64. */
static bool emit_add_rax_imm32(rv64_jit_writer_t *w, int32_t imm)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x05) && emit_u32(w, (uint32_t)imm);
}

/* Emit `and rax, imm32`, whose immediate is sign-extended by x86-64. */
static bool emit_and_rax_imm32(rv64_jit_writer_t *w, int32_t imm)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x25) && emit_u32(w, (uint32_t)imm);
}

/* Emit one RAX op RCX 64-bit ALU instruction selected by the opcode byte. */
static bool emit_rax_rcx_alu64(rv64_jit_writer_t *w, uint8_t opcode)
{
    /*
     * Opcodes use ModRM C8 (`rax, rcx`): 01=ADD, 29=SUB, 31=XOR,
     * 09=OR and 21=AND. REX.W makes the operation full 64-bit.
     */
    return emit_u8(w, 0x48) && emit_u8(w, opcode) && emit_u8(w, 0xc8);
}

/* Emit one EAX op ECX 32-bit ALU instruction, then sign-extend to 64 bits. */
static bool emit_eax_ecx_alu32_sext(rv64_jit_writer_t *w, uint8_t opcode)
{
    /* W-form RV64 ALU operations keep low 32 bits, then CDQE sign-extends EAX. */
    return emit_u8(w, opcode) && emit_u8(w, 0xc8) &&
           emit_u8(w, 0x48) && emit_u8(w, 0x98);
}

/* Emit a 64-bit immediate shift of RAX. */
static bool emit_shift_rax_imm(rv64_jit_writer_t *w, uint8_t subop, uint8_t shamt)
{
    /* Group-2 ModRM subops are e0=SHL, e8=SHR and f8=SAR on RAX. */
    return emit_u8(w, 0x48) && emit_u8(w, 0xc1) && emit_u8(w, subop) && emit_u8(w, shamt);
}

/* Emit a 32-bit immediate shift of EAX, then sign-extend to 64 bits. */
static bool emit_shift_eax_imm_sext(rv64_jit_writer_t *w, uint8_t subop, uint8_t shamt)
{
    /* Group-2 ModRM subops are e0=SHL, e8=SHR and f8=SAR on EAX. */
    return emit_u8(w, 0xc1) && emit_u8(w, subop) && emit_u8(w, shamt) &&
           emit_u8(w, 0x48) && emit_u8(w, 0x98);
}

/* Emit a 64-bit variable shift of RAX by CL. */
static bool emit_shift_rax_cl(rv64_jit_writer_t *w, uint8_t subop)
{
    /* D3 uses CL as the variable shift count; RISC-V masks the count similarly. */
    return emit_u8(w, 0x48) && emit_u8(w, 0xd3) && emit_u8(w, subop);
}

/* Emit a 32-bit variable shift of EAX by CL, then sign-extend to 64 bits. */
static bool emit_shift_eax_cl_sext(rv64_jit_writer_t *w, uint8_t subop)
{
    /* D3 uses CL as the variable shift count; CDQE sign-extends W-form results. */
    return emit_u8(w, 0xd3) && emit_u8(w, subop) &&
           emit_u8(w, 0x48) && emit_u8(w, 0x98);
}

/* Emit `cmp rax, rcx` for signed or unsigned setcc operations. */
static bool emit_cmp_rax_rcx(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x39) && emit_u8(w, 0xc8);
}

/* Emit `cmp rax, imm32`, using x86-64 sign-extension of the immediate. */
static bool emit_cmp_rax_imm32(rv64_jit_writer_t *w, int32_t imm)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x3d) && emit_u32(w, (uint32_t)imm);
}

/* Materialise a condition-code result as 0 or 1 in RAX. */
static bool emit_setcc_rax(rv64_jit_writer_t *w, uint8_t setcc_opcode)
{
    /* `0f setcc c0` writes AL, then `0f b6 c0` zero-extends AL into EAX/RAX. */
    return emit_u8(w, 0x0f) && emit_u8(w, setcc_opcode) &&
           emit_u8(w, 0xc0) &&
           emit_u8(w, 0x0f) && emit_u8(w, 0xb6) && emit_u8(w, 0xc0);
}

/* Emit a conditional branch with a rel32 placeholder and return its patch site. */
static bool emit_jcc_rel32_placeholder(rv64_jit_writer_t *w, uint8_t jcc_opcode,
                                       uint8_t **disp)
{
    /* x86 near conditional branches are `0f 8x disp32`; `disp` points at disp32. */
    if (!emit_u8(w, 0x0f) || !emit_u8(w, jcc_opcode))
    {
        return false;
    }

    *disp = w->cur;
    return emit_u32(w, 0);
}

/* Emit an unconditional `jmp rel32` and return its displacement patch site. */
static bool emit_jmp_rel32_placeholder(rv64_jit_writer_t *w, uint8_t **disp)
{
    /* `e9 disp32` jumps relative to the byte after the 32-bit displacement. */
    if (!emit_u8(w, 0xe9))
    {
        return false;
    }

    *disp = w->cur;
    return emit_u32(w, 0);
}

/* Emit `movabs rax, target; call rax` for rare helper-backed side paths. */
static bool emit_call_abs(rv64_jit_writer_t *w, uintptr_t target)
{
    return emit_movabs_rax(w, (uint64_t)target) &&
           emit_u8(w, 0xff) && emit_u8(w, 0xd0);
}

/* Patch a rel32 displacement emitted by a previous branch helper. */
static void patch_rel32(uint8_t *disp, const uint8_t *target)
{
    int64_t rel = target - (disp + 4);
    Assert(rel >= INT32_MIN && rel <= INT32_MAX, "jit: rel32 target is out of range");
    int32_t rel32 = (int32_t)rel;
    memcpy(disp, &rel32, sizeof(rel32));
}

/* Emit a side exit that lets the interpreter execute the current instruction. */
static bool emit_interpreter_side_exit(rv64_jit_writer_t *w, vaddr_t pc,
                                       uint32_t completed_count,
                                       bool loop_count_needed)
{
    return emit_store_pc_imm(w, pc) &&
           (loop_count_needed ? emit_return_loop_count(w, completed_count)
                              : emit_return_count(w, completed_count));
}

/* Emit the x86 load instruction matching one RV64 load funct3 field. */
static bool emit_direct_pmem_load_rax(rv64_jit_writer_t *w, uint32_t funct3)
{
    /*
     * RDX is the byte offset from CONFIG_MBASE and R10 is the host pointer for
     * CONFIG_MBASE. Signed byte/half/word forms use x86 sign-extension loads;
     * unsigned forms write EAX, which zeroes the upper half of RAX by x86-64
     * rule. LD is a plain 64-bit load.
     */
    switch (funct3)
    {
    case 0x0: /* LB: movsx rax, byte ptr [r10 + rdx]. */
        return emit_u8(w, 0x49) && emit_u8(w, 0x0f) && emit_u8(w, 0xbe) &&
               emit_u8(w, 0x04) && emit_u8(w, 0x12);
    case 0x1: /* LH: movsx rax, word ptr [r10 + rdx]. */
        return emit_u8(w, 0x49) && emit_u8(w, 0x0f) && emit_u8(w, 0xbf) &&
               emit_u8(w, 0x04) && emit_u8(w, 0x12);
    case 0x2: /* LW: movsxd rax, dword ptr [r10 + rdx]. */
        return emit_u8(w, 0x49) && emit_u8(w, 0x63) &&
               emit_u8(w, 0x04) && emit_u8(w, 0x12);
    case 0x3: /* LD: mov rax, qword ptr [r10 + rdx]. */
        return emit_u8(w, 0x49) && emit_u8(w, 0x8b) &&
               emit_u8(w, 0x04) && emit_u8(w, 0x12);
    case 0x4: /* LBU: movzx eax, byte ptr [r10 + rdx]. */
        return emit_u8(w, 0x41) && emit_u8(w, 0x0f) && emit_u8(w, 0xb6) &&
               emit_u8(w, 0x04) && emit_u8(w, 0x12);
    case 0x5: /* LHU: movzx eax, word ptr [r10 + rdx]. */
        return emit_u8(w, 0x41) && emit_u8(w, 0x0f) && emit_u8(w, 0xb7) &&
               emit_u8(w, 0x04) && emit_u8(w, 0x12);
    case 0x6: /* LWU: mov eax, dword ptr [r10 + rdx]. */
        return emit_u8(w, 0x41) && emit_u8(w, 0x8b) &&
               emit_u8(w, 0x04) && emit_u8(w, 0x12);
    default:
        return false;
    }
}

/* Emit one guarded bare-mode RV64 load that falls back before unsafe accesses. */
static bool emit_load_instr(rv64_jit_writer_t *w, uint32_t instr, vaddr_t pc,
                            uint32_t completed_count, bool loop_count_needed)
{
    const uint32_t rd = bits(instr, 11, 7);
    const uint32_t funct3 = bits(instr, 14, 12);
    const uint32_t rs1 = bits(instr, 19, 15);
    const int32_t imm = (int32_t)imm_i(instr);
    uint32_t len = 0;
    uint8_t *align_slow_disp = NULL;
    uint8_t *range_slow_disp = NULL;
    uint8_t *done_disp = NULL;

    switch (funct3)
    {
    case 0x0: /* LB */
    case 0x4: /* LBU */
        len = 1;
        break;
    case 0x1: /* LH */
    case 0x5: /* LHU */
        len = 2;
        break;
    case 0x2: /* LW */
    case 0x6: /* LWU */
        len = 4;
        break;
    case 0x3: /* LD */
        len = 8;
        break;
    default:
        return false;
    }

    /*
     * This first memory tier is intentionally bare-mode only. The block cache
     * is tagged by satp, so a block compiled while satp.MODE is Bare cannot run
     * after paging is enabled. Sv39 gets its own dependency-tracked fast path
     * later instead of reusing this direct physical-address proof.
     */
    if ((cpu.csr.satp >> 60) != 0)
    {
        return false;
    }

    if (!emit_load_gpr_rax(w, rs1) ||
        !emit_add_rax_imm32(w, imm))
    {
        return false;
    }

    if (len > 1 &&
        (!emit_test_al_imm8(w, (uint8_t)(len - 1u)) ||
         !emit_jcc_rel32_placeholder(w, 0x85, &align_slow_disp)))
    {
        return false;
    }

    /*
     * Guard the complete physical byte range before touching host memory:
     *   offset = guest_addr - CONFIG_MBASE
     *   accept only offset <= CONFIG_MSIZE - len
     * Unsigned JA catches underflow, wraparound, MMIO and out-of-PMEM addresses.
     */
    if (!emit_mov_rdx_rax(w) ||
        !emit_movabs_rcx(w, (uint64_t)CONFIG_MBASE) ||
        !emit_sub_rdx_rcx(w) ||
        !emit_movabs_rcx(w, (uint64_t)CONFIG_MSIZE - len) ||
        !emit_cmp_rdx_rcx(w) ||
        !emit_jcc_rel32_placeholder(w, 0x87, &range_slow_disp) ||
        !emit_direct_pmem_load_rax(w, funct3) ||
        !emit_store_rax_gpr(w, rd) ||
        !emit_jmp_rel32_placeholder(w, &done_disp))
    {
        return false;
    }

    if (align_slow_disp != NULL)
    {
        patch_rel32(align_slow_disp, w->cur);
    }
    patch_rel32(range_slow_disp, w->cur);

    if (!emit_interpreter_side_exit(w, pc, completed_count, loop_count_needed))
    {
        return false;
    }

    patch_rel32(done_disp, w->cur);
    JIT_STAT_INC(native_loads);
    return true;
}

/* Emit one guarded bare-mode RV64 store that commits through paddr_write(). */
static bool emit_store_instr(rv64_jit_writer_t *w, uint32_t instr, vaddr_t pc,
                             vaddr_t next_pc, uint32_t completed_count,
                             bool loop_count_needed)
{
    const uint32_t funct3 = bits(instr, 14, 12);
    const uint32_t rs1 = bits(instr, 19, 15);
    const uint32_t rs2 = bits(instr, 24, 20);
    const int32_t imm = (int32_t)imm_s(instr);
    uint32_t len = 0;
    uint8_t *align_slow_disp = NULL;
    uint8_t *range_slow_disp = NULL;

    switch (funct3)
    {
    case 0x0: /* SB */
        len = 1;
        break;
    case 0x1: /* SH */
        len = 2;
        break;
    case 0x2: /* SW */
        len = 4;
        break;
    case 0x3: /* SD */
        len = 8;
        break;
    default:
        return false;
    }

    if ((cpu.csr.satp >> 60) != 0)
    {
        return false;
    }

    if (!emit_load_gpr_rax(w, rs1) ||
        !emit_add_rax_imm32(w, imm))
    {
        return false;
    }

    if (len > 1 &&
        (!emit_test_al_imm8(w, (uint8_t)(len - 1u)) ||
         !emit_jcc_rel32_placeholder(w, 0x85, &align_slow_disp)))
    {
        return false;
    }

    if (!emit_mov_rdx_rax(w) ||
        !emit_movabs_rcx(w, (uint64_t)CONFIG_MBASE) ||
        !emit_sub_rdx_rcx(w) ||
        !emit_movabs_rcx(w, (uint64_t)CONFIG_MSIZE - len) ||
        !emit_cmp_rdx_rcx(w) ||
        !emit_jcc_rel32_placeholder(w, 0x87, &range_slow_disp))
    {
        return false;
    }

    /*
     * Convert the proven PMEM offset back to a physical address for paddr_write:
     *   RDI = offset + CONFIG_MBASE, ESI = byte width, RDX = store data.
     * The helper may clobber caller-saved registers, so reload R11 before
     * updating cpu.pc for the completed store side exit.
     */
    if (!emit_mov_rdi_rdx(w) ||
        !emit_movabs_rcx(w, (uint64_t)CONFIG_MBASE) ||
        !emit_add_rdi_rcx(w) ||
        !emit_load_gpr_rdx(w, rs2) ||
        !emit_mov_esi_imm32(w, len) ||
        !emit_store_pc_imm(w, pc) ||
        !emit_call_abs(w, (uintptr_t)jit_store_pmem) ||
        !emit_load_cpu_base(w) ||
        !emit_store_pc_imm(w, next_pc) ||
        !(loop_count_needed ? emit_return_loop_count(w, completed_count + 1u)
                             : emit_return_count(w, completed_count + 1u)))
    {
        return false;
    }

    if (align_slow_disp != NULL)
    {
        patch_rel32(align_slow_disp, w->cur);
    }
    patch_rel32(range_slow_disp, w->cur);

    if (!emit_interpreter_side_exit(w, pc, completed_count, loop_count_needed))
    {
        return false;
    }

    JIT_STAT_INC(native_stores);
    return true;
}

/* Emit a helper-backed RV64M operation and keep compiling after the call. */
static bool emit_m_helper(rv64_jit_writer_t *w, uint32_t instr,
                          uint32_t rd, uint32_t rs1, uint32_t rs2)
{
    /*
     * System V arguments are RDI, RSI, RDX. The helper returns the result in
     * RAX. Because a C call may clobber caller-saved R10/R11, reload both JIT
     * base registers before storing the result or emitting later PMEM accesses.
     */
    if (!emit_load_gpr_rax(w, rs1) ||
        !emit_mov_rdi_rax(w) ||
        !emit_load_gpr_rdx(w, rs2) ||
        !emit_mov_rsi_rdx(w) ||
        !emit_mov_edx_imm32(w, instr) ||
        !emit_call_abs(w, (uintptr_t)jit_m_result) ||
        !emit_load_cpu_base(w) ||
        !emit_movabs_r10(w, (uint64_t)(uintptr_t)guest_to_host(CONFIG_MBASE)) ||
        !emit_store_rax_gpr(w, rd))
    {
        return false;
    }

    JIT_STAT_INC(native_m_ops);
    return true;
}

/* Emit a 64-bit RISC-V OP-IMM instruction into native code. */
static bool emit_op_imm(rv64_jit_writer_t *w, uint32_t instr)
{
    const uint32_t rd = bits(instr, 11, 7);
    const uint32_t funct3 = bits(instr, 14, 12);
    const uint32_t rs1 = bits(instr, 19, 15);
    const int32_t imm = (int32_t)imm_i(instr);

    if (!emit_load_gpr_rax(w, rs1))
    {
        return false;
    }

    switch (funct3)
    {
    case 0x0: /* ADDI */
        return emit_add_rax_imm32(w, imm) && emit_store_rax_gpr(w, rd);
    case 0x2: /* SLTI, signed compare; SETL is opcode 0x9c. */
        return emit_cmp_rax_imm32(w, imm) && emit_setcc_rax(w, 0x9c) && emit_store_rax_gpr(w, rd);
    case 0x3: /* SLTIU, unsigned compare; SETB is opcode 0x92. */
        return emit_cmp_rax_imm32(w, imm) && emit_setcc_rax(w, 0x92) && emit_store_rax_gpr(w, rd);
    case 0x4: /* XORI; `48 35 imm32` is XOR RAX, sign-extended imm32. */
        return emit_u8(w, 0x48) && emit_u8(w, 0x35) && emit_u32(w, (uint32_t)imm) && emit_store_rax_gpr(w, rd);
    case 0x6: /* ORI; `48 0d imm32` is OR RAX, sign-extended imm32. */
        return emit_u8(w, 0x48) && emit_u8(w, 0x0d) && emit_u32(w, (uint32_t)imm) && emit_store_rax_gpr(w, rd);
    case 0x7: /* ANDI; `48 25 imm32` is AND RAX, sign-extended imm32. */
        return emit_u8(w, 0x48) && emit_u8(w, 0x25) && emit_u32(w, (uint32_t)imm) && emit_store_rax_gpr(w, rd);
    case 0x1: /* SLLI; funct6 must be 000000 for RV64 base shifts. */
        if (bits(instr, 31, 26) != 0x00)
        {
            return false;
        }
        return emit_shift_rax_imm(w, 0xe0, (uint8_t)bits(instr, 25, 20)) && emit_store_rax_gpr(w, rd);
    case 0x5: /* SRLI/SRAI; funct6 selects logical versus arithmetic right shift. */
        if (bits(instr, 31, 26) == 0x00)
        {
            return emit_shift_rax_imm(w, 0xe8, (uint8_t)bits(instr, 25, 20)) && emit_store_rax_gpr(w, rd);
        }

        if (bits(instr, 31, 26) == 0x10)
        {
            return emit_shift_rax_imm(w, 0xf8, (uint8_t)bits(instr, 25, 20)) && emit_store_rax_gpr(w, rd);
        }
        return false;
    default:
        return false;
    }
}

/* Emit an RV64 OP-IMM-32 instruction and sign-extend the 32-bit result. */
static bool emit_op_imm32(rv64_jit_writer_t *w, uint32_t instr)
{
    const uint32_t rd = bits(instr, 11, 7);
    const uint32_t funct3 = bits(instr, 14, 12);
    const uint32_t rs1 = bits(instr, 19, 15);
    const int32_t imm = (int32_t)imm_i(instr);

    if (!emit_load_gpr_rax(w, rs1))
    {
        return false;
    }

    switch (funct3)
    {
    case 0x0: /* ADDIW; EAX addition naturally drops to 32 bits, then CDQE. */
        return emit_u8(w, 0x05) && emit_u32(w, (uint32_t)imm) &&
               emit_u8(w, 0x48) && emit_u8(w, 0x98) &&
               emit_store_rax_gpr(w, rd);
    case 0x1: /* SLLIW; funct7 must be zero and shamt is five bits. */
        if (bits(instr, 31, 25) != 0x00)
        {
            return false;
        }
        return emit_shift_eax_imm_sext(w, 0xe0, (uint8_t)bits(instr, 24, 20)) && emit_store_rax_gpr(w, rd);
    case 0x5: /* SRLIW/SRAIW; funct7 distinguishes logical from arithmetic. */
        if (bits(instr, 31, 25) == 0x00)
        {
            return emit_shift_eax_imm_sext(w, 0xe8, (uint8_t)bits(instr, 24, 20)) && emit_store_rax_gpr(w, rd);
        }

        if (bits(instr, 31, 25) == 0x20)
        {
            return emit_shift_eax_imm_sext(w, 0xf8, (uint8_t)bits(instr, 24, 20)) && emit_store_rax_gpr(w, rd);
        }
        return false;
    default:
        return false;
    }
}

/* Emit a 64-bit RV64 OP instruction for the integer ALU subset. */
static bool emit_op(rv64_jit_writer_t *w, uint32_t instr)
{
    const uint32_t rd = bits(instr, 11, 7);
    const uint32_t funct3 = bits(instr, 14, 12);
    const uint32_t rs1 = bits(instr, 19, 15);
    const uint32_t rs2 = bits(instr, 24, 20);
    const uint32_t key = (bits(instr, 31, 25) << 3) | funct3;

    if (!emit_load_gpr_rax(w, rs1) || !emit_load_gpr_rcx(w, rs2))
    {
        return false;
    }

    switch (key)
    {
    case 0x000: /* ADD */
        return emit_rax_rcx_alu64(w, 0x01) && emit_store_rax_gpr(w, rd);
    case 0x100: /* SUB */
        return emit_rax_rcx_alu64(w, 0x29) && emit_store_rax_gpr(w, rd);
    case 0x001: /* SLL */
        return emit_shift_rax_cl(w, 0xe0) && emit_store_rax_gpr(w, rd);
    case 0x002: /* SLT, signed compare; SETL is opcode 0x9c. */
        return emit_cmp_rax_rcx(w) && emit_setcc_rax(w, 0x9c) && emit_store_rax_gpr(w, rd);
    case 0x003: /* SLTU, unsigned compare; SETB is opcode 0x92. */
        return emit_cmp_rax_rcx(w) && emit_setcc_rax(w, 0x92) && emit_store_rax_gpr(w, rd);
    case 0x004: /* XOR */
        return emit_rax_rcx_alu64(w, 0x31) && emit_store_rax_gpr(w, rd);
    case 0x005: /* SRL */
        return emit_shift_rax_cl(w, 0xe8) && emit_store_rax_gpr(w, rd);
    case 0x105: /* SRA */
        return emit_shift_rax_cl(w, 0xf8) && emit_store_rax_gpr(w, rd);
    case 0x006: /* OR */
        return emit_rax_rcx_alu64(w, 0x09) && emit_store_rax_gpr(w, rd);
    case 0x007: /* AND */
        return emit_rax_rcx_alu64(w, 0x21) && emit_store_rax_gpr(w, rd);
    case 0x008: /* MUL; low 64 bits match x86-64 IMUL RAX, RCX. */
        JIT_STAT_INC(native_m_ops);
        return emit_u8(w, 0x48) && emit_u8(w, 0x0f) && emit_u8(w, 0xaf) && emit_u8(w, 0xc1) &&
               emit_store_rax_gpr(w, rd);
    case 0x009: /* MULH */
    case 0x00a: /* MULHSU */
    case 0x00b: /* MULHU */
    case 0x00c: /* DIV */
    case 0x00d: /* DIVU */
    case 0x00e: /* REM */
    case 0x00f: /* REMU */
        return emit_m_helper(w, instr, rd, rs1, rs2);
    default:
        return false;
    }
}

/* Emit an RV64 OP-32 instruction and sign-extend the 32-bit result. */
static bool emit_op32(rv64_jit_writer_t *w, uint32_t instr)
{
    const uint32_t rd = bits(instr, 11, 7);
    const uint32_t funct3 = bits(instr, 14, 12);
    const uint32_t rs1 = bits(instr, 19, 15);
    const uint32_t rs2 = bits(instr, 24, 20);
    const uint32_t key = (bits(instr, 31, 25) << 3) | funct3;

    if (!emit_load_gpr_rax(w, rs1) || !emit_load_gpr_rcx(w, rs2))
    {
        return false;
    }

    switch (key)
    {
    case 0x000: /* ADDW */
        return emit_eax_ecx_alu32_sext(w, 0x01) && emit_store_rax_gpr(w, rd);
    case 0x100: /* SUBW */
        return emit_eax_ecx_alu32_sext(w, 0x29) && emit_store_rax_gpr(w, rd);
    case 0x001: /* SLLW */
        return emit_shift_eax_cl_sext(w, 0xe0) && emit_store_rax_gpr(w, rd);
    case 0x005: /* SRLW */
        return emit_shift_eax_cl_sext(w, 0xe8) && emit_store_rax_gpr(w, rd);
    case 0x105: /* SRAW */
        return emit_shift_eax_cl_sext(w, 0xf8) && emit_store_rax_gpr(w, rd);
    case 0x008: /* MULW; IMUL low 32 bits, then CDQE sign-extension. */
        JIT_STAT_INC(native_m_ops);
        return emit_u8(w, 0x0f) && emit_u8(w, 0xaf) && emit_u8(w, 0xc1) &&
               emit_u8(w, 0x48) && emit_u8(w, 0x98) &&
               emit_store_rax_gpr(w, rd);
    case 0x00c: /* DIVW */
    case 0x00d: /* DIVUW */
    case 0x00e: /* REMW */
    case 0x00f: /* REMUW */
        return emit_m_helper(w, instr, rd, rs1, rs2);
    default:
        return false;
    }
}

/* Emit the taken side of a branch that can jump back to the native loop head. */
static bool emit_branch_chain_backedge(rv64_jit_writer_t *w, vaddr_t target,
                                       uint32_t exit_count,
                                       const uint8_t *target_native)
{
    uint8_t *over_budget_disp = NULL;
    uint8_t *loop_disp = NULL;

    /*
     * The current lap has completed `exit_count` guest instructions at the
     * branch.  EAX becomes the total completed count including this lap.  ECX
     * then looks one more full lap ahead; only if that still fits
     * `jit_entry_budget` do we store EAX in `jit_loop_extra` and jump back to
     * the native loop body.  Otherwise, returning EAX keeps cpu_exec() budget
     * accounting exact.
     */
    if (!emit_movabs_rdx(w, (uint64_t)(uintptr_t)&jit_loop_extra) ||
        !emit_mov_eax_m32_rdx(w) ||
        !emit_add_eax_imm32(w, exit_count) ||
        !emit_mov_ecx_eax(w) ||
        !emit_add_ecx_imm32(w, exit_count) ||
        !emit_movabs_rdx(w, (uint64_t)(uintptr_t)&jit_entry_budget) ||
        !emit_cmp_ecx_m32_rdx(w) ||
        !emit_jcc_rel32_placeholder(w, 0x87, &over_budget_disp) || /* JA: unsigned proposed count > budget. */
        !emit_movabs_rdx(w, (uint64_t)(uintptr_t)&jit_loop_extra) ||
        !emit_mov_m32_rdx_eax(w) ||
        !emit_jmp_rel32_placeholder(w, &loop_disp))
    {
        return false;
    }

    patch_rel32(loop_disp, target_native);
    patch_rel32(over_budget_disp, w->cur);
    /*
     * EAX contains the completed count, but emit_store_pc_imm() uses RAX as its
     * immediate scratch register. Preserve the count in ECX across the PC store
     * and restore EAX before the native function returns.
     */
    return emit_mov_ecx_eax(w) &&
           emit_store_pc_imm(w, target) &&
           emit_mov_eax_ecx(w) &&
           emit_return_eax(w);
}

/* Emit one conditional branch with a taken side exit and fall-through fast path. */
static bool emit_branch(rv64_jit_writer_t *w, uint32_t instr, vaddr_t pc,
                        vaddr_t block_start_pc, const uint8_t *block_start_native,
                        bool loop_count_needed, bool chain_safe,
                        bool *branch_chained, uint32_t exit_count)
{
    const uint32_t funct3 = bits(instr, 14, 12);
    const uint32_t rs1 = bits(instr, 19, 15);
    const uint32_t rs2 = bits(instr, 24, 20);
    const vaddr_t target = pc + imm_b(instr);
    uint8_t inverse_jcc = 0;
    uint8_t *fallthrough_disp = NULL;

    if ((target & RV64_BRANCH_ALIGN_MASK) != 0)
    {
        return false;
    }

    switch (funct3)
    {
    case 0x0: /* BEQ: inverse JNE falls through when not equal. */
        inverse_jcc = 0x85;
        break;
    case 0x1: /* BNE: inverse JE falls through when equal. */
        inverse_jcc = 0x84;
        break;
    case 0x4: /* BLT: inverse JGE falls through for signed greater/equal. */
        inverse_jcc = 0x8d;
        break;
    case 0x5: /* BGE: inverse JL falls through for signed less-than. */
        inverse_jcc = 0x8c;
        break;
    case 0x6: /* BLTU: inverse JAE falls through for unsigned above/equal. */
        inverse_jcc = 0x83;
        break;
    case 0x7: /* BGEU: inverse JB falls through for unsigned below. */
        inverse_jcc = 0x82;
        break;
    default:
        return false;
    }

    if (!emit_load_gpr_rax(w, rs1) ||
        !emit_load_gpr_rcx(w, rs2) ||
        !emit_cmp_rax_rcx(w) ||
        !emit_jcc_rel32_placeholder(w, inverse_jcc, &fallthrough_disp))
    {
        return false;
    }

    if (chain_safe && target == block_start_pc)
    {
        if (!emit_branch_chain_backedge(w, target, exit_count, block_start_native))
        {
            return false;
        }
        *branch_chained = true;
    }
    else if (!emit_store_pc_imm(w, target) ||
             !(loop_count_needed ? emit_return_loop_count(w, exit_count)
                                  : emit_return_count(w, exit_count)))
    {
        return false;
    }

    patch_rel32(fallthrough_disp, w->cur);
    return true;
}

/* Emit JAL or JALR, both of which end the current native block. */
static bool emit_jump_instr(rv64_jit_writer_t *w, uint32_t instr, vaddr_t pc,
                            uint32_t completed_count, bool loop_count_needed)
{
    const uint32_t opcode = instr & RV64_OPCODE_MASK;
    const uint32_t rd = bits(instr, 11, 7);
    const vaddr_t link = pc + RV64_INSN_SIZE;
    uint8_t *misaligned_disp = NULL;

    if (opcode == RV64_OPCODE_JAL)
    {
        const vaddr_t target = pc + imm_j(instr);

        if ((target & RV64_BRANCH_ALIGN_MASK) != 0)
        {
            return false;
        }

        JIT_STAT_INC(native_jumps);
        return emit_movabs_rax(w, link) &&
               emit_store_rax_gpr(w, rd) &&
               emit_store_pc_imm(w, target) &&
               (loop_count_needed ? emit_return_loop_count(w, completed_count + 1u)
                                  : emit_return_count(w, completed_count + 1u));
    }

    if (opcode != RV64_OPCODE_JALR || bits(instr, 14, 12) != 0)
    {
        return false;
    }

    /*
     * JALR computes `(rs1 + imm) & ~1`, then checks instruction alignment after
     * clearing bit zero.  The misaligned case returns before JALR executes so
     * the interpreter raises the same trap and does not write the link register.
     */
    if (!emit_load_gpr_rax(w, bits(instr, 19, 15)) ||
        !emit_add_rax_imm32(w, (int32_t)imm_i(instr)) ||
        !emit_and_rax_imm32(w, -2) ||
        !emit_test_al_imm8(w, RV64_BRANCH_ALIGN_MASK) ||
        !emit_jcc_rel32_placeholder(w, 0x85, &misaligned_disp) ||
        !emit_mov_rcx_rax(w) ||
        !emit_movabs_rax(w, link) ||
        !emit_store_rax_gpr(w, rd) ||
        !emit_mov_rax_rcx(w) ||
        !emit_store_rax_pc(w) ||
        !(loop_count_needed ? emit_return_loop_count(w, completed_count + 1u)
                             : emit_return_count(w, completed_count + 1u)))
    {
        return false;
    }

    patch_rel32(misaligned_disp, w->cur);
    if (!emit_interpreter_side_exit(w, pc, completed_count, loop_count_needed))
    {
        return false;
    }

    JIT_STAT_INC(native_jumps);
    return true;
}

/* Dispatch one supported non-branch RISC-V instruction to the native emitter. */
static bool emit_instr(rv64_jit_writer_t *w, uint32_t instr, vaddr_t pc,
                       uint32_t exit_count)
{
    const uint32_t opcode = instr & RV64_OPCODE_MASK;
    const uint32_t rd = bits(instr, 11, 7);

    switch (opcode)
    {
    case RV64_OPCODE_OP_IMM: /* ADDI/SLTI/SLTIU/XORI/ORI/ANDI/SLLI/SRLI/SRAI. */
        return emit_op_imm(w, instr);
    case RV64_OPCODE_OP_IMM_32: /* ADDIW/SLLIW/SRLIW/SRAIW. */
        return emit_op_imm32(w, instr);
    case RV64_OPCODE_OP: /* 64-bit register-register integer ALU subset. */
        return emit_op(w, instr);
    case RV64_OPCODE_OP_32: /* W-form register-register integer ALU subset. */
        return emit_op32(w, instr);
    case RV64_OPCODE_LUI: /* LUI materialises the sign-extended U immediate. */
        return emit_movabs_rax(w, (uint64_t)imm_u_sext(instr)) &&
               emit_store_rax_gpr(w, rd);
    case RV64_OPCODE_AUIPC: /* AUIPC adds the sign-extended U immediate to PC. */
        return emit_movabs_rax(w, (uint64_t)(pc + imm_u_sext(instr))) &&
               emit_store_rax_gpr(w, rd);
    default:
        (void)exit_count;
        return false;
    }
}

/* Translate an instruction-fetch virtual PC to its physical source address. */
static bool jit_translate_ifetch(vaddr_t pc, paddr_t *paddr)
{
    /* Only 32-bit base instructions are compiled; compressed fetch is fallback. */
    const int mmu = isa_mmu_check(pc, RV64_INSN_SIZE, MEM_TYPE_IFETCH);

    if (mmu == MMU_DIRECT)
    {
        *paddr = (paddr_t)pc;
        return true;
    }

    /*
     * Keep the first RV64 JIT milestone conservative.  Sv39 instruction fetch
     * depends on page-table memory as well as the final instruction bytes; the
     * RV32 JIT tracks those page-table dependencies, but this minimal RV64
     * emitter does not yet.  Falling back here preserves strict interpreter
     * behaviour for paged kernels until that dependency tracking is added.
     */
    if (mmu == MMU_TRANSLATE)
    {
        return false;
    }

    return false;
}

/* Check whether a cache slot still describes the current PC and source bytes. */
static bool jit_block_matches(const rv64_jit_block_t *block, vaddr_t pc)
{
    if (!block->valid || block->pc != pc || block->satp != cpu.csr.satp)
    {
        return false;
    }

    paddr_t now = 0;
    return jit_translate_ifetch(pc, &now) && now == block->paddr_start;
}

/* Publish a negative cache entry for a currently unsupported instruction. */
static void jit_mark_unsupported(vaddr_t pc, paddr_t paddr)
{
    JIT_STAT_INC(blocks_unsupported);

    rv64_jit_block_t *block = jit_cache_slot(pc);
    *block = (rv64_jit_block_t){
        .valid = true,
        .pc = pc,
        .satp = cpu.csr.satp,
        .paddr_start = paddr,
        .source_len = RV64_INSN_SIZE,
        .insn_count = 0,
        .entry = NULL,
    };
}

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
                                             paddr_t first_paddr)
{
    vaddr_t cur_pc = pc;
    uint32_t count = 0;
    uint32_t source_len = 0;

    while (count < max_insns && count < RV64_JIT_BLOCK_MAX_INSNS)
    {
        paddr_t cur_paddr = 0;

        if (!jit_translate_ifetch(cur_pc, &cur_paddr) || !in_pmem(cur_paddr) ||
            cur_paddr != first_paddr + (paddr_t)source_len)
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
        source_len += RV64_INSN_SIZE;
        count++;
    }

    return false;
}

/* Compile one straight-line block starting at the current guest PC. */
static rv64_jit_block_t *jit_compile_block(vaddr_t pc, uint32_t max_insns)
{
    if (!jit_code_init() || max_insns == 0)
    {
        return NULL;
    }

    if (jit_code_used + RV64_JIT_BLOCK_CODE_HEADROOM > RV64_JIT_CODE_SIZE)
    {
        jit_arena_reset();
    }

    jit_code_used = jit_align_up(jit_code_used, RV64_JIT_CODE_ALIGN);

    paddr_t first_paddr = 0;
    if (!jit_translate_ifetch(pc, &first_paddr) || !in_pmem(first_paddr))
    {
        return NULL;
    }

    rv64_jit_writer_t w = {
        .start = jit_code + jit_code_used,
        .cur = jit_code + jit_code_used,
        .end = jit_code + RV64_JIT_CODE_SIZE,
    };

    if (!emit_prologue(&w))
    {
        return NULL;
    }

    const bool loop_count_needed =
        jit_block_has_chainable_backedge(pc, max_insns, first_paddr);
    const uint8_t *block_start_native = w.cur;
    vaddr_t cur_pc = pc;
    uint32_t count = 0;
    uint32_t source_len = 0;
    bool chain_safe = loop_count_needed;
    bool chained_loop = false;

    while (count < max_insns && count < RV64_JIT_BLOCK_MAX_INSNS)
    {
        paddr_t cur_paddr = 0;

        if (!jit_translate_ifetch(cur_pc, &cur_paddr) || !in_pmem(cur_paddr))
        {
            break;
        }

        if (cur_paddr != first_paddr + (paddr_t)source_len)
        {
            break;
        }

        const uint32_t instr = (uint32_t)vaddr_ifetch(cur_pc, RV64_INSN_SIZE);
        const uint32_t opcode = instr & RV64_OPCODE_MASK;
        uint8_t *instr_start = w.cur;
        bool end_block = false;

        if (opcode == RV64_OPCODE_JAL ||
            opcode == RV64_OPCODE_JALR)
        {
            if ((opcode == RV64_OPCODE_JALR && count == 0) ||
                !emit_jump_instr(&w, instr, cur_pc, count, loop_count_needed))
            {
                w.cur = instr_start;
                break;
            }
            end_block = true;
        }
        else if (opcode == RV64_OPCODE_LOAD)
        {
            /*
             * A slow load side exit returns before the load executes. Avoid
             * publishing a block whose first possible exit would report zero
             * retired instructions; the interpreter will handle that leading
             * load directly.
             */
            if (count == 0 ||
                !emit_load_instr(&w, instr, cur_pc, count, loop_count_needed))
            {
                w.cur = instr_start;
                break;
            }
        }
        else if (opcode == RV64_OPCODE_STORE)
        {
            /*
             * The helper-backed store path returns after committing the store.
             * If the store is the first instruction, an unsafe-address side exit
             * would need to return zero retired instructions, so leave that case
             * to the interpreter.
             */
            if (count == 0 ||
                !emit_store_instr(&w, instr, cur_pc, cur_pc + RV64_INSN_SIZE,
                                  count, loop_count_needed))
            {
                w.cur = instr_start;
                break;
            }
            end_block = true;
        }
        else if (opcode == RV64_OPCODE_BRANCH)
        {
            bool branch_chained = false;

            if (!emit_branch(&w, instr, cur_pc, pc, block_start_native,
                             loop_count_needed, chain_safe, &branch_chained,
                             count + 1u))
            {
                w.cur = instr_start;
                break;
            }

            if (branch_chained)
            {
                chained_loop = true;
                end_block = true;
            }
        }
        else if (!emit_instr(&w, instr, cur_pc, count + 1u))
        {
            w.cur = instr_start;
            break;
        }

        cur_pc += RV64_INSN_SIZE;
        source_len += RV64_INSN_SIZE;
        count++;

        /*
         * A chained back-edge is both a taken-loop fast path and the natural end
         * of this native block.  Its fall-through path returns below with
         * `jit_loop_extra + count`, while taken laps jump back to
         * `block_start_native` without re-running the prologue.
         */
        if (end_block)
        {
            break;
        }
    }

    if (count == 0)
    {
        jit_mark_unsupported(pc, first_paddr);
        return NULL;
    }

    if (!emit_store_pc_imm(&w, cur_pc) ||
        !(chained_loop ? emit_return_loop_count(&w, count)
                       : emit_return_count(&w, count)))
    {
        return NULL;
    }

    __builtin___clear_cache((char *)w.start, (char *)w.cur);

    rv64_jit_block_t *block = jit_cache_slot(pc);
    *block = (rv64_jit_block_t){
        .valid = true,
        .pc = pc,
        .satp = cpu.csr.satp,
        .paddr_start = first_paddr,
        .source_len = source_len,
        .insn_count = count,
        .entry = (rv64_jit_entry_t)w.start,
    };

    jit_code_used = (size_t)(w.cur - jit_code);
    JIT_STAT_INC(blocks_compiled);
    JIT_STAT_ADD(compiled_insns, count);
    return block;
}

/* Report whether native RV64 JIT execution can be attempted in this run. */
bool isa_jit_available(void)
{
    return RV64_JIT_ENABLED && !jit_runtime_disabled();
}

/* Drop all cached native blocks and reset private RV64 JIT state. */
void isa_jit_flush_all(void)
{
    if (jit_code != NULL)
    {
        jit_arena_reset();
    }
}

/* Invalidate native blocks whose physical source bytes overlap a PMEM write. */
void isa_jit_invalidate_paddr(paddr_t addr, int len)
{
    JIT_STAT_INC(invalidation_requests);

    if (len <= 0 || jit_code == NULL)
    {
        return;
    }

    for (size_t i = 0; i < RV64_JIT_CACHE_SIZE; i++)
    {
        rv64_jit_block_t *block = &jit_cache[i];

        if (!block->valid)
        {
            continue;
        }

        if (jit_ranges_overlap(block->paddr_start, block->source_len, addr, len))
        {
            block->valid = false;
            block->entry = NULL;
            JIT_STAT_INC(invalidated_blocks);
        }
    }
}

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

        if (block_budget > RV64_JIT_BLOCK_MAX_INSNS)
        {
            block_budget = RV64_JIT_BLOCK_MAX_INSNS;
        }

        rv64_jit_block_t *block = jit_cache_slot(cpu.pc);

        if (jit_block_matches(block, cpu.pc))
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
            block = jit_compile_block(cpu.pc, block_budget);
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
         * and generated code. `jit_entry_budget` is the maximum work this entry
         * may retire; `jit_loop_extra` starts at zero and accumulates completed
         * native loop laps before the final block exit returns the total.
         */
        jit_entry_budget = remaining_budget;
        jit_loop_extra = 0;
        const uint32_t ran = block->entry();
        Assert(ran > 0 && ran <= remaining_budget,
               "jit: invalid RV64 executed count %u", ran);
        JIT_STAT_INC(blocks_executed);
        JIT_STAT_ADD(executed_insns, ran);
        total += ran;
    }

    *executed = total;
    return total > 0;
}

#if RV64_JIT_STATS
/* Compute a rounded fixed-point ratio with two decimal digits. */
static uint64_t jit_ratio_x100(uint64_t numerator, uint64_t denominator)
{
    if (denominator == 0)
    {
        return 0;
    }

    return (numerator * 100u + denominator / 2u) / denominator;
}

/* Compute a rounded fixed-point percentage with two decimal digits. */
static uint64_t jit_percent_x100(uint64_t numerator, uint64_t denominator)
{
    if (denominator == 0)
    {
        return 0;
    }

    return (numerator * 10000u + denominator / 2u) / denominator;
}
#endif

/* Print optional RV64 JIT counters at the end of execution. */
void isa_jit_dump_stats(void)
{
    jit_init_runtime_options();

    if (jit_runtime_disabled())
    {
        Log("jit: disabled by NEMU_DISABLE_JIT=1");
        return;
    }

#if RV64_JIT_STATS
    if (!jit_stats_enabled || !RV64_JIT_ENABLED)
    {
        return;
    }

    const uint64_t cache_total = jit_stats.cache_hits + jit_stats.cache_misses;
    const uint64_t cache_hit_pct =
        jit_percent_x100(jit_stats.cache_hits, cache_total);
    const uint64_t avg_compile_len =
        jit_ratio_x100(jit_stats.compiled_insns, jit_stats.blocks_compiled);
    const uint64_t avg_exec_len =
        jit_ratio_x100(jit_stats.executed_insns, jit_stats.blocks_executed);

    Log("jit: exec requests = %" PRIu64
        ", cache hits = %" PRIu64
        ", misses = %" PRIu64
        ", hit rate = %" PRIu64 ".%02" PRIu64 "%%",
        jit_stats.exec_requests,
        jit_stats.cache_hits,
        jit_stats.cache_misses,
        cache_hit_pct / 100u,
        cache_hit_pct % 100u);
    Log("jit: compiled blocks = %" PRIu64
        ", unsupported blocks = %" PRIu64
        ", avg compiled length = %" PRIu64 ".%02" PRIu64 " insn",
        jit_stats.blocks_compiled,
        jit_stats.blocks_unsupported,
        avg_compile_len / 100u,
        avg_compile_len % 100u);
    Log("jit: executed blocks = %" PRIu64
        ", JIT instructions = %" PRIu64
        ", avg executed block = %" PRIu64 ".%02" PRIu64 " insn"
        ", unsupported hits = %" PRIu64,
        jit_stats.blocks_executed,
        jit_stats.executed_insns,
        avg_exec_len / 100u,
        avg_exec_len % 100u,
        jit_stats.unsupported_hits);
    Log("jit: invalidation requests = %" PRIu64
        ", invalidated blocks = %" PRIu64
        ", arena resets = %" PRIu64,
        jit_stats.invalidation_requests,
        jit_stats.invalidated_blocks,
        jit_stats.arena_resets);
    Log("jit: native loads = %" PRIu64,
        jit_stats.native_loads);
    Log("jit: native stores = %" PRIu64,
        jit_stats.native_stores);
    Log("jit: native jumps = %" PRIu64,
        jit_stats.native_jumps);
    Log("jit: native M ops = %" PRIu64,
        jit_stats.native_m_ops);
#else
    if (jit_stats_enabled)
    {
        Log("jit: stats requested, but this binary was built without RV64_JIT_STATS=1");
    }
#endif
}
