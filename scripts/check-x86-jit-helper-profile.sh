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
  echo "x86 JIT helper profile check failed: $*" >&2
  cat "$out" >&2
  exit 1
}

cd "$ROOT"
make -C "$NEMU_HOME" x86-am-jit_defconfig >/dev/null

if ! NEMU_JIT_STATS=1 NEMU_X86_JIT_HELPERS=1 \
    make -C am-kernels/benchmarks/microbench ARCH="$ARCH" \
    NEMU_DEFCONFIG=x86-am-jit_defconfig mainargs=test run >"$out" 2>&1; then
  fail "microbench test failed"
fi

grep -q 'jit: helper profile ' "$out" || fail "missing helper profile stats"
grep -q 'jit: helper profile .* calls = [1-9][0-9]*' "$out" || \
  fail "helper profile stats did not report any dynamic calls"

echo "x86 JIT helper profile check passed"
