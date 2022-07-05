#ifndef __COMMON_H__
#define __COMMON_H__

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>

#include <generated/autoconf.h>
#include <macro.h>

#ifdef CONFIG_TARGET_AM
#include <klib.h>
#else
#include <assert.h>
#include <stdlib.h>
#endif

#if CONFIG_MBASE + CONFIG_MSIZE > 0x100000000ul
#define PMEM64 1
#endif

typedef MUXDEF(CONFIG_ISA64, uint64_t, uint32_t) word_t;
typedef MUXDEF(CONFIG_ISA64, int64_t, int32_t)  sword_t;
#define FMT_WORD MUXDEF(CONFIG_ISA64, "0x%016lx", "0x%08x")
#define FMT_DECIMAL_WORD MUXDEF(CONFIG_ISA64, "%" PRIu64, "%" PRIu32)
#define FMT_DECIMAL_WORD_SIGN MUXDEF(CONFIG_ISA64, "%" PRId64, "%" PRId32)
#define FMT_WORD_SCAN MUXDEF(CONFIG_ISA64, "%" SCNu64, "%" SCNu32)
#define FMT_WORD_PURE MUXDEF(CONFIG_ISA64, PRIu64, PRIu32)

typedef word_t rtlreg_t;
typedef word_t vaddr_t;
typedef MUXDEF(PMEM64, uint64_t, uint32_t) paddr_t;
#define FMT_PADDR MUXDEF(PMEM64, "0x%016lx", "0x%08x")
typedef uint16_t ioaddr_t;

#include <debug.h>

#endif
