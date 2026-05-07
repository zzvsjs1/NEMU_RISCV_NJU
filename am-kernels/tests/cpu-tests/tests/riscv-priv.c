#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 32

#include <stdint.h>

volatile uint32_t riscv_priv_trap_seen = 0;
volatile uint32_t riscv_priv_saved_mstatus = 0;
volatile uint32_t riscv_priv_restore_mtvec = 0;

/*
 * The vector table is written in inline assembly so the test controls exact
 * instruction placement and avoids compiler-generated prologue code. mtvec
 * vectored mode still sends synchronous exceptions to BASE, so slot zero must
 * be the only handler reached by the ecall test.
 */
asm(
  ".section .text\n"
  ".option push\n"
  ".option norvc\n"
  ".align 6\n"
  ".globl riscv_priv_vector_table\n"
  "riscv_priv_vector_table:\n"
  "  j riscv_priv_sync_handler\n"
  "  j riscv_priv_vector_wrong\n"
  "  j riscv_priv_vector_wrong\n"
  "  j riscv_priv_vector_wrong\n"
  "  j riscv_priv_vector_wrong\n"
  "  j riscv_priv_vector_wrong\n"
  "  j riscv_priv_vector_wrong\n"
  "  j riscv_priv_vector_wrong\n"
  "  j riscv_priv_vector_wrong\n"
  "  j riscv_priv_vector_wrong\n"
  "  j riscv_priv_vector_wrong\n"
  "  j riscv_priv_vector_wrong\n"
  "  j riscv_priv_vector_wrong\n"
  "  j riscv_priv_vector_wrong\n"
  "  j riscv_priv_vector_wrong\n"
  "  j riscv_priv_vector_wrong\n"
  "riscv_priv_sync_handler:\n"
  "  csrr t0, mepc\n"
  "  addi t0, t0, 4\n"
  "  csrw mepc, t0\n"
  "  la t0, riscv_priv_trap_seen\n"
  "  li t1, 1\n"
  "  sw t1, 0(t0)\n"
  "  mret\n"
  "riscv_priv_vector_wrong:\n"
  "  li a0, 1\n"
  "  .word 0x0000006b\n"
  "  j riscv_priv_vector_wrong\n"
  ".globl riscv_priv_mstatus_handler\n"
  "riscv_priv_mstatus_handler:\n"
  "  csrr t1, mstatus\n"
  "  la t0, riscv_priv_saved_mstatus\n"
  "  sw t1, 0(t0)\n"
  "  la t0, riscv_priv_restore_mtvec\n"
  "  lw t1, 0(t0)\n"
  "  csrw mtvec, t1\n"
  "  csrr t0, mepc\n"
  "  addi t0, t0, 4\n"
  "  csrw mepc, t0\n"
  "  mret\n"
  ".option pop\n"
);

extern void riscv_priv_vector_table(void);
extern void riscv_priv_mstatus_handler(void);

static inline uintptr_t read_mstatus(void)
{
  uintptr_t v;
  asm volatile("csrr %0, mstatus" : "=r"(v));
  return v;
}

static inline uintptr_t read_mscratch(void)
{
  uintptr_t v;
  asm volatile("csrr %0, mscratch" : "=r"(v));
  return v;
}

static inline uintptr_t read_mtvec(void)
{
  uintptr_t v;
  asm volatile("csrr %0, mtvec" : "=r"(v));
  return v;
}

static inline void write_mtvec(uintptr_t v)
{
  asm volatile("csrw mtvec, %0" : : "r"(v) : "memory");
}

static void test_csr_immediate(void)
{
  /*
   * CSR immediate instructions use a five-bit unsigned immediate encoded in the
   * instruction itself. mscratch is safe scratch state for this test because the
   * cpu-test harness does not rely on AM's trap nesting convention here.
   */
  uintptr_t old;

  asm volatile("csrw mscratch, %0" : : "r"(0x13579u) : "memory");

  asm volatile("csrrwi %0, mscratch, 5" : "=r"(old) : : "memory");
  check(old == 0x13579u);
  check(read_mscratch() == 5u);

  asm volatile("csrrsi %0, mscratch, 18" : "=r"(old) : : "memory");
  check(old == 5u);
  check(read_mscratch() == (5u | 18u));

  asm volatile("csrrci %0, mscratch, 16" : "=r"(old) : : "memory");
  check(old == (5u | 18u));
  check(read_mscratch() == ((5u | 18u) & ~16u));
}

static void test_vectored_mtvec_keeps_sync_exception_at_base(void)
{
  /*
   * RISC-V vectored mtvec applies its per-cause offset only to interrupts.
   * Synchronous exceptions such as ecall must jump to BASE, which is exactly
   * what this test records through riscv_priv_trap_seen.
   */
  uintptr_t old_mtvec = read_mtvec();

  riscv_priv_trap_seen = 0;
  write_mtvec(((uintptr_t)riscv_priv_vector_table) | 1u);

  asm volatile("li a7, 11; ecall" : : : "a7", "memory");

  write_mtvec(old_mtvec);
  check(riscv_priv_trap_seen == 1);
}

static void test_mret_clears_mprv_when_returning_to_u_mode(void)
{
  /*
   * The privileged spec requires mret to clear MPRV when returning below
   * machine mode. NEMU must implement that side effect or later memory accesses
   * can run with stale effective privilege.
   */
  uintptr_t old_mstatus = read_mstatus();
  uintptr_t old_mtvec = read_mtvec();
  uintptr_t mstatus = old_mstatus;

  mstatus |= (1u << 17);       // MPRV = 1
  mstatus &= ~(3u << 11);      // MPP = U
  mstatus |= (1u << 7);        // MPIE = 1
  riscv_priv_saved_mstatus = 0;
  riscv_priv_restore_mtvec = old_mtvec;
  write_mtvec((uintptr_t)riscv_priv_mstatus_handler);

  asm volatile(
    "csrw mstatus, %[mstatus]\n"
    "la t0, 1f\n"
    "csrw mepc, t0\n"
    "mret\n"
    "1:\n"
    "li a7, 11\n"
    "ecall\n"
    :
    : [mstatus] "r"(mstatus)
    : "t0", "a7", "memory"
  );

  check((riscv_priv_saved_mstatus & (1u << 17)) == 0);
}

#endif

int main(void)
{
#if defined(__riscv) && __riscv_xlen == 32
  test_csr_immediate();
  test_vectored_mtvec_keeps_sync_exception_at_base();
  test_mret_clears_mprv_when_returning_to_u_mode();
#endif

  return 0;
}
