#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
export AM_HOME="$ROOT/abstract-machine"
export NEMU_HOME="$ROOT/nemu"
export NAVY_HOME="$ROOT/navy-apps"
export ISA=riscv64
export ARCH=riscv64-nemu
export SDL_AUDIODRIVER=dummy
export SDL_VIDEODRIVER=dummy

DEFCONFIG="$NEMU_HOME/configs/riscv64-am-headless-jit_defconfig"
TESTS=(riscv64-jit-strict riscv64-jit-smc riscv64-jit-load-fast riscv64-jit-store-fast)

fail() {
  echo "RISC-V64 JIT correctness check failed: $*" >&2
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

require_positive_native_loads() {
  local log=$1
  local test_name=$2
  local native_loads

  native_loads=$(sed -n 's/.*native loads = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$native_loads" ]; then
    echo "Failed to find native load stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$native_loads" -le 0 ]; then
    echo "Expected positive native load count for $test_name, got $native_loads" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_positive_native_stores() {
  local log=$1
  local test_name=$2
  local native_stores

  native_stores=$(sed -n 's/.*native stores = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$native_stores" ]; then
    echo "Failed to find native store stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$native_stores" -le 0 ]; then
    echo "Expected positive native store count for $test_name, got $native_stores" >&2
    cat "$log" >&2
    exit 1
  fi
}

cd "$ROOT"

[ -f "$DEFCONFIG" ] || fail "missing $DEFCONFIG"
make -C "$NEMU_HOME" riscv64-am-headless-jit_defconfig >/dev/null

for test_name in "${TESTS[@]}"; do
  out=$(mktemp)
  trap 'rm -f "$out"' EXIT

  if ! NEMU_JIT_STATS=1 make -C am-kernels/tests/cpu-tests ARCH="$ARCH" ALL="$test_name" run >"$out" 2>&1; then
    echo "$test_name failed" >&2
    cat "$out" >&2
    exit 2
  fi

  require_positive_jit_instructions "$out" "$test_name"
  if [ "$test_name" = "riscv64-jit-load-fast" ]; then
    require_positive_native_loads "$out" "$test_name"
  fi
  if [ "$test_name" = "riscv64-jit-store-fast" ]; then
    require_positive_native_stores "$out" "$test_name"
  fi
  rm -f "$out"
  trap - EXIT
done

echo "RISC-V64 JIT correctness gate passed: ${TESTS[*]}"
