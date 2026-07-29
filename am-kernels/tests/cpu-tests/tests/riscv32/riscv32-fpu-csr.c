#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 32

#include <stdint.h>

volatile uint32_t rv32_fpu_csr_trap_count = 0;
volatile uint32_t rv32_fpu_csr_saved_mcause = UINT32_MAX;
volatile uint32_t rv32_fpu_csr_saved_mepc = UINT32_MAX;

#define MSTATUS_FS_MASK ((uintptr_t)3u << 13)
#define MSTATUS_FS_OFF ((uintptr_t)0u << 13)
#define MSTATUS_FS_INITIAL ((uintptr_t)1u << 13)
#define MSTATUS_FS_CLEAN ((uintptr_t)2u << 13)
#define MSTATUS_FS_DIRTY ((uintptr_t)3u << 13)
#define MSTATUS_VS_MASK ((uintptr_t)3u << 9)
#define MSTATUS_XS_MASK ((uintptr_t)3u << 15)
#define MSTATUS_SD (UINT32_C(1) << 31)

/*
 * No floating-point register is touched here.  This permits the expected
 * FS-Off illegal instruction, and any unexpectedly missing F support, to
 * return to integer-only C code without recursively trapping.
 */
asm(
    ".section .text\n"
    ".align 2\n"
    ".option push\n"
    ".option norvc\n"
    ".globl rv32_fpu_csr_trap_handler\n"
    ".type rv32_fpu_csr_trap_handler, @function\n"
    "rv32_fpu_csr_trap_handler:\n"
    "  la t0, rv32_fpu_csr_trap_count\n"
    "  lw t1, 0(t0)\n"
    "  addi t1, t1, 1\n"
    "  sw t1, 0(t0)\n"
    "  csrr t1, mcause\n"
    "  la t0, rv32_fpu_csr_saved_mcause\n"
    "  sw t1, 0(t0)\n"
    "  csrr t1, mepc\n"
    "  la t0, rv32_fpu_csr_saved_mepc\n"
    "  sw t1, 0(t0)\n"
    "  addi t1, t1, 4\n"
    "  csrw mepc, t1\n"
    "  mret\n"
    ".size rv32_fpu_csr_trap_handler, .-rv32_fpu_csr_trap_handler\n"
    ".option pop\n");

extern void rv32_fpu_csr_trap_handler(void);

/*
 * These helpers keep the global C ABI integer-only.  The F instructions are
 * enabled only inside this local assembler option block.
 */
asm(
    ".section .text\n"
    ".align 2\n"
    ".option push\n"
    ".option norvc\n"
    ".option arch, +f\n"

    ".globl rv32_fpu_probe_fcsr_with_fs_off\n"
    ".type rv32_fpu_probe_fcsr_with_fs_off, @function\n"
    "rv32_fpu_probe_fcsr_with_fs_off:\n"
    "  li a0, 0x5a\n"
    ".globl rv32_fpu_fs_off_csr_insn\n"
    "rv32_fpu_fs_off_csr_insn:\n"
    "  csrr a0, 0x003\n"
    "  ret\n"
    ".size rv32_fpu_probe_fcsr_with_fs_off, "
    ".-rv32_fpu_probe_fcsr_with_fs_off\n"

    ".globl rv32_fpu_write_f0\n"
    ".type rv32_fpu_write_f0, @function\n"
    "rv32_fpu_write_f0:\n"
    "  fmv.w.x f0, a0\n"
    "  ret\n"
    ".size rv32_fpu_write_f0, .-rv32_fpu_write_f0\n"

    ".globl rv32_fpu_read_f0\n"
    ".type rv32_fpu_read_f0, @function\n"
    "rv32_fpu_read_f0:\n"
    "  fmv.x.w a0, f0\n"
    "  ret\n"
    ".size rv32_fpu_read_f0, .-rv32_fpu_read_f0\n"

    ".option pop\n");

extern uint32_t rv32_fpu_probe_fcsr_with_fs_off(void);
extern char rv32_fpu_fs_off_csr_insn[];
extern void rv32_fpu_write_f0(uint32_t bits);
extern uint32_t rv32_fpu_read_f0(void);

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

static uintptr_t read_misa(void)
{
    uintptr_t value;
    asm volatile("csrr %0, misa" : "=r"(value));
    return value;
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
    rv32_fpu_csr_trap_count = 0;
    rv32_fpu_csr_saved_mcause = UINT32_MAX;
    rv32_fpu_csr_saved_mepc = UINT32_MAX;
}

static void test_misa_advertises_rv32f_only(void)
{
    const uintptr_t misa = read_misa();

    check((misa >> 30) == 1);
    check((misa & ((uintptr_t)1u << ('I' - 'A'))) != 0);
    check((misa & ((uintptr_t)1u << ('M' - 'A'))) != 0);
    check((misa & ((uintptr_t)1u << ('F' - 'A'))) != 0);
    check((misa & ((uintptr_t)1u << ('D' - 'A'))) == 0);
}

static void test_fs_off_blocks_fp_csr_access(void)
{
    const uintptr_t old_mstatus = read_mstatus();
    const uintptr_t old_mtvec = read_mtvec();
    uint32_t result;

    reset_trap_observation();
    write_mtvec((uintptr_t)rv32_fpu_csr_trap_handler);
    write_mstatus(replace_fs(old_mstatus, MSTATUS_FS_OFF));

    result = rv32_fpu_probe_fcsr_with_fs_off();

    write_mstatus(old_mstatus);
    write_mtvec(old_mtvec);

    check(result == UINT32_C(0x5a));
    check(rv32_fpu_csr_trap_count == 1);
    check(rv32_fpu_csr_saved_mcause == 2);
    check(rv32_fpu_csr_saved_mepc ==
          (uintptr_t)rv32_fpu_fs_off_csr_insn);
}

static void test_fcsr_aliases_and_fs_state(void)
{
    const uintptr_t old_mstatus = read_mstatus();
    const uintptr_t old_mtvec = read_mtvec();
    uintptr_t fcsr_after_full_write;
    uintptr_t fflags_after_full_write;
    uintptr_t frm_after_full_write;
    uintptr_t fcsr_after_fflags_write;
    uintptr_t fcsr_after_frm_write;
    uintptr_t dirty_after_csr_write;
    uintptr_t fcsr_after_clean_read;
    uintptr_t clean_after_read;
    uintptr_t extension_fields_after_write;
    uintptr_t dirty_after_fpr_write;
    uint32_t f0_after_fs_toggle;
    uintptr_t fcsr_after_fs_toggle;
    uintptr_t initial_after_toggle;

    reset_trap_observation();
    write_mtvec((uintptr_t)rv32_fpu_csr_trap_handler);
    write_mstatus(replace_fs(old_mstatus, MSTATUS_FS_INITIAL));

    /* fcsr implements bits [7:0], with fflags and frm as field aliases. */
    write_fcsr(UINT32_C(0x1234abcd));
    fcsr_after_full_write = read_fcsr();
    fflags_after_full_write = read_fflags();
    frm_after_full_write = read_frm();

    write_fflags(UINT32_C(0x15));
    fcsr_after_fflags_write = read_fcsr();
    write_frm(UINT32_C(0x03));
    fcsr_after_frm_write = read_fcsr();
    dirty_after_csr_write = read_mstatus();

    /*
     * An FP CSR read is not a state write, so it must leave explicitly Clean
     * state Clean and leave the derived RV32 SD bit clear.
     */
    write_mstatus(replace_fs(dirty_after_csr_write, MSTATUS_FS_CLEAN));
    fcsr_after_clean_read = read_fcsr();
    clean_after_read = read_mstatus();

    /*
     * This target has neither V nor additional extension state.  Their status
     * fields remain zero and therefore cannot set SD.
     */
    write_mstatus(clean_after_read | MSTATUS_VS_MASK | MSTATUS_XS_MASK);
    extension_fields_after_write = read_mstatus();

    rv32_fpu_write_f0(UINT32_C(0x3f800000));
    dirty_after_fpr_write = read_mstatus();

    /*
     * FS is context status rather than destructive storage control.  Off then
     * Initial must preserve both the FPR bits and fcsr.
     */
    write_mstatus(replace_fs(dirty_after_fpr_write, MSTATUS_FS_OFF));
    write_mstatus(replace_fs(read_mstatus(), MSTATUS_FS_INITIAL));
    f0_after_fs_toggle = rv32_fpu_read_f0();
    fcsr_after_fs_toggle = read_fcsr();
    initial_after_toggle = read_mstatus();

    write_mstatus(old_mstatus);
    write_mtvec(old_mtvec);

    /* Unexpected missing FP support reaches this ordinary check failure. */
    check(rv32_fpu_csr_trap_count == 0);
    check(fcsr_after_full_write == UINT32_C(0xcd));
    check(fflags_after_full_write == UINT32_C(0x0d));
    check(frm_after_full_write == UINT32_C(0x06));
    check(fcsr_after_fflags_write == UINT32_C(0xd5));
    check(fcsr_after_frm_write == UINT32_C(0x75));
    check((dirty_after_csr_write & MSTATUS_FS_MASK) == MSTATUS_FS_DIRTY);
    check((dirty_after_csr_write & MSTATUS_SD) != 0);
    check(fcsr_after_clean_read == UINT32_C(0x75));
    check((clean_after_read & MSTATUS_FS_MASK) == MSTATUS_FS_CLEAN);
    check((clean_after_read & MSTATUS_SD) == 0);
    check((extension_fields_after_write &
           (MSTATUS_VS_MASK | MSTATUS_XS_MASK)) == 0);
    check((extension_fields_after_write & MSTATUS_SD) == 0);
    check((dirty_after_fpr_write & MSTATUS_FS_MASK) == MSTATUS_FS_DIRTY);
    check((dirty_after_fpr_write & MSTATUS_SD) != 0);
    check(f0_after_fs_toggle == UINT32_C(0x3f800000));
    check(fcsr_after_fs_toggle == UINT32_C(0x75));
    check((initial_after_toggle & MSTATUS_FS_MASK) == MSTATUS_FS_INITIAL);
    check((initial_after_toggle & MSTATUS_SD) == 0);
}

#endif

int main(void)
{
#if defined(__riscv) && __riscv_xlen == 32
    test_misa_advertises_rv32f_only();
    test_fs_off_blocks_fp_csr_access();
    test_fcsr_aliases_and_fs_state();
#endif

    return 0;
}
