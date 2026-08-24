#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 32

#include <stdint.h>

volatile uint32_t rv32_fpu_trap_count = 0;
volatile uint32_t rv32_fpu_trap_mcause = UINT32_MAX;
volatile uint32_t rv32_fpu_trap_mepc = UINT32_MAX;
volatile uint32_t rv32_fpu_trap_mtval = UINT32_MAX;

#define MSTATUS_FS_MASK ((uintptr_t)3u << 13)
#define MSTATUS_FS_OFF ((uintptr_t)0u << 13)
#define MSTATUS_FS_INITIAL ((uintptr_t)1u << 13)

/*
 * The handler uses integer state only, so it remains safe while FS is Off.
 * Advancing mepc lets each probe inspect the state that an illegal or
 * misaligned floating-point instruction was required not to modify.
 */
asm(".section .text\n"
    ".align 2\n"
    ".option push\n"
    ".option norvc\n"
    ".globl rv32_fpu_trap_handler\n"
    ".type rv32_fpu_trap_handler, @function\n"
    "rv32_fpu_trap_handler:\n"
    "  la t0, rv32_fpu_trap_count\n"
    "  lw t1, 0(t0)\n"
    "  addi t1, t1, 1\n"
    "  sw t1, 0(t0)\n"
    "  csrr t1, mcause\n"
    "  la t0, rv32_fpu_trap_mcause\n"
    "  sw t1, 0(t0)\n"
    "  csrr t1, mepc\n"
    "  la t0, rv32_fpu_trap_mepc\n"
    "  sw t1, 0(t0)\n"
    "  csrr t1, mtval\n"
    "  la t0, rv32_fpu_trap_mtval\n"
    "  sw t1, 0(t0)\n"
    "  csrr t1, mepc\n"
    "  addi t1, t1, 4\n"
    "  csrw mepc, t1\n"
    "  mret\n"
    ".size rv32_fpu_trap_handler, .-rv32_fpu_trap_handler\n"
    ".option pop\n");

extern void rv32_fpu_trap_handler(void);

/*
 * The AM build keeps its integer-only ABI. Local architecture options permit
 * explicit F instructions, while raw words cover encodings that a correct
 * RV32F assembler refuses because they require D or RV64.
 */
asm(".section .text\n"
    ".align 2\n"
    ".option push\n"
    ".option norvc\n"
    ".option arch, +f\n"

    /*
     * The vector label is exactly four bytes after the trapping instruction.
     * A JIT must therefore use the executor's explicit result rather than
     * deciding that dnpc == pc + 4 means normal completion.
     */
    ".globl rv32_fpu_sequential_mtvec_probe\n"
    ".type rv32_fpu_sequential_mtvec_probe, @function\n"
    "rv32_fpu_sequential_mtvec_probe:\n"
    ".globl rv32_fpu_sequential_mtvec_insn\n"
    "rv32_fpu_sequential_mtvec_insn:\n"
    "  .word 0xf0050053\n" /* FMV.W.X f0, a0 */
    ".globl rv32_fpu_sequential_mtvec_vector\n"
    "rv32_fpu_sequential_mtvec_vector:\n"
    "  j rv32_fpu_sequential_mtvec_handler\n"
    "rv32_fpu_sequential_mtvec_resume:\n"
    "  ret\n"
    ".size rv32_fpu_sequential_mtvec_probe, "
    ".-rv32_fpu_sequential_mtvec_probe\n"

    ".type rv32_fpu_sequential_mtvec_handler, @function\n"
    "rv32_fpu_sequential_mtvec_handler:\n"
    "  la t0, rv32_fpu_trap_count\n"
    "  lw t1, 0(t0)\n"
    "  addi t1, t1, 1\n"
    "  sw t1, 0(t0)\n"
    "  csrr t1, mcause\n"
    "  la t0, rv32_fpu_trap_mcause\n"
    "  sw t1, 0(t0)\n"
    "  csrr t1, mepc\n"
    "  la t0, rv32_fpu_trap_mepc\n"
    "  sw t1, 0(t0)\n"
    "  csrr t1, mtval\n"
    "  la t0, rv32_fpu_trap_mtval\n"
    "  sw t1, 0(t0)\n"
    "  la t1, rv32_fpu_sequential_mtvec_resume\n"
    "  csrw mepc, t1\n"
    "  mret\n"
    ".size rv32_fpu_sequential_mtvec_handler, "
    ".-rv32_fpu_sequential_mtvec_handler\n"

    ".globl rv32_fpu_fs_off_probe\n"
    ".type rv32_fpu_fs_off_probe, @function\n"
    "rv32_fpu_fs_off_probe:\n"
    ".globl rv32_fpu_fs_off_insn\n"
    "rv32_fpu_fs_off_insn:\n"
    "  .word 0xf0050053\n" /* FMV.W.X f0, a0 */
    "  ret\n"
    ".size rv32_fpu_fs_off_probe, .-rv32_fpu_fs_off_probe\n"

    ".globl rv32_fpu_bad_static_rm\n"
    ".type rv32_fpu_bad_static_rm, @function\n"
    "rv32_fpu_bad_static_rm:\n"
    "  fmv.w.x f0, a0\n"
    "  fmv.w.x f1, a1\n"
    "  fmv.w.x f2, a2\n"
    ".globl rv32_fpu_bad_static_rm_insn\n"
    "rv32_fpu_bad_static_rm_insn:\n"
    "  .word 0x0020d053\n" /* FADD.S f0, f1, f2 with reserved rm=101 */
    "  fmv.x.w a0, f0\n"
    "  ret\n"
    ".size rv32_fpu_bad_static_rm, .-rv32_fpu_bad_static_rm\n"

    ".globl rv32_fpu_bad_dynamic_rm\n"
    ".type rv32_fpu_bad_dynamic_rm, @function\n"
    "rv32_fpu_bad_dynamic_rm:\n"
    "  fmv.w.x f0, a0\n"
    "  fmv.w.x f1, a1\n"
    "  fmv.w.x f2, a2\n"
    ".globl rv32_fpu_bad_dynamic_rm_insn\n"
    "rv32_fpu_bad_dynamic_rm_insn:\n"
    "  .word 0x0020f053\n" /* FADD.S f0, f1, f2 with dynamic rm */
    "  fmv.x.w a0, f0\n"
    "  ret\n"
    ".size rv32_fpu_bad_dynamic_rm, .-rv32_fpu_bad_dynamic_rm\n"

    ".globl rv32_fpu_misaligned_flw\n"
    ".type rv32_fpu_misaligned_flw, @function\n"
    "rv32_fpu_misaligned_flw:\n"
    "  fmv.w.x f0, a1\n"
    ".globl rv32_fpu_misaligned_flw_insn\n"
    "rv32_fpu_misaligned_flw_insn:\n"
    "  flw f0, 0(a0)\n"
    "  fmv.x.w a0, f0\n"
    "  ret\n"
    ".size rv32_fpu_misaligned_flw, .-rv32_fpu_misaligned_flw\n"

    ".globl rv32_fpu_misaligned_fsw\n"
    ".type rv32_fpu_misaligned_fsw, @function\n"
    "rv32_fpu_misaligned_fsw:\n"
    "  fmv.w.x f0, a1\n"
    ".globl rv32_fpu_misaligned_fsw_insn\n"
    "rv32_fpu_misaligned_fsw_insn:\n"
    "  fsw f0, 0(a0)\n"
    "  ret\n"
    ".size rv32_fpu_misaligned_fsw, .-rv32_fpu_misaligned_fsw\n"

    ".globl rv32_fpu_illegal_fld\n"
    ".type rv32_fpu_illegal_fld, @function\n"
    "rv32_fpu_illegal_fld:\n"
    "  fmv.w.x f0, a1\n"
    ".globl rv32_fpu_illegal_fld_insn\n"
    "rv32_fpu_illegal_fld_insn:\n"
    "  .word 0x00053007\n" /* FLD f0, 0(a0), absent from RV32F */
    "  fmv.x.w a0, f0\n"
    "  ret\n"
    ".size rv32_fpu_illegal_fld, .-rv32_fpu_illegal_fld\n"

    ".globl rv32_fpu_illegal_fsd\n"
    ".type rv32_fpu_illegal_fsd, @function\n"
    "rv32_fpu_illegal_fsd:\n"
    "  fmv.w.x f0, a1\n"
    ".globl rv32_fpu_illegal_fsd_insn\n"
    "rv32_fpu_illegal_fsd_insn:\n"
    "  .word 0x00053027\n" /* FSD f0, 0(a0), absent from RV32F */
    "  ret\n"
    ".size rv32_fpu_illegal_fsd, .-rv32_fpu_illegal_fsd\n"

    ".globl rv32_fpu_illegal_madd_d\n"
    ".type rv32_fpu_illegal_madd_d, @function\n"
    "rv32_fpu_illegal_madd_d:\n"
    "  fmv.w.x f0, a0\n"
    "  fmv.w.x f1, a1\n"
    "  fmv.w.x f2, a2\n"
    ".globl rv32_fpu_illegal_madd_d_insn\n"
    "rv32_fpu_illegal_madd_d_insn:\n"
    "  .word 0x02208043\n" /* FMADD.D f0, f1, f2, f0 */
    "  fmv.x.w a0, f0\n"
    "  ret\n"
    ".size rv32_fpu_illegal_madd_d, .-rv32_fpu_illegal_madd_d\n"

    ".globl rv32_fpu_illegal_cvt_s_d\n"
    ".type rv32_fpu_illegal_cvt_s_d, @function\n"
    "rv32_fpu_illegal_cvt_s_d:\n"
    "  fmv.w.x f1, a0\n"
    "  fmv.w.x f0, a1\n"
    ".globl rv32_fpu_illegal_cvt_s_d_insn\n"
    "rv32_fpu_illegal_cvt_s_d_insn:\n"
    "  .word 0x40108053\n" /* FCVT.S.D f0, f1 */
    "  fmv.x.w a0, f0\n"
    "  ret\n"
    ".size rv32_fpu_illegal_cvt_s_d, .-rv32_fpu_illegal_cvt_s_d\n"

    ".globl rv32_fpu_illegal_move_x_d\n"
    ".type rv32_fpu_illegal_move_x_d, @function\n"
    "rv32_fpu_illegal_move_x_d:\n"
    "  fmv.w.x f0, a0\n"
    "  li a0, 0x13579bdf\n"
    ".globl rv32_fpu_illegal_move_x_d_insn\n"
    "rv32_fpu_illegal_move_x_d_insn:\n"
    "  .word 0xe2000553\n" /* FMV.X.D a0, f0, absent from RV32F */
    "  ret\n"
    ".size rv32_fpu_illegal_move_x_d, .-rv32_fpu_illegal_move_x_d\n"

    ".globl rv32_fpu_illegal_add_d\n"
    ".type rv32_fpu_illegal_add_d, @function\n"
    "rv32_fpu_illegal_add_d:\n"
    "  fmv.w.x f0, a0\n"
    "  fmv.w.x f1, a1\n"
    "  fmv.w.x f2, a2\n"
    ".globl rv32_fpu_illegal_add_d_insn\n"
    "rv32_fpu_illegal_add_d_insn:\n"
    "  .word 0x02208053\n" /* FADD.D f0, f1, f2 */
    "  fmv.x.w a0, f0\n"
    "  ret\n"
    ".size rv32_fpu_illegal_add_d, .-rv32_fpu_illegal_add_d\n"

    ".globl rv32_fpu_illegal_fcvt_l_s\n"
    ".type rv32_fpu_illegal_fcvt_l_s, @function\n"
    "rv32_fpu_illegal_fcvt_l_s:\n"
    "  fmv.w.x f0, a0\n"
    "  li a0, 0x13579bdf\n"
    ".globl rv32_fpu_illegal_fcvt_l_s_insn\n"
    "rv32_fpu_illegal_fcvt_l_s_insn:\n"
    "  .word 0xc0200553\n" /* FCVT.L.S a0, f0, RV64-only */
    "  ret\n"
    ".size rv32_fpu_illegal_fcvt_l_s, .-rv32_fpu_illegal_fcvt_l_s\n"

    ".globl rv32_fpu_illegal_fcvt_s_l\n"
    ".type rv32_fpu_illegal_fcvt_s_l, @function\n"
    "rv32_fpu_illegal_fcvt_s_l:\n"
    "  fmv.w.x f0, a1\n"
    ".globl rv32_fpu_illegal_fcvt_s_l_insn\n"
    "rv32_fpu_illegal_fcvt_s_l_insn:\n"
    "  .word 0xd0250053\n" /* FCVT.S.L f0, a0, RV64-only */
    "  fmv.x.w a0, f0\n"
    "  ret\n"
    ".size rv32_fpu_illegal_fcvt_s_l, .-rv32_fpu_illegal_fcvt_s_l\n"

    ".option pop\n");

extern void rv32_fpu_sequential_mtvec_probe(uint32_t);
extern char rv32_fpu_sequential_mtvec_insn[];
extern char rv32_fpu_sequential_mtvec_vector[];
extern void rv32_fpu_fs_off_probe(uint32_t);
extern char rv32_fpu_fs_off_insn[];
extern uint32_t rv32_fpu_bad_static_rm(uint32_t, uint32_t, uint32_t);
extern char rv32_fpu_bad_static_rm_insn[];
extern uint32_t rv32_fpu_bad_dynamic_rm(uint32_t, uint32_t, uint32_t);
extern char rv32_fpu_bad_dynamic_rm_insn[];
extern uint32_t rv32_fpu_misaligned_flw(const void *, uint32_t);
extern char rv32_fpu_misaligned_flw_insn[];
extern void rv32_fpu_misaligned_fsw(void *, uint32_t);
extern char rv32_fpu_misaligned_fsw_insn[];
extern uint32_t rv32_fpu_illegal_fld(const void *, uint32_t);
extern char rv32_fpu_illegal_fld_insn[];
extern void rv32_fpu_illegal_fsd(void *, uint32_t);
extern char rv32_fpu_illegal_fsd_insn[];
extern uint32_t rv32_fpu_illegal_madd_d(uint32_t, uint32_t, uint32_t);
extern char rv32_fpu_illegal_madd_d_insn[];
extern uint32_t rv32_fpu_illegal_cvt_s_d(uint32_t, uint32_t);
extern char rv32_fpu_illegal_cvt_s_d_insn[];
extern uint32_t rv32_fpu_illegal_move_x_d(uint32_t);
extern char rv32_fpu_illegal_move_x_d_insn[];
extern uint32_t rv32_fpu_illegal_add_d(uint32_t, uint32_t, uint32_t);
extern char rv32_fpu_illegal_add_d_insn[];
extern uint32_t rv32_fpu_illegal_fcvt_l_s(uint32_t);
extern char rv32_fpu_illegal_fcvt_l_s_insn[];
extern uint32_t rv32_fpu_illegal_fcvt_s_l(uint32_t, uint32_t);
extern char rv32_fpu_illegal_fcvt_s_l_insn[];

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
    rv32_fpu_trap_count = 0;
    rv32_fpu_trap_mcause = UINT32_MAX;
    rv32_fpu_trap_mepc = UINT32_MAX;
    rv32_fpu_trap_mtval = UINT32_MAX;
}

static void check_trap(uint32_t cause, const char *instruction, uintptr_t expected_tval)
{
    check(rv32_fpu_trap_count == 1);
    check(rv32_fpu_trap_mcause == cause);
    check(rv32_fpu_trap_mepc == (uintptr_t)instruction);
    check(rv32_fpu_trap_mtval == expected_tval);
}

static void test_fs_off_and_reserved_rounding(uintptr_t base_mstatus)
{
    const uint32_t sentinel = UINT32_C(0x3f000000);

    reset_trap();
    write_mstatus((base_mstatus & ~MSTATUS_FS_MASK) | MSTATUS_FS_OFF);
    rv32_fpu_fs_off_probe(UINT32_C(0x3f800000));
    check_trap(2, rv32_fpu_fs_off_insn, 0);

    reset_trap();
    write_mtvec((uintptr_t)rv32_fpu_sequential_mtvec_vector);
    rv32_fpu_sequential_mtvec_probe(UINT32_C(0x3f800000));
    check_trap(2, rv32_fpu_sequential_mtvec_insn, 0);
    write_mtvec((uintptr_t)rv32_fpu_trap_handler);

    write_mstatus((base_mstatus & ~MSTATUS_FS_MASK) | MSTATUS_FS_INITIAL);
    reset_trap();
    check(rv32_fpu_bad_static_rm(sentinel, UINT32_C(0x3f800000), UINT32_C(0x40000000)) == sentinel);
    check_trap(2, rv32_fpu_bad_static_rm_insn, 0);

    write_frm(5);
    reset_trap();
    check(rv32_fpu_bad_dynamic_rm(sentinel, UINT32_C(0x3f800000), UINT32_C(0x40000000)) == sentinel);
    check_trap(2, rv32_fpu_bad_dynamic_rm_insn, 0);
    write_frm(0);
}

static void test_misaligned_memory_traps(void)
{
    uint32_t storage[4] = {
        UINT32_C(0x11223344),
        UINT32_C(0x55667788),
        UINT32_C(0x99aabbcc),
        UINT32_C(0xddeeff00),
    };
    uint8_t *const misaligned = (uint8_t *)storage + 1;
    const uint32_t fp_sentinel = UINT32_C(0x12345678);

    reset_trap();
    check(rv32_fpu_misaligned_flw(misaligned, fp_sentinel) == fp_sentinel);
    check_trap(4, rv32_fpu_misaligned_flw_insn, (uintptr_t)misaligned);

    reset_trap();
    rv32_fpu_misaligned_fsw(misaligned, UINT32_C(0xdeadbeef));
    check_trap(6, rv32_fpu_misaligned_fsw_insn, (uintptr_t)misaligned);
    check(storage[0] == UINT32_C(0x11223344));
    check(storage[1] == UINT32_C(0x55667788));
}

static void test_rv32_rejects_d_and_long_conversions(void)
{
    const uint32_t fp_sentinel = UINT32_C(0x3f000000);
    uint32_t storage[2] = {
        UINT32_C(0x11223344),
        UINT32_C(0x55667788),
    };

    /*
     * Each probe enters a distinct decoder family.  Besides checking the
     * illegal-instruction cause, sentinels prove that rejection happens before
     * an FPR, GPR, or memory destination can be modified.
     */
    reset_trap();
    check(rv32_fpu_illegal_fld(storage, fp_sentinel) == fp_sentinel);
    check_trap(2, rv32_fpu_illegal_fld_insn, 0);

    reset_trap();
    rv32_fpu_illegal_fsd(storage, UINT32_C(0xdeadbeef));
    check_trap(2, rv32_fpu_illegal_fsd_insn, 0);
    check(storage[0] == UINT32_C(0x11223344));
    check(storage[1] == UINT32_C(0x55667788));

    reset_trap();
    check(rv32_fpu_illegal_madd_d(fp_sentinel, UINT32_C(0x3f800000), UINT32_C(0x40000000)) == fp_sentinel);
    check_trap(2, rv32_fpu_illegal_madd_d_insn, 0);

    reset_trap();
    check(rv32_fpu_illegal_cvt_s_d(UINT32_C(0x3f800000), fp_sentinel) == fp_sentinel);
    check_trap(2, rv32_fpu_illegal_cvt_s_d_insn, 0);

    reset_trap();
    check(rv32_fpu_illegal_move_x_d(UINT32_C(0x3f800000)) == UINT32_C(0x13579bdf));
    check_trap(2, rv32_fpu_illegal_move_x_d_insn, 0);

    reset_trap();
    check(rv32_fpu_illegal_add_d(fp_sentinel, UINT32_C(0x3f800000), UINT32_C(0x40000000)) == fp_sentinel);
    check_trap(2, rv32_fpu_illegal_add_d_insn, 0);

    reset_trap();
    check(rv32_fpu_illegal_fcvt_l_s(UINT32_C(0x3f800000)) == UINT32_C(0x13579bdf));
    check_trap(2, rv32_fpu_illegal_fcvt_l_s_insn, 0);

    reset_trap();
    check(rv32_fpu_illegal_fcvt_s_l(UINT32_C(1), fp_sentinel) == fp_sentinel);
    check_trap(2, rv32_fpu_illegal_fcvt_s_l_insn, 0);
}

#endif

int main(void)
{
#if defined(__riscv) && __riscv_xlen == 32
    const uintptr_t old_mstatus = read_mstatus();
    const uintptr_t old_mtvec = read_mtvec();

    write_mtvec((uintptr_t)rv32_fpu_trap_handler);
    test_fs_off_and_reserved_rounding(old_mstatus);
    write_mstatus((old_mstatus & ~MSTATUS_FS_MASK) | MSTATUS_FS_INITIAL);
    test_misaligned_memory_traps();
    test_rv32_rejects_d_and_long_conversions();
    write_mstatus(old_mstatus);
    write_mtvec(old_mtvec);
#endif

    return 0;
}
