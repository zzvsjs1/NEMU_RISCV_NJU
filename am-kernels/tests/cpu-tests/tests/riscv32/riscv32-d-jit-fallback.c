#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 32

#include <stdint.h>

volatile uint32_t rv32_d_jit_trap_count = 0;

#define MSTATUS_FS_MASK ((uintptr_t)3u << 13)
#define MSTATUS_FS_INITIAL ((uintptr_t)1u << 13)

typedef uint32_t (*rv32_d_store_patch_fn_t)(const uint32_t *, uint32_t *);
typedef uint32_t (*rv32_d_patch_suffix_fn_t)(void);

/*
 * FLD reads these two little-endian instruction words as one binary64 payload.
 * FSD then replaces both stale instructions in the writable sequence below.
 * Keeping the payload as words makes every guest instruction encoding visible
 * and avoids asking the ILP32 C ABI to carry a binary64 value in a GPR.
 */
static const uint32_t rv32_d_store_patch_payload[2] __attribute__((aligned(8))) = {
    UINT32_C(0x00700513), /* ADDI a0, zero, 7 */
    UINT32_C(0x00008067), /* JALR zero, 0(ra), the RET encoding */
};

static uint32_t rv32_d_store_patch_code[4] __attribute__((aligned(16))) = {
    UINT32_C(0x00053007), /* FLD f0, 0(a0) */
    UINT32_C(0x0005b027), /* FSD f0, 0(a1) */
    UINT32_C(0x00100513), /* stale ADDI a0, zero, 1 */
    UINT32_C(0x00008067), /* stale RET */
};

/*
 * Keep an unexpected architectural trap bounded so the probe reaches a clear
 * trap-count failure without using floating-point state in the handler.
 */
asm(".section .text\n"
    ".align 2\n"
    ".option push\n"
    ".option norvc\n"
    ".globl rv32_d_jit_trap_handler\n"
    ".type rv32_d_jit_trap_handler, @function\n"
    "rv32_d_jit_trap_handler:\n"
    "  la t0, rv32_d_jit_trap_count\n"
    "  lw t1, 0(t0)\n"
    "  addi t1, t1, 1\n"
    "  sw t1, 0(t0)\n"
    "  csrr t0, mepc\n"
    "  addi t0, t0, 4\n"
    "  csrw mepc, t0\n"
    "  mret\n"
    ".size rv32_d_jit_trap_handler, .-rv32_d_jit_trap_handler\n"
    ".option pop\n");

extern void rv32_d_jit_trap_handler(void);

/*
 * Binary64 data crosses the ILP32 boundary only through aligned memory.
 *
 * The first helper begins with FLD, while the second performs a visible
 * integer update first. Their counters make the conservative FP-memory block
 * boundaries observable. The final helper proves both directions of GPR cache
 * coherence around OP-FP helper calls.
 */
asm(".section .text\n"
    ".align 2\n"
    ".option push\n"
    ".option norvc\n"
    ".option arch, +f\n"
    ".option arch, +d\n"

    ".globl rv32_d_jit_at_block_entry\n"
    ".type rv32_d_jit_at_block_entry, @function\n"
    "rv32_d_jit_at_block_entry:\n"
    "  fld f0, 0(a0)\n"
    "  fadd.d f0, f0, f0, rne\n"
    "  fsd f0, 0(a1)\n"
    "  lw t0, 0(a2)\n"
    "  addi t0, t0, 1\n"
    "  sw t0, 0(a2)\n"
    "  ret\n"
    ".size rv32_d_jit_at_block_entry, .-rv32_d_jit_at_block_entry\n"

    ".globl rv32_d_jit_after_integer_prefix\n"
    ".type rv32_d_jit_after_integer_prefix, @function\n"
    "rv32_d_jit_after_integer_prefix:\n"
    "  lw t0, 0(a2)\n"
    "  addi t0, t0, 1\n"
    "  sw t0, 0(a2)\n"
    "  fld f0, 0(a0)\n"
    "  fadd.d f0, f0, f0, rne\n"
    "  fsd f0, 0(a1)\n"
    "  ret\n"
    ".size rv32_d_jit_after_integer_prefix, "
    ".-rv32_d_jit_after_integer_prefix\n"

    /*
     * One checked binary64 sequence covers LOAD-FP, STORE-FP, the FMADD,
     * FMSUB, FNMSUB, and FNMADD major opcodes, and OP-FP. No D value crosses
     * the soft-float ILP32 ABI boundary except through aligned memory.
     */
    ".globl rv32_d_jit_all_major_opcodes\n"
    ".type rv32_d_jit_all_major_opcodes, @function\n"
    "rv32_d_jit_all_major_opcodes:\n"
    "  fld f0, 0(a0)\n"
    "  fld f1, 8(a0)\n"
    "  fld f2, 16(a0)\n"
    "  fmadd.d f3, f0, f1, f2, rne\n"
    "  fmsub.d f4, f0, f1, f2, rne\n"
    "  fnmsub.d f5, f0, f1, f2, rne\n"
    "  fnmadd.d f6, f0, f1, f2, rne\n"
    "  fadd.d f7, f0, f2, rne\n"
    "  fsd f3, 0(a1)\n"
    "  fsd f4, 8(a1)\n"
    "  fsd f5, 16(a1)\n"
    "  fsd f6, 24(a1)\n"
    "  fsd f7, 32(a1)\n"
    "  ret\n"
    ".size rv32_d_jit_all_major_opcodes, "
    ".-rv32_d_jit_all_major_opcodes\n"

    ".globl rv32_d_jit_gpr_cache_roundtrip\n"
    ".type rv32_d_jit_gpr_cache_roundtrip, @function\n"
    "rv32_d_jit_gpr_cache_roundtrip:\n"
    "  fld f0, 0(a0)\n"
    "  addi a2, a2, 1\n"
    "  fcvt.d.w f1, a2\n"
    "  fadd.d f0, f0, f1, rne\n"
    "  fcvt.w.d a0, f0, rtz\n"
    "  addi a0, a0, 7\n"
    "  fsd f0, 0(a1)\n"
    "  ret\n"
    ".size rv32_d_jit_gpr_cache_roundtrip, "
    ".-rv32_d_jit_gpr_cache_roundtrip\n"

    ".option pop\n");

extern void rv32_d_jit_at_block_entry(const uint64_t *, uint64_t *, uint32_t *);
extern void rv32_d_jit_after_integer_prefix(const uint64_t *, uint64_t *, uint32_t *);
extern void rv32_d_jit_all_major_opcodes(const uint64_t *, uint64_t *);
extern uint32_t rv32_d_jit_gpr_cache_roundtrip(const uint64_t *, uint64_t *, uint32_t);

static uintptr_t read_mstatus(void)
{
    uintptr_t value;
    asm volatile("csrr %0, mstatus" : "=r"(value));
    return value;
}

static void write_mstatus(uintptr_t value)
{
    asm volatile("csrw mstatus, %0" : : "r"(value) : "memory");
}

static uintptr_t read_mtvec(void)
{
    uintptr_t value;
    asm volatile("csrr %0, mtvec" : "=r"(value));
    return value;
}

static void write_mtvec(uintptr_t value)
{
    asm volatile("csrw mtvec, %0" : : "r"(value) : "memory");
}

static void check_major_opcode_results(const uint64_t *result)
{
    /*
     * The operands 2.0, 3.0, and 1.0 produce exact binary64 results:
     * 2*3+1, 2*3-1, -(2*3)+1, -(2*3)-1, and 2+1. Literal encodings keep the
     * oracle independent of the floating-point implementation under test.
     */
    check(result[0] == UINT64_C(0x401c000000000000)); /* +7.0 */
    check(result[1] == UINT64_C(0x4014000000000000)); /* +5.0 */
    check(result[2] == UINT64_C(0xc014000000000000)); /* -5.0 */
    check(result[3] == UINT64_C(0xc01c000000000000)); /* -7.0 */
    check(result[4] == UINT64_C(0x4008000000000000)); /* +3.0 */
}

static void test_all_fp_major_opcodes_cold_and_warm(void)
{
    const uint64_t input[3] __attribute__((aligned(8))) = {
        UINT64_C(0x4000000000000000), /* +2.0 */
        UINT64_C(0x4008000000000000), /* +3.0 */
        UINT64_C(0x3ff0000000000000), /* +1.0 */
    };
    uint64_t cold_result[5] __attribute__((aligned(8))) = {
        UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX,
    };
    uint64_t warm_result[5] __attribute__((aligned(8))) = {
        UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX,
    };

    rv32_d_jit_all_major_opcodes(input, cold_result);
    check_major_opcode_results(cold_result);

    /*
     * The second call enters the same translated guest PCs with fresh result
     * storage, which exercises warm LOAD-FP, arithmetic, and STORE-FP sites.
     */
    rv32_d_jit_all_major_opcodes(input, warm_result);
    check_major_opcode_results(warm_result);
}

static void test_fp_store_source_invalidation_boundary(void)
{
    const rv32_d_store_patch_fn_t patch = (rv32_d_store_patch_fn_t)(uintptr_t)rv32_d_store_patch_code;
    const rv32_d_patch_suffix_fn_t stale_suffix = (rv32_d_patch_suffix_fn_t)(uintptr_t)&rv32_d_store_patch_code[2];

    /*
     * Cache the suffix returning 1 before FSD overwrites both of its words.
     * Returning 7 proves the store invalidated that cached source and ended its
     * own native block; either stale path would return 1.
     */
    check(stale_suffix() == 1);
    check(patch(rv32_d_store_patch_payload, &rv32_d_store_patch_code[2]) == 7);
    check(rv32_d_store_patch_code[2] == UINT32_C(0x00700513));
    check(rv32_d_store_patch_code[3] == UINT32_C(0x00008067));
}

static void test_d_jit_fallback_boundaries(void)
{
    uint64_t input __attribute__((aligned(8))) = UINT64_C(0x3ff8000000000000); /* +1.5 */
    uint64_t entry_result __attribute__((aligned(8))) = UINT64_C(0xfeedfacecafebeef);
    uint64_t prefix_result __attribute__((aligned(8))) = UINT64_C(0xfeedfacecafebeef);
    uint64_t cold_roundtrip_result __attribute__((aligned(8))) = UINT64_C(0xfeedfacecafebeef);
    uint64_t warm_roundtrip_result __attribute__((aligned(8))) = UINT64_C(0xfeedfacecafebeef);
    uint32_t entry_counter = 0;
    uint32_t prefix_counter = 0;

    rv32_d_jit_at_block_entry(&input, &entry_result, &entry_counter);
    rv32_d_jit_after_integer_prefix(&input, &prefix_result, &prefix_counter);

    /* 1.5 + 1.5 = 3.0 exactly in binary64. */
    check(entry_result == UINT64_C(0x4008000000000000));
    check(prefix_result == UINT64_C(0x4008000000000000));
    check(entry_counter == 1);
    check(prefix_counter == 1);

    /*
     * The integer prefix turns the third argument from 0 into 1 before
     * FCVT.D.W reads it, proving dirty cached GPR state reaches the helper.
     * Adding that 1.0 to 1.5 gives exactly 2.5. FCVT.W.D writes 2 to a0, and
     * the native suffix adds 7. Separate destinations cover cold and warm FSD.
     */
    check(rv32_d_jit_gpr_cache_roundtrip(&input, &cold_roundtrip_result, 0) == 9);
    check(cold_roundtrip_result == UINT64_C(0x4004000000000000));
    check(rv32_d_jit_gpr_cache_roundtrip(&input, &warm_roundtrip_result, 0) == 9);
    check(warm_roundtrip_result == UINT64_C(0x4004000000000000));

    test_all_fp_major_opcodes_cold_and_warm();
    test_fp_store_source_invalidation_boundary();
    check(rv32_d_jit_trap_count == 0);
}

#endif

int main(void)
{
#if defined(__riscv) && __riscv_xlen == 32
    const uintptr_t old_mstatus = read_mstatus();
    const uintptr_t old_mtvec = read_mtvec();

    rv32_d_jit_trap_count = 0;
    write_mtvec((uintptr_t)rv32_d_jit_trap_handler);
    write_mstatus((old_mstatus & ~MSTATUS_FS_MASK) | MSTATUS_FS_INITIAL);

    test_d_jit_fallback_boundaries();

    write_mstatus(old_mstatus);
    write_mtvec(old_mtvec);
#endif

    return 0;
}
