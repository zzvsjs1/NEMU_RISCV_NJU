#include <am.h>
#include <klib.h>
#define NEMU_PLATFORM_CONSTANTS_ONLY
#include <nemu.h>
#include <stdint.h>

#define IOMARK_ITERS 1000000u
#define IOMARK_EXPECTED 0x214f3b42u

static uint64_t uptime_us(void)
{
    return io_read(AM_TIMER_UPTIME).us;
}

static uintptr_t vgactl_reg(uint32_t reg)
{
    return VGACTL_ADDR + (uintptr_t)reg * sizeof(uint32_t);
}

static uint32_t mmio_read32(uintptr_t addr)
{
    return *(volatile uint32_t *)addr;
}

static void mmio_write32(uintptr_t addr, uint32_t data)
{
    *(volatile uint32_t *)addr = data;
}

__attribute__((noinline)) static uint32_t io_hot_loop(uint32_t iters)
{
    uint32_t acc = 0x13579bdfu;

    for (uint32_t i = 0; i < iters; i++)
    {
        /*
         * BLIT_SRC is a plain VGA-control register until BLIT_CMD is written, so
         * this loop exercises MMIO dispatch without causing screen updates.
         */
        mmio_write32(vgactl_reg(NEMU_VGACTL_BLIT_SRC), acc ^ i);
        uint32_t src = mmio_read32(vgactl_reg(NEMU_VGACTL_BLIT_SRC));
        uint32_t info = mmio_read32(vgactl_reg(NEMU_VGACTL_INFO));
        acc = (acc << 5) | (acc >> 27);
        acc ^= src + info + i * 0x45d9f3bu;
    }

    return acc;
}

int main(const char *args)
{
    (void)args;
    ioe_init();

    uint64_t start = uptime_us();
    uint32_t checksum = io_hot_loop(IOMARK_ITERS);
    uint64_t end = uptime_us();
    bool pass = checksum == IOMARK_EXPECTED;

    printf("iomark_total_us: %d\n", (int)(end - start));
    printf("iomark_iters: %u\n", IOMARK_ITERS);
    printf("iomark_checksum: 0x%x\n", checksum);
    printf("IOMark %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
