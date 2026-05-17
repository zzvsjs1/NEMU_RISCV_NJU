#include <isa-jit.h>
#include <isa.h>
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

#define RV64_JIT_BLOCK_MAX_INSNS 32u
#define RV64_JIT_BATCH_MAX_INSNS 65536u
#define RV64_JIT_CACHE_SIZE 65536u
#define RV64_JIT_CODE_SIZE (64u * 1024u * 1024u)
#define RV64_JIT_CODE_ALIGN 16u
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
    return (value >> lo) & ((1u << (hi - lo + 1)) - 1u);
}

/* Sign-extend an instruction field whose sign bit is at width - 1. */
static int64_t sext(uint32_t value, unsigned width)
{
    const uint32_t shift = 32u - width;
    return (int64_t)((int32_t)(value << shift) >> shift);
}

/* Decode the I-format immediate as a signed XLEN value. */
static int64_t imm_i(uint32_t instr)
{
    return sext(bits(instr, 31, 20), 12);
}

/* Decode the B-format immediate, including the implicit low zero bit. */
static int64_t imm_b(uint32_t instr)
{
    uint32_t imm = (bits(instr, 11, 8) << 1) |
                   (bits(instr, 30, 25) << 5) |
                   (bits(instr, 7, 7) << 11) |
                   (bits(instr, 31, 31) << 12);
    return sext(imm, 13);
}

/* Decode and sign-extend a U-format immediate for RV64 LUI/AUIPC. */
static int64_t imm_u_sext(uint32_t instr)
{
    return (int64_t)(int32_t)(instr & 0xfffff000u);
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
    for (int i = 0; i < 4; i++)
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
    for (int i = 0; i < 8; i++)
    {
        if (!emit_u8(w, (uint8_t)(value >> (i * 8))))
        {
            return false;
        }
    }

    return true;
}

/* Emit `sub rsp, 8`, aligning the stack before any helper call. */
static bool emit_prologue(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x83) &&
           emit_u8(w, 0xec) && emit_u8(w, 0x08) &&
           emit_u8(w, 0x49) && emit_u8(w, 0xbb) &&
           emit_u64(w, (uint64_t)(uintptr_t)&cpu);
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

/* Emit `movabs rax, imm64`, used for full-width constants and helper targets. */
static bool emit_movabs_rax(rv64_jit_writer_t *w, uint64_t value)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0xb8) && emit_u64(w, value);
}

/* Emit a 32-bit zeroing idiom for RAX. */
static bool emit_zero_rax(rv64_jit_writer_t *w)
{
    return emit_u8(w, 0x31) && emit_u8(w, 0xc0);
}

/* Load one guest register into RAX, treating x0 as the constant zero. */
static bool emit_load_gpr_rax(rv64_jit_writer_t *w, uint32_t reg)
{
    if (reg == 0)
    {
        return emit_zero_rax(w);
    }

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

    return emit_u8(w, 0x49) && emit_u8(w, 0x8b) &&
           emit_u8(w, 0x8b) && emit_u32(w, jit_gpr_offset(reg));
}

/* Store RAX into a guest register, ignoring writes to x0. */
static bool emit_store_rax_gpr(rv64_jit_writer_t *w, uint32_t reg)
{
    if (reg == 0)
    {
        return true;
    }

    return emit_u8(w, 0x49) && emit_u8(w, 0x89) &&
           emit_u8(w, 0x83) && emit_u32(w, jit_gpr_offset(reg));
}

/* Store an immediate guest PC by materialising it in RAX first. */
static bool emit_store_pc_imm(rv64_jit_writer_t *w, vaddr_t pc)
{
    return emit_movabs_rax(w, pc) &&
           emit_u8(w, 0x49) && emit_u8(w, 0x89) &&
           emit_u8(w, 0x83) && emit_u32(w, jit_pc_offset());
}

/* Emit `add rax, imm32`, whose immediate is sign-extended by x86-64. */
static bool emit_add_rax_imm32(rv64_jit_writer_t *w, int32_t imm)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0x05) && emit_u32(w, (uint32_t)imm);
}

/* Emit one RAX op RCX 64-bit ALU instruction selected by the opcode byte. */
static bool emit_rax_rcx_alu64(rv64_jit_writer_t *w, uint8_t opcode)
{
    return emit_u8(w, 0x48) && emit_u8(w, opcode) && emit_u8(w, 0xc8);
}

/* Emit one EAX op ECX 32-bit ALU instruction, then sign-extend to 64 bits. */
static bool emit_eax_ecx_alu32_sext(rv64_jit_writer_t *w, uint8_t opcode)
{
    return emit_u8(w, opcode) && emit_u8(w, 0xc8) &&
           emit_u8(w, 0x48) && emit_u8(w, 0x98);
}

/* Emit a 64-bit immediate shift of RAX. */
static bool emit_shift_rax_imm(rv64_jit_writer_t *w, uint8_t subop, uint8_t shamt)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0xc1) && emit_u8(w, subop) && emit_u8(w, shamt);
}

/* Emit a 32-bit immediate shift of EAX, then sign-extend to 64 bits. */
static bool emit_shift_eax_imm_sext(rv64_jit_writer_t *w, uint8_t subop, uint8_t shamt)
{
    return emit_u8(w, 0xc1) && emit_u8(w, subop) && emit_u8(w, shamt) &&
           emit_u8(w, 0x48) && emit_u8(w, 0x98);
}

/* Emit a 64-bit variable shift of RAX by CL. */
static bool emit_shift_rax_cl(rv64_jit_writer_t *w, uint8_t subop)
{
    return emit_u8(w, 0x48) && emit_u8(w, 0xd3) && emit_u8(w, subop);
}

/* Emit a 32-bit variable shift of EAX by CL, then sign-extend to 64 bits. */
static bool emit_shift_eax_cl_sext(rv64_jit_writer_t *w, uint8_t subop)
{
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
    return emit_u8(w, 0x0f) && emit_u8(w, setcc_opcode) &&
           emit_u8(w, 0xc0) &&
           emit_u8(w, 0x0f) && emit_u8(w, 0xb6) && emit_u8(w, 0xc0);
}

/* Emit a conditional branch with a rel32 placeholder and return its patch site. */
static bool emit_jcc_rel32_placeholder(rv64_jit_writer_t *w, uint8_t jcc_opcode,
                                       uint8_t **disp)
{
    if (!emit_u8(w, 0x0f) || !emit_u8(w, jcc_opcode))
    {
        return false;
    }

    *disp = w->cur;
    return emit_u32(w, 0);
}

/* Patch a rel32 displacement emitted by a previous branch helper. */
static void patch_rel32(uint8_t *disp, const uint8_t *target)
{
    int64_t rel = target - (disp + 4);
    Assert(rel >= INT32_MIN && rel <= INT32_MAX, "jit: rel32 target is out of range");
    int32_t rel32 = (int32_t)rel;
    memcpy(disp, &rel32, sizeof(rel32));
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
    case 0x0:
        return emit_add_rax_imm32(w, imm) && emit_store_rax_gpr(w, rd);
    case 0x2:
        return emit_cmp_rax_imm32(w, imm) && emit_setcc_rax(w, 0x9c) && emit_store_rax_gpr(w, rd);
    case 0x3:
        return emit_cmp_rax_imm32(w, imm) && emit_setcc_rax(w, 0x92) && emit_store_rax_gpr(w, rd);
    case 0x4:
        return emit_u8(w, 0x48) && emit_u8(w, 0x35) && emit_u32(w, (uint32_t)imm) && emit_store_rax_gpr(w, rd);
    case 0x6:
        return emit_u8(w, 0x48) && emit_u8(w, 0x0d) && emit_u32(w, (uint32_t)imm) && emit_store_rax_gpr(w, rd);
    case 0x7:
        return emit_u8(w, 0x48) && emit_u8(w, 0x25) && emit_u32(w, (uint32_t)imm) && emit_store_rax_gpr(w, rd);
    case 0x1:
        if (bits(instr, 31, 26) != 0x00)
        {
            return false;
        }
        return emit_shift_rax_imm(w, 0xe0, (uint8_t)bits(instr, 25, 20)) && emit_store_rax_gpr(w, rd);
    case 0x5:
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
    case 0x0:
        return emit_u8(w, 0x05) && emit_u32(w, (uint32_t)imm) &&
               emit_u8(w, 0x48) && emit_u8(w, 0x98) &&
               emit_store_rax_gpr(w, rd);
    case 0x1:
        if (bits(instr, 31, 25) != 0x00)
        {
            return false;
        }
        return emit_shift_eax_imm_sext(w, 0xe0, (uint8_t)bits(instr, 24, 20)) && emit_store_rax_gpr(w, rd);
    case 0x5:
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
    case 0x000:
        return emit_rax_rcx_alu64(w, 0x01) && emit_store_rax_gpr(w, rd);
    case 0x100:
        return emit_rax_rcx_alu64(w, 0x29) && emit_store_rax_gpr(w, rd);
    case 0x001:
        return emit_shift_rax_cl(w, 0xe0) && emit_store_rax_gpr(w, rd);
    case 0x002:
        return emit_cmp_rax_rcx(w) && emit_setcc_rax(w, 0x9c) && emit_store_rax_gpr(w, rd);
    case 0x003:
        return emit_cmp_rax_rcx(w) && emit_setcc_rax(w, 0x92) && emit_store_rax_gpr(w, rd);
    case 0x004:
        return emit_rax_rcx_alu64(w, 0x31) && emit_store_rax_gpr(w, rd);
    case 0x005:
        return emit_shift_rax_cl(w, 0xe8) && emit_store_rax_gpr(w, rd);
    case 0x105:
        return emit_shift_rax_cl(w, 0xf8) && emit_store_rax_gpr(w, rd);
    case 0x006:
        return emit_rax_rcx_alu64(w, 0x09) && emit_store_rax_gpr(w, rd);
    case 0x007:
        return emit_rax_rcx_alu64(w, 0x21) && emit_store_rax_gpr(w, rd);
    case 0x008:
        return emit_u8(w, 0x48) && emit_u8(w, 0x0f) && emit_u8(w, 0xaf) && emit_u8(w, 0xc1) &&
               emit_store_rax_gpr(w, rd);
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
    case 0x000:
        return emit_eax_ecx_alu32_sext(w, 0x01) && emit_store_rax_gpr(w, rd);
    case 0x100:
        return emit_eax_ecx_alu32_sext(w, 0x29) && emit_store_rax_gpr(w, rd);
    case 0x001:
        return emit_shift_eax_cl_sext(w, 0xe0) && emit_store_rax_gpr(w, rd);
    case 0x005:
        return emit_shift_eax_cl_sext(w, 0xe8) && emit_store_rax_gpr(w, rd);
    case 0x105:
        return emit_shift_eax_cl_sext(w, 0xf8) && emit_store_rax_gpr(w, rd);
    case 0x008:
        return emit_u8(w, 0x0f) && emit_u8(w, 0xaf) && emit_u8(w, 0xc1) &&
               emit_u8(w, 0x48) && emit_u8(w, 0x98) &&
               emit_store_rax_gpr(w, rd);
    default:
        return false;
    }
}

/* Emit one conditional branch with a taken side exit and fall-through fast path. */
static bool emit_branch(rv64_jit_writer_t *w, uint32_t instr, vaddr_t pc,
                        uint32_t exit_count)
{
    const uint32_t funct3 = bits(instr, 14, 12);
    const uint32_t rs1 = bits(instr, 19, 15);
    const uint32_t rs2 = bits(instr, 24, 20);
    const vaddr_t target = pc + imm_b(instr);
    uint8_t inverse_jcc = 0;
    uint8_t *fallthrough_disp = NULL;

    if ((target & 0x3u) != 0)
    {
        return false;
    }

    switch (funct3)
    {
    case 0x0:
        inverse_jcc = 0x85;
        break;
    case 0x1:
        inverse_jcc = 0x84;
        break;
    case 0x4:
        inverse_jcc = 0x8d;
        break;
    case 0x5:
        inverse_jcc = 0x8c;
        break;
    case 0x6:
        inverse_jcc = 0x83;
        break;
    case 0x7:
        inverse_jcc = 0x82;
        break;
    default:
        return false;
    }

    if (!emit_load_gpr_rax(w, rs1) ||
        !emit_load_gpr_rcx(w, rs2) ||
        !emit_cmp_rax_rcx(w) ||
        !emit_jcc_rel32_placeholder(w, inverse_jcc, &fallthrough_disp) ||
        !emit_store_pc_imm(w, target) ||
        !emit_return_count(w, exit_count))
    {
        return false;
    }

    patch_rel32(fallthrough_disp, w->cur);
    return true;
}

/* Dispatch one supported RISC-V instruction to the native emitter. */
static bool emit_instr(rv64_jit_writer_t *w, uint32_t instr, vaddr_t pc,
                       uint32_t exit_count)
{
    const uint32_t opcode = instr & 0x7fu;
    const uint32_t rd = bits(instr, 11, 7);

    switch (opcode)
    {
    case 0x13:
        return emit_op_imm(w, instr);
    case 0x1b:
        return emit_op_imm32(w, instr);
    case 0x33:
        return emit_op(w, instr);
    case 0x3b:
        return emit_op32(w, instr);
    case 0x37:
        return emit_movabs_rax(w, (uint64_t)imm_u_sext(instr)) &&
               emit_store_rax_gpr(w, rd);
    case 0x17:
        return emit_movabs_rax(w, (uint64_t)(pc + imm_u_sext(instr))) &&
               emit_store_rax_gpr(w, rd);
    case 0x63:
        return emit_branch(w, instr, pc, exit_count);
    default:
        return false;
    }
}

/* Translate an instruction-fetch virtual PC to its physical source address. */
static bool jit_translate_ifetch(vaddr_t pc, paddr_t *paddr)
{
    const int mmu = isa_mmu_check(pc, 4, MEM_TYPE_IFETCH);

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
        .source_len = 4,
        .insn_count = 0,
        .entry = NULL,
    };
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

    vaddr_t cur_pc = pc;
    uint32_t count = 0;
    uint32_t source_len = 0;

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

        const uint32_t instr = (uint32_t)vaddr_ifetch(cur_pc, 4);
        uint8_t *instr_start = w.cur;

        if (!emit_instr(&w, instr, cur_pc, count + 1u))
        {
            w.cur = instr_start;
            break;
        }

        cur_pc += 4;
        source_len += 4;
        count++;

        if ((instr & 0x7fu) == 0x63)
        {
            continue;
        }
    }

    if (count == 0)
    {
        jit_mark_unsupported(pc, first_paddr);
        return NULL;
    }

    if (!emit_store_pc_imm(&w, cur_pc) || !emit_return_count(&w, count))
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
#else
    if (jit_stats_enabled)
    {
        Log("jit: stats requested, but this binary was built without RV64_JIT_STATS=1");
    }
#endif
}
