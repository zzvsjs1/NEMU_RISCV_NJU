#include <generated/autoconf.h>

#ifndef CONFIG_RV64

/*
 * The RV32 JIT is intentionally split into normal compilation units:
 *
 *   jit-rv32-core.c     public hooks, cache/arena state, and dispatch
 *   jit-rv32-compile.c  fetch validation and native block publication
 *   jit-rv32-emit.c     x86-64 emission and register caching
 *   jit-rv32-mem.c      Sv32/PMEM helpers and source-byte tracking
 *   jit-rv32-stats.c    optional statistics presentation
 *
 * Shared private declarations live in jit-rv32-internal.h. This tracked file
 * remains as a compatibility source-map signpost and deliberately defines no
 * symbols.
 */

#endif /* !CONFIG_RV64 */
