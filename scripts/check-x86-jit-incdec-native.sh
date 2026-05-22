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
  echo "x86 JIT native inc/dec check failed: $*" >&2
  cat "$out" >&2
  exit 1
}

cd "$ROOT"
make -C "$NEMU_HOME" x86-am-jit_defconfig >/dev/null

if ! NEMU_JIT_STATS=1 NEMU_X86_JIT_HELPERS=1 \
    make -C am-kernels/tests/cpu-tests ARCH="$ARCH" \
    NEMU_DEFCONFIG=x86-am-jit_defconfig ALL="jit-incdec-reg jit-incdec-loop" run >"$out" 2>&1; then
  fail "jit-incdec CPU tests failed"
fi

native_incdec=$(sed -n 's/.*native inc\/dec ops = \([0-9][0-9]*\).*/\1/p' "$out" | tail -n 1)
native_incdec_jcc=$(sed -n 's/.*native inc\/dec Jcc backedges = \([0-9][0-9]*\).*/\1/p' "$out" | tail -n 1)
helper_incdec=$(sed -n 's/.*helper inc\/dec calls = \([0-9][0-9]*\).*/\1/p' "$out" | tail -n 1)
helper_incdec_reg=$(sed -n 's/.*helper inc\/dec register calls = \([0-9][0-9]*\).*/\1/p' "$out" | tail -n 1)

[ -n "$native_incdec" ] || fail "missing native inc/dec stats"
[ -n "$native_incdec_jcc" ] || fail "missing native inc/dec Jcc backedge stats"
[ -n "$helper_incdec" ] || fail "missing helper inc/dec stats"
[ -n "$helper_incdec_reg" ] || fail "missing helper register inc/dec stats"

if [ "$native_incdec" -lt 1 ]; then
  fail "expected native inc/dec lowering, got $native_incdec native ops"
fi

if [ "$native_incdec_jcc" -lt 1 ]; then
  fail "expected native inc/dec Jcc backedge fusion, got $native_incdec_jcc"
fi

if [ "$helper_incdec_reg" -ne 0 ]; then
  fail "expected 32-bit register inc/dec to avoid helper calls, got $helper_incdec_reg"
fi

echo "x86 JIT native inc/dec check passed: native=$native_incdec fused=$native_incdec_jcc helper=$helper_incdec helper_reg=$helper_incdec_reg"
