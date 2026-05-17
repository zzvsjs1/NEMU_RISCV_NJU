#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 64

#include <stdint.h>

typedef uint64_t (*generated_fn_t)(uint64_t);

static uint32_t negative_cache_code[3] __attribute__((aligned(16))) = {
    0x0000000fu, /* fence: supported by the interpreter, unsupported by RV64 JIT. */
    0x00150513u, /* addi a0, a0, 1 */
    0x00008067u, /* ret is jalr zero, 0(ra). */
};

/* Encode `jal zero, offset`; this test uses it to skip one instruction. */
static uint32_t jal_zero_offset(uint32_t offset)
{
    /*
     * JAL immediates are split as imm[20|10:1|11|19:12].  The low bit is not
     * stored because JAL targets are at least halfword-aligned.
     */
    return (((offset >> 20) & 0x1u) << 31) |
           (((offset >> 1) & 0x3ffu) << 21) |
           (((offset >> 11) & 0x1u) << 20) |
           (((offset >> 12) & 0xffu) << 12) |
           0x6fu;
}

/* Enter the generated code page through a normal RV64 function call. */
static uint64_t call_generated(uint64_t input)
{
    return ((generated_fn_t)(uintptr_t)negative_cache_code)(input);
}

/* Issue FENCE.I after rewriting code bytes, matching the architectural contract. */
static void local_fence_i(void)
{
    asm volatile("fence.i" : : : "memory");
}

/* Verify rewriting an unsupported first instruction clears the negative marker. */
static void test_negative_cache_invalidation(void)
{
    check(call_generated(10) == 11);

    negative_cache_code[0] = jal_zero_offset(8);
    local_fence_i();

    check(call_generated(10) == 10);
}

#endif

/* Keep the source buildable outside RV64 while exercising the RV64-only path. */
int main(void)
{
#if defined(__riscv) && __riscv_xlen == 64
    test_negative_cache_invalidation();
#endif

    return 0;
}
