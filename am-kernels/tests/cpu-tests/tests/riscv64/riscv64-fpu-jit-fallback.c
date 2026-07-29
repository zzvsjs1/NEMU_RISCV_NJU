#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 64

#include <stdint.h>

enum
{
    MSTATUS_FS_SHIFT = 13,
};

#define MSTATUS_FS_MASK ((uintptr_t)3u << MSTATUS_FS_SHIFT)
#define MSTATUS_FS_INITIAL ((uintptr_t)1u << MSTATUS_FS_SHIFT)

typedef uint64_t (*rv64_fpu_store_patch_fn_t)(uint32_t, uint32_t *);
typedef uint64_t (*rv64_fpu_patch_suffix_fn_t)(void);

/*
 * The raw buffer keeps the source-invalidation assertion independent of the
 * host assembler. Translation first sees an ADDI returning 1. FMV.W.X supplies
 * the replacement encoding to FSW, which rewrites that next instruction. A
 * correct FP-store boundary returns 7; stale native continuation returns 1.
 */
static uint32_t rv64_fpu_store_patch_code[4] __attribute__((aligned(16))) = {
    UINT32_C(0xf0050053), /* FMV.W.X f0, a0 */
    UINT32_C(0x0005a027), /* FSW f0, 0(a1) */
    UINT32_C(0x00100513), /* stale ADDI a0, zero, 1 */
    UINT32_C(0x00008067), /* JALR zero, 0(ra), the RET encoding */
};

/*
 * The AM build deliberately remains rv64im/lp64.  Local architecture options
 * permit explicit F/D instructions without changing the ABI or asking the C
 * compiler to allocate floating-point registers.
 *
 * The first helper starts with an FP instruction. The second performs a
 * visible integer counter update before reaching FP. Together they keep the
 * block-entry and integer-prefix boundaries visible while the JIT executes FP
 * helper sites.
 *
 * The final helper is the register-cache coherence probe. Its integer prefix
 * changes a cached GPR before FMV.D.X reads it, while FCVT.W.D writes a
 * different GPR which the immediately following integer suffix consumes.
 */
asm(".section .text\n"
    ".align 2\n"
    ".option push\n"
    ".option norvc\n"
    ".option arch, +f\n"
    ".option arch, +d\n"

    ".globl rv64_fpu_at_block_entry\n"
    ".type rv64_fpu_at_block_entry, @function\n"
    "rv64_fpu_at_block_entry:\n"
    "  fmv.d.x f0, a0\n"
    "  fadd.d f0, f0, f0, rne\n"
    "  ld t0, 0(a1)\n"
    "  addi t0, t0, 1\n"
    "  sd t0, 0(a1)\n"
    "  fmv.x.d a0, f0\n"
    "  ret\n"
    ".size rv64_fpu_at_block_entry, .-rv64_fpu_at_block_entry\n"

    ".globl rv64_fpu_after_integer_prefix\n"
    ".type rv64_fpu_after_integer_prefix, @function\n"
    "rv64_fpu_after_integer_prefix:\n"
    "  ld t0, 0(a1)\n"
    "  addi t0, t0, 1\n"
    "  sd t0, 0(a1)\n"
    "  fmv.d.x f0, a0\n"
    "  fadd.d f0, f0, f0, rne\n"
    "  fmv.x.d a0, f0\n"
    "  ret\n"
    ".size rv64_fpu_after_integer_prefix, .-rv64_fpu_after_integer_prefix\n"

    /*
     * This binary64 sequence gives permanent behavioural coverage for all
     * seven floating-point major opcode classes: LOAD-FP, STORE-FP, the four
     * fused families, and OP-FP. All arithmetic is exact for the chosen values.
     */
    ".globl rv64_fpu_all_major_opcodes\n"
    ".type rv64_fpu_all_major_opcodes, @function\n"
    "rv64_fpu_all_major_opcodes:\n"
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
    ".size rv64_fpu_all_major_opcodes, "
    ".-rv64_fpu_all_major_opcodes\n"

    ".globl rv64_fpu_gpr_cache_roundtrip\n"
    ".type rv64_fpu_gpr_cache_roundtrip, @function\n"
    "rv64_fpu_gpr_cache_roundtrip:\n"
    "  addi a0, a0, 1\n"
    "  fmv.d.x f0, a0\n"
    "  fadd.d f0, f0, f0, rne\n"
    "  fcvt.w.d a1, f0, rtz\n"
    "  addi a0, a1, 7\n"
    "  ret\n"
    ".size rv64_fpu_gpr_cache_roundtrip, "
    ".-rv64_fpu_gpr_cache_roundtrip\n"

    ".option pop\n");

extern uint64_t rv64_fpu_at_block_entry(uint64_t input,
                                         uint64_t *counter);
extern uint64_t rv64_fpu_after_integer_prefix(uint64_t input,
                                              uint64_t *counter);
extern void rv64_fpu_all_major_opcodes(const uint64_t *, uint64_t *);
extern uint64_t rv64_fpu_gpr_cache_roundtrip(uint64_t input);

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

static uintptr_t enable_initial_fp_state(void)
{
    const uintptr_t old = read_mstatus();
    const uintptr_t next =
        (old & ~MSTATUS_FS_MASK) | MSTATUS_FS_INITIAL;

    write_mstatus(next);
    return old;
}

static void check_major_opcode_results(const uint64_t *result)
{
    /*
     * For 2.0, 3.0, and 1.0 these are the hand-derived encodings of
     * 2*3+1, 2*3-1, -(2*3)+1, -(2*3)-1, and 2+1 respectively. The test
     * oracle therefore does not depend on C or host floating-point arithmetic.
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

    rv64_fpu_all_major_opcodes(input, cold_result);
    check_major_opcode_results(cold_result);

    /*
     * Calling the identical guest addresses again exercises cached translations
     * while separate result storage prevents the cold call masking missing work.
     */
    rv64_fpu_all_major_opcodes(input, warm_result);
    check_major_opcode_results(warm_result);
}

static void test_fp_store_source_invalidation_boundary(void)
{
    const rv64_fpu_store_patch_fn_t patch = (rv64_fpu_store_patch_fn_t)(uintptr_t)rv64_fpu_store_patch_code;
    const rv64_fpu_patch_suffix_fn_t stale_suffix = (rv64_fpu_patch_suffix_fn_t)(uintptr_t)&rv64_fpu_store_patch_code[2];

    /*
     * Warm the stale suffix first. FSW must invalidate that separate cached
     * translation and terminate its own block before dispatch reaches it.
     */
    check(stale_suffix() == 1);
    check(patch(UINT32_C(0x00700513), &rv64_fpu_store_patch_code[2]) == 7);
    check(rv64_fpu_store_patch_code[2] == UINT32_C(0x00700513));
}

static void test_fpu_jit_fallback_boundaries(void)
{
    const uint64_t one_point_five = UINT64_C(0x3ff8000000000000);
    const uint64_t three = UINT64_C(0x4008000000000000);
    uint64_t entry_counter = 0;
    uint64_t prefix_counter = 0;

    check(rv64_fpu_at_block_entry(one_point_five, &entry_counter) == three);
    check(entry_counter == 1);

    check(rv64_fpu_after_integer_prefix(one_point_five,
                                        &prefix_counter) == three);
    check(prefix_counter == 1);

    /*
     * Adding one to this literal produces the exact binary64 encoding of
     * +1.5. Doubling gives +3.0, conversion yields 3, and the native suffix
     * adds 7. Repeating the same call covers cold and warm JIT paths.
     */
    check(rv64_fpu_gpr_cache_roundtrip(UINT64_C(0x3ff7ffffffffffff)) == 10);
    check(rv64_fpu_gpr_cache_roundtrip(UINT64_C(0x3ff7ffffffffffff)) == 10);

    test_all_fp_major_opcodes_cold_and_warm();
    test_fp_store_source_invalidation_boundary();
}

#endif

int main(void)
{
#if defined(__riscv) && __riscv_xlen == 64
    const uintptr_t old_mstatus = enable_initial_fp_state();

    test_fpu_jit_fallback_boundaries();

    write_mstatus(old_mstatus);
#endif

    return 0;
}
