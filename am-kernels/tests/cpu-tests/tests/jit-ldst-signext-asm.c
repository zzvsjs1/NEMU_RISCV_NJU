#include "trap.h"
#include <stdint.h>

static volatile uint8_t bytes[16] __attribute__((aligned(4))) = {
    0x00,
    0x7f,
    0x80,
    0xff,
    0x34,
    0x12,
    0x78,
    0x56,
    0xaa,
    0xbb,
    0xcc,
    0xdd,
    0x11,
    0x22,
    0x33,
    0x44,
};

static uint32_t load_lb(const volatile uint8_t *p)
{
    uint32_t out;
    asm volatile("lb %0, 0(%1)" : "=r"(out) : "r"(p) : "memory");
    return out;
}

static uint32_t load_lbu(const volatile uint8_t *p)
{
    uint32_t out;
    asm volatile("lbu %0, 0(%1)" : "=r"(out) : "r"(p) : "memory");
    return out;
}

static uint32_t load_lh(const volatile uint8_t *p)
{
    uint32_t out;
    asm volatile("lh %0, 0(%1)" : "=r"(out) : "r"(p) : "memory");
    return out;
}

static uint32_t load_lhu(const volatile uint8_t *p)
{
    uint32_t out;
    asm volatile("lhu %0, 0(%1)" : "=r"(out) : "r"(p) : "memory");
    return out;
}

static uint32_t load_lw(const volatile uint8_t *p)
{
    uint32_t out;
    asm volatile("lw %0, 0(%1)" : "=r"(out) : "r"(p) : "memory");
    return out;
}

static void store_sb(volatile uint8_t *p, uint32_t value)
{
    asm volatile("sb %1, 0(%0)" : : "r"(p), "r"(value) : "memory");
}

static void store_sh(volatile uint8_t *p, uint32_t value)
{
    asm volatile("sh %1, 0(%0)" : : "r"(p), "r"(value) : "memory");
}

static void store_sw(volatile uint8_t *p, uint32_t value)
{
    asm volatile("sw %1, 0(%0)" : : "r"(p), "r"(value) : "memory");
}

static uint32_t load_to_x0_keeps_following_state(uint32_t before,
                                                 const volatile uint8_t *p)
{
    uint32_t after = before;
    asm volatile(
        "lb x0, 0(%1)\n"
        "addi %0, %0, 1\n"
        : "+r"(after)
        : "r"(p)
        : "memory");
    return after;
}

static uint32_t load_to_x0_consumes_mouse_event(void)
{
    volatile uint32_t *mouse_type = (volatile uint32_t *)0xa0000070u;
    uint32_t second_type;
    asm volatile(
        "lw x0, 0(%1)\n"
        "lw %0, 0(%1)\n"
        : "=&r"(second_type)
        : "r"(mouse_type)
        : "memory");
    return second_type;
}

int main(void)
{
    check(load_lb(&bytes[1]) == 0x0000007fu);
    check(load_lb(&bytes[2]) == 0xffffff80u);
    check(load_lb(&bytes[3]) == 0xffffffffu);
    check(load_lbu(&bytes[2]) == 0x00000080u);
    check(load_lbu(&bytes[3]) == 0x000000ffu);

    check(load_lh(&bytes[1]) == 0xffff807fu);
    check(load_lh(&bytes[4]) == 0x00001234u);
    check(load_lhu(&bytes[1]) == 0x0000807fu);
    check(load_lhu(&bytes[4]) == 0x00001234u);
    check(load_lw(&bytes[4]) == 0x56781234u);
    check(load_lw(&bytes[5]) == 0xaa567812u);

    store_sb((volatile uint8_t *)&bytes[8], 0x123456ffu);
    check(bytes[8] == 0xffu);
    store_sh((volatile uint8_t *)&bytes[9], 0xabcdef01u);
    check(bytes[9] == 0x01u);
    check(bytes[10] == 0xefu);
    store_sw((volatile uint8_t *)&bytes[12], 0x78563412u);
    check(load_lw(&bytes[12]) == 0x78563412u);

    check(load_to_x0_keeps_following_state(0x12345678u, &bytes[2]) ==
          0x12345679u);
    /* With NEMU_MOUSE_SCRIPT providing one event, the x0 load must consume it.
   * Without a script this still passes, but it does not exercise the side
   * effect. */
    check(load_to_x0_consumes_mouse_event() == 0u);

    return 0;
}
