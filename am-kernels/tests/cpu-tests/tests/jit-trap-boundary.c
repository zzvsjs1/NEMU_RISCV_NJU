#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 32

#include <stdint.h>

volatile uint32_t jit_trap_seen = 0;
volatile uint32_t jit_trap_mepc = 0;
volatile uint32_t jit_trap_mcause = 0;
volatile uint32_t jit_trap_a0 = 0;

asm(
  ".section .text\n"
  ".option push\n"
  ".option norvc\n"
  ".align 2\n"
  ".globl jit_trap_boundary_handler\n"
  "jit_trap_boundary_handler:\n"
  "  csrr t0, mepc\n"
  "  la t1, jit_trap_mepc\n"
  "  sw t0, 0(t1)\n"
  "  csrr t0, mcause\n"
  "  la t1, jit_trap_mcause\n"
  "  sw t0, 0(t1)\n"
  "  la t1, jit_trap_a0\n"
  "  sw a0, 0(t1)\n"
  "  la t1, jit_trap_seen\n"
  "  lw t0, 0(t1)\n"
  "  addi t0, t0, 1\n"
  "  sw t0, 0(t1)\n"
  "  csrr t0, mepc\n"
  "  addi t0, t0, 4\n"
  "  csrw mepc, t0\n"
  "  mret\n"
  ".option pop\n"
);

extern void jit_trap_boundary_handler(void);

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

__attribute__((noinline))
static uint32_t hot_state_before_trap(uint32_t seed)
{
  uint32_t a = seed;
  uint32_t b = 0x9e3779b9u;
  for (uint32_t i = 0; i < 4096u; i++)
  {
    a += (b ^ i) + 3u;
    b = (b << 5) | (b >> 27);
    b ^= a;
  }
  return a ^ b;
}

static void run_ecall_after_hot_block(void)
{
  uintptr_t old_mtvec = read_mtvec();
  uintptr_t expected_mepc = 0;
  uint32_t value = hot_state_before_trap(0x12345678u);

  jit_trap_seen = 0;
  jit_trap_mepc = 0;
  jit_trap_mcause = 0;
  jit_trap_a0 = 0;
  write_mtvec((uintptr_t)jit_trap_boundary_handler);

  asm volatile(
    "la %[expected], 1f\n"
    "mv a0, %[value]\n"
    "li a7, 11\n"
    "1:\n"
    "ecall\n"
    : [expected] "=&r"(expected_mepc)
    : [value] "r"(value)
    : "a0", "a7", "t0", "t1", "memory"
  );

  write_mtvec(old_mtvec);
  check(jit_trap_seen == 1u);
  check(jit_trap_mepc == (uint32_t)expected_mepc);
  check(jit_trap_mcause == 11u);
  check(jit_trap_a0 == value);
  check(value == hot_state_before_trap(0x12345678u));
}

#endif

int main(void)
{
#if defined(__riscv) && __riscv_xlen == 32
  run_ecall_after_hot_block();
#endif
  return 0;
}
