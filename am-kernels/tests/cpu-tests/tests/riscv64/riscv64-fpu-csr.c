#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 64

#include <stdint.h>

volatile uint64_t rv64_fpu_csr_trap_count = 0;
volatile uint64_t rv64_fpu_csr_saved_mcause = UINT64_MAX;
volatile uint64_t rv64_fpu_csr_saved_mepc = UINT64_MAX;

enum
{
    CSR_FFLAGS = 0x001,
    CSR_FRM = 0x002,
    CSR_FCSR = 0x003,
    MSTATUS_FS_SHIFT = 13,
};

#define MSTATUS_FS_MASK ((uintptr_t)3u << MSTATUS_FS_SHIFT)
#define MSTATUS_FS_OFF ((uintptr_t)0u << MSTATUS_FS_SHIFT)
#define MSTATUS_FS_INITIAL ((uintptr_t)1u << MSTATUS_FS_SHIFT)
#define MSTATUS_FS_CLEAN ((uintptr_t)2u << MSTATUS_FS_SHIFT)
#define MSTATUS_FS_DIRTY ((uintptr_t)3u << MSTATUS_FS_SHIFT)
#define MSTATUS_VS_MASK ((uintptr_t)3u << 9)
#define MSTATUS_XS_MASK ((uintptr_t)3u << 15)
#define MSTATUS_SD (UINT64_C(1) << 63)

/*
 * The trap handler touches integer temporaries only.  This matters for the
 * FS-Off case: using an FPR while handling the expected illegal instruction
 * would recursively trap and hide the behaviour being tested.
 */
asm(
    ".section .text\n"
    ".align 2\n"
    ".option push\n"
    ".option norvc\n"
    ".globl rv64_fpu_csr_trap_handler\n"
    ".type rv64_fpu_csr_trap_handler, @function\n"
    "rv64_fpu_csr_trap_handler:\n"
    "  la t0, rv64_fpu_csr_trap_count\n"
    "  ld t1, 0(t0)\n"
    "  addi t1, t1, 1\n"
    "  sd t1, 0(t0)\n"
    "  csrr t1, mcause\n"
    "  la t0, rv64_fpu_csr_saved_mcause\n"
    "  sd t1, 0(t0)\n"
    "  csrr t1, mepc\n"
    "  la t0, rv64_fpu_csr_saved_mepc\n"
    "  sd t1, 0(t0)\n"
    "  addi t1, t1, 4\n"
    "  csrw mepc, t1\n"
    "  mret\n"
    ".size rv64_fpu_csr_trap_handler, .-rv64_fpu_csr_trap_handler\n"
    ".option pop\n");

extern void rv64_fpu_csr_trap_handler(void);

/*
 * These helpers deliberately name an F instruction and an FP CSR while the C
 * translation unit retains the integer-only ABI.  The exported instruction
 * labels let the C test prove that the expected instruction, rather than a
 * nearby setup instruction, caused the trap.
 */
asm(
    ".section .text\n"
    ".align 2\n"
    ".option push\n"
    ".option norvc\n"
    ".option arch, +f\n"
    ".option arch, +d\n"

    ".globl rv64_fpu_probe_fcsr_with_fs_off\n"
    ".type rv64_fpu_probe_fcsr_with_fs_off, @function\n"
    "rv64_fpu_probe_fcsr_with_fs_off:\n"
    "  li a0, 0x5a\n"
    ".globl rv64_fpu_fs_off_csr_insn\n"
    "rv64_fpu_fs_off_csr_insn:\n"
    "  csrr a0, 0x003\n"
    "  ret\n"
    ".size rv64_fpu_probe_fcsr_with_fs_off, .-rv64_fpu_probe_fcsr_with_fs_off\n"

    ".globl rv64_fpu_write_f0\n"
    ".type rv64_fpu_write_f0, @function\n"
    "rv64_fpu_write_f0:\n"
    "  fmv.w.x f0, a0\n"
    "  ret\n"
    ".size rv64_fpu_write_f0, .-rv64_fpu_write_f0\n"

    ".globl rv64_fpu_read_f0\n"
    ".type rv64_fpu_read_f0, @function\n"
    "rv64_fpu_read_f0:\n"
    "  fmv.x.d a0, f0\n"
    "  ret\n"
    ".size rv64_fpu_read_f0, .-rv64_fpu_read_f0\n"

    ".option pop\n");

extern uint64_t rv64_fpu_probe_fcsr_with_fs_off(void);
extern char rv64_fpu_fs_off_csr_insn[];
extern void rv64_fpu_write_f0(uint32_t bits);
extern uint64_t rv64_fpu_read_f0(void);

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

static uintptr_t read_misa(void)
{
    uintptr_t value;
    asm volatile("csrr %0, misa" : "=r"(value));
    return value;
}

static void write_mtvec(uintptr_t value)
{
    asm volatile("csrw mtvec, %0" : : "r"(value) : "memory");
}

static uintptr_t read_fflags(void)
{
    uintptr_t value;
    asm volatile("csrr %0, 0x001" : "=r"(value));
    return value;
}

static uintptr_t read_frm(void)
{
    uintptr_t value;
    asm volatile("csrr %0, 0x002" : "=r"(value));
    return value;
}

static uintptr_t read_fcsr(void)
{
    uintptr_t value;
    asm volatile("csrr %0, 0x003" : "=r"(value));
    return value;
}

static void write_fflags(uintptr_t value)
{
    asm volatile("csrw 0x001, %0" : : "r"(value) : "memory");
}

static void write_frm(uintptr_t value)
{
    asm volatile("csrw 0x002, %0" : : "r"(value) : "memory");
}

static void write_fcsr(uintptr_t value)
{
    asm volatile("csrw 0x003, %0" : : "r"(value) : "memory");
}

static uintptr_t replace_fs(uintptr_t status, uintptr_t fs)
{
    return (status & ~MSTATUS_FS_MASK) | fs;
}

static void reset_trap_observation(void)
{
    rv64_fpu_csr_trap_count = 0;
    rv64_fpu_csr_saved_mcause = UINT64_MAX;
    rv64_fpu_csr_saved_mepc = UINT64_MAX;
}

static void test_misa_advertises_rv64_fd(void)
{
    const uintptr_t misa = read_misa();
    const uintptr_t mxl = misa >> 62;

    check(mxl == 2);
    check((misa & ((uintptr_t)1u << ('I' - 'A'))) != 0);
    check((misa & ((uintptr_t)1u << ('M' - 'A'))) != 0);
    check((misa & ((uintptr_t)1u << ('F' - 'A'))) != 0);
    check((misa & ((uintptr_t)1u << ('D' - 'A'))) != 0);
}

static void test_fs_off_blocks_fp_csr_access(void)
{
    const uintptr_t old_mstatus = read_mstatus();
    const uintptr_t old_mtvec = read_mtvec();

    reset_trap_observation();
    write_mtvec((uintptr_t)rv64_fpu_csr_trap_handler);
    write_mstatus(replace_fs(old_mstatus, MSTATUS_FS_OFF));

    check(rv64_fpu_probe_fcsr_with_fs_off() == UINT64_C(0x5a));
    check(rv64_fpu_csr_trap_count == 1);
    check(rv64_fpu_csr_saved_mcause == 2);
    check(rv64_fpu_csr_saved_mepc ==
          (uintptr_t)rv64_fpu_fs_off_csr_insn);

    write_mstatus(old_mstatus);
    write_mtvec(old_mtvec);
}

static void test_fcsr_aliases_and_dirty_state(void)
{
    const uintptr_t old_mstatus = read_mstatus();

    write_mstatus(replace_fs(old_mstatus, MSTATUS_FS_INITIAL));

    /* fcsr exposes only bits [7:0]; all upper written bits are ignored. */
    write_fcsr(UINT64_C(0x1234abcd));
    check(read_fcsr() == UINT64_C(0xcd));
    check(read_fflags() == UINT64_C(0x0d));
    check(read_frm() == UINT64_C(0x06));

    /* Alias writes replace only their own field. */
    write_fflags(UINT64_C(0x15));
    check(read_fcsr() == UINT64_C(0xd5));
    write_frm(UINT64_C(0x03));
    check(read_fcsr() == UINT64_C(0x75));

    {
        const uintptr_t dirty = read_mstatus();
        check((dirty & MSTATUS_FS_MASK) == MSTATUS_FS_DIRTY);
        check((dirty & MSTATUS_SD) != 0);
    }

    /*
     * Reading an FP CSR is not a state write.  Once software explicitly marks
     * the state Clean, a read must leave both FS and the derived SD bit clean.
     */
    write_mstatus(replace_fs(read_mstatus(), MSTATUS_FS_CLEAN));
    check(read_fcsr() == UINT64_C(0x75));
    check((read_mstatus() & MSTATUS_FS_MASK) == MSTATUS_FS_CLEAN);
    check((read_mstatus() & MSTATUS_SD) == 0);

    /*
     * This NEMU target implements neither V nor non-standard extension state.
     * Their mstatus summary fields are therefore read-only zero and cannot
     * contribute a contradictory Dirty indication while SD is clear.
     */
    write_mstatus(read_mstatus() | MSTATUS_VS_MASK | MSTATUS_XS_MASK);
    check((read_mstatus() & (MSTATUS_VS_MASK | MSTATUS_XS_MASK)) == 0);
    check((read_mstatus() & MSTATUS_SD) == 0);

    /* Writing an FPR changes Clean to Dirty and sets derived SD. */
    rv64_fpu_write_f0(UINT32_C(0x3f800000));
    check((read_mstatus() & MSTATUS_FS_MASK) == MSTATUS_FS_DIRTY);
    check((read_mstatus() & MSTATUS_SD) != 0);

    /*
     * FS is a context-status summary, not destructive storage control.
     * Turning FP Off and then selecting Initial must preserve both the backing
     * FPR bits and fcsr until privileged software explicitly reinitialises them.
     */
    write_mstatus(replace_fs(read_mstatus(), MSTATUS_FS_OFF));
    write_mstatus(replace_fs(read_mstatus(), MSTATUS_FS_INITIAL));
    check(rv64_fpu_read_f0() == UINT64_C(0xffffffff3f800000));
    check(read_fcsr() == UINT64_C(0x75));
    check((read_mstatus() & MSTATUS_FS_MASK) == MSTATUS_FS_INITIAL);
    check((read_mstatus() & MSTATUS_SD) == 0);

    write_mstatus(old_mstatus);
}

#endif

int main(void)
{
#if defined(__riscv) && __riscv_xlen == 64
    test_misa_advertises_rv64_fd();
    test_fs_off_blocks_fp_csr_access();
    test_fcsr_aliases_and_dirty_state();
#endif

    return 0;
}
