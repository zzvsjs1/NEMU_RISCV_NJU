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

asm(".section .text\n"
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

asm(".section .text\n"
    ".align 2\n"
    ".option push\n"
    ".option norvc\n"
    ".option arch, +f\n"
    ".option arch, +d\n"

    /*
     * Keep mtvec equal to the sequential PC so the JIT must consume the
     * executor's explicit trap result instead of comparing PC values.
     */
    ".globl rv64_fpu_sequential_mtvec_probe\n"
    ".type rv64_fpu_sequential_mtvec_probe, @function\n"
    "rv64_fpu_sequential_mtvec_probe:\n"
    ".globl rv64_fpu_sequential_mtvec_insn\n"
    "rv64_fpu_sequential_mtvec_insn:\n"
    "  .word 0xf0050053\n" /* FMV.W.X f0, a0 */
    ".globl rv64_fpu_sequential_mtvec_vector\n"
    "rv64_fpu_sequential_mtvec_vector:\n"
    "  j rv64_fpu_sequential_mtvec_handler\n"
    "rv64_fpu_sequential_mtvec_resume:\n"
    "  ret\n"
    ".size rv64_fpu_sequential_mtvec_probe, "
    ".-rv64_fpu_sequential_mtvec_probe\n"

    ".type rv64_fpu_sequential_mtvec_handler, @function\n"
    "rv64_fpu_sequential_mtvec_handler:\n"
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
    "  la t1, rv64_fpu_sequential_mtvec_resume\n"
    "  csrw mepc, t1\n"
    "  mret\n"
    ".size rv64_fpu_sequential_mtvec_handler, "
    ".-rv64_fpu_sequential_mtvec_handler\n"

    ".globl rv64_fpu_fs_off_probe\n"
    ".type rv64_fpu_fs_off_probe, @function\n"
    "rv64_fpu_fs_off_probe:\n"
    /*
     * Both additions precede the guarded FP operation, leaving dirty cached
     * GPR values and a positive retired prefix. The suffix consumes both
     * values after either native completion or trap recovery.
     */
    "  addi a0, a0, 1\n"
    "  addi a1, a1, 2\n"
    ".globl rv64_fpu_fs_off_insn\n"
    "rv64_fpu_fs_off_insn:\n"
    "  .word 0xf0050053\n" /* FMV.W.X f0, a0 */
    "  add a0, a0, a1\n"
    "  ret\n"
    ".size rv64_fpu_fs_off_probe, .-rv64_fpu_fs_off_probe\n"

    /*
     * Suppressing an integer result with rd=x0 must not suppress the FS
     * permission check. FCLASS is separately covered because it is also
     * read-only, while FSGNJ covers an FPR-writing exact operation.
     */
    ".globl rv64_fpu_fs_off_move_x_zero\n"
    ".type rv64_fpu_fs_off_move_x_zero, @function\n"
    "rv64_fpu_fs_off_move_x_zero:\n"
    ".globl rv64_fpu_fs_off_move_x_zero_insn\n"
    "rv64_fpu_fs_off_move_x_zero_insn:\n"
    "  .word 0xe0000053\n" /* FMV.X.W zero, f0 */
    "  ret\n"
    ".size rv64_fpu_fs_off_move_x_zero, "
    ".-rv64_fpu_fs_off_move_x_zero\n"

    ".globl rv64_fpu_fs_off_class_zero\n"
    ".type rv64_fpu_fs_off_class_zero, @function\n"
    "rv64_fpu_fs_off_class_zero:\n"
    ".globl rv64_fpu_fs_off_class_zero_insn\n"
    "rv64_fpu_fs_off_class_zero_insn:\n"
    "  .word 0xe0001053\n" /* FCLASS.S zero, f0 */
    "  ret\n"
    ".size rv64_fpu_fs_off_class_zero, "
    ".-rv64_fpu_fs_off_class_zero\n"

    ".globl rv64_fpu_fs_off_sgnj\n"
    ".type rv64_fpu_fs_off_sgnj, @function\n"
    "rv64_fpu_fs_off_sgnj:\n"
    ".globl rv64_fpu_fs_off_sgnj_insn\n"
    "rv64_fpu_fs_off_sgnj_insn:\n"
    "  .word 0x20000053\n" /* FSGNJ.S f0, f0, f0 */
    "  ret\n"
    ".size rv64_fpu_fs_off_sgnj, .-rv64_fpu_fs_off_sgnj\n"

    /*
     * These encodings resemble the native exact tier but retain architecturally
     * reserved fields. They must reach the precise illegal-instruction path,
     * never a permissive fast-path decoder.
     */
    ".globl rv64_fpu_bad_fmv_x_w_rs2\n"
    ".type rv64_fpu_bad_fmv_x_w_rs2, @function\n"
    "rv64_fpu_bad_fmv_x_w_rs2:\n"
    ".globl rv64_fpu_bad_fmv_x_w_rs2_insn\n"
    "rv64_fpu_bad_fmv_x_w_rs2_insn:\n"
    "  .word 0xe0100553\n" /* FMV.X.W-like encoding with rs2=1 */
    "  ret\n"
    ".size rv64_fpu_bad_fmv_x_w_rs2, "
    ".-rv64_fpu_bad_fmv_x_w_rs2\n"

    ".globl rv64_fpu_bad_fclass_s_rs2\n"
    ".type rv64_fpu_bad_fclass_s_rs2, @function\n"
    "rv64_fpu_bad_fclass_s_rs2:\n"
    ".globl rv64_fpu_bad_fclass_s_rs2_insn\n"
    "rv64_fpu_bad_fclass_s_rs2_insn:\n"
    "  .word 0xe0101553\n" /* FCLASS.S-like encoding with rs2=1 */
    "  ret\n"
    ".size rv64_fpu_bad_fclass_s_rs2, "
    ".-rv64_fpu_bad_fclass_s_rs2\n"

    ".globl rv64_fpu_bad_fmv_w_x_rs2\n"
    ".type rv64_fpu_bad_fmv_w_x_rs2, @function\n"
    "rv64_fpu_bad_fmv_w_x_rs2:\n"
    ".globl rv64_fpu_bad_fmv_w_x_rs2_insn\n"
    "rv64_fpu_bad_fmv_w_x_rs2_insn:\n"
    "  .word 0xf0150053\n" /* FMV.W.X-like encoding with rs2=1 */
    "  ret\n"
    ".size rv64_fpu_bad_fmv_w_x_rs2, "
    ".-rv64_fpu_bad_fmv_w_x_rs2\n"

    ".globl rv64_fpu_bad_fsgnj_s_funct3\n"
    ".type rv64_fpu_bad_fsgnj_s_funct3, @function\n"
    "rv64_fpu_bad_fsgnj_s_funct3:\n"
    ".globl rv64_fpu_bad_fsgnj_s_funct3_insn\n"
    "rv64_fpu_bad_fsgnj_s_funct3_insn:\n"
    "  .word 0x20003053\n" /* FSGNJ.S-like encoding with funct3=3 */
    "  ret\n"
    ".size rv64_fpu_bad_fsgnj_s_funct3, "
    ".-rv64_fpu_bad_fsgnj_s_funct3\n"

    ".globl rv64_fpu_bad_fmv_x_d_rs2\n"
    ".type rv64_fpu_bad_fmv_x_d_rs2, @function\n"
    "rv64_fpu_bad_fmv_x_d_rs2:\n"
    ".globl rv64_fpu_bad_fmv_x_d_rs2_insn\n"
    "rv64_fpu_bad_fmv_x_d_rs2_insn:\n"
    "  .word 0xe2100553\n" /* FMV.X.D-like encoding with rs2=1 */
    "  ret\n"
    ".size rv64_fpu_bad_fmv_x_d_rs2, "
    ".-rv64_fpu_bad_fmv_x_d_rs2\n"

    ".globl rv64_fpu_bad_fclass_d_rs2\n"
    ".type rv64_fpu_bad_fclass_d_rs2, @function\n"
    "rv64_fpu_bad_fclass_d_rs2:\n"
    ".globl rv64_fpu_bad_fclass_d_rs2_insn\n"
    "rv64_fpu_bad_fclass_d_rs2_insn:\n"
    "  .word 0xe2101553\n" /* FCLASS.D-like encoding with rs2=1 */
    "  ret\n"
    ".size rv64_fpu_bad_fclass_d_rs2, "
    ".-rv64_fpu_bad_fclass_d_rs2\n"

    ".globl rv64_fpu_bad_fmv_d_x_rs2\n"
    ".type rv64_fpu_bad_fmv_d_x_rs2, @function\n"
    "rv64_fpu_bad_fmv_d_x_rs2:\n"
    ".globl rv64_fpu_bad_fmv_d_x_rs2_insn\n"
    "rv64_fpu_bad_fmv_d_x_rs2_insn:\n"
    "  .word 0xf2150053\n" /* FMV.D.X-like encoding with rs2=1 */
    "  ret\n"
    ".size rv64_fpu_bad_fmv_d_x_rs2, "
    ".-rv64_fpu_bad_fmv_d_x_rs2\n"

    ".globl rv64_fpu_bad_fsgnj_d_funct3\n"
    ".type rv64_fpu_bad_fsgnj_d_funct3, @function\n"
    "rv64_fpu_bad_fsgnj_d_funct3:\n"
    ".globl rv64_fpu_bad_fsgnj_d_funct3_insn\n"
    "rv64_fpu_bad_fsgnj_d_funct3_insn:\n"
    "  .word 0x22003053\n" /* FSGNJ.D-like encoding with funct3=3 */
    "  ret\n"
    ".size rv64_fpu_bad_fsgnj_d_funct3, "
    ".-rv64_fpu_bad_fsgnj_d_funct3\n"

    ".globl rv64_fpu_bad_static_rm\n"
    ".type rv64_fpu_bad_static_rm, @function\n"
    "rv64_fpu_bad_static_rm:\n"
    "  fmv.w.x f0, a0\n"
    "  fmv.w.x f1, a1\n"
    "  fmv.w.x f2, a2\n"
    ".globl rv64_fpu_bad_static_rm_insn\n"
    "rv64_fpu_bad_static_rm_insn:\n"
    "  .word 0x0020d053\n" /* FADD.S; NEMU chooses to trap reserved rm=101 */
    "  fmv.x.d a0, f0\n"
    "  ret\n"
    ".size rv64_fpu_bad_static_rm, .-rv64_fpu_bad_static_rm\n"

    /*
     * A classified arithmetic helper reads no GPR, so the optimised path may
     * carry these six dirty values through the C call.  The specification
     * reserves rm=101 rather than mandating one outcome; NEMU's permitted
     * policy is to raise an illegal-instruction trap.  The generated trap-only
     * stub must therefore publish the values before the guest handler and
     * resumed suffix run from separate native entries.  The trap handler
     * intentionally uses only t0/t1.
     */
    ".globl rv64_fpu_bad_static_rm_dirty\n"
    ".type rv64_fpu_bad_static_rm_dirty, @function\n"
    "rv64_fpu_bad_static_rm_dirty:\n"
    "  addi t2, zero, 0x112\n"
    "  addi t3, zero, 0x223\n"
    "  addi t4, zero, 0x334\n"
    "  addi t5, zero, 0x445\n"
    "  addi t6, zero, 0x556\n"
    "  addi a2, zero, 0x667\n"
    ".globl rv64_fpu_bad_static_rm_dirty_insn\n"
    "rv64_fpu_bad_static_rm_dirty_insn:\n"
    "  .word 0x0020d053\n" /* FADD.S; NEMU chooses to trap reserved rm=101 */
    "  add a0, t2, t3\n"
    "  add a0, a0, t4\n"
    "  add a0, a0, t5\n"
    "  add a0, a0, t6\n"
    "  add a0, a0, a2\n"
    "  ret\n"
    ".size rv64_fpu_bad_static_rm_dirty, "
    ".-rv64_fpu_bad_static_rm_dirty\n"

    /*
     * The potential success edge writes t2, but NEMU's permitted policy traps
     * on reserved rm=101 before that writeback.  The trap stub must therefore
     * publish the dirty old t2 value before success-only cache invalidation is
     * applied to compiler metadata.
     */
    ".globl rv64_fpu_bad_fcvt_dirty_rd\n"
    ".type rv64_fpu_bad_fcvt_dirty_rd, @function\n"
    "rv64_fpu_bad_fcvt_dirty_rd:\n"
    "  addi t2, zero, 0x345\n"
    ".globl rv64_fpu_bad_fcvt_dirty_rd_insn\n"
    "rv64_fpu_bad_fcvt_dirty_rd_insn:\n"
    "  .word 0xc20053d3\n" /* FCVT.W.D; NEMU traps reserved rm=101 */
    "  addi a0, t2, 0\n"
    "  ret\n"
    ".size rv64_fpu_bad_fcvt_dirty_rd, "
    ".-rv64_fpu_bad_fcvt_dirty_rd\n"

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

    /*
     * Fill all six ordinary JIT GPR-cache slots with dirty values, then use an
     * uncached seventh register as the misaligned FLW base. Materialising a0
     * must spill the oldest value without allowing the alignment side exit to
     * flush a stale pre-eviction cache description over that correct spill.
     * The trap handler uses only t0/t1, leaving all six observed registers
     * available to the suffix.
     */
    ".globl rv64_fpu_misaligned_cache_eviction\n"
    ".type rv64_fpu_misaligned_cache_eviction, @function\n"
    "rv64_fpu_misaligned_cache_eviction:\n"
    "  addi t2, zero, 0x112\n"
    "  addi t3, zero, 0x223\n"
    "  addi t4, zero, 0x334\n"
    "  addi t5, zero, 0x445\n"
    "  addi t6, zero, 0x556\n"
    "  addi a2, zero, 0x667\n"
    ".globl rv64_fpu_misaligned_cache_eviction_insn\n"
    "rv64_fpu_misaligned_cache_eviction_insn:\n"
    "  flw f31, 0(a0)\n"
    "  sd t2, 0(a1)\n"
    "  sd t3, 8(a1)\n"
    "  sd t4, 16(a1)\n"
    "  sd t5, 24(a1)\n"
    "  sd t6, 32(a1)\n"
    "  sd a2, 40(a1)\n"
    "  ret\n"
    ".size rv64_fpu_misaligned_cache_eviction, "
    ".-rv64_fpu_misaligned_cache_eviction\n"

    /*
     * Repeat the eviction shape for the dedicated FP-store emitter. Seed the
     * source FPR first, then make all six resident GPR values dirty before the
     * uncached base read and pre-store alignment exit.
     */
    ".globl rv64_fpu_misaligned_store_cache_eviction\n"
    ".type rv64_fpu_misaligned_store_cache_eviction, @function\n"
    "rv64_fpu_misaligned_store_cache_eviction:\n"
    "  fmv.w.x f30, a2\n"
    "  addi t2, zero, 0x112\n"
    "  addi t3, zero, 0x223\n"
    "  addi t4, zero, 0x334\n"
    "  addi t5, zero, 0x445\n"
    "  addi t6, zero, 0x556\n"
    "  addi a2, zero, 0x667\n"
    ".globl rv64_fpu_misaligned_store_cache_eviction_insn\n"
    "rv64_fpu_misaligned_store_cache_eviction_insn:\n"
    "  fsw f30, 0(a0)\n"
    "  sd t2, 0(a1)\n"
    "  sd t3, 8(a1)\n"
    "  sd t4, 16(a1)\n"
    "  sd t5, 24(a1)\n"
    "  sd t6, 32(a1)\n"
    "  sd a2, 40(a1)\n"
    "  ret\n"
    ".size rv64_fpu_misaligned_store_cache_eviction, "
    ".-rv64_fpu_misaligned_store_cache_eviction\n"

    /*
     * Each probe is first executed with FS enabled, then through the same
     * compiled PC with FS Off and a deliberately misaligned address. Illegal
     * instruction must win over alignment, while both dirty integer prefix
     * values remain visible to the suffix after trap recovery.
     */
    ".globl rv64_fpu_fs_off_flw\n"
    ".type rv64_fpu_fs_off_flw, @function\n"
    "rv64_fpu_fs_off_flw:\n"
    "  addi a1, a1, 1\n"
    "  addi a2, a2, 2\n"
    ".globl rv64_fpu_fs_off_flw_insn\n"
    "rv64_fpu_fs_off_flw_insn:\n"
    "  flw f8, 0(a0)\n"
    "  add a0, a1, a2\n"
    "  ret\n"
    ".size rv64_fpu_fs_off_flw, .-rv64_fpu_fs_off_flw\n"

    ".globl rv64_fpu_fs_off_fld\n"
    ".type rv64_fpu_fs_off_fld, @function\n"
    "rv64_fpu_fs_off_fld:\n"
    "  addi a1, a1, 1\n"
    "  addi a2, a2, 2\n"
    ".globl rv64_fpu_fs_off_fld_insn\n"
    "rv64_fpu_fs_off_fld_insn:\n"
    "  fld f9, 0(a0)\n"
    "  add a0, a1, a2\n"
    "  ret\n"
    ".size rv64_fpu_fs_off_fld, .-rv64_fpu_fs_off_fld\n"

    ".globl rv64_fpu_fs_off_fsw\n"
    ".type rv64_fpu_fs_off_fsw, @function\n"
    "rv64_fpu_fs_off_fsw:\n"
    "  addi a1, a1, 1\n"
    "  addi a2, a2, 2\n"
    ".globl rv64_fpu_fs_off_fsw_insn\n"
    "rv64_fpu_fs_off_fsw_insn:\n"
    "  fsw f10, 0(a0)\n"
    "  add a0, a1, a2\n"
    "  ret\n"
    ".size rv64_fpu_fs_off_fsw, .-rv64_fpu_fs_off_fsw\n"

    ".globl rv64_fpu_fs_off_fsd\n"
    ".type rv64_fpu_fs_off_fsd, @function\n"
    "rv64_fpu_fs_off_fsd:\n"
    "  addi a1, a1, 1\n"
    "  addi a2, a2, 2\n"
    ".globl rv64_fpu_fs_off_fsd_insn\n"
    "rv64_fpu_fs_off_fsd_insn:\n"
    "  fsd f11, 0(a0)\n"
    "  add a0, a1, a2\n"
    "  ret\n"
    ".size rv64_fpu_fs_off_fsd, .-rv64_fpu_fs_off_fsd\n"

    ".globl rv64_fpu_seed_fs_off_load_regs\n"
    ".type rv64_fpu_seed_fs_off_load_regs, @function\n"
    "rv64_fpu_seed_fs_off_load_regs:\n"
    "  fmv.d.x f8, a0\n"
    "  fmv.d.x f9, a1\n"
    "  ret\n"
    ".size rv64_fpu_seed_fs_off_load_regs, "
    ".-rv64_fpu_seed_fs_off_load_regs\n"

    ".globl rv64_fpu_read_fs_off_f8\n"
    ".type rv64_fpu_read_fs_off_f8, @function\n"
    "rv64_fpu_read_fs_off_f8:\n"
    "  fmv.x.d a0, f8\n"
    "  ret\n"
    ".size rv64_fpu_read_fs_off_f8, .-rv64_fpu_read_fs_off_f8\n"

    ".globl rv64_fpu_read_fs_off_f9\n"
    ".type rv64_fpu_read_fs_off_f9, @function\n"
    "rv64_fpu_read_fs_off_f9:\n"
    "  fmv.x.d a0, f9\n"
    "  ret\n"
    ".size rv64_fpu_read_fs_off_f9, .-rv64_fpu_read_fs_off_f9\n"

    ".globl rv64_fpu_seed_fs_off_store_regs\n"
    ".type rv64_fpu_seed_fs_off_store_regs, @function\n"
    "rv64_fpu_seed_fs_off_store_regs:\n"
    "  fmv.d.x f10, a0\n"
    "  fmv.d.x f11, a1\n"
    "  ret\n"
    ".size rv64_fpu_seed_fs_off_store_regs, "
    ".-rv64_fpu_seed_fs_off_store_regs\n"

    ".option pop\n");

extern void rv64_fpu_sequential_mtvec_probe(uint32_t);
extern char rv64_fpu_sequential_mtvec_insn[];
extern char rv64_fpu_sequential_mtvec_vector[];
extern uint64_t rv64_fpu_fs_off_probe(uint64_t, uint64_t);
extern char rv64_fpu_fs_off_insn[];
extern void rv64_fpu_fs_off_move_x_zero(void);
extern char rv64_fpu_fs_off_move_x_zero_insn[];
extern void rv64_fpu_fs_off_class_zero(void);
extern char rv64_fpu_fs_off_class_zero_insn[];
extern void rv64_fpu_fs_off_sgnj(void);
extern char rv64_fpu_fs_off_sgnj_insn[];
extern void rv64_fpu_bad_fmv_x_w_rs2(void);
extern char rv64_fpu_bad_fmv_x_w_rs2_insn[];
extern void rv64_fpu_bad_fclass_s_rs2(void);
extern char rv64_fpu_bad_fclass_s_rs2_insn[];
extern void rv64_fpu_bad_fmv_w_x_rs2(void);
extern char rv64_fpu_bad_fmv_w_x_rs2_insn[];
extern void rv64_fpu_bad_fsgnj_s_funct3(void);
extern char rv64_fpu_bad_fsgnj_s_funct3_insn[];
extern void rv64_fpu_bad_fmv_x_d_rs2(void);
extern char rv64_fpu_bad_fmv_x_d_rs2_insn[];
extern void rv64_fpu_bad_fclass_d_rs2(void);
extern char rv64_fpu_bad_fclass_d_rs2_insn[];
extern void rv64_fpu_bad_fmv_d_x_rs2(void);
extern char rv64_fpu_bad_fmv_d_x_rs2_insn[];
extern void rv64_fpu_bad_fsgnj_d_funct3(void);
extern char rv64_fpu_bad_fsgnj_d_funct3_insn[];
extern uint64_t rv64_fpu_bad_static_rm(uint32_t, uint32_t, uint32_t);
extern char rv64_fpu_bad_static_rm_insn[];
extern uint64_t rv64_fpu_bad_static_rm_dirty(void);
extern char rv64_fpu_bad_static_rm_dirty_insn[];
extern uint64_t rv64_fpu_bad_fcvt_dirty_rd(void);
extern char rv64_fpu_bad_fcvt_dirty_rd_insn[];
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
extern void rv64_fpu_misaligned_cache_eviction(const void *, uint64_t observed[6]);
extern char rv64_fpu_misaligned_cache_eviction_insn[];
extern void rv64_fpu_misaligned_store_cache_eviction(void *, uint64_t observed[6], uint32_t);
extern char rv64_fpu_misaligned_store_cache_eviction_insn[];
extern uint64_t rv64_fpu_fs_off_flw(const void *, uint64_t, uint64_t);
extern char rv64_fpu_fs_off_flw_insn[];
extern uint64_t rv64_fpu_fs_off_fld(const void *, uint64_t, uint64_t);
extern char rv64_fpu_fs_off_fld_insn[];
extern uint64_t rv64_fpu_fs_off_fsw(void *, uint64_t, uint64_t);
extern char rv64_fpu_fs_off_fsw_insn[];
extern uint64_t rv64_fpu_fs_off_fsd(void *, uint64_t, uint64_t);
extern char rv64_fpu_fs_off_fsd_insn[];
extern void rv64_fpu_seed_fs_off_load_regs(uint64_t, uint64_t);
extern uint64_t rv64_fpu_read_fs_off_f8(void);
extern uint64_t rv64_fpu_read_fs_off_f9(void);
extern void rv64_fpu_seed_fs_off_store_regs(uint64_t, uint64_t);

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

static void check_trap(uint64_t cause, const char *instruction, uintptr_t expected_tval)
{
    check(rv64_fpu_trap_count == 1);
    check(rv64_fpu_trap_mcause == cause);
    check(rv64_fpu_trap_mepc == (uintptr_t)instruction);
    check(rv64_fpu_trap_mtval == expected_tval);
}

static void test_fs_off_and_reserved_rounding(uintptr_t base_mstatus)
{
    const uint64_t boxed_sentinel = UINT64_C(0xffffffff3f000000);

    /*
     * Warm the exact same PC while FS is enabled. The second call must reuse
     * that compiled block after FS changes to Off; this is the case which
     * requires the emitted run-time guard because FS is not part of the JIT
     * cache key.
     */
    write_mstatus((base_mstatus & ~MSTATUS_FS_MASK) | MSTATUS_FS_INITIAL);
    check(rv64_fpu_fs_off_probe(10, 20) == 33);

    reset_trap();
    write_mstatus((base_mstatus & ~MSTATUS_FS_MASK) | MSTATUS_FS_OFF);
    check(rv64_fpu_fs_off_probe(10, 20) == 33);
    check_trap(2, rv64_fpu_fs_off_insn, 0);

    reset_trap();
    rv64_fpu_fs_off_move_x_zero();
    check_trap(2, rv64_fpu_fs_off_move_x_zero_insn, 0);

    reset_trap();
    rv64_fpu_fs_off_class_zero();
    check_trap(2, rv64_fpu_fs_off_class_zero_insn, 0);

    reset_trap();
    rv64_fpu_fs_off_sgnj();
    check_trap(2, rv64_fpu_fs_off_sgnj_insn, 0);

    reset_trap();
    write_mtvec((uintptr_t)rv64_fpu_sequential_mtvec_vector);
    rv64_fpu_sequential_mtvec_probe(UINT32_C(0x3f800000));
    check_trap(2, rv64_fpu_sequential_mtvec_insn, 0);
    write_mtvec((uintptr_t)rv64_fpu_trap_handler);

    write_mstatus((base_mstatus & ~MSTATUS_FS_MASK) | MSTATUS_FS_INITIAL);
    reset_trap();
    check(rv64_fpu_bad_static_rm(UINT32_C(0x3f000000), UINT32_C(0x3f800000), UINT32_C(0x40000000)) == boxed_sentinel);
    check_trap(2, rv64_fpu_bad_static_rm_insn, 0);

    reset_trap();
    check(rv64_fpu_bad_static_rm_dirty() == UINT64_C(0x166b));
    check_trap(2, rv64_fpu_bad_static_rm_dirty_insn, 0);

    reset_trap();
    check(rv64_fpu_bad_fcvt_dirty_rd() == UINT64_C(0x345));
    check_trap(2, rv64_fpu_bad_fcvt_dirty_rd_insn, 0);

    write_frm(5);
    reset_trap();
    check(rv64_fpu_bad_dynamic_rm(UINT32_C(0x3f000000), UINT32_C(0x3f800000), UINT32_C(0x40000000)) == boxed_sentinel);
    check_trap(2, rv64_fpu_bad_dynamic_rm_insn, 0);
    write_frm(0);
}

static void test_reserved_exact_encodings(uintptr_t base_mstatus)
{
#define CHECK_BAD_EXACT(probe, instruction) \
    do \
    { \
        reset_trap(); \
        probe(); \
        check_trap(2, instruction, 0); \
    } while (0)

    write_mstatus((base_mstatus & ~MSTATUS_FS_MASK) | MSTATUS_FS_INITIAL);

    CHECK_BAD_EXACT(rv64_fpu_bad_fmv_x_w_rs2, rv64_fpu_bad_fmv_x_w_rs2_insn);
    CHECK_BAD_EXACT(rv64_fpu_bad_fclass_s_rs2, rv64_fpu_bad_fclass_s_rs2_insn);
    CHECK_BAD_EXACT(rv64_fpu_bad_fmv_w_x_rs2, rv64_fpu_bad_fmv_w_x_rs2_insn);
    CHECK_BAD_EXACT(rv64_fpu_bad_fsgnj_s_funct3, rv64_fpu_bad_fsgnj_s_funct3_insn);
    CHECK_BAD_EXACT(rv64_fpu_bad_fmv_x_d_rs2, rv64_fpu_bad_fmv_x_d_rs2_insn);
    CHECK_BAD_EXACT(rv64_fpu_bad_fclass_d_rs2, rv64_fpu_bad_fclass_d_rs2_insn);
    CHECK_BAD_EXACT(rv64_fpu_bad_fmv_d_x_rs2, rv64_fpu_bad_fmv_d_x_rs2_insn);
    CHECK_BAD_EXACT(rv64_fpu_bad_fsgnj_d_funct3, rv64_fpu_bad_fsgnj_d_funct3_insn);

#undef CHECK_BAD_EXACT
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
    uint64_t observed_cache_values[6] = {0};
    static const uint64_t expected_cache_values[6] = {
        UINT64_C(0x112), UINT64_C(0x223), UINT64_C(0x334), UINT64_C(0x445), UINT64_C(0x556), UINT64_C(0x667),
    };

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

    reset_trap();
    rv64_fpu_misaligned_cache_eviction(misaligned, observed_cache_values);
    check_trap(4, rv64_fpu_misaligned_cache_eviction_insn, (uintptr_t)misaligned);
    for (uint32_t i = 0; i < 6; i++)
    {
        check(observed_cache_values[i] == expected_cache_values[i]);
    }

    for (uint32_t i = 0; i < 6; i++)
    {
        observed_cache_values[i] = 0;
    }

    reset_trap();
    rv64_fpu_misaligned_store_cache_eviction(misaligned, observed_cache_values, UINT32_C(0xdeadbeef));
    check_trap(6, rv64_fpu_misaligned_store_cache_eviction_insn, (uintptr_t)misaligned);
    check(storage[0] == UINT64_C(0x1122334455667788));
    check(storage[1] == UINT64_C(0x99aabbccddeeff00));
    for (uint32_t i = 0; i < 6; i++)
    {
        check(observed_cache_values[i] == expected_cache_values[i]);
    }
}

static void test_warmed_fs_off_memory(uintptr_t base_mstatus)
{
    const uint64_t f8_sentinel = UINT64_C(0x0123456789abcdef);
    const uint64_t f9_sentinel = UINT64_C(0xfedcba9876543210);
    const uint32_t word_source = UINT32_C(0x7f800123);
    const uint64_t double_source = UINT64_C(0x7ff0000000000456);
    uint32_t word_destination = 0;
    uint64_t double_destination = 0;

    write_mstatus((base_mstatus & ~MSTATUS_FS_MASK) | MSTATUS_FS_INITIAL);
    check(rv64_fpu_fs_off_flw(&word_source, 10, 20) == 33);
    rv64_fpu_seed_fs_off_load_regs(f8_sentinel, f9_sentinel);

    reset_trap();
    write_mstatus((base_mstatus & ~MSTATUS_FS_MASK) | MSTATUS_FS_OFF);
    check(rv64_fpu_fs_off_flw((const uint8_t *)&word_source + 1, 10, 20) == 33);
    check_trap(2, rv64_fpu_fs_off_flw_insn, 0);

    write_mstatus((base_mstatus & ~MSTATUS_FS_MASK) | MSTATUS_FS_INITIAL);
    check(rv64_fpu_read_fs_off_f8() == f8_sentinel);
    check(rv64_fpu_fs_off_fld(&double_source, 10, 20) == 33);
    rv64_fpu_seed_fs_off_load_regs(f8_sentinel, f9_sentinel);

    reset_trap();
    write_mstatus((base_mstatus & ~MSTATUS_FS_MASK) | MSTATUS_FS_OFF);
    check(rv64_fpu_fs_off_fld((const uint8_t *)&double_source + 1, 10, 20) == 33);
    check_trap(2, rv64_fpu_fs_off_fld_insn, 0);

    write_mstatus((base_mstatus & ~MSTATUS_FS_MASK) | MSTATUS_FS_INITIAL);
    check(rv64_fpu_read_fs_off_f9() == f9_sentinel);
    rv64_fpu_seed_fs_off_store_regs(UINT64_C(0x012345677f800123), UINT64_C(0xfff8000000000789));
    check(rv64_fpu_fs_off_fsw(&word_destination, 10, 20) == 33);
    check(word_destination == UINT32_C(0x7f800123));
    word_destination = UINT32_C(0xa5a55a5a);

    reset_trap();
    write_mstatus((base_mstatus & ~MSTATUS_FS_MASK) | MSTATUS_FS_OFF);
    check(rv64_fpu_fs_off_fsw((uint8_t *)&word_destination + 1, 10, 20) == 33);
    check_trap(2, rv64_fpu_fs_off_fsw_insn, 0);
    check(word_destination == UINT32_C(0xa5a55a5a));

    write_mstatus((base_mstatus & ~MSTATUS_FS_MASK) | MSTATUS_FS_INITIAL);
    rv64_fpu_seed_fs_off_store_regs(UINT64_C(0x012345677f800123), UINT64_C(0xfff8000000000789));
    check(rv64_fpu_fs_off_fsd(&double_destination, 10, 20) == 33);
    check(double_destination == UINT64_C(0xfff8000000000789));
    double_destination = UINT64_C(0x1122334455667788);

    reset_trap();
    write_mstatus((base_mstatus & ~MSTATUS_FS_MASK) | MSTATUS_FS_OFF);
    check(rv64_fpu_fs_off_fsd((uint8_t *)&double_destination + 1, 10, 20) == 33);
    check_trap(2, rv64_fpu_fs_off_fsd_insn, 0);
    check(double_destination == UINT64_C(0x1122334455667788));
}

#endif

int main(void)
{
#if defined(__riscv) && __riscv_xlen == 64
    const uintptr_t old_mstatus = read_mstatus();
    const uintptr_t old_mtvec = read_mtvec();

    write_mtvec((uintptr_t)rv64_fpu_trap_handler);
    test_fs_off_and_reserved_rounding(old_mstatus);
    test_reserved_exact_encodings(old_mstatus);
    test_misaligned_memory_traps();
    test_warmed_fs_off_memory(old_mstatus);
    write_mstatus(old_mstatus);
    write_mtvec(old_mtvec);
#endif

    return 0;
}
