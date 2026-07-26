#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 64

#include <stdint.h>

volatile uint64_t rv64_fpu_trap_count = 0;
volatile uint64_t rv64_fpu_trap_mcause = UINT64_MAX;
volatile uint64_t rv64_fpu_trap_mepc = UINT64_MAX;
volatile uint64_t rv64_fpu_trap_mtval = UINT64_MAX;

#define MSTATUS_FS_MASK ((uintptr_t)3u << 13)
#define MSTATUS_FS_OFF ((uintptr_t)0u << 13)
#define MSTATUS_FS_INITIAL ((uintptr_t)1u << 13)

asm(
    ".section .text\n"
    ".align 2\n"
    ".option push\n"
    ".option norvc\n"
    ".globl rv64_fpu_trap_handler\n"
    ".type rv64_fpu_trap_handler, @function\n"
    "rv64_fpu_trap_handler:\n"
    "  la t0, rv64_fpu_trap_count\n"
    "  ld t1, 0(t0)\n"
    "  addi t1, t1, 1\n"
    "  sd t1, 0(t0)\n"
    "  csrr t1, mcause\n"
    "  la t0, rv64_fpu_trap_mcause\n"
    "  sd t1, 0(t0)\n"
    "  csrr t1, mepc\n"
    "  la t0, rv64_fpu_trap_mepc\n"
    "  sd t1, 0(t0)\n"
    "  csrr t1, mtval\n"
    "  la t0, rv64_fpu_trap_mtval\n"
    "  sd t1, 0(t0)\n"
    "  csrr t1, mepc\n"
    "  addi t1, t1, 4\n"
    "  csrw mepc, t1\n"
    "  mret\n"
    ".size rv64_fpu_trap_handler, .-rv64_fpu_trap_handler\n"
    ".option pop\n");

extern void rv64_fpu_trap_handler(void);

asm(
    ".section .text\n"
    ".align 2\n"
    ".option push\n"
    ".option norvc\n"
    ".option arch, +f\n"
    ".option arch, +d\n"

    ".globl rv64_fpu_fs_off_probe\n"
    ".type rv64_fpu_fs_off_probe, @function\n"
    "rv64_fpu_fs_off_probe:\n"
    ".globl rv64_fpu_fs_off_insn\n"
    "rv64_fpu_fs_off_insn:\n"
    "  .word 0xf0050053\n" /* FMV.W.X f0, a0 */
    "  ret\n"
    ".size rv64_fpu_fs_off_probe, .-rv64_fpu_fs_off_probe\n"

    ".globl rv64_fpu_bad_static_rm\n"
    ".type rv64_fpu_bad_static_rm, @function\n"
    "rv64_fpu_bad_static_rm:\n"
    "  fmv.w.x f0, a0\n"
    "  fmv.w.x f1, a1\n"
    "  fmv.w.x f2, a2\n"
    ".globl rv64_fpu_bad_static_rm_insn\n"
    "rv64_fpu_bad_static_rm_insn:\n"
    "  .word 0x0020d053\n" /* FADD.S f0, f1, f2 with reserved rm=101 */
    "  fmv.x.d a0, f0\n"
    "  ret\n"
    ".size rv64_fpu_bad_static_rm, .-rv64_fpu_bad_static_rm\n"

    ".globl rv64_fpu_bad_dynamic_rm\n"
    ".type rv64_fpu_bad_dynamic_rm, @function\n"
    "rv64_fpu_bad_dynamic_rm:\n"
    "  fmv.w.x f0, a0\n"
    "  fmv.w.x f1, a1\n"
    "  fmv.w.x f2, a2\n"
    ".globl rv64_fpu_bad_dynamic_rm_insn\n"
    "rv64_fpu_bad_dynamic_rm_insn:\n"
    "  .word 0x0020f053\n" /* FADD.S f0, f1, f2 with dynamic rm */
    "  fmv.x.d a0, f0\n"
    "  ret\n"
    ".size rv64_fpu_bad_dynamic_rm, .-rv64_fpu_bad_dynamic_rm\n"

    ".globl rv64_fpu_misaligned_flw\n"
    ".type rv64_fpu_misaligned_flw, @function\n"
    "rv64_fpu_misaligned_flw:\n"
    "  fmv.d.x f0, a1\n"
    ".globl rv64_fpu_misaligned_flw_insn\n"
    "rv64_fpu_misaligned_flw_insn:\n"
    "  flw f0, 0(a0)\n"
    "  fmv.x.d a0, f0\n"
    "  ret\n"
    ".size rv64_fpu_misaligned_flw, .-rv64_fpu_misaligned_flw\n"

    ".globl rv64_fpu_misaligned_fld\n"
    ".type rv64_fpu_misaligned_fld, @function\n"
    "rv64_fpu_misaligned_fld:\n"
    "  fmv.d.x f0, a1\n"
    ".globl rv64_fpu_misaligned_fld_insn\n"
    "rv64_fpu_misaligned_fld_insn:\n"
    "  fld f0, 0(a0)\n"
    "  fmv.x.d a0, f0\n"
    "  ret\n"
    ".size rv64_fpu_misaligned_fld, .-rv64_fpu_misaligned_fld\n"

    ".globl rv64_fpu_misaligned_fsw\n"
    ".type rv64_fpu_misaligned_fsw, @function\n"
    "rv64_fpu_misaligned_fsw:\n"
    "  fmv.w.x f0, a1\n"
    ".globl rv64_fpu_misaligned_fsw_insn\n"
    "rv64_fpu_misaligned_fsw_insn:\n"
    "  fsw f0, 0(a0)\n"
    "  ret\n"
    ".size rv64_fpu_misaligned_fsw, .-rv64_fpu_misaligned_fsw\n"

    ".globl rv64_fpu_misaligned_fsd\n"
    ".type rv64_fpu_misaligned_fsd, @function\n"
    "rv64_fpu_misaligned_fsd:\n"
    "  fmv.d.x f0, a1\n"
    ".globl rv64_fpu_misaligned_fsd_insn\n"
    "rv64_fpu_misaligned_fsd_insn:\n"
    "  fsd f0, 0(a0)\n"
    "  ret\n"
    ".size rv64_fpu_misaligned_fsd, .-rv64_fpu_misaligned_fsd\n"

    ".option pop\n");

extern void rv64_fpu_fs_off_probe(uint32_t);
extern char rv64_fpu_fs_off_insn[];
extern uint64_t rv64_fpu_bad_static_rm(uint32_t, uint32_t, uint32_t);
extern char rv64_fpu_bad_static_rm_insn[];
extern uint64_t rv64_fpu_bad_dynamic_rm(uint32_t, uint32_t, uint32_t);
extern char rv64_fpu_bad_dynamic_rm_insn[];
extern uint64_t rv64_fpu_misaligned_flw(const void *, uint64_t);
extern char rv64_fpu_misaligned_flw_insn[];
extern uint64_t rv64_fpu_misaligned_fld(const void *, uint64_t);
extern char rv64_fpu_misaligned_fld_insn[];
extern void rv64_fpu_misaligned_fsw(void *, uint32_t);
extern char rv64_fpu_misaligned_fsw_insn[];
extern void rv64_fpu_misaligned_fsd(void *, uint64_t);
extern char rv64_fpu_misaligned_fsd_insn[];

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

static void write_frm(uintptr_t value)
{
    asm volatile("csrw 0x002, %0" : : "r"(value) : "memory");
}

static void reset_trap(void)
{
    rv64_fpu_trap_count = 0;
    rv64_fpu_trap_mcause = UINT64_MAX;
    rv64_fpu_trap_mepc = UINT64_MAX;
    rv64_fpu_trap_mtval = UINT64_MAX;
}

static void check_trap(uint64_t cause, const char *instruction,
                       uintptr_t expected_tval)
{
    check(rv64_fpu_trap_count == 1);
    check(rv64_fpu_trap_mcause == cause);
    check(rv64_fpu_trap_mepc == (uintptr_t)instruction);
    check(rv64_fpu_trap_mtval == expected_tval);
}

static void test_fs_off_and_reserved_rounding(uintptr_t base_mstatus)
{
    const uint64_t boxed_sentinel = UINT64_C(0xffffffff3f000000);

    reset_trap();
    write_mstatus((base_mstatus & ~MSTATUS_FS_MASK) | MSTATUS_FS_OFF);
    rv64_fpu_fs_off_probe(UINT32_C(0x3f800000));
    check_trap(2, rv64_fpu_fs_off_insn, 0);

    write_mstatus((base_mstatus & ~MSTATUS_FS_MASK) | MSTATUS_FS_INITIAL);
    reset_trap();
    check(rv64_fpu_bad_static_rm(UINT32_C(0x3f000000),
                                UINT32_C(0x3f800000),
                                UINT32_C(0x40000000)) ==
          boxed_sentinel);
    check_trap(2, rv64_fpu_bad_static_rm_insn, 0);

    write_frm(5);
    reset_trap();
    check(rv64_fpu_bad_dynamic_rm(UINT32_C(0x3f000000),
                                 UINT32_C(0x3f800000),
                                 UINT32_C(0x40000000)) ==
          boxed_sentinel);
    check_trap(2, rv64_fpu_bad_dynamic_rm_insn, 0);
    write_frm(0);
}

static void test_misaligned_memory_traps(void)
{
    uint64_t storage[3] = {
        UINT64_C(0x1122334455667788),
        UINT64_C(0x99aabbccddeeff00),
        UINT64_C(0x0123456789abcdef),
    };
    uint8_t *const misaligned = (uint8_t *)storage + 1;
    const uint64_t fp_sentinel = UINT64_C(0x123456789abcdef0);

    reset_trap();
    check(rv64_fpu_misaligned_flw(misaligned, fp_sentinel) == fp_sentinel);
    check_trap(4, rv64_fpu_misaligned_flw_insn, (uintptr_t)misaligned);

    reset_trap();
    check(rv64_fpu_misaligned_fld(misaligned, fp_sentinel) == fp_sentinel);
    check_trap(4, rv64_fpu_misaligned_fld_insn, (uintptr_t)misaligned);

    reset_trap();
    rv64_fpu_misaligned_fsw(misaligned, UINT32_C(0xdeadbeef));
    check_trap(6, rv64_fpu_misaligned_fsw_insn, (uintptr_t)misaligned);
    check(storage[0] == UINT64_C(0x1122334455667788));
    check(storage[1] == UINT64_C(0x99aabbccddeeff00));

    reset_trap();
    rv64_fpu_misaligned_fsd(misaligned, UINT64_C(0xdeadbeefcafebabe));
    check_trap(6, rv64_fpu_misaligned_fsd_insn, (uintptr_t)misaligned);
    check(storage[0] == UINT64_C(0x1122334455667788));
    check(storage[1] == UINT64_C(0x99aabbccddeeff00));
}

#endif

int main(void)
{
#if defined(__riscv) && __riscv_xlen == 64
    const uintptr_t old_mstatus = read_mstatus();
    const uintptr_t old_mtvec = read_mtvec();

    write_mtvec((uintptr_t)rv64_fpu_trap_handler);
    test_fs_off_and_reserved_rounding(old_mstatus);
    test_misaligned_memory_traps();
    write_mstatus(old_mstatus);
    write_mtvec(old_mtvec);
#endif

    return 0;
}
