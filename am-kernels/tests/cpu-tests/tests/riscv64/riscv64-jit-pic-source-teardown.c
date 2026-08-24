#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 64

#include <stdint.h>

typedef uint64_t (*pic_target_t)(void);

/*
 * Keep every mutable block and its direct wrapper in a separate source-map
 * chunk. The wrappers use fixed JAL edges, leaving source_pic_entry's explicit
 * JALR as the only general-JALR PIC involved in the teardown observation.
 */
asm(".option push\n"
    ".option norvc\n"
    ".pushsection .text\n"
    ".balign 64\n"
    ".global call_source_pic\n"
    ".type call_source_pic, @function\n"
    "call_source_pic:\n"
    "  j source_pic_entry\n"
    ".size call_source_pic, .-call_source_pic\n"
    ".balign 64\n"
    ".global source_pic_entry\n"
    ".type source_pic_entry, @function\n"
    "source_pic_entry:\n"
    "  .word 0x00050367\n" /* jalr t1, 0(a0) */
    ".size source_pic_entry, .-source_pic_entry\n"
    ".balign 64\n"
    ".global call_target_direct\n"
    ".type call_target_direct, @function\n"
    "call_target_direct:\n"
    "  j target_pic_entry\n"
    ".size call_target_direct, .-call_target_direct\n"
    ".balign 64\n"
    ".global target_pic_entry\n"
    ".type target_pic_entry, @function\n"
    "target_pic_entry:\n"
    "  .word 0x00100513\n" /* addi a0, zero, 1 */
    "  .word 0x00008067\n" /* jalr zero, 0(ra) */
    ".size target_pic_entry, .-target_pic_entry\n"
    ".popsection\n"
    ".option pop\n");

extern uint64_t call_source_pic(pic_target_t a0_target, pic_target_t a1_target);
extern uint64_t call_target_direct(void);
extern uint32_t source_pic_entry[];
extern uint32_t target_pic_entry[];

static void local_fence_i(void)
{
    asm volatile("fence.i" : : : "memory");
}

static void test_pic_source_teardown(void)
{
    pic_target_t target = (pic_target_t)(uintptr_t)target_pic_entry;

    for (uint32_t i = 0; i < 8u; i++)
    {
        check(call_source_pic(target, target) == 1u);
    }

    /*
     * Replacing the JALR source must remove its dynamic incoming-list record
     * from the still-valid target before the source's native storage retires.
     */
    source_pic_entry[0] = 0x00058367u; /* jalr t1, 0(a1) */
    local_fence_i();

    /*
     * Do not republish the source yet. If source teardown left its old record
     * attached, this target invalidation will encounter that dangling node.
     */
    target_pic_entry[0] = 0x00200513u; /* addi a0, zero, 2 */
    local_fence_i();
    check(call_target_direct() == 2u);

    for (uint32_t i = 0; i < 8u; i++)
    {
        check(call_source_pic(target, target) == 2u);
    }
}

#endif

int main(void)
{
#if defined(__riscv) && __riscv_xlen == 64
    test_pic_source_teardown();
#endif

    return 0;
}
