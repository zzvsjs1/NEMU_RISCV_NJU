#include <am.h>
#include <klib.h>
#include <stdint.h>

#if __riscv_xlen != 64
#error "FPHelperMark requires RV64"
#endif

enum
{
    FPHELPERMARK_WARMUP_ROUNDS = 4096,
    FPHELPERMARK_TIMED_ROUNDS = 5000000,
    MSTATUS_FS_SHIFT = 13,
};

#define MSTATUS_FS_MASK ((uintptr_t)3u << MSTATUS_FS_SHIFT)
#define MSTATUS_FS_INITIAL ((uintptr_t)1u << MSTATUS_FS_SHIFT)

/*
 * Keep exactly six integer registers live around four non-memory FP helpers.
 * That fills the ordinary RV64 JIT cache (RBX, RBP and R12-R15) without using
 * the helper-free seventh slot.  Between calls, integer updates make the
 * preserved mappings immediately useful and dirty them for the next required
 * architectural-state flush.
 *
 * FCVT.D.L and FADD.D leave every integer register unchanged.  FCVT.L.D and
 * FEQ.D overwrite only t4 and t5 respectively.  The benchmark therefore
 * exercises both successful helper effect classes introduced by the selective
 * cache-preservation path.  Resetting t0 to 3 on every lap keeps all FP results
 * exact and makes the final checksum a short closed-form calculation.
 */
asm(".section .text\n"
    ".align 2\n"
    ".option push\n"
    ".option norvc\n"
    ".option arch, +f\n"
    ".option arch, +d\n"

    ".globl rv64_fphelpermark_kernel\n"
    ".type rv64_fphelpermark_kernel, @function\n"
    "rv64_fphelpermark_kernel:\n"
    "  li t0, 3\n"
    "  mv t1, a0\n"
    "  li t2, 0\n"
    "  li t3, 0\n"
    "  li t4, 0\n"
    "  li t5, 0\n"
    ".Lfphelpermark_loop:\n"
    "  addi t0, t0, 1\n"
    "  addi t1, t1, -1\n"
    "  addi t2, t2, 2\n"
    "  addi t3, t3, 3\n"
    "  addi t4, t4, 4\n"
    "  addi t5, t5, 5\n"
    "  fcvt.d.l f0, t0, rne\n"
    "  fadd.d f0, f0, f0, rne\n"
    "  fcvt.l.d t4, f0, rtz\n"
    "  feq.d t5, f0, f0\n"
    "  addi t0, t0, -1\n"
    "  add t2, t2, t4\n"
    "  add t3, t3, t5\n"
    "  bnez t1, .Lfphelpermark_loop\n"
    "  xor t2, t2, t3\n"
    "  xor t2, t2, t0\n"
    "  xor t2, t2, t4\n"
    "  xor a0, t2, t5\n"
    "  ret\n"
    ".size rv64_fphelpermark_kernel, .-rv64_fphelpermark_kernel\n"

    ".option pop\n");

extern uint64_t rv64_fphelpermark_kernel(uint64_t rounds);

static uint64_t uptime_us(void)
{
    return io_read(AM_TIMER_UPTIME).us;
}

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

static uintptr_t read_fflags(void)
{
    uintptr_t value;
    asm volatile("csrr %0, 0x001" : "=r"(value));
    return value;
}

static void write_fflags(uintptr_t value)
{
    asm volatile("csrw 0x001, %0" : : "r"(value) : "memory");
}

/*
 * Each lap adds 10 to t2 and 4 to t3.  The fixed final registers contribute
 * 3 ^ 8 ^ 1 = 10, giving an exact modulo-2^64 result without a second loop.
 */
static uint64_t reference_result(uint64_t rounds)
{
    return (UINT64_C(10) * rounds) ^ (UINT64_C(4) * rounds) ^
           UINT64_C(10);
}

int main(void)
{
    ioe_init();

    const uintptr_t old_mstatus = read_mstatus();
    write_mstatus((old_mstatus & ~MSTATUS_FS_MASK) | MSTATUS_FS_INITIAL);
    write_fflags(0);

    /* Exclude initial translation and direct-link installation from timing. */
    const uint64_t warm_result =
        rv64_fphelpermark_kernel(FPHELPERMARK_WARMUP_ROUNDS);

    const uint64_t start = uptime_us();
    const uint64_t result =
        rv64_fphelpermark_kernel(FPHELPERMARK_TIMED_ROUNDS);
    const uint64_t end = uptime_us();
    const uint64_t expected = reference_result(FPHELPERMARK_TIMED_ROUNDS);
    const uint32_t checksum_hi = (uint32_t)(result >> 32);
    const uint32_t checksum_lo = (uint32_t)result;
    const bool pass =
        warm_result == reference_result(FPHELPERMARK_WARMUP_ROUNDS) &&
        result == expected && read_fflags() == 0;

    printf("fphelpermark_total_us: %d\n", (int)(end - start));
    printf("fphelpermark_checksum_hi: 0x%x\n", checksum_hi);
    printf("fphelpermark_checksum_lo: 0x%x\n", checksum_lo);
    printf("FPHelperMark %s\n", pass ? "PASS" : "FAIL");

    write_mstatus(old_mstatus);
    return pass ? 0 : 1;
}
