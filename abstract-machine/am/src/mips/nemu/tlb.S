/*
 * A first-level MIPS32 TLB refill can arrive while the architectural stack
 * pointer names an untranslated user page.  This entry therefore preserves
 * the interrupted SP in reserved k1 and switches to a private kernel-segment
 * stack before its first store.  k0 and k1 remain refill scratch registers;
 * they are deliberately neither saved nor restored as user-visible state.
 */

#define REFILL_GPR1_OFFSET       (0 * 4)
#define REFILL_GPR2_OFFSET       (1 * 4)
#define REFILL_GPR3_OFFSET       (2 * 4)
#define REFILL_GPR4_OFFSET       (3 * 4)
#define REFILL_GPR5_OFFSET       (4 * 4)
#define REFILL_GPR6_OFFSET       (5 * 4)
#define REFILL_GPR7_OFFSET       (6 * 4)
#define REFILL_GPR8_OFFSET       (7 * 4)
#define REFILL_GPR9_OFFSET       (8 * 4)
#define REFILL_GPR10_OFFSET      (9 * 4)
#define REFILL_GPR11_OFFSET     (10 * 4)
#define REFILL_GPR12_OFFSET     (11 * 4)
#define REFILL_GPR13_OFFSET     (12 * 4)
#define REFILL_GPR14_OFFSET     (13 * 4)
#define REFILL_GPR15_OFFSET     (14 * 4)
#define REFILL_GPR16_OFFSET     (15 * 4)
#define REFILL_GPR17_OFFSET     (16 * 4)
#define REFILL_GPR18_OFFSET     (17 * 4)
#define REFILL_GPR19_OFFSET     (18 * 4)
#define REFILL_GPR20_OFFSET     (19 * 4)
#define REFILL_GPR21_OFFSET     (20 * 4)
#define REFILL_GPR22_OFFSET     (21 * 4)
#define REFILL_GPR23_OFFSET     (22 * 4)
#define REFILL_GPR24_OFFSET     (23 * 4)
#define REFILL_GPR25_OFFSET     (24 * 4)
#define REFILL_GPR28_OFFSET     (25 * 4)
#define REFILL_GPR30_OFFSET     (26 * 4)
#define REFILL_GPR31_OFFSET     (27 * 4)
#define REFILL_ORIGINAL_SP      (28 * 4)
#define REFILL_HI_OFFSET        (29 * 4)
#define REFILL_LO_OFFSET        (30 * 4)
#define REFILL_USER_TRAP_TOP    (31 * 4)

/*
 * GPRs 1-25 (25), 28/30/31 (3), original SP (1), HI/LO (2), and the saved
 * user-trap-top hand-off (1) occupy exactly 32 words.  The hand-off reuses the
 * former padding word, so the frame does not grow.  A separate outgoing area
 * is placed below this frame at call time, so O32 callees may use 0..15($sp)
 * without corrupting any saved architectural state.
 */
#define REFILL_FRAME_SIZE       (32 * 4)
#define O32_CALLER_AREA_SIZE    (4 * 4)

#define REFILL_GPRS(f) \
  f( 1, REFILL_GPR1_OFFSET)  f( 2, REFILL_GPR2_OFFSET)  \
  f( 3, REFILL_GPR3_OFFSET)  f( 4, REFILL_GPR4_OFFSET)  \
  f( 5, REFILL_GPR5_OFFSET)  f( 6, REFILL_GPR6_OFFSET)  \
  f( 7, REFILL_GPR7_OFFSET)  f( 8, REFILL_GPR8_OFFSET)  \
  f( 9, REFILL_GPR9_OFFSET)  f(10, REFILL_GPR10_OFFSET) \
  f(11, REFILL_GPR11_OFFSET) f(12, REFILL_GPR12_OFFSET) \
  f(13, REFILL_GPR13_OFFSET) f(14, REFILL_GPR14_OFFSET) \
  f(15, REFILL_GPR15_OFFSET) f(16, REFILL_GPR16_OFFSET) \
  f(17, REFILL_GPR17_OFFSET) f(18, REFILL_GPR18_OFFSET) \
  f(19, REFILL_GPR19_OFFSET) f(20, REFILL_GPR20_OFFSET) \
  f(21, REFILL_GPR21_OFFSET) f(22, REFILL_GPR22_OFFSET) \
  f(23, REFILL_GPR23_OFFSET) f(24, REFILL_GPR24_OFFSET) \
  f(25, REFILL_GPR25_OFFSET) f(28, REFILL_GPR28_OFFSET) \
  f(30, REFILL_GPR30_OFFSET) f(31, REFILL_GPR31_OFFSET)

#define REFILL_SAVE(reg, offset) sw $reg, offset($sp);
#define REFILL_LOAD(reg, offset) lw $reg, offset($k0);

.section .bss
.balign 16
.type __am_tlb_refill_scratch_stack, @object
__am_tlb_refill_scratch_stack:
  .space 4096
.size __am_tlb_refill_scratch_stack, 4096
__am_tlb_refill_scratch_stack_top:

.section .text
.balign 4
.set noreorder
.set noat
.extern __am_user_trap_stack_top
.globl __am_tlb_refill_entry
.type __am_tlb_refill_entry, @function
__am_tlb_refill_entry:
  # Do not touch memory until SP names the dedicated, translated scratch stack.
  move $k1, $sp
  lui $sp, %hi(__am_tlb_refill_scratch_stack_top)
  addiu $sp, $sp, %lo(__am_tlb_refill_scratch_stack_top)
  addiu $sp, $sp, -REFILL_FRAME_SIZE

  REFILL_GPRS(REFILL_SAVE)
  sw $k1, REFILL_ORIGINAL_SP($sp)

  mfhi $k0
  sw $k0, REFILL_HI_OFFSET($sp)
  mflo $k0
  sw $k0, REFILL_LO_OFFSET($sp)

  # A first-level refill occurs while the next normal user trap's kernel-stack
  # top is armed.  Preserve that one-shot value in the existing final frame
  # word, then clear the direct-mapped global before entering C.  Any nested
  # general exception will consequently retain the refill scratch SP and push
  # its own frame below this one.
  lui $k0, %hi(__am_user_trap_stack_top)
  lw $k1, %lo(__am_user_trap_stack_top)($k0)
  sw $k1, REFILL_USER_TRAP_TOP($sp)
  sw $zero, %lo(__am_user_trap_stack_top)($k0)

  # Keep saved state above a distinct O32 argument-save area for the C call.
  addiu $sp, $sp, -O32_CALLER_AREA_SIZE
  jal __am_tlb_refill
  nop
  addiu $sp, $sp, O32_CALLER_AREA_SIZE

  # k0 is reserved by the AM ABI and remains a stable restore-base register.
  move $k0, $sp
  lw $k1, REFILL_HI_OFFSET($k0)
  mthi $k1
  lw $k1, REFILL_LO_OFFSET($k0)
  mtlo $k1

  REFILL_GPRS(REFILL_LOAD)

  # Restore the interrupted SP first while k0 still addresses this direct-
  # mapped frame.  Rearm the preserved normal-trap top with the final store
  # immediately before ERET; k0 and k1 are reserved AM scratch registers.
  lw $sp, REFILL_ORIGINAL_SP($k0)
  lw $k1, REFILL_USER_TRAP_TOP($k0)
  lui $k0, %hi(__am_user_trap_stack_top)
  sw $k1, %lo(__am_user_trap_stack_top)($k0)
  eret
  nop
.size __am_tlb_refill_entry, . - __am_tlb_refill_entry

.set at
.set reorder
