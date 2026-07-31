#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 64

#include <stdint.h>

typedef uint64_t (*smc_func_t)(void);

static uint32_t smc_code[2] __attribute__((aligned(16))) = {
    0x00100513u, /* addi a0, zero, 1 */
    0x00008067u, /* jalr zero, 0(ra) */
};

/* Keep the wrapper's transfer indirect so one unchanged JALR owns the PIC. */
static smc_func_t volatile smc_target =
    (smc_func_t)(uintptr_t)smc_code;

/*
 * Tail-jump through one unchanged, non-return-hinted JALR source. Keeping the
 * wrapper in global assembly guarantees there is no compiler-generated frame
 * to unwind after the target returns directly to this wrapper's caller. Bit
 * zero is set deliberately so the same path also retains JALR's architectural
 * target masking coverage.
 */
extern uint64_t call_smc_jump_cache(smc_func_t target);

asm(
    ".option push\n"
    ".option norvc\n"
    ".balign 4\n"
    ".globl call_smc_jump_cache\n"
    "call_smc_jump_cache:\n"
    "  ori t2, a0, 1\n"
    "  jalr zero, 0(t2)\n"
    ".option pop\n");

/*
 * Keep the source JAL physically separate from its target instruction. The
 * target rewrite must invalidate and unpatch only the destination block; the
 * unchanged source block remains cached and would otherwise retain a stale
 * native target address.
 */
static uint32_t linked_smc_code[16] __attribute__((aligned(16))) = {
    [0] = 0x0200006fu, /* jal zero, +32 */
    [8] = 0x00100513u, /* addi a0, zero, 1 */
    [9] = 0x00008067u, /* jalr zero, 0(ra) */
};

/* Enter the tiny generated code buffer through a normal RV64 function call. */
__attribute__((noinline)) static uint64_t call_smc_code(void)
{
    uint64_t result = smc_target();

    /* Keep work after the call so this lifecycle edge writes ra, not x0. */
    asm volatile("" : "+r"(result) : : "memory");
    return result;
}

/*
 * Republish a rewritten target through a physically different indirect exit.
 * The empty inline assembly keeps the loaded function pointer in a register and
 * prevents the compiler from folding this wrapper into call_smc_code().
 */
__attribute__((noinline)) static uint64_t republish_smc_code(void)
{
    smc_func_t target = smc_target;
    uint64_t result;

    asm volatile("" : "+r"(target) : : "memory");
    result = target();
    asm volatile("" : "+r"(result) : : "memory");
    return result;
}

/* Enter the source JAL whose destination is rewritten by the focused test. */
__attribute__((noinline)) static uint64_t call_linked_smc_code(void)
{
    uint64_t result = ((smc_func_t)(uintptr_t)linked_smc_code)();

    asm volatile("" : "+r"(result) : : "memory");
    return result;
}

/* Issue FENCE.I after rewriting code bytes, matching the architectural contract. */
static void local_fence_i(void)
{
    asm volatile("fence.i" : : : "memory");
}

/* Verify PMEM write invalidation prevents stale translated source execution. */
static void test_self_modifying_code(void)
{
    /*
     * Re-enter the unchanged indirect source enough times to populate both the
     * authoritative block cache and its future per-exit PIC before invalidating
     * only the target bytes.
     */
    for (uint32_t i = 0; i < 8u; i++)
    {
        check(call_smc_code() == 1);
        check(call_smc_jump_cache(smc_target) == 1);
    }

    smc_code[0] = 0x00200513u; /* addi a0, zero, 2 */
    local_fence_i();

    /*
     * The cold wrapper first installs the rewritten target's new block
     * generation. The original warm PIC must then reject its old generation;
     * merely testing the slot's invalid bit is no longer sufficient to pass.
     */
    check(republish_smc_code() == 2);
    check(call_smc_code() == 2);
    /* The first call refills; this one must execute the refreshed patch. */
    check(call_smc_code() == 2);

    /*
     * The unchanged x0 source retains its data-only entry across the rewrite.
     * It must reject the old generation, refill from the republished slot, and
     * then hit the refreshed entry without ever owning a native reverse link.
     */
    check(call_smc_jump_cache(smc_target) == 2);
    check(call_smc_jump_cache(smc_target) == 2);
}

/* Verify target invalidation disconnects an already-warm incoming native edge. */
static void test_linked_self_modifying_code(void)
{
    for (uint32_t i = 0; i < 8u; i++)
    {
        check(call_linked_smc_code() == 1);
    }

    linked_smc_code[8] = 0x00200513u; /* addi a0, zero, 2 */
    local_fence_i();

    check(call_linked_smc_code() == 2);
}

#endif

/* Run the self-modifying-code test only when the target is RV64. */
int main(void)
{
#if defined(__riscv) && __riscv_xlen == 64
    test_self_modifying_code();
    test_linked_self_modifying_code();
#endif

    return 0;
}
