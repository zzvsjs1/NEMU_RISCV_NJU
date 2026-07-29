#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 32

#include <stdint.h>

volatile uint32_t rv32_d_traps_count = 0;
volatile uint32_t rv32_d_traps_mcause = UINT32_MAX;
volatile uint32_t rv32_d_traps_mepc = UINT32_MAX;
volatile uint32_t rv32_d_traps_mtval = UINT32_MAX;

enum
{
    MSTATUS_FS_SHIFT = 13,
    CAUSE_ILLEGAL_INSTRUCTION = 2,
    CAUSE_LOAD_ADDRESS_MISALIGNED = 4,
    CAUSE_STORE_ADDRESS_MISALIGNED = 6,
    FFLAG_DZ = 1u << 3,
};

#define MSTATUS_FS_MASK ((uintptr_t)3u << MSTATUS_FS_SHIFT)
#define MSTATUS_FS_OFF ((uintptr_t)0u << MSTATUS_FS_SHIFT)
#define MSTATUS_FS_INITIAL ((uintptr_t)1u << MSTATUS_FS_SHIFT)

/*
 * Every probe below contains exactly one instruction expected to trap.  The
 * handler records its architectural evidence and advances mepc by one
 * uncompressed instruction.  It uses integer registers only, so it is also
 * safe for the probe that deliberately sets mstatus.FS to Off.
 */
asm(
    ".section .text\n"
    ".align 2\n"
    ".option push\n"
    ".option norvc\n"
    ".globl rv32_d_traps_handler\n"
    ".type rv32_d_traps_handler, @function\n"
    "rv32_d_traps_handler:\n"
    "  la t0, rv32_d_traps_count\n"
    "  lw t1, 0(t0)\n"
    "  addi t1, t1, 1\n"
    "  sw t1, 0(t0)\n"
    "  csrr t1, mcause\n"
    "  la t0, rv32_d_traps_mcause\n"
    "  sw t1, 0(t0)\n"
    "  csrr t1, mepc\n"
    "  la t0, rv32_d_traps_mepc\n"
    "  sw t1, 0(t0)\n"
    "  csrr t1, mtval\n"
    "  la t0, rv32_d_traps_mtval\n"
    "  sw t1, 0(t0)\n"
    "  csrr t1, mepc\n"
    "  addi t1, t1, 4\n"
    "  csrw mepc, t1\n"
    "  mret\n"
    ".size rv32_d_traps_handler, .-rv32_d_traps_handler\n"
    ".option pop\n");

extern void rv32_d_traps_handler(void);

#define RV32_D_FLOAT_TO_LONG_PROBE(name, label, encoding) \
    ".globl " #name "\n" \
    ".type " #name ", @function\n" \
    #name ":\n" \
    "  fld f0, 0(a0)\n" \
    "  mv a0, a1\n" \
    ".globl " #label "\n" \
    #label ":\n" \
    "  .word " #encoding "\n" \
    "  ret\n" \
    ".size " #name ", .-" #name "\n"

#define RV32_D_LONG_TO_FLOAT_PROBE(name, label, encoding) \
    ".globl " #name "\n" \
    ".type " #name ", @function\n" \
    #name ":\n" \
    "  fld f0, 0(a1)\n" \
    ".globl " #label "\n" \
    #label ":\n" \
    "  .word " #encoding "\n" \
    "  fsd f0, 0(a2)\n" \
    "  ret\n" \
    ".size " #name ", .-" #name "\n"

/*
 * The translation unit keeps the soft, integer-only ILP32 ABI.  Legal D
 * setup and observation use FLD/FSD through aligned pointers.  Raw words are
 * intentional for RV64-only encodings that a conforming RV32 assembler must
 * reject; none of these helpers uses FMV.X.D or FMV.D.X to observe an FPR.
 */
asm(
    ".section .text\n"
    ".align 2\n"
    ".option push\n"
    ".option norvc\n"
    ".option arch, +f\n"
    ".option arch, +d\n"

    ".globl rv32_d_traps_fs_off\n"
    ".type rv32_d_traps_fs_off, @function\n"
    "rv32_d_traps_fs_off:\n"
    ".globl rv32_d_traps_fs_off_insn\n"
    "rv32_d_traps_fs_off_insn:\n"
    "  fld f0, 0(a0)\n"
    "  ret\n"
    ".size rv32_d_traps_fs_off, .-rv32_d_traps_fs_off\n"

    ".globl rv32_d_traps_misaligned_fld\n"
    ".type rv32_d_traps_misaligned_fld, @function\n"
    "rv32_d_traps_misaligned_fld:\n"
    "  fld f0, 0(a1)\n"
    ".globl rv32_d_traps_misaligned_fld_insn\n"
    "rv32_d_traps_misaligned_fld_insn:\n"
    "  fld f0, 0(a0)\n"
    "  fsd f0, 0(a2)\n"
    "  ret\n"
    ".size rv32_d_traps_misaligned_fld, "
    ".-rv32_d_traps_misaligned_fld\n"

    ".globl rv32_d_traps_misaligned_fsd\n"
    ".type rv32_d_traps_misaligned_fsd, @function\n"
    "rv32_d_traps_misaligned_fsd:\n"
    "  fld f0, 0(a1)\n"
    ".globl rv32_d_traps_misaligned_fsd_insn\n"
    "rv32_d_traps_misaligned_fsd_insn:\n"
    "  fsd f0, 0(a0)\n"
    "  ret\n"
    ".size rv32_d_traps_misaligned_fsd, "
    ".-rv32_d_traps_misaligned_fsd\n"

    ".globl rv32_d_traps_bad_static_rm\n"
    ".type rv32_d_traps_bad_static_rm, @function\n"
    "rv32_d_traps_bad_static_rm:\n"
    "  fld f0, 0(a0)\n"
    "  fld f1, 0(a1)\n"
    "  fld f2, 0(a2)\n"
    ".globl rv32_d_traps_bad_static_rm_insn\n"
    "rv32_d_traps_bad_static_rm_insn:\n"
    "  .word 0x0220d053\n" /* FADD.D f0, f1, f2 with rm=101. */
    "  fsd f0, 0(a3)\n"
    "  ret\n"
    ".size rv32_d_traps_bad_static_rm, "
    ".-rv32_d_traps_bad_static_rm\n"

    ".globl rv32_d_traps_bad_dynamic_rm\n"
    ".type rv32_d_traps_bad_dynamic_rm, @function\n"
    "rv32_d_traps_bad_dynamic_rm:\n"
    "  fld f0, 0(a0)\n"
    "  fld f1, 0(a1)\n"
    "  fld f2, 0(a2)\n"
    ".globl rv32_d_traps_bad_dynamic_rm_insn\n"
    "rv32_d_traps_bad_dynamic_rm_insn:\n"
    "  .word 0x0220f053\n" /* FADD.D f0, f1, f2 with rm=DYN. */
    "  fsd f0, 0(a3)\n"
    "  ret\n"
    ".size rv32_d_traps_bad_dynamic_rm, "
    ".-rv32_d_traps_bad_dynamic_rm\n"

    ".globl rv32_d_traps_bad_sqrt_rs2\n"
    ".type rv32_d_traps_bad_sqrt_rs2, @function\n"
    "rv32_d_traps_bad_sqrt_rs2:\n"
    "  fld f0, 0(a0)\n"
    "  fld f1, 0(a1)\n"
    ".globl rv32_d_traps_bad_sqrt_rs2_insn\n"
    "rv32_d_traps_bad_sqrt_rs2_insn:\n"
    "  .word 0x5a108053\n" /* FSQRT.D f0, f1 with reserved rs2=1. */
    "  fsd f0, 0(a2)\n"
    "  ret\n"
    ".size rv32_d_traps_bad_sqrt_rs2, "
    ".-rv32_d_traps_bad_sqrt_rs2\n"

    /*
     * FCVT.L[U].S and FCVT.L[U].D are RV64-only.  Each probe preloads a0
     * with a literal sentinel immediately before the raw instruction.
     */
    RV32_D_FLOAT_TO_LONG_PROBE(
        rv32_d_traps_fcvt_l_s, rv32_d_traps_fcvt_l_s_insn, 0xc0200553)
    RV32_D_FLOAT_TO_LONG_PROBE(
        rv32_d_traps_fcvt_lu_s, rv32_d_traps_fcvt_lu_s_insn, 0xc0300553)
    RV32_D_FLOAT_TO_LONG_PROBE(
        rv32_d_traps_fcvt_l_d, rv32_d_traps_fcvt_l_d_insn, 0xc2200553)
    RV32_D_FLOAT_TO_LONG_PROBE(
        rv32_d_traps_fcvt_lu_d, rv32_d_traps_fcvt_lu_d_insn, 0xc2300553)

    /*
     * FCVT.S.L[U] and FCVT.D.L[U] are also RV64-only.  The helpers preserve a
     * complete 64-bit FPR sentinel through FLD/FSD if rejection precedes
     * destination writeback.
     */
    RV32_D_LONG_TO_FLOAT_PROBE(
        rv32_d_traps_fcvt_s_l, rv32_d_traps_fcvt_s_l_insn, 0xd0250053)
    RV32_D_LONG_TO_FLOAT_PROBE(
        rv32_d_traps_fcvt_s_lu, rv32_d_traps_fcvt_s_lu_insn, 0xd0350053)
    RV32_D_LONG_TO_FLOAT_PROBE(
        rv32_d_traps_fcvt_d_l, rv32_d_traps_fcvt_d_l_insn, 0xd2250053)
    RV32_D_LONG_TO_FLOAT_PROBE(
        rv32_d_traps_fcvt_d_lu, rv32_d_traps_fcvt_d_lu_insn, 0xd2350053)

    /*
     * The standard provides no whole-double FPR/GPR move when XLEN is 32.
     * These two raw encodings must therefore trap even though D is present.
     */
    RV32_D_FLOAT_TO_LONG_PROBE(
        rv32_d_traps_fmv_x_d, rv32_d_traps_fmv_x_d_insn, 0xe2000553)
    RV32_D_LONG_TO_FLOAT_PROBE(
        rv32_d_traps_fmv_d_x, rv32_d_traps_fmv_d_x_insn, 0xf2050053)

    ".option pop\n");

extern void rv32_d_traps_fs_off(const uint64_t *);
extern char rv32_d_traps_fs_off_insn[];
extern void rv32_d_traps_misaligned_fld(const void *, const uint64_t *,
                                         uint64_t *);
extern char rv32_d_traps_misaligned_fld_insn[];
extern void rv32_d_traps_misaligned_fsd(void *, const uint64_t *);
extern char rv32_d_traps_misaligned_fsd_insn[];
extern void rv32_d_traps_bad_static_rm(const uint64_t *, const uint64_t *,
                                        const uint64_t *, uint64_t *);
extern char rv32_d_traps_bad_static_rm_insn[];
extern void rv32_d_traps_bad_dynamic_rm(const uint64_t *, const uint64_t *,
                                         const uint64_t *, uint64_t *);
extern char rv32_d_traps_bad_dynamic_rm_insn[];
extern void rv32_d_traps_bad_sqrt_rs2(const uint64_t *, const uint64_t *,
                                       uint64_t *);
extern char rv32_d_traps_bad_sqrt_rs2_insn[];

extern uint32_t rv32_d_traps_fcvt_l_s(const uint64_t *, uint32_t);
extern char rv32_d_traps_fcvt_l_s_insn[];
extern uint32_t rv32_d_traps_fcvt_lu_s(const uint64_t *, uint32_t);
extern char rv32_d_traps_fcvt_lu_s_insn[];
extern uint32_t rv32_d_traps_fcvt_l_d(const uint64_t *, uint32_t);
extern char rv32_d_traps_fcvt_l_d_insn[];
extern uint32_t rv32_d_traps_fcvt_lu_d(const uint64_t *, uint32_t);
extern char rv32_d_traps_fcvt_lu_d_insn[];

extern void rv32_d_traps_fcvt_s_l(uint32_t, const uint64_t *, uint64_t *);
extern char rv32_d_traps_fcvt_s_l_insn[];
extern void rv32_d_traps_fcvt_s_lu(uint32_t, const uint64_t *, uint64_t *);
extern char rv32_d_traps_fcvt_s_lu_insn[];
extern void rv32_d_traps_fcvt_d_l(uint32_t, const uint64_t *, uint64_t *);
extern char rv32_d_traps_fcvt_d_l_insn[];
extern void rv32_d_traps_fcvt_d_lu(uint32_t, const uint64_t *, uint64_t *);
extern char rv32_d_traps_fcvt_d_lu_insn[];

extern uint32_t rv32_d_traps_fmv_x_d(const uint64_t *, uint32_t);
extern char rv32_d_traps_fmv_x_d_insn[];
extern void rv32_d_traps_fmv_d_x(uint32_t, const uint64_t *, uint64_t *);
extern char rv32_d_traps_fmv_d_x_insn[];

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

static void write_frm(uintptr_t value)
{
    asm volatile("csrw 0x002, %0" : : "r"(value) : "memory");
}

static void reset_trap_record(void)
{
    rv32_d_traps_count = 0;
    rv32_d_traps_mcause = UINT32_MAX;
    rv32_d_traps_mepc = UINT32_MAX;
    rv32_d_traps_mtval = UINT32_MAX;
}

static void check_illegal_trap(const char *instruction)
{
    check(rv32_d_traps_count == 1);
    check(rv32_d_traps_mcause == CAUSE_ILLEGAL_INSTRUCTION);
    check(rv32_d_traps_mepc == (uintptr_t)instruction);
}

static void check_memory_trap(uint32_t cause, const char *instruction,
                              uintptr_t address)
{
    check(rv32_d_traps_count == 1);
    check(rv32_d_traps_mcause == cause);
    check(rv32_d_traps_mepc == (uintptr_t)instruction);
    check(rv32_d_traps_mtval == address);
}

static void test_fs_off(uintptr_t base_mstatus)
{
    const uint64_t source __attribute__((aligned(8))) =
        UINT64_C(0x3ff0000000000000);

    reset_trap_record();
    write_mstatus((base_mstatus & ~MSTATUS_FS_MASK) | MSTATUS_FS_OFF);
    rv32_d_traps_fs_off(&source);
    check_illegal_trap(rv32_d_traps_fs_off_insn);

    /*
     * The rejected instruction must not silently enable or dirty FP state.
     * This check is performed before restoring Initial for the remaining probes.
     */
    check((read_mstatus() & MSTATUS_FS_MASK) == MSTATUS_FS_OFF);
}

static void test_misaligned_d_memory(void)
{
    uint32_t storage[4] __attribute__((aligned(8))) = {
        UINT32_C(0x11223344),
        UINT32_C(0x55667788),
        UINT32_C(0x99aabbcc),
        UINT32_C(0xddeeff00),
    };
    const uint64_t fpr_sentinel __attribute__((aligned(8))) =
        UINT64_C(0x0123456789abcdef);
    const uint64_t store_source __attribute__((aligned(8))) =
        UINT64_C(0xfedcba9876543210);
    uint64_t observed __attribute__((aligned(8))) = 0;
    uint8_t *const misaligned = (uint8_t *)storage + 1;

    reset_trap_record();
    rv32_d_traps_misaligned_fld(misaligned, &fpr_sentinel, &observed);
    check_memory_trap(CAUSE_LOAD_ADDRESS_MISALIGNED,
                      rv32_d_traps_misaligned_fld_insn,
                      (uintptr_t)misaligned);
    check(observed == UINT64_C(0x0123456789abcdef));

    reset_trap_record();
    rv32_d_traps_misaligned_fsd(misaligned, &store_source);
    check_memory_trap(CAUSE_STORE_ADDRESS_MISALIGNED,
                      rv32_d_traps_misaligned_fsd_insn,
                      (uintptr_t)misaligned);

    /*
     * All four words bracket the attempted eight-byte write.  Their literal
     * values prove FSD checked alignment before committing either 32-bit half.
     */
    check(storage[0] == UINT32_C(0x11223344));
    check(storage[1] == UINT32_C(0x55667788));
    check(storage[2] == UINT32_C(0x99aabbcc));
    check(storage[3] == UINT32_C(0xddeeff00));
}

static void test_reserved_rounding_and_rs2(void)
{
    const uint64_t destination_sentinel __attribute__((aligned(8))) =
        UINT64_C(0x0123456789abcdef);
    const uint64_t one __attribute__((aligned(8))) =
        UINT64_C(0x3ff0000000000000);
    const uint64_t two __attribute__((aligned(8))) =
        UINT64_C(0x4000000000000000);
    uint64_t observed __attribute__((aligned(8))) = 0;

    write_fflags(FFLAG_DZ);
    reset_trap_record();
    rv32_d_traps_bad_static_rm(&destination_sentinel, &one, &two,
                                &observed);
    check_illegal_trap(rv32_d_traps_bad_static_rm_insn);
    check(observed == UINT64_C(0x0123456789abcdef));
    check(read_fflags() == FFLAG_DZ);

    /*
     * rm=DYN is also illegal when frm contains reserved value 5.  Resolution
     * must happen before arithmetic, destination writeback, or flag accrual.
     */
    write_frm(5);
    write_fflags(FFLAG_DZ);
    reset_trap_record();
    rv32_d_traps_bad_dynamic_rm(&destination_sentinel, &one, &two,
                                 &observed);
    check_illegal_trap(rv32_d_traps_bad_dynamic_rm_insn);
    check(observed == UINT64_C(0x0123456789abcdef));
    check(read_fflags() == FFLAG_DZ);
    write_frm(0);

    /* FSQRT.D reserves rs2 and must reject one before touching f0. */
    write_fflags(FFLAG_DZ);
    reset_trap_record();
    rv32_d_traps_bad_sqrt_rs2(&destination_sentinel, &two, &observed);
    check_illegal_trap(rv32_d_traps_bad_sqrt_rs2_insn);
    check(observed == UINT64_C(0x0123456789abcdef));
    check(read_fflags() == FFLAG_DZ);
    write_fflags(0);
}

static void check_float_to_long_probes(void)
{
    const uint64_t boxed_single_one __attribute__((aligned(8))) =
        UINT64_C(0xffffffff3f800000);
    const uint64_t double_one __attribute__((aligned(8))) =
        UINT64_C(0x3ff0000000000000);
    const uint32_t gpr_sentinel = UINT32_C(0x13579bdf);

#define CHECK_FLOAT_TO_LONG(function, instruction, source) \
    do \
    { \
        reset_trap_record(); \
        check(function(&(source), gpr_sentinel) == gpr_sentinel); \
        check_illegal_trap(instruction); \
    } while (0)

    CHECK_FLOAT_TO_LONG(rv32_d_traps_fcvt_l_s,
                        rv32_d_traps_fcvt_l_s_insn,
                        boxed_single_one);
    CHECK_FLOAT_TO_LONG(rv32_d_traps_fcvt_lu_s,
                        rv32_d_traps_fcvt_lu_s_insn,
                        boxed_single_one);
    CHECK_FLOAT_TO_LONG(rv32_d_traps_fcvt_l_d,
                        rv32_d_traps_fcvt_l_d_insn,
                        double_one);
    CHECK_FLOAT_TO_LONG(rv32_d_traps_fcvt_lu_d,
                        rv32_d_traps_fcvt_lu_d_insn,
                        double_one);
    CHECK_FLOAT_TO_LONG(rv32_d_traps_fmv_x_d,
                        rv32_d_traps_fmv_x_d_insn,
                        double_one);

#undef CHECK_FLOAT_TO_LONG
}

static void check_long_to_float_probes(void)
{
    const uint64_t fpr_sentinel __attribute__((aligned(8))) =
        UINT64_C(0x0123456789abcdef);
    const uint32_t integer_source = UINT32_C(2);
    uint64_t observed __attribute__((aligned(8))) = 0;

#define CHECK_LONG_TO_FLOAT(function, instruction) \
    do \
    { \
        observed = 0; \
        reset_trap_record(); \
        function(integer_source, &fpr_sentinel, &observed); \
        check_illegal_trap(instruction); \
        check(observed == UINT64_C(0x0123456789abcdef)); \
    } while (0)

    CHECK_LONG_TO_FLOAT(rv32_d_traps_fcvt_s_l,
                        rv32_d_traps_fcvt_s_l_insn);
    CHECK_LONG_TO_FLOAT(rv32_d_traps_fcvt_s_lu,
                        rv32_d_traps_fcvt_s_lu_insn);
    CHECK_LONG_TO_FLOAT(rv32_d_traps_fcvt_d_l,
                        rv32_d_traps_fcvt_d_l_insn);
    CHECK_LONG_TO_FLOAT(rv32_d_traps_fcvt_d_lu,
                        rv32_d_traps_fcvt_d_lu_insn);
    CHECK_LONG_TO_FLOAT(rv32_d_traps_fmv_d_x,
                        rv32_d_traps_fmv_d_x_insn);

#undef CHECK_LONG_TO_FLOAT
}

#endif

int main(void)
{
#if defined(__riscv) && __riscv_xlen == 32
    const uintptr_t old_mstatus = read_mstatus();
    const uintptr_t old_mtvec = read_mtvec();

    write_mtvec((uintptr_t)rv32_d_traps_handler);
    test_fs_off(old_mstatus);

    write_mstatus((old_mstatus & ~MSTATUS_FS_MASK) | MSTATUS_FS_INITIAL);
    test_misaligned_d_memory();
    test_reserved_rounding_and_rs2();
    check_float_to_long_probes();
    check_long_to_float_probes();

    write_mstatus(old_mstatus);
    write_mtvec(old_mtvec);
#endif

    return 0;
}
