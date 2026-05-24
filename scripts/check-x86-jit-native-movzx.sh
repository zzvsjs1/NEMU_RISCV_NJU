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
  echo "x86 JIT native-movzx check failed: $*" >&2
  cat "$out" >&2
  exit 1
}

cd "$ROOT"
make -C "$NEMU_HOME" x86-am-jit_defconfig >/dev/null

if ! NEMU_JIT_STATS=1 NEMU_X86_JIT_HELPERS=1 \
    make -C am-kernels/tests/cpu-tests ARCH="$ARCH" \
    NEMU_DEFCONFIG=x86-am-jit_defconfig ALL=jit-movzx run >"$out" 2>&1; then
  fail "jit-movzx CPU test failed"
fi

native_movzx=$(sed -n 's/.*native movzx ops = \([0-9][0-9]*\).*/\1/p' "$out" | tail -n 1)
[ -n "$native_movzx" ] || fail "missing native movzx stats"
movzx_helpers=$(sed -n 's/.*helper profile movzx-reg-rm8[[:space:]]*calls = \([0-9][0-9]*\).*/\1/p' "$out" | tail -n 1)
movzx_helpers=${movzx_helpers:-0}

if [ "$native_movzx" -lt 1 ]; then
  fail "expected at least one native movzx op, got $native_movzx"
fi

if [ "$movzx_helpers" -ne 0 ]; then
  fail "expected high-byte MOVZX to stay native, got movzx-reg-rm8 helpers=$movzx_helpers"
fi

echo "x86 JIT native-movzx check passed: native_movzx=$native_movzx movzx_helpers=$movzx_helpers"
