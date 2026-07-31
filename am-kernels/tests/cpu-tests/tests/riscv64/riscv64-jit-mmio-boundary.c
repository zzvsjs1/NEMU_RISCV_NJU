#include "trap.h"

#include <stdint.h>

/*
 * The audio-control map contains nine 32-bit registers, so its final valid byte
 * is at offset 35.  An aligned LD beginning at offset 32 starts inside the map
 * but crosses its end.  NEMU must reject the complete access before invoking
 * the callback or reading host backing bytes.
 */
#define NEMU_AUDIO_MMIO 0xa0000200ull
#define NEMU_AUDIO_CROSSING_LD (NEMU_AUDIO_MMIO + 32ull)

int main(void)
{
    uint64_t observed = 0;
    const uintptr_t address = (uintptr_t)NEMU_AUDIO_CROSSING_LD;

    /*
     * Literal assembly guarantees one naturally aligned eight-byte guest load.
     * Reaching the return is a test failure detected by the host-side gate.
     */
    asm volatile("ld %0, 0(%1)"
                 : "=r"(observed)
                 : "r"(address)
                 : "memory");
    (void)observed;
    return 0;
}
