#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT

with_compiler=0
if [ "${1:-}" = --with-compiler ]; then
  with_compiler=1
  shift
fi

if [ "$#" -gt 1 ]; then
  printf 'Usage: %s [--with-compiler] [snapshot-dir]\n' "${0##*/}" >&2
  exit 1
fi

# The generated configuration remains the authority for ISA/CPU layout. Copy
# it so the two instrumentation variants never change a developer's .config.
mkdir -p "$build_dir/include/generated"
config="$ROOT/nemu/include/generated/autoconf.h"
if [ ! -f "$config" ] || ! grep -q '^#define CONFIG_RV64 1$' "$config"; then
  printf 'Generate an RV64 NEMU configuration before running this fixture.\n' >&2
  exit 1
fi

if [ "$with_compiler" -eq 1 ]; then
  # Unlike the host fixtures, this test links actual NEMU objects. Their CPU
  # layout and instrumentation must match the original generated configuration.
  for required in CONFIG_RV64_JIT CONFIG_RV64_JIT_STATS CONFIG_TARGET_NATIVE_ELF; do
    if ! grep -q "^#define $required 1$" "$config"; then
      printf '%s must be enabled in the current NEMU configuration for --with-compiler.\n' "$required" >&2
      exit 1
    fi
  done

  if grep -Eq '^#define CONFIG_(TRACE|DIFFTEST|WATCHPOINT|MTRACE|FTRACE) 1$' "$config"; then
    printf 'The current NEMU configuration excludes native RV64 JIT execution.\n' >&2
    exit 1
  fi
fi

# Optional output directory retains byte/metadata snapshots for a before/after
# diff; otherwise they are checked and removed with the temporary build tree.
output_dir=${1:-$build_dir/results}
mkdir -p "$output_dir"

for stats in 0 1; do
  cp "$config" "$build_dir/include/generated/autoconf.h"
  printf '\n#undef CONFIG_RV64_JIT_STATS\n' >>"$build_dir/include/generated/autoconf.h"
  if [ "$stats" -eq 1 ]; then
    printf '#define CONFIG_RV64_JIT_STATS 1\n' >>"$build_dir/include/generated/autoconf.h"
  fi

  # Private implementation inclusion is a fixture technique, never a NEMU
  # module boundary. Dead-section removal avoids stubbing unrelated emulation.
  "${CC:-gcc}" -O2 -g -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -D__GUEST_ISA__=riscv64 \
    -I"$build_dir/include" -I"$ROOT/nemu/include" \
    -I"$ROOT/nemu/src/isa/riscv64/include" \
    "$SCRIPT_DIR/fixtures/rv64-jit-link-pilot.c" \
    "$SCRIPT_DIR/fixtures/rv64-jit-emitter-pilot.c" \
    -o "$build_dir/pilot-$stats"

  "$build_dir/pilot-$stats" >"$output_dir/pilot-stats-$stats.txt"
  tail -n 1 "$output_dir/pilot-stats-$stats.txt"

  if [ "${RV64_JIT_PILOT_BENCH_ITERATIONS:-0}" -gt 0 ]; then
    for sample in 1 2 3 4 5; do
      "$build_dir/pilot-$stats" --benchmark "$RV64_JIT_PILOT_BENCH_ITERATIONS"
    done >"$output_dir/emission-stats-$stats.txt"
  fi
done

if [ "$with_compiler" -eq 1 ]; then
  # Extend the real Makefile for this invocation only. Its selected objects,
  # archives, toolchain, flags and dependency rules remain authoritative; this
  # cannot accidentally link stale objects from another configuration directory.
  export NEMU_HOME="$ROOT/nemu"
  make --no-print-directory -C "$NEMU_HOME" -f Makefile -f - jit-maintainability-compiler-fixture \
    RV64_JIT_FIXTURE_SOURCE="$SCRIPT_DIR/fixtures/rv64-jit-compile-lifecycle.c" \
    RV64_JIT_FIXTURE_OBJECT="$build_dir/compiler-fixture.o" \
    RV64_JIT_FIXTURE_BINARY="$build_dir/compiler-fixture" <<'MAKE'
.PHONY: jit-maintainability-compiler-fixture
jit-maintainability-compiler-fixture: $(OBJS) $(ARCHIVES)
	@$(CC) $(CFLAGS) -UNDEBUG -c "$(RV64_JIT_FIXTURE_SOURCE)" -o "$(RV64_JIT_FIXTURE_OBJECT)"
	@$(LD) -o "$(RV64_JIT_FIXTURE_BINARY)" "$(RV64_JIT_FIXTURE_OBJECT)" \
	  $(foreach object,$(filter-out $(OBJ_DIR)/src/nemu-main.o,$(OBJS)),"$(object)") \
	  $(LDFLAGS) $(ARCHIVES) $(LIBS) \
	  -Wl,--wrap=rv64_jit_emit_load_instr -Wl,--wrap=rv64_jit_perf_map_publish
MAKE

  "$build_dir/compiler-fixture" >"$output_dir/compiler-lifecycle.txt"
  tail -n 1 "$output_dir/compiler-lifecycle.txt"

  if [ "${RV64_JIT_COMPILE_BENCH_ITERATIONS:-0}" -gt 0 ]; then
    for sample in 1 2 3 4 5; do
      "$build_dir/compiler-fixture" --benchmark "$RV64_JIT_COMPILE_BENCH_ITERATIONS"
    done >"$output_dir/compiler-benchmark.txt"
  fi
fi
