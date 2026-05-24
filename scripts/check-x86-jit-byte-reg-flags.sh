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
  echo "x86 JIT byte register flags check failed: $*" >&2
  cat "$out" >&2
  exit 1
}

cd "$ROOT"
make -C "$NEMU_HOME" x86-am-jit_defconfig >/dev/null

if ! NEMU_JIT_STATS=1 NEMU_X86_JIT_HELPERS=1 \
    make -C am-kernels/tests/cpu-tests ARCH="$ARCH" \
    NEMU_DEFCONFIG=x86-am-jit_defconfig ALL=jit-byte-reg-flags run \
    >"$out" 2>&1; then
  fail "jit-byte-reg-flags CPU test failed"
fi

native_alu=$(sed -n 's/.*native ALU ops = \([0-9][0-9]*\).*/\1/p' "$out" | tail -n 1)
alu_helpers=$(sed -n 's/.*helper profile alu-.*calls = \([0-9][0-9]*\).*/\1/p' "$out" | awk '{sum += $1} END {print sum + 0}')
neg_helpers=$(sed -n 's/.*helper profile neg-rm[[:space:]]*calls = \([0-9][0-9]*\).*/\1/p' "$out" | tail -n 1)
neg_helpers=${neg_helpers:-0}

[ -n "$native_alu" ] || fail "missing native ALU stats"

if [ "$native_alu" -lt 6 ]; then
  fail "expected byte register ALU to lower natively, got native_alu=$native_alu"
fi

if [ "$alu_helpers" -ne 0 ] || [ "$neg_helpers" -ne 0 ]; then
  fail "expected byte register ALU to avoid helpers, got alu_helpers=$alu_helpers neg_helpers=$neg_helpers"
fi

echo "x86 JIT byte register flags check passed: native_alu=$native_alu"
