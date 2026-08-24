#include "trap.h"

#include <stdint.h>

/*
 * The audio-control map ends at offset 35. An aligned SD at offset 32 starts
 * inside the map but crosses its end. NEMU must reject the whole span before
 * changing host backing storage or invoking the device callback.
 */
#define NEMU_AUDIO_MMIO UINT64_C(0xa0000200)
#define NEMU_AUDIO_CROSSING_SD (NEMU_AUDIO_MMIO + UINT64_C(32))

int main(void)
{
    const uintptr_t address = (uintptr_t)NEMU_AUDIO_CROSSING_SD;
    const uint64_t value = UINT64_C(0x8877665544332211);

    asm volatile("sd %0, 0(%1)" : : "r"(value), "r"(address) : "memory");

    /* Reaching the normal return is a host-gate failure. */
    return 0;
}
