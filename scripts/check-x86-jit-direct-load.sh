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

out=$(mktemp)

cleanup() {
  rm -f "$out"
}

trap cleanup EXIT

fail() {
  echo "x86 JIT direct-load check failed: $*" >&2
  cat "$out" >&2
  exit 1
}

cd "$ROOT"
make -C "$NEMU_HOME" x86-am-jit_defconfig >/dev/null

if ! NEMU_JIT_STATS=1 NEMU_X86_JIT_HELPERS=1 \
    make -C am-kernels/tests/cpu-tests ARCH="$ARCH" \
    NEMU_DEFCONFIG=x86-am-jit_defconfig ALL=jit-direct-load run >"$out" 2>&1; then
  fail "jit-direct-load CPU test failed"
fi

native_loads=$(sed -n 's/.*native PMEM loads = \([0-9][0-9]*\).*/\1/p' "$out" | tail -n 1)
[ -n "$native_loads" ] || fail "missing native PMEM load stats"

if [ "$native_loads" -lt 1 ]; then
  fail "expected at least one native PMEM load, got $native_loads"
fi

echo "x86 JIT direct-load check passed: native_loads=$native_loads"
