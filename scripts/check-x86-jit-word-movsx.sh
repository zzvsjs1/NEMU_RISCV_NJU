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
  echo "x86 JIT word MOVSX check failed: $*" >&2
  cat "$out" >&2
  exit 1
}

cd "$ROOT"
make -C "$NEMU_HOME" x86-am-jit_defconfig >/dev/null

if ! NEMU_JIT_STATS=1 NEMU_X86_JIT_HELPERS=1 \
    make -C am-kernels/tests/cpu-tests ARCH="$ARCH" \
    NEMU_DEFCONFIG=x86-am-jit_defconfig ALL=jit-word-movsx run >"$out" 2>&1; then
  fail "jit-word-movsx CPU test failed"
fi

native_movsx=$(sed -n 's/.*native movsx ops = \([0-9][0-9]*\).*/\1/p' "$out" | tail -n 1)
unsupported_66_hits=$(sed -n 's/.*unsupported-hit opcode 0x66 = \([0-9][0-9]*\).*/\1/p' "$out" | tail -n 1)
unsupported_66_hits=${unsupported_66_hits:-0}

[ -n "$native_movsx" ] || fail "missing native movsx stats"

if [ "$native_movsx" -lt 2 ]; then
  fail "expected native word MOVSX lowering, got native_movsx=$native_movsx"
fi

if [ "$unsupported_66_hits" -ne 0 ]; then
  fail "expected word MOVSX not to publish unsupported 0x66 blocks, got $unsupported_66_hits"
fi

echo "x86 JIT word MOVSX check passed: native_movsx=$native_movsx unsupported_66_hits=$unsupported_66_hits"
