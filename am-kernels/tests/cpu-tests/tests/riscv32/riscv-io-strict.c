#include "trap.h"

#if defined(__riscv) && __riscv_xlen == 32

#include <stdint.h>

#define NEMU_MOUSE_ADDR 0xa0000070u

enum
{
    MOUSE_REG_TYPE = 0x00u,
    MOUSE_REG_X = 0x04u,
    MOUSE_REG_Y = 0x08u,
    MOUSE_REG_BUTTON = 0x0cu,
    MOUSE_REG_BUTTONS = 0x10u,
    MOUSE_REG_WHEEL_X = 0x14u,
    MOUSE_REG_WHEEL_Y = 0x18u,
};

static uint32_t mmio_read32(uintptr_t addr)
{
    return *(volatile uint32_t *)addr;
}

static void test_mouse_type_read_latches_one_complete_event(void)
{
    /*
     * Reading the mouse type register dequeues and latches exactly one event.
     * The payload registers then describe that same event, even if more events
     * are waiting behind it. The Makefile feeds a deterministic script. This
     * first check consumes the first move event and leaves later events queued.
     */
    uintptr_t base = NEMU_MOUSE_ADDR;
    uint32_t type = mmio_read32(base + MOUSE_REG_TYPE);

    check(type == 1u);
    check(mmio_read32(base + MOUSE_REG_X) == 31u);
    check(mmio_read32(base + MOUSE_REG_Y) == 47u);
    check(mmio_read32(base + MOUSE_REG_BUTTON) == 0u);
    check(mmio_read32(base + MOUSE_REG_BUTTONS) == 0u);
    check(mmio_read32(base + MOUSE_REG_WHEEL_X) == 0u);
    check(mmio_read32(base + MOUSE_REG_WHEEL_Y) == 0u);
}

static void test_load_to_x0_still_consumes_mmio_event(void)
{
    /*
     * Loads to x0 discard the loaded value, but they are still real loads. Here
     * the first type-register load targets x0 and must still dequeue the second
     * scripted move event. The visible read afterwards then observes the queued
     * wheel event.
     */
    uintptr_t base = NEMU_MOUSE_ADDR;
    uint32_t type = 0;
    uint32_t wheel_y = 0;

    asm volatile(
        "lw x0, 0(%[type_addr])\n"
        "lw %[type], 0(%[type_addr])\n"
        "lw %[wheel_y], 0(%[wheel_y_addr])\n"
        : [type] "=&r"(type), [wheel_y] "=&r"(wheel_y)
        : [type_addr] "r"(base + MOUSE_REG_TYPE),
          [wheel_y_addr] "r"(base + MOUSE_REG_WHEEL_Y)
        : "memory");

    check(type == 4u);
    check(mmio_read32(base + MOUSE_REG_X) == 31u);
    check(mmio_read32(base + MOUSE_REG_Y) == 47u);
    check(mmio_read32(base + MOUSE_REG_BUTTON) == 5u);
    check(mmio_read32(base + MOUSE_REG_BUTTONS) == 0u);
    check(mmio_read32(base + MOUSE_REG_WHEEL_X) == 2u);
    check(wheel_y == (uint32_t)-3);
    check(mmio_read32(base + MOUSE_REG_TYPE) == 0u);
}

#endif

int main(void)
{
#if defined(__riscv) && __riscv_xlen == 32
    test_mouse_type_read_latches_one_complete_event();
    test_load_to_x0_still_consumes_mmio_event();
#endif

    return 0;
}
