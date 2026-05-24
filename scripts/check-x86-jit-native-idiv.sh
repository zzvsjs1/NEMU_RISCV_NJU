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
  echo "x86 JIT native IDIV check failed: $*" >&2
  cat "$out" >&2
  exit 1
}

cd "$ROOT"
make -C "$NEMU_HOME" x86-am-jit_defconfig >/dev/null

if ! NEMU_JIT_STATS=1 NEMU_X86_JIT_HELPERS=1 NEMU_X86_JIT_NATIVE_IDIV=1 \
    make -C am-kernels/tests/cpu-tests ARCH="$ARCH" \
    NEMU_DEFCONFIG=x86-am-jit_defconfig ALL=jit-idiv-native run \
    >"$out" 2>&1; then
  fail "jit-idiv-native CPU test failed"
fi

native_div=$(sed -n 's/.*native div ops = \([0-9][0-9]*\).*/\1/p' "$out" | tail -n 1)
idiv_helpers=$(sed -n 's/.*helper profile idiv-rm[[:space:]]*calls = \([0-9][0-9]*\).*/\1/p' "$out" | tail -n 1)
idiv_helpers=${idiv_helpers:-0}

[ -n "$native_div" ] || fail "missing native div stats"

if [ "$native_div" -lt 2 ]; then
  fail "expected native IDIV lowering, got native_div=$native_div"
fi

if [ "$idiv_helpers" -ne 0 ]; then
  fail "expected safe IDIV to avoid helpers, got idiv-rm helpers=$idiv_helpers"
fi

echo "x86 JIT native IDIV check passed: native_div=$native_div idiv_helpers=$idiv_helpers"
