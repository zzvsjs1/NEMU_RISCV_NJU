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
  echo "x86 JIT direct-store check failed: $*" >&2
  cat "$out" >&2
  exit 1
}

cd "$ROOT"
make -C "$NEMU_HOME" x86-am-jit_defconfig >/dev/null

if ! NEMU_JIT_STATS=1 NEMU_X86_JIT_HELPERS=1 \
    make -C am-kernels/tests/cpu-tests ARCH="$ARCH" \
    NEMU_DEFCONFIG=x86-am-jit_defconfig ALL=jit-direct-store run >"$out" 2>&1; then
  fail "jit-direct-store CPU test failed"
fi

native_stores=$(sed -n 's/.*native PMEM stores = \([0-9][0-9]*\).*/\1/p' "$out" | tail -n 1)
[ -n "$native_stores" ] || fail "missing native PMEM store stats"

if [ "$native_stores" -lt 1 ]; then
  fail "expected at least one native PMEM store, got $native_stores"
fi

echo "x86 JIT direct-store check passed: native_stores=$native_stores"
