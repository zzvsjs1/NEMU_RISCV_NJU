#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
export AM_HOME="$ROOT/abstract-machine"
export NEMU_HOME="$ROOT/nemu"
export ISA=riscv64
export ARCH=riscv64-nemu
export SDL_AUDIODRIVER=dummy
export SDL_VIDEODRIVER=dummy

BENCH_DIR="$ROOT/am-kernels/benchmarks/storemark"
JIT_DEFCONFIG=riscv64-am-headless-jit-stats_defconfig
MAX_INVALIDATION_REQUESTS=100000
MAX_HELPER_STORES=100000

fail() {
  echo "RISC-V64 JIT store invalidation check failed: $*" >&2
  exit 1
}

out=$(mktemp)
trap 'rm -f "$out"' EXIT

make -C "$NEMU_HOME" "$JIT_DEFCONFIG" >/dev/null

NEMU_JIT_STATS=1 make -C "$BENCH_DIR" ARCH="$ARCH" run >"$out" 2>&1 || {
  cat "$out" >&2
  exit 2
}

grep -q 'StoreMark PASS' "$out" || {
  cat "$out" >&2
  fail "StoreMark did not pass"
}

invalidations=$(sed -n 's/.*invalidation requests = \([0-9][0-9]*\).*/\1/p' "$out" | tail -n 1)
[ -n "$invalidations" ] || {
  cat "$out" >&2
  fail "could not parse invalidation request count"
}

if [ "$invalidations" -gt "$MAX_INVALIDATION_REQUESTS" ]; then
  cat "$out" >&2
  fail "expected at most $MAX_INVALIDATION_REQUESTS invalidation requests, got $invalidations"
fi

storemark_us=$(sed -n 's/.*storemark_total_us: \([0-9][0-9]*\).*/\1/p' "$out" | tail -n 1)
helper_stores=$(sed -n 's/.*helper loads = [0-9][0-9]*, helper stores = \([0-9][0-9]*\).*/\1/p' "$out" | tail -n 1)
[ -n "$helper_stores" ] || {
  cat "$out" >&2
  fail "could not parse helper store count"
}

if [ "$helper_stores" -gt "$MAX_HELPER_STORES" ]; then
  cat "$out" >&2
  fail "expected at most $MAX_HELPER_STORES helper stores, got $helper_stores"
fi

printf "storemark_us=%s invalidation_requests=%s helper_stores=%s\n" \
  "${storemark_us:-unknown}" "$invalidations" "$helper_stores"
