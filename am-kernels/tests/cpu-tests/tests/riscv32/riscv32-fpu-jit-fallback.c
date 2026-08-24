#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 32

#include <stdint.h>

enum
{
    MSTATUS_FS_SHIFT = 13,
};

#define MSTATUS_FS_MASK ((uintptr_t)3u << MSTATUS_FS_SHIFT)
#define MSTATUS_FS_INITIAL ((uintptr_t)1u << MSTATUS_FS_SHIFT)

volatile uint32_t rv32_fpu_jit_trap_count = 0;

/*
 * This integer-only handler turns missing interpreter support into a bounded
 * assertion failure during the TDD RED run. A completed implementation never
 * enters it.
 */
asm(".section .text\n"
    ".align 2\n"
    ".option push\n"
    ".option norvc\n"
    ".globl rv32_fpu_jit_trap_handler\n"
    ".type rv32_fpu_jit_trap_handler, @function\n"
    "rv32_fpu_jit_trap_handler:\n"
    "  la t0, rv32_fpu_jit_trap_count\n"
    "  lw t1, 0(t0)\n"
    "  addi t1, t1, 1\n"
    "  sw t1, 0(t0)\n"
    "  csrr t1, mepc\n"
    "  addi t1, t1, 4\n"
    "  csrw mepc, t1\n"
    "  mret\n"
    ".size rv32_fpu_jit_trap_handler, .-rv32_fpu_jit_trap_handler\n"
    ".option pop\n");

extern void rv32_fpu_jit_trap_handler(void);

typedef uint32_t (*rv32_fpu_store_patch_fn_t)(uint32_t, uint32_t *);
typedef uint32_t (*rv32_fpu_patch_suffix_fn_t)(void);

/*
 * This writable instruction stream starts with an OP-FP instruction and then
 * uses STORE-FP to replace the immediately following ADDI. The JIT sees the
 * old ADDI while translating the block. Therefore, returning 7 proves that
 * FSW ended native execution and that dispatch observed the rewritten source;
 * unsafe continuation through stale translated bytes would return 1.
 */
static uint32_t rv32_fpu_store_patch_code[4] __attribute__((aligned(16))) = {
    UINT32_C(0xf0050053), /* FMV.W.X f0, a0 */
    UINT32_C(0x0005a027), /* FSW f0, 0(a1) */
    UINT32_C(0x00100513), /* stale ADDI a0, zero, 1 */
    UINT32_C(0x00008067), /* JALR zero, 0(ra), the RET encoding */
};

/*
 * The first helper starts with an FP instruction. The second updates an
 * integer counter before reaching FP. Together they keep the block-entry and
 * integer-prefix boundaries visible while the JIT executes FP helper sites.
 *
 * The final helper is the register-cache coherence probe. Its integer prefix
 * changes a cached GPR before FMV.W.X reads it, while FCVT.W.S writes a
 * different GPR which the immediately following integer suffix consumes.
 */
asm(".section .text\n"
    ".align 2\n"
    ".option push\n"
    ".option norvc\n"
    ".option arch, +f\n"

    ".globl rv32_fpu_at_block_entry\n"
    ".type rv32_fpu_at_block_entry, @function\n"
    "rv32_fpu_at_block_entry:\n"
    "  fmv.w.x f0, a0\n"
    "  fadd.s f0, f0, f0, rne\n"
    "  lw t0, 0(a1)\n"
    "  addi t0, t0, 1\n"
    "  sw t0, 0(a1)\n"
    "  fmv.x.w a0, f0\n"
    "  ret\n"
    ".size rv32_fpu_at_block_entry, .-rv32_fpu_at_block_entry\n"

    ".globl rv32_fpu_after_integer_prefix\n"
    ".type rv32_fpu_after_integer_prefix, @function\n"
    "rv32_fpu_after_integer_prefix:\n"
    "  lw t0, 0(a1)\n"
    "  addi t0, t0, 1\n"
    "  sw t0, 0(a1)\n"
    "  fmv.w.x f0, a0\n"
    "  fadd.s f0, f0, f0, rne\n"
    "  fmv.x.w a0, f0\n"
    "  ret\n"
    ".size rv32_fpu_after_integer_prefix, .-rv32_fpu_after_integer_prefix\n"

    /*
     * One checked sequence covers every F-extension major opcode class:
     * LOAD-FP, STORE-FP, all four fused families, and OP-FP. The chosen
     * operands make every result exact, so no host floating-point behaviour or
     * tolerance is involved in the expected values.
     */
    ".globl rv32_fpu_all_major_opcodes\n"
    ".type rv32_fpu_all_major_opcodes, @function\n"
    "rv32_fpu_all_major_opcodes:\n"
    "  flw f0, 0(a0)\n"
    "  flw f1, 4(a0)\n"
    "  flw f2, 8(a0)\n"
    "  fmadd.s f3, f0, f1, f2, rne\n"
    "  fmsub.s f4, f0, f1, f2, rne\n"
    "  fnmsub.s f5, f0, f1, f2, rne\n"
    "  fnmadd.s f6, f0, f1, f2, rne\n"
    "  fadd.s f7, f0, f2, rne\n"
    "  fsw f3, 0(a1)\n"
    "  fsw f4, 4(a1)\n"
    "  fsw f5, 8(a1)\n"
    "  fsw f6, 12(a1)\n"
    "  fsw f7, 16(a1)\n"
    "  ret\n"
    ".size rv32_fpu_all_major_opcodes, "
    ".-rv32_fpu_all_major_opcodes\n"

    ".globl rv32_fpu_gpr_cache_roundtrip\n"
    ".type rv32_fpu_gpr_cache_roundtrip, @function\n"
    "rv32_fpu_gpr_cache_roundtrip:\n"
    "  addi a0, a0, 1\n"
    "  fmv.w.x f0, a0\n"
    "  fcvt.w.s a1, f0, rtz\n"
    "  addi a0, a1, 7\n"
    "  ret\n"
    ".size rv32_fpu_gpr_cache_roundtrip, "
    ".-rv32_fpu_gpr_cache_roundtrip\n"

    ".option pop\n");

extern uint32_t rv32_fpu_at_block_entry(uint32_t input, uint32_t *counter);
extern uint32_t rv32_fpu_after_integer_prefix(uint32_t input, uint32_t *counter);
extern void rv32_fpu_all_major_opcodes(const uint32_t *, uint32_t *);
extern uint32_t rv32_fpu_gpr_cache_roundtrip(uint32_t input);

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

static uintptr_t enable_initial_fp_state(void)
{
    const uintptr_t old = read_mstatus();
    const uintptr_t next = (old & ~MSTATUS_FS_MASK) | MSTATUS_FS_INITIAL;

    write_mstatus(next);
    return old;
}

static void check_major_opcode_results(const uint32_t *result)
{
    /*
     * With 2.0, 3.0, and 1.0, the five operations are respectively
     * 2*3+1, 2*3-1, -(2*3)+1, -(2*3)-1, and 2+1. Their binary32 encodings
     * below are hand-derived literals rather than values calculated by the
     * implementation under test.
     */
    check(result[0] == UINT32_C(0x40e00000)); /* +7.0 */
    check(result[1] == UINT32_C(0x40a00000)); /* +5.0 */
    check(result[2] == UINT32_C(0xc0a00000)); /* -5.0 */
    check(result[3] == UINT32_C(0xc0e00000)); /* -7.0 */
    check(result[4] == UINT32_C(0x40400000)); /* +3.0 */
}

static void test_all_fp_major_opcodes_cold_and_warm(void)
{
    const uint32_t input[3] __attribute__((aligned(4))) = {
        UINT32_C(0x40000000), /* +2.0 */
        UINT32_C(0x40400000), /* +3.0 */
        UINT32_C(0x3f800000), /* +1.0 */
    };
    uint32_t cold_result[5] __attribute__((aligned(4))) = {
        UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX,
    };
    uint32_t warm_result[5] __attribute__((aligned(4))) = {
        UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX,
    };

    rv32_fpu_all_major_opcodes(input, cold_result);
    check_major_opcode_results(cold_result);

    /*
     * Re-enter exactly the same guest PCs after their translations have been
     * cached. This gives the warm helper path independent destination storage.
     */
    rv32_fpu_all_major_opcodes(input, warm_result);
    check_major_opcode_results(warm_result);
}

static void test_fp_store_source_invalidation_boundary(void)
{
    const rv32_fpu_store_patch_fn_t patch = (rv32_fpu_store_patch_fn_t)(uintptr_t)rv32_fpu_store_patch_code;
    const rv32_fpu_patch_suffix_fn_t stale_suffix = (rv32_fpu_patch_suffix_fn_t)(uintptr_t)&rv32_fpu_store_patch_code[2];

    /*
     * Cache the old suffix first. FSW must invalidate that translation as well
     * as ending its own native block before the rewritten suffix is dispatched.
     */
    check(stale_suffix() == 1);
    check(patch(UINT32_C(0x00700513), &rv32_fpu_store_patch_code[2]) == 7);
    check(rv32_fpu_store_patch_code[2] == UINT32_C(0x00700513));
}

static void test_fpu_jit_fallback_boundaries(void)
{
    const uint32_t one_point_five = UINT32_C(0x3fc00000);
    const uint32_t three = UINT32_C(0x40400000);
    uint32_t entry_counter = 0;
    uint32_t prefix_counter = 0;

    check(rv32_fpu_at_block_entry(one_point_five, &entry_counter) == three);
    check(entry_counter == 1);

    check(rv32_fpu_after_integer_prefix(one_point_five, &prefix_counter) == three);
    check(prefix_counter == 1);

    /*
     * Adding one to this literal produces the exact binary32 encoding of
     * +1.0. Conversion with round-towards-zero yields 1, then the native
     * suffix adds 7. Repeating the same call covers cold and warm JIT paths.
     */
    check(rv32_fpu_gpr_cache_roundtrip(UINT32_C(0x3f7fffff)) == 8);
    check(rv32_fpu_gpr_cache_roundtrip(UINT32_C(0x3f7fffff)) == 8);

    test_all_fp_major_opcodes_cold_and_warm();
    test_fp_store_source_invalidation_boundary();
    check(rv32_fpu_jit_trap_count == 0);
}

#endif

int main(void)
{
#if defined(__riscv) && __riscv_xlen == 32
    const uintptr_t old_mstatus = enable_initial_fp_state();
    const uintptr_t old_mtvec = read_mtvec();

    write_mtvec((uintptr_t)rv32_fpu_jit_trap_handler);
    rv32_fpu_jit_trap_count = 0;
    test_fpu_jit_fallback_boundaries();
    write_mstatus(old_mstatus);
    write_mtvec(old_mtvec);
#endif

    return 0;
}
