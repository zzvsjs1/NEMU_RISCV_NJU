#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 64

#include <stdint.h>

/*
 * Each entry fills the six ordinary JIT cache slots and keeps all six
 * values live across an operation with x0. The new t6 destination must
 * therefore cause pressure without replacing a retained source value.
 * x0 is hardwired to zero; it must never become an evictable cached value.
 *
 * Separate function entries keep each complete case within one native
 * block. The repeated assembly is deliberate: a call or branch inside the
 * pressure sequence would flush mappings and hide the allocation defect.
 * Only caller-saved registers are changed, so no stack frame is needed.
 */
asm(".option push\n"
    ".option norvc\n"
    ".text\n"

    ".balign 4\n"
    ".globl rv64_jit_x0_add_rhs\n"
    ".type rv64_jit_x0_add_rhs, @function\n"
    "rv64_jit_x0_add_rhs:\n"
    "addi t0, zero, 1\n"
    "addi t1, zero, 2\n"
    "addi t2, zero, 3\n"
    "addi t3, zero, 4\n"
    "addi t4, zero, 5\n"
    "addi t5, zero, 6\n"
    "add t6, t0, zero\n"
    "add a0, t6, t0\n"
    "add a0, a0, t1\n"
    "add a0, a0, t2\n"
    "add a0, a0, t3\n"
    "add a0, a0, t4\n"
    "add a0, a0, t5\n"
    "ret\n"
    ".size rv64_jit_x0_add_rhs, .-rv64_jit_x0_add_rhs\n"

    ".balign 4\n"
    ".globl rv64_jit_x0_add_lhs\n"
    ".type rv64_jit_x0_add_lhs, @function\n"
    "rv64_jit_x0_add_lhs:\n"
    "addi t0, zero, 1\n"
    "addi t1, zero, 2\n"
    "addi t2, zero, 3\n"
    "addi t3, zero, 4\n"
    "addi t4, zero, 5\n"
    "addi t5, zero, 6\n"
    "add t6, zero, t0\n"
    "add a0, t6, t0\n"
    "add a0, a0, t1\n"
    "add a0, a0, t2\n"
    "add a0, a0, t3\n"
    "add a0, a0, t4\n"
    "add a0, a0, t5\n"
    "ret\n"
    ".size rv64_jit_x0_add_lhs, .-rv64_jit_x0_add_lhs\n"

    ".balign 4\n"
    ".globl rv64_jit_x0_sub_rhs\n"
    ".type rv64_jit_x0_sub_rhs, @function\n"
    "rv64_jit_x0_sub_rhs:\n"
    "addi t0, zero, 1\n"
    "addi t1, zero, 2\n"
    "addi t2, zero, 3\n"
    "addi t3, zero, 4\n"
    "addi t4, zero, 5\n"
    "addi t5, zero, 6\n"
    "sub t6, t0, zero\n"
    "add a0, t6, t0\n"
    "add a0, a0, t1\n"
    "add a0, a0, t2\n"
    "add a0, a0, t3\n"
    "add a0, a0, t4\n"
    "add a0, a0, t5\n"
    "ret\n"
    ".size rv64_jit_x0_sub_rhs, .-rv64_jit_x0_sub_rhs\n"

    ".balign 4\n"
    ".globl rv64_jit_x0_sub_lhs\n"
    ".type rv64_jit_x0_sub_lhs, @function\n"
    "rv64_jit_x0_sub_lhs:\n"
    "addi t0, zero, 1\n"
    "addi t1, zero, 2\n"
    "addi t2, zero, 3\n"
    "addi t3, zero, 4\n"
    "addi t4, zero, 5\n"
    "addi t5, zero, 6\n"
    "sub t6, zero, t0\n"
    "add a0, t6, t0\n"
    "add a0, a0, t1\n"
    "add a0, a0, t2\n"
    "add a0, a0, t3\n"
    "add a0, a0, t4\n"
    "add a0, a0, t5\n"
    "ret\n"
    ".size rv64_jit_x0_sub_lhs, .-rv64_jit_x0_sub_lhs\n"

    ".balign 4\n"
    ".globl rv64_jit_x0_xor_rhs\n"
    ".type rv64_jit_x0_xor_rhs, @function\n"
    "rv64_jit_x0_xor_rhs:\n"
    "addi t0, zero, 1\n"
    "addi t1, zero, 2\n"
    "addi t2, zero, 3\n"
    "addi t3, zero, 4\n"
    "addi t4, zero, 5\n"
    "addi t5, zero, 6\n"
    "xor t6, t0, zero\n"
    "add a0, t6, t0\n"
    "add a0, a0, t1\n"
    "add a0, a0, t2\n"
    "add a0, a0, t3\n"
    "add a0, a0, t4\n"
    "add a0, a0, t5\n"
    "ret\n"
    ".size rv64_jit_x0_xor_rhs, .-rv64_jit_x0_xor_rhs\n"

    ".balign 4\n"
    ".globl rv64_jit_x0_xor_lhs\n"
    ".type rv64_jit_x0_xor_lhs, @function\n"
    "rv64_jit_x0_xor_lhs:\n"
    "addi t0, zero, 1\n"
    "addi t1, zero, 2\n"
    "addi t2, zero, 3\n"
    "addi t3, zero, 4\n"
    "addi t4, zero, 5\n"
    "addi t5, zero, 6\n"
    "xor t6, zero, t0\n"
    "add a0, t6, t0\n"
    "add a0, a0, t1\n"
    "add a0, a0, t2\n"
    "add a0, a0, t3\n"
    "add a0, a0, t4\n"
    "add a0, a0, t5\n"
    "ret\n"
    ".size rv64_jit_x0_xor_lhs, .-rv64_jit_x0_xor_lhs\n"

    ".balign 4\n"
    ".globl rv64_jit_x0_or_rhs\n"
    ".type rv64_jit_x0_or_rhs, @function\n"
    "rv64_jit_x0_or_rhs:\n"
    "addi t0, zero, 1\n"
    "addi t1, zero, 2\n"
    "addi t2, zero, 3\n"
    "addi t3, zero, 4\n"
    "addi t4, zero, 5\n"
    "addi t5, zero, 6\n"
    "or t6, t0, zero\n"
    "add a0, t6, t0\n"
    "add a0, a0, t1\n"
    "add a0, a0, t2\n"
    "add a0, a0, t3\n"
    "add a0, a0, t4\n"
    "add a0, a0, t5\n"
    "ret\n"
    ".size rv64_jit_x0_or_rhs, .-rv64_jit_x0_or_rhs\n"

    ".balign 4\n"
    ".globl rv64_jit_x0_or_lhs\n"
    ".type rv64_jit_x0_or_lhs, @function\n"
    "rv64_jit_x0_or_lhs:\n"
    "addi t0, zero, 1\n"
    "addi t1, zero, 2\n"
    "addi t2, zero, 3\n"
    "addi t3, zero, 4\n"
    "addi t4, zero, 5\n"
    "addi t5, zero, 6\n"
    "or t6, zero, t0\n"
    "add a0, t6, t0\n"
    "add a0, a0, t1\n"
    "add a0, a0, t2\n"
    "add a0, a0, t3\n"
    "add a0, a0, t4\n"
    "add a0, a0, t5\n"
    "ret\n"
    ".size rv64_jit_x0_or_lhs, .-rv64_jit_x0_or_lhs\n"

    ".balign 4\n"
    ".globl rv64_jit_x0_and_rhs\n"
    ".type rv64_jit_x0_and_rhs, @function\n"
    "rv64_jit_x0_and_rhs:\n"
    "addi t0, zero, 1\n"
    "addi t1, zero, 2\n"
    "addi t2, zero, 3\n"
    "addi t3, zero, 4\n"
    "addi t4, zero, 5\n"
    "addi t5, zero, 6\n"
    "and t6, t0, zero\n"
    "add a0, t6, t0\n"
    "add a0, a0, t1\n"
    "add a0, a0, t2\n"
    "add a0, a0, t3\n"
    "add a0, a0, t4\n"
    "add a0, a0, t5\n"
    "ret\n"
    ".size rv64_jit_x0_and_rhs, .-rv64_jit_x0_and_rhs\n"

    ".balign 4\n"
    ".globl rv64_jit_x0_and_lhs\n"
    ".type rv64_jit_x0_and_lhs, @function\n"
    "rv64_jit_x0_and_lhs:\n"
    "addi t0, zero, 1\n"
    "addi t1, zero, 2\n"
    "addi t2, zero, 3\n"
    "addi t3, zero, 4\n"
    "addi t4, zero, 5\n"
    "addi t5, zero, 6\n"
    "and t6, zero, t0\n"
    "add a0, t6, t0\n"
    "add a0, a0, t1\n"
    "add a0, a0, t2\n"
    "add a0, a0, t3\n"
    "add a0, a0, t4\n"
    "add a0, a0, t5\n"
    "ret\n"
    ".size rv64_jit_x0_and_lhs, .-rv64_jit_x0_and_lhs\n"

    ".option pop\n");

extern uint64_t rv64_jit_x0_add_rhs(void);
extern uint64_t rv64_jit_x0_add_lhs(void);
extern uint64_t rv64_jit_x0_sub_rhs(void);
extern uint64_t rv64_jit_x0_sub_lhs(void);
extern uint64_t rv64_jit_x0_xor_rhs(void);
extern uint64_t rv64_jit_x0_xor_lhs(void);
extern uint64_t rv64_jit_x0_or_rhs(void);
extern uint64_t rv64_jit_x0_or_lhs(void);
extern uint64_t rv64_jit_x0_and_rhs(void);
extern uint64_t rv64_jit_x0_and_lhs(void);

#endif

int main(void)
{
#if defined(__riscv) && __riscv_xlen == 64
    /* The retained inputs sum to 1 + 2 + 3 + 4 + 5 + 6 = 21. */
    check(rv64_jit_x0_add_rhs() == UINT64_C(22));
    check(rv64_jit_x0_add_lhs() == UINT64_C(22));
    check(rv64_jit_x0_sub_rhs() == UINT64_C(22));
    check(rv64_jit_x0_sub_lhs() == UINT64_C(20));
    check(rv64_jit_x0_xor_rhs() == UINT64_C(22));
    check(rv64_jit_x0_xor_lhs() == UINT64_C(22));
    check(rv64_jit_x0_or_rhs() == UINT64_C(22));
    check(rv64_jit_x0_or_lhs() == UINT64_C(22));
    check(rv64_jit_x0_and_rhs() == UINT64_C(21));
    check(rv64_jit_x0_and_lhs() == UINT64_C(21));
#endif

    return 0;
}
