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
  echo "x86 JIT high-byte TEST check failed: $*" >&2
  cat "$out" >&2
  exit 1
}

cd "$ROOT"
make -C "$NEMU_HOME" x86-am-jit_defconfig >/dev/null

if ! NEMU_JIT_STATS=1 NEMU_X86_JIT_HELPERS=1 \
    NEMU_X86_JIT_HIGH_BYTE_TEST=1 \
    make -C am-kernels/tests/cpu-tests ARCH="$ARCH" \
    NEMU_DEFCONFIG=x86-am-jit_defconfig ALL=jit-high-byte-test run \
    >"$out" 2>&1; then
  fail "jit-high-byte-test CPU test failed"
fi

native_alu=$(sed -n 's/.*native ALU ops = \([0-9][0-9]*\).*/\1/p' "$out" | tail -n 1)
test_helpers=$(sed -n 's/.*helper profile test-imm-rm[[:space:]]*calls = \([0-9][0-9]*\).*/\1/p' "$out" | tail -n 1)
test_helpers=${test_helpers:-0}

[ -n "$native_alu" ] || fail "missing native ALU stats"

if [ "$native_alu" -lt 4 ]; then
  fail "expected high-byte TEST to lower natively, got native_alu=$native_alu"
fi

if [ "$test_helpers" -ne 0 ]; then
  fail "expected high-byte TEST to avoid helpers, got test-imm-rm helpers=$test_helpers"
fi

echo "x86 JIT high-byte TEST check passed: native_alu=$native_alu test_helpers=$test_helpers"
