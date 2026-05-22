#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
export AM_HOME="$ROOT/abstract-machine"
export NEMU_HOME="$ROOT/nemu"
export NAVY_HOME="$ROOT/navy-apps"
export ISA=x86
export ARCH=x86-nemu
export SDL_AUDIODRIVER=dummy
export SDL_VIDEODRIVER=dummy

MAX_PRECISE_SCANS=1000

fail() {
  echo "x86 JIT invalidation filter check failed: $*" >&2
  exit 1
}

out=$(mktemp)
trap 'rm -f "$out"' EXIT

make -C "$NEMU_HOME" x86-am-jit_defconfig >/dev/null

NEMU_X86_JIT_HELPERS=1 NEMU_JIT_STATS=1 \
  make -C "$ROOT/am-kernels/benchmarks/microbench" \
  ARCH="$ARCH" NEMU_DEFCONFIG=x86-am-jit_defconfig mainargs=test run >"$out" 2>&1 || {
    cat "$out" >&2
    exit 2
  }

grep -q 'MicroBench PASS' "$out" || {
  cat "$out" >&2
  fail "MicroBench did not pass"
}

precise_scans=$(
  sed -n 's/.*precise invalidation scans = \([0-9][0-9]*\).*/\1/p' "$out" |
    tail -n 1
)

if [ -z "$precise_scans" ]; then
  precise_scans=$(
    sed -n 's/.*invalidation requests = \([0-9][0-9]*\).*/\1/p' "$out" |
      tail -n 1
  )
fi

[ -n "$precise_scans" ] || {
  cat "$out" >&2
  fail "could not parse invalidation scan count"
}

if [ "$precise_scans" -gt "$MAX_PRECISE_SCANS" ]; then
  cat "$out" >&2
  fail "expected at most $MAX_PRECISE_SCANS precise invalidation scans, got $precise_scans"
fi

printf "x86_jit_precise_invalidation_scans=%s\n" "$precise_scans"
