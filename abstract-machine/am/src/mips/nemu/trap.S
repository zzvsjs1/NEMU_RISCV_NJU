#define MAP(c, f) c(f)

#define REGS(f) \
      f( 1) f( 2) f( 3) f( 4) f( 5) f( 6) f( 7) f( 8) f( 9) \
f(10) f(11) f(12) f(13) f(14) f(15) f(16) f(17) f(18) f(19) \
f(20) f(21) f(22) f(23) f(24) f(25)             f(28)       \
f(30) f(31)

#define PUSH(n) sw $n, (n * 4)($k0);
#define POP(n)  lw $n, (n * 4)($k0);

#define CONTEXT_SIZE  (38 * 4)
#define OFFSET_SP     (29 * 4)
#define OFFSET_LO     (32 * 4)
#define OFFSET_HI     (33 * 4)
#define OFFSET_CAUSE  (34 * 4)
#define OFFSET_STATUS (35 * 4)
#define OFFSET_EPC    (36 * 4)
#define OFFSET_NP     (37 * 4)

/*
 * An O32 caller owns a 16-byte argument-save area at 0..15($sp).  The selected
 * Context pointer therefore lives in a protected local word at offset 16,
 * followed by one word of padding so the Context begins eight-byte aligned.
 *
 *   0..15   outgoing/callee argument-save area
 *   16..19  selected Context pointer across __am_switch
 *   20..23  alignment padding
 *   24..175 saved Context (152 bytes)
 */
#define OFFSET_SELECTED_CONTEXT 16
#define CONTEXT_IN_FRAME         24
#define TRAP_FRAME_SIZE          (CONTEXT_IN_FRAME + CONTEXT_SIZE)

#define CP0_STATUS 12
#define CP0_CAUSE  13
#define CP0_EPC    14

.section .text
.balign 4
.set noreorder
.set noat
.extern __am_user_trap_stack_top
.globl __am_asm_trap
.type __am_asm_trap, @function
__am_asm_trap:
  # Preserve the interrupted architectural stack pointer before consulting
  # the direct-mapped hand-off word.  __am_switch arms that word with the
  # selected user Context's kernel-stack top.  Consume it exactly once: a
  # nested kernel trap then sees zero, retains the current kernel SP, and
  # allocates its frame below this one rather than overwriting it.
  move $k1, $sp
  lui $k0, %hi(__am_user_trap_stack_top)
  lw $sp, %lo(__am_user_trap_stack_top)($k0)
  sw $zero, %lo(__am_user_trap_stack_top)($k0)
  sltu $k0, $zero, $sp
  movz $sp, $k1, $sp

  # The selected SP is now direct-mapped.  Allocate the unchanged 176-byte,
  # eight-byte-aligned trap frame.  Record np before reusing reserved k0 as
  # the Context base: one means the hand-off selected a user kernel stack,
  # while zero means this nested/kernel trap retained its interrupted SP.
  addiu $sp, $sp, -TRAP_FRAME_SIZE
  sw $k0, (CONTEXT_IN_FRAME + OFFSET_NP)($sp)
  addiu $k0, $sp, CONTEXT_IN_FRAME

  MAP(REGS, PUSH)

  sw $k1, OFFSET_SP($k0)

  mflo $t0
  mfhi $t1
  mfc0 $t2, $CP0_CAUSE
  mfc0 $t3, $CP0_STATUS
  mfc0 $t4, $CP0_EPC
  sw $t0, OFFSET_LO($k0)
  sw $t1, OFFSET_HI($k0)
  sw $t2, OFFSET_CAUSE($k0)
  sw $t3, OFFSET_STATUS($k0)
  sw $t4, OFFSET_EPC($k0)

  # allow nested exception
  li $a0, ~0x3
  and $t3, $t3, $a0  # clear status.exl and status.ie
  mtc0 $t3, $CP0_STATUS

  # Pass the Context, not the trap-frame base.  The current sp points at a
  # separate O32 caller area and remains eight-byte aligned for the C call.
  move $a0, $k0
  jal __am_irq_handle
  nop

  # The handler may select another saved Context.  Save its returned pointer
  # outside the 0..15($sp) caller spill area before calling __am_switch; v0 and
  # every other caller-saved register may legitimately be clobbered there.
  sw $v0, OFFSET_SELECTED_CONTEXT($sp)
  move $a0, $v0
  jal __am_switch
  nop

  # Reload the protected pointer after the C call and use reserved k0 as the
  # restore base.  This works for both the interrupted Context in this frame
  # and a Context selected from another kernel stack.
  lw $k0, OFFSET_SELECTED_CONTEXT($sp)
  lw $t0, OFFSET_LO($k0)
  lw $t1, OFFSET_HI($k0)
  lw $t3, OFFSET_STATUS($k0)
  lw $t4, OFFSET_EPC($k0)

  # set status.exl
  ori $t3, $t3, 0x2

  mtlo $t0
  mthi $t1
  mtc0 $t3, $CP0_STATUS
  mtc0 $t4, $CP0_EPC

  MAP(REGS, POP)

  # Restore the selected context's architectural stack pointer.
  lw $sp, OFFSET_SP($k0)
  eret
  nop
.size __am_asm_trap, . - __am_asm_trap

.set at
.set reorder
