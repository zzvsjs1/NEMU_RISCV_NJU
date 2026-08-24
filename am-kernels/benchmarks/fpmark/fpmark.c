#include <am.h>
#include <klib.h>
#include <stdint.h>

#if __riscv_xlen != 64
#error "FPMark requires RV64"
#endif

enum
{
    FPMARK_WARMUP_ROUNDS = 1024,
    FPMARK_MOVE_SIGN_ROUNDS = 750000,
    FPMARK_CLASS_ROUNDS = 1000000,
    MSTATUS_FS_SHIFT = 13,
};

#define MSTATUS_FS_MASK ((uintptr_t)3u << MSTATUS_FS_SHIFT)
#define MSTATUS_FS_INITIAL ((uintptr_t)1u << MSTATUS_FS_SHIFT)

/*
 * Each kernel begins at its loop header. The first exercises all four raw
 * moves and all six sign-injection forms. The second isolates both FCLASS
 * widths while retaining native moves to refresh its FPR inputs. All referenced
 * GPRs fit in the seven-slot stable cache, and no timed instruction needs a C
 * helper after the exact-FP lowering is enabled.
 */
asm(".section .text\n"
    ".align 2\n"
    ".option push\n"
    ".option norvc\n"
    ".option arch, +f\n"
    ".option arch, +d\n"

    ".globl rv64_fpmark_move_sign_kernel\n"
    ".type rv64_fpmark_move_sign_kernel, @function\n"
    "rv64_fpmark_move_sign_kernel:\n"
    "  fmv.d.x f0, a0\n"
    "  fmv.d.x f1, a1\n"
    "  fsgnj.d f2, f0, f1\n"
    "  fsgnjn.d f3, f2, f0\n"
    "  fsgnjx.d f4, f3, f1\n"
    "  fmv.x.d a0, f4\n"
    "  fmv.w.x f5, a0\n"
    "  fmv.w.x f6, a1\n"
    "  fsgnj.s f7, f5, f6\n"
    "  fsgnjn.s f8, f7, f5\n"
    "  fsgnjx.s f9, f8, f6\n"
    "  fmv.x.w a3, f9\n"
    "  xor a0, a0, a3\n"
    "  addi a0, a0, 17\n"
    "  xori a1, a1, -1\n"
    "  addi a2, a2, -1\n"
    "  bne a2, zero, rv64_fpmark_move_sign_kernel\n"
    "  ret\n"
    ".size rv64_fpmark_move_sign_kernel, "
    ".-rv64_fpmark_move_sign_kernel\n"

    ".globl rv64_fpmark_class_kernel\n"
    ".type rv64_fpmark_class_kernel, @function\n"
    "rv64_fpmark_class_kernel:\n"
    "  fmv.d.x f0, a0\n"
    "  fclass.d a3, f0\n"
    "  fmv.w.x f1, a1\n"
    "  fclass.s a4, f1\n"
    "  xor a0, a0, a3\n"
    "  xor a1, a1, a4\n"
    "  xori a0, a0, -1\n"
    "  slli a1, a1, 1\n"
    "  xori a1, a1, 0x155\n"
    "  addi a2, a2, -1\n"
    "  bne a2, zero, rv64_fpmark_class_kernel\n"
    "  xor a0, a0, a1\n"
    "  ret\n"
    ".size rv64_fpmark_class_kernel, "
    ".-rv64_fpmark_class_kernel\n"

    ".option pop\n");

extern uint64_t rv64_fpmark_move_sign_kernel(uint64_t, uint64_t, uint64_t);
extern uint64_t rv64_fpmark_class_kernel(uint64_t, uint64_t, uint64_t);

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

static uint64_t rotate_left_64(uint64_t value, unsigned amount)
{
    return (value << amount) | (value >> (64u - amount));
}

static uint64_t run_move_sign_kernel(uint64_t rounds)
{
    return rv64_fpmark_move_sign_kernel(UINT64_C(0x3ff123456789abcd), UINT64_C(0xbff76543210fedcb), rounds);
}

static uint64_t run_class_kernel(uint64_t rounds)
{
    return rv64_fpmark_class_kernel(UINT64_C(0x7ff0000000000001), UINT64_C(0x000000007f800001), rounds);
}

int main(void)
{
    ioe_init();

    const uintptr_t old_mstatus = read_mstatus();
    write_mstatus((old_mstatus & ~MSTATUS_FS_MASK) | MSTATUS_FS_INITIAL);
    write_fflags(UINT64_C(0x1f));

    /*
     * Warm both guest PCs before timing. This excludes translation and direct
     * link installation so the result isolates steady-state exact-FP work.
     */
    (void)run_move_sign_kernel(FPMARK_WARMUP_ROUNDS);
    (void)run_class_kernel(FPMARK_WARMUP_ROUNDS);

    const uint64_t move_start = uptime_us();
    const uint64_t move_result = run_move_sign_kernel(FPMARK_MOVE_SIGN_ROUNDS);
    const uint64_t move_end = uptime_us();

    const uint64_t class_start = uptime_us();
    const uint64_t class_result = run_class_kernel(FPMARK_CLASS_ROUNDS);
    const uint64_t class_end = uptime_us();

    const uint64_t checksum = move_result ^ rotate_left_64(class_result, 23);
    const uint32_t checksum_hi = (uint32_t)(checksum >> 32);
    const uint32_t checksum_lo = (uint32_t)checksum;
    const bool pass = checksum_hi == UINT32_C(0xbf67a9eb) && checksum_lo == UINT32_C(0x1b4961ca) && read_fflags() == UINT64_C(0x1f);

    printf("fpmark_move_sign_us: %d\n", (int)(move_end - move_start));
    printf("fpmark_class_us: %d\n", (int)(class_end - class_start));
    printf("fpmark_total_us: %d\n", (int)((move_end - move_start) + (class_end - class_start)));
    printf("fpmark_checksum_hi: 0x%x\n", checksum_hi);
    printf("fpmark_checksum_lo: 0x%x\n", checksum_lo);
    printf("FPMark %s\n", pass ? "PASS" : "FAIL");

    write_mstatus(old_mstatus);
    return pass ? 0 : 1;
}
