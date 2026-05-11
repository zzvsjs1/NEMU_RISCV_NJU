#ifndef __CONFIG_H__
#define __CONFIG_H__

#define HAS_GUI
#define SIZE_OPT

#define SOUND_NONE 0
#define SOUND_LQ 1
#define SOUND_HQ 2

#define PERF_LOW 0
#define PERF_MIDDLE 1
#define PERF_HIGH 2

// The AM/Navy port selects a conservative feature profile at compile time.
// Native and QEMU builds can keep full audio and no frame skipping, while
// NEMU uses a lighter profile so the emulator remains responsive there.  This
// mirrors the Navy performance work for PAL/ONScripter: keep the app contract
// unchanged, but reduce emulator-side CPU/audio work on the slowest backend.
#if defined(__ARCH_NATIVE) || defined(__PLATFORM_QEMU)
#define PERF_CONFIG PERF_HIGH
#elif defined(__PLATFORM_NEMU) || defined(__NAVY__)
#define PERF_CONFIG PERF_HIGH
#else
#define PERF_CONFIG PERF_LOW
#endif

#if PERF_CONFIG == PERF_HIGH
#define NR_FRAMESKIP 0
#define SOUND_CONFIG SOUND_HQ
#define FUNC_IDX_MAX256
#elif PERF_CONFIG == PERF_MIDDLE
#define NR_FRAMESKIP 1
#define SOUND_CONFIG SOUND_LQ
#define FUNC_IDX_MAX256
#else
#define NR_FRAMESKIP 2
#define SOUND_CONFIG SOUND_NONE
#define FUNC_IDX_MAX16
#endif

enum
{
    NES_BASE_WIDTH = 256,
    NES_BASE_HEIGHT = 240,
    NES_MAX_SCALE = 2,
};

#endif
