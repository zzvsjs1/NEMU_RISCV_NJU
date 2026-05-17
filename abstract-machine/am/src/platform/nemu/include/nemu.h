#ifndef NEMU_H__
#define NEMU_H__

#ifndef NEMU_PLATFORM_CONSTANTS_ONLY
#include <klib-macros.h>

#include ISA_H // the macro `ISA_H` is defined in CFLAGS
               // it will be expanded as "x86/x86.h", "mips/mips32.h", ...

#if defined(__ISA_X86__)
#define nemu_trap(code) asm volatile(".byte 0xd6" : : "a"(code))
#elif defined(__ISA_MIPS32__)
#define nemu_trap(code) asm volatile("move $v0, %0; .word 0xf0000000" : : "r"(code))
#elif defined(__ISA_RISCV32__) || defined(__ISA_RISCV64__) || defined(__riscv)
#define nemu_trap(code) asm volatile("mv a0, %0; .word 0x0000006b" : : "r"(code))
#else
#error unsupported ISA __ISA__
#endif
#endif

#if defined(__ARCH_X86_NEMU)
#define DEVICE_BASE 0x0
#else
#define DEVICE_BASE 0xa0000000
#endif

#define MMIO_BASE 0xa0000000

/* These addresses are part of the guest/device ABI: AM code must treat them
 * as fixed MMIO registers, while NEMU supplies the implementation behind each
 * range.  Keep framebuffer and audio sample storage outside the small control
 * page because bulk pixel/sample traffic would otherwise overlap registers.
 */
#define SERIAL_PORT (DEVICE_BASE + 0x00003f8)
#define KBD_ADDR (DEVICE_BASE + 0x0000060)
#define MOUSE_ADDR (DEVICE_BASE + 0x0000070)
#define RTC_ADDR (DEVICE_BASE + 0x0000048)
#define VGACTL_ADDR (DEVICE_BASE + 0x0000100)

enum
{
    NEMU_RTC_UPTIME_US_LO = 0x00u,
    NEMU_RTC_UPTIME_US_HI = 0x04u,
    NEMU_RTC_EPOCH_SEC_LO = 0x08u,
    NEMU_RTC_EPOCH_SEC_HI = 0x0cu,
    NEMU_RTC_EPOCH_US_LO = 0x10u,
    NEMU_RTC_EPOCH_US_HI = 0x14u,
    NEMU_RTC_MMIO_SIZE = 0x18u,
};

/*
 * The registers below are NEMU-private extensions in the VGA control page.
 * They are shared between the NEMU platform implementation in AM and the NEMU
 * device model so both sides agree on the same compact register layout.
 *
 * These values are not public AM GPU events. Portable AM code should continue
 * to use the normal AM_GPU_* interfaces; only platform-specific NEMU glue may
 * issue these commands to batch framebuffer copies or request a hidden
 * framebuffer capture.
 */
enum
{
    NEMU_VGACTL_INFO = 0u,
    NEMU_VGACTL_SYNC = 1u,
    NEMU_VGACTL_BLIT_SRC = 2u,
    NEMU_VGACTL_BLIT_POS = 3u,
    NEMU_VGACTL_BLIT_SIZE = 4u,
    NEMU_VGACTL_BLIT_CMD = 5u,
    NEMU_VGACTL_CAPTURE_DST = 6u,
    NEMU_VGACTL_CAPTURE_CMD = 7u,
};

#define NEMU_VGACTL_BLIT_CMD_COPY 1u
#define NEMU_VGACTL_CAPTURE_CMD_COPY 1u

#define AUDIO_ADDR (DEVICE_BASE + 0x0000200)
#define DISK_ADDR (DEVICE_BASE + 0x0000300)
#define FB_ADDR (MMIO_BASE + 0x1000000)
#define AUDIO_SBUF_ADDR (MMIO_BASE + 0x1200000)

extern char _pmem_start;
#define PMEM_SIZE (128 * 1024 * 1024)
#define PMEM_END ((uintptr_t) & _pmem_start + PMEM_SIZE)

#define AUDIO_SBUF_SIZE 0x10000

#define NEMU_PADDR_SPACE \
    RANGE(&_pmem_start, PMEM_END), \
        RANGE(FB_ADDR, FB_ADDR + 0x200000), \
        RANGE(MMIO_BASE, MMIO_BASE + 0x1000),                     /* serial, rtc, screen, keyboard, audio-ctl, disk */ \
        RANGE(AUDIO_SBUF_ADDR, AUDIO_SBUF_ADDR + AUDIO_SBUF_SIZE) /* audio sample buffer */

typedef uintptr_t PTE;

#define PGSIZE 4096

#endif
