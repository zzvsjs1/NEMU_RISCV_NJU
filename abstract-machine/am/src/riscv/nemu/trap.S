#define concat_temp(x, y) x ## y
#define concat(x, y) concat_temp(x, y)
#define MAP(c, f) c(f)

#if __riscv_xlen == 32
#define LOAD  lw
#define STORE sw
#define XLEN  4
#else
#define LOAD  ld
#define STORE sd
#define XLEN  8
#endif

#define REGS(f) \
      f( 1)       f( 3) f( 4) f( 5) f( 6) f( 7) f( 8) f( 9) \
f(10) f(11) f(12) f(13) f(14) f(15) f(16) f(17) f(18) f(19) \
f(20) f(21) f(22) f(23) f(24) f(25) f(26) f(27) f(28) f(29) \
f(30) f(31)

// x0 is constant zero and x2 is saved separately as the interrupted stack
// pointer, so the bulk save/restore list covers every other general register.
#define PUSH(n) STORE concat(x, n), (n * XLEN)(sp);
#define POP(n)  LOAD  concat(x, n), (n * XLEN)(sp);

#define CONTEXT_SIZE  ((32 + 3 + 1 + 1 + 3) * XLEN)
#define OFFSET_SP     ( 2 * XLEN)
#define OFFSET_CAUSE  (32 * XLEN)
#define OFFSET_STATUS (33 * XLEN)
#define OFFSET_EPC    (34 * XLEN)
#define OFFSET_KSP    (36 * XLEN)
#define OFFSET_NP     (37 * XLEN)

// These offsets must stay in lock-step with the C Context layout. The assembly
// path cannot use field names, so all trap entry and return code depends on
// this compact positional contract.
.align 3
.globl __am_asm_trap
__am_asm_trap:
  // If mscratch is non-zero, we trapped from user mode and it holds the
  // kernel stack top. If it is zero, we trapped while already in kernel mode.
  // csrrw performs two jobs atomically: it gives the handler a kernel stack and
  // preserves the interrupted user x2 in mscratch until it is saved below.
  csrrw sp, mscratch, sp
  bnez sp, 1f

  // Kernel trap: restore the interrupted kernel sp from mscratch.
  // In this path the earlier csrrw wrote the interrupted kernel sp into
  // mscratch and left sp as zero, so read it back before allocating Context.
  csrr sp, mscratch
  addi sp, sp, -CONTEXT_SIZE
  MAP(REGS, PUSH)
  li t0, 0
  j 2f

1:
  addi sp, sp, -CONTEXT_SIZE

  MAP(REGS, PUSH)
  li t0, 1

2:
  STORE t0, OFFSET_NP(sp)

  // Save interrupted sp to context.
  csrr t3, mscratch
  STORE t3, OFFSET_SP(sp)

  // Remember this context's kernel stack top for diagnostics/compatibility.
  addi t3, sp, CONTEXT_SIZE
  STORE t3, OFFSET_KSP(sp)

  csrr t0, mcause
  csrr t1, mstatus
  csrr t2, mepc

  STORE t0, OFFSET_CAUSE(sp)
  STORE t1, OFFSET_STATUS(sp)
  STORE t2, OFFSET_EPC(sp)

  // The Nanos-lite syscall path passes user pointers directly into C helpers.
  // While the trap handler still executes in machine mode, make its data
  // accesses use the active page table with supervisor permissions:
  // - kernel mappings are U=0, so supervisor data accesses can touch stacks,
  //   globals, and MMIO;
  // - user mappings are U=1, and SUM lets syscalls copy user buffers.
  //
  // The original mstatus value was already saved in Context above and is
  // restored before mret, so this only affects handler-side loads/stores.
  li a0, (3 << 11)
  not a0, a0
  and t1, t1, a0
  li a0, ((1 << 17) | (1 << 18) | (1 << 11))
  or t1, t1, a0
  csrw mstatus, t1

  // CTE has switched to a kernel stack. Any nested trap before returning must
  // be treated as a kernel trap.
  csrw mscratch, zero

  mv a0, sp
  call __am_irq_handle
  
  // Switch sp to new context.
  mv sp, a0

  # Switch address space after moving to the next context stack.
  mv a0, sp
  call __am_switch

  // Prepare mscratch for the privilege mode we are about to resume.
  // User return: mscratch must contain this context's kernel stack top for the
  // next trap. Kernel return: mscratch must stay zero so nested traps do not
  // reinterpret a kernel sp as a saved user sp.
  LOAD t0, OFFSET_NP(sp)
  beqz t0, 3f
  addi t0, sp, CONTEXT_SIZE
  csrw mscratch, t0
  j 4f
3:
  csrw mscratch, zero

4:
  LOAD t1, OFFSET_STATUS(sp)
  LOAD t2, OFFSET_EPC(sp)
  csrw mstatus, t1
  csrw mepc, t2

  MAP(REGS, POP)

  // Restore target x2 from Context.gpr[2], not from Context pointer arithmetic.
  LOAD sp, OFFSET_SP(sp)
  mret
