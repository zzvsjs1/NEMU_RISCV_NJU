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

DEFCONFIG="$NEMU_HOME/configs/x86-am-jit_defconfig"
TESTS=(add)

tmp_files=()

cleanup() {
  rm -f "${tmp_files[@]}"
}

trap cleanup EXIT

fail() {
  echo "x86 JIT smoke check failed: $*" >&2
  exit 1
}

require_positive_jit_instructions() {
  local log=$1
  local test_name=$2
  local jit_insns

  jit_insns=$(sed -n 's/.*JIT instructions = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$jit_insns" ]; then
    echo "Failed to find JIT instruction stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$jit_insns" -le 0 ]; then
    echo "Expected positive JIT instruction count for $test_name, got $jit_insns" >&2
    cat "$log" >&2
    exit 1
  fi
}

cd "$ROOT"

[ -f "$DEFCONFIG" ] || fail "missing $DEFCONFIG"
make -C "$NEMU_HOME" x86-am-jit_defconfig >/dev/null

for test_name in "${TESTS[@]}"; do
  out=$(mktemp)
  tmp_files+=("$out")

  if ! NEMU_JIT_STATS=1 make -C am-kernels/tests/cpu-tests ARCH="$ARCH" \
      NEMU_DEFCONFIG=x86-am-jit_defconfig ALL="$test_name" run >"$out" 2>&1; then
    echo "$test_name failed" >&2
    cat "$out" >&2
    exit 2
  fi

  require_positive_jit_instructions "$out" "$test_name"
done

echo "x86 JIT smoke gate passed: ${TESTS[*]}"
