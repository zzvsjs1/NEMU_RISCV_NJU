#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 64

#include <stdint.h>

typedef uint64_t (*smc_func_t)(void);

static uint32_t smc_code[2] __attribute__((aligned(16))) = {
    0x00100513u, /* addi a0, zero, 1 */
    0x00008067u, /* jalr zero, 0(ra) */
};

/* Enter the tiny generated code buffer through a normal RV64 function call. */
static uint64_t call_smc_code(void)
{
    return ((smc_func_t)(uintptr_t)smc_code)();
}

/* Issue FENCE.I after rewriting code bytes, matching the architectural contract. */
static void local_fence_i(void)
{
    asm volatile("fence.i" : : : "memory");
}

/* Verify PMEM write invalidation prevents stale translated source execution. */
static void test_self_modifying_code(void)
{
    check(call_smc_code() == 1);

    smc_code[0] = 0x00200513u; /* addi a0, zero, 2 */
    local_fence_i();

    check(call_smc_code() == 2);
}

#endif

/* Run the self-modifying-code test only when the target is RV64. */
int main(void)
{
#if defined(__riscv) && __riscv_xlen == 64
    test_self_modifying_code();
#endif

    return 0;
}
