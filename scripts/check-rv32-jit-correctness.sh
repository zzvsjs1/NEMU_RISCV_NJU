#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
export AM_HOME="$ROOT/abstract-machine"
export NEMU_HOME="$ROOT/nemu"
export NAVY_HOME="$ROOT/navy-apps"
export ISA=riscv32
export ARCH=riscv32-nemu
export SDL_AUDIODRIVER=dummy
export SDL_VIDEODRIVER=dummy

DEFCONFIG="$NEMU_HOME/configs/riscv32-am-headless-jit_defconfig"
TESTS=(
  riscv32-jit-system-fence
  jit-ldst-signext-asm
  jit-branches-asm
  jit-smc
  jit-paging-remap
  jit-paging-cross-page
  jit-trap-boundary
  riscv-csr-trap-strict
  riscv-fault-order-strict
  div
  mul-longlong
)

tmp_files=()

cleanup() {
  rm -f "${tmp_files[@]}"
}

trap cleanup EXIT

fail() {
  echo "RISC-V32 JIT correctness check failed: $*" >&2
  exit 1
}

extract_last_stat() {
  local pattern=$1
  local log=$2

  sed -n "$pattern" "$log" | tail -n 1
}

require_positive_jit_instructions() {
  local log=$1
  local test_name=$2
  local jit_insns

  jit_insns=$(extract_last_stat 's/.*JIT instructions = \([0-9][0-9]*\).*/\1/p' "$log")
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

require_positive_unsupported_hits() {
  local log=$1
  local test_name=$2
  local unsupported_hits

  unsupported_hits=$(extract_last_stat 's/.*unsupported hits = \([0-9][0-9]*\).*/\1/p' "$log")
  if [ -z "$unsupported_hits" ]; then
    echo "Failed to find unsupported-hit stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$unsupported_hits" -le 0 ]; then
    echo "Expected positive unsupported-hit count for $test_name, got $unsupported_hits" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_positive_invalidated_blocks() {
  local log=$1
  local test_name=$2
  local invalidated_blocks

  invalidated_blocks=$(extract_last_stat 's/.*invalidated blocks = \([0-9][0-9]*\).*/\1/p' "$log")
  if [ -z "$invalidated_blocks" ]; then
    echo "Failed to find invalidated-block stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$invalidated_blocks" -le 0 ]; then
    echo "Expected positive invalidated-block count for $test_name, got $invalidated_blocks" >&2
    cat "$log" >&2
    exit 1
  fi
}

cd "$ROOT"

[ -f "$DEFCONFIG" ] || fail "missing $DEFCONFIG"
make -C "$NEMU_HOME" riscv32-am-headless-jit_defconfig >/dev/null

for test_name in "${TESTS[@]}"; do
  out=$(mktemp)
  tmp_files+=("$out")

  if ! NEMU_JIT_STATS=1 make -C am-kernels/tests/cpu-tests ARCH="$ARCH" ALL="$test_name" run >"$out" 2>&1; then
    echo "$test_name failed" >&2
    cat "$out" >&2
    exit 2
  fi

  require_positive_jit_instructions "$out" "$test_name"
  if [ "$test_name" = "riscv32-jit-system-fence" ]; then
    require_positive_unsupported_hits "$out" "$test_name"
  fi
  if [ "$test_name" = "jit-smc" ]; then
    require_positive_invalidated_blocks "$out" "$test_name"
  fi
done

scripts/check-rv32-jit-branch-chain.sh

echo "RISC-V32 JIT correctness gate passed: ${TESTS[*]}"
