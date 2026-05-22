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
TESTS=(add sum fib mov-c bit movsx shift rotate incdec byte-div mul-longlong jit-direct-load jit-direct-store jit-incdec-reg jit-incdec-loop jit-imul jit-indirect-jmp jit-movzx jit-not jit-word-ops)

tmp_files=()

cleanup() {
  rm -f "${tmp_files[@]}"
}

trap cleanup EXIT

fail() {
  echo "x86 JIT smoke check failed: $*" >&2
  exit 1
}

required_jit_instructions() {
  case "$1" in
    bit) echo 400 ;;
    byte-div) echo 20 ;;
    fib) echo 700 ;;
    incdec) echo 80 ;;
    jit-direct-load) echo 50 ;;
    jit-direct-store) echo 50 ;;
    jit-incdec-reg) echo 100 ;;
    jit-incdec-loop) echo 10000 ;;
    jit-indirect-jmp) echo 10000 ;;
    jit-movzx) echo 10000 ;;
    jit-not) echo 10000 ;;
    jit-word-ops) echo 12000 ;;
    mov-c) echo 100 ;;
    movsx) echo 150 ;;
    jit-imul) echo 10000 ;;
    mul-longlong) echo 100 ;;
    rotate) echo 20 ;;
    shift) echo 300 ;;
    sum) echo 250 ;;
    *) echo 1 ;;
  esac
}

required_max_unsupported_hits() {
  case "$1" in
    jit-indirect-jmp) echo 1000 ;;
    jit-imul) echo 100 ;;
    jit-movzx) echo 100 ;;
    jit-not) echo 100 ;;
    jit-incdec-reg) echo 20 ;;
    jit-incdec-loop) echo 10 ;;
    incdec) echo 50 ;;
    jit-direct-load) echo 20 ;;
    jit-direct-store) echo 20 ;;
    byte-div) echo 50 ;;
    jit-word-ops) echo 1000 ;;
    movsx) echo 20 ;;
    mul-longlong) echo 100 ;;
    rotate) echo 20 ;;
    shift) echo 20 ;;
    *) echo "" ;;
  esac
}

required_max_executed_blocks() {
  case "$1" in
    jit-word-ops) echo 1000 ;;
    *) echo "" ;;
  esac
}

required_min_native_alu_ops() {
  case "$1" in
    sum) echo 5 ;;
    *) echo "" ;;
  esac
}

required_min_native_incdec_ops() {
  case "$1" in
    jit-incdec-reg) echo 2 ;;
    jit-incdec-loop) echo 1 ;;
    *) echo "" ;;
  esac
}

required_min_native_incdec_jcc_backedges() {
  case "$1" in
    jit-incdec-loop) echo 1 ;;
    *) echo "" ;;
  esac
}

required_max_helper_incdec_reg_calls() {
  case "$1" in
    jit-incdec-reg) echo 0 ;;
    jit-incdec-loop) echo 0 ;;
    *) echo "" ;;
  esac
}

required_min_native_pmem_loads() {
  case "$1" in
    jit-direct-load) echo 1 ;;
    *) echo "" ;;
  esac
}

required_min_native_pmem_stores() {
  case "$1" in
    jit-direct-store) echo 1 ;;
    *) echo "" ;;
  esac
}

required_min_native_imul_ops() {
  case "$1" in
    jit-imul) echo 1 ;;
    *) echo "" ;;
  esac
}

required_min_native_shift_ops() {
  case "$1" in
    rotate) echo 1 ;;
    shift) echo 1 ;;
    *) echo "" ;;
  esac
}

required_min_native_not_ops() {
  case "$1" in
    jit-not) echo 1 ;;
    *) echo "" ;;
  esac
}

required_min_native_movzx_ops() {
  case "$1" in
    jit-movzx) echo 1 ;;
    *) echo "" ;;
  esac
}

require_min_jit_instructions() {
  local log=$1
  local test_name=$2
  local min_insns=$3
  local jit_insns

  jit_insns=$(sed -n 's/.*JIT instructions = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$jit_insns" ]; then
    echo "Failed to find JIT instruction stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$jit_insns" -lt "$min_insns" ]; then
    echo "Expected at least $min_insns JIT instructions for $test_name, got $jit_insns" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_max_unsupported_hits() {
  local log=$1
  local test_name=$2
  local max_hits=$3
  local unsupported_hits

  [ -n "$max_hits" ] || return 0

  unsupported_hits=$(sed -n 's/.*unsupported hits = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$unsupported_hits" ]; then
    echo "Failed to find unsupported-hit stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$unsupported_hits" -gt "$max_hits" ]; then
    echo "Expected at most $max_hits unsupported hits for $test_name, got $unsupported_hits" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_max_executed_blocks() {
  local log=$1
  local test_name=$2
  local max_blocks=$3
  local executed_blocks

  [ -n "$max_blocks" ] || return 0

  executed_blocks=$(sed -n 's/.*executed blocks = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$executed_blocks" ]; then
    echo "Failed to find executed-block stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$executed_blocks" -gt "$max_blocks" ]; then
    echo "Expected at most $max_blocks executed JIT blocks for $test_name, got $executed_blocks" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_min_native_alu_ops() {
  local log=$1
  local test_name=$2
  local min_ops=$3
  local native_ops

  [ -n "$min_ops" ] || return 0

  native_ops=$(sed -n 's/.*native ALU ops = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$native_ops" ]; then
    echo "Failed to find native ALU stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$native_ops" -lt "$min_ops" ]; then
    echo "Expected at least $min_ops native ALU ops for $test_name, got $native_ops" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_min_native_incdec_ops() {
  local log=$1
  local test_name=$2
  local min_ops=$3
  local native_ops

  [ -n "$min_ops" ] || return 0

  native_ops=$(sed -n 's/.*native inc\/dec ops = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$native_ops" ]; then
    echo "Failed to find native inc/dec stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$native_ops" -lt "$min_ops" ]; then
    echo "Expected at least $min_ops native inc/dec ops for $test_name, got $native_ops" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_min_native_incdec_jcc_backedges() {
  local log=$1
  local test_name=$2
  local min_edges=$3
  local native_edges

  [ -n "$min_edges" ] || return 0

  native_edges=$(sed -n 's/.*native inc\/dec Jcc backedges = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$native_edges" ]; then
    echo "Failed to find native inc/dec Jcc backedge stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$native_edges" -lt "$min_edges" ]; then
    echo "Expected at least $min_edges native inc/dec Jcc backedges for $test_name, got $native_edges" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_max_helper_incdec_reg_calls() {
  local log=$1
  local test_name=$2
  local max_calls=$3
  local helper_calls

  [ -n "$max_calls" ] || return 0

  helper_calls=$(sed -n 's/.*helper inc\/dec register calls = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$helper_calls" ]; then
    echo "Failed to find helper register inc/dec stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$helper_calls" -gt "$max_calls" ]; then
    echo "Expected at most $max_calls helper register inc/dec calls for $test_name, got $helper_calls" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_min_native_pmem_loads() {
  local log=$1
  local test_name=$2
  local min_loads=$3
  local native_loads

  [ -n "$min_loads" ] || return 0

  native_loads=$(sed -n 's/.*native PMEM loads = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$native_loads" ]; then
    echo "Failed to find native PMEM load stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$native_loads" -lt "$min_loads" ]; then
    echo "Expected at least $min_loads native PMEM loads for $test_name, got $native_loads" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_min_native_pmem_stores() {
  local log=$1
  local test_name=$2
  local min_stores=$3
  local native_stores

  [ -n "$min_stores" ] || return 0

  native_stores=$(sed -n 's/.*native PMEM stores = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$native_stores" ]; then
    echo "Failed to find native PMEM store stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$native_stores" -lt "$min_stores" ]; then
    echo "Expected at least $min_stores native PMEM stores for $test_name, got $native_stores" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_min_native_imul_ops() {
  local log=$1
  local test_name=$2
  local min_ops=$3
  local native_ops

  [ -n "$min_ops" ] || return 0

  native_ops=$(sed -n 's/.*native imul ops = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$native_ops" ]; then
    echo "Failed to find native imul stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$native_ops" -lt "$min_ops" ]; then
    echo "Expected at least $min_ops native imul ops for $test_name, got $native_ops" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_min_native_shift_ops() {
  local log=$1
  local test_name=$2
  local min_ops=$3
  local native_ops

  [ -n "$min_ops" ] || return 0

  native_ops=$(sed -n 's/.*native shift\/rotate ops = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$native_ops" ]; then
    echo "Failed to find native shift/rotate stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$native_ops" -lt "$min_ops" ]; then
    echo "Expected at least $min_ops native shift/rotate ops for $test_name, got $native_ops" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_min_native_not_ops() {
  local log=$1
  local test_name=$2
  local min_ops=$3
  local native_ops

  [ -n "$min_ops" ] || return 0

  native_ops=$(sed -n 's/.*native not ops = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$native_ops" ]; then
    echo "Failed to find native not stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$native_ops" -lt "$min_ops" ]; then
    echo "Expected at least $min_ops native not ops for $test_name, got $native_ops" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_min_native_movzx_ops() {
  local log=$1
  local test_name=$2
  local min_ops=$3
  local native_ops

  [ -n "$min_ops" ] || return 0

  native_ops=$(sed -n 's/.*native movzx ops = \([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)
  if [ -z "$native_ops" ]; then
    echo "Failed to find native movzx stats for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  if [ "$native_ops" -lt "$min_ops" ]; then
    echo "Expected at least $min_ops native movzx ops for $test_name, got $native_ops" >&2
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

  if ! NEMU_JIT_STATS=1 NEMU_X86_JIT_HELPERS=1 \
      make -C am-kernels/tests/cpu-tests ARCH="$ARCH" \
      NEMU_DEFCONFIG=x86-am-jit_defconfig ALL="$test_name" run >"$out" 2>&1; then
    echo "$test_name failed" >&2
    cat "$out" >&2
    exit 2
  fi

  require_min_jit_instructions "$out" "$test_name" "$(required_jit_instructions "$test_name")"
  require_max_unsupported_hits "$out" "$test_name" "$(required_max_unsupported_hits "$test_name")"
  require_max_executed_blocks "$out" "$test_name" "$(required_max_executed_blocks "$test_name")"
  require_min_native_alu_ops "$out" "$test_name" "$(required_min_native_alu_ops "$test_name")"
  require_min_native_incdec_ops "$out" "$test_name" "$(required_min_native_incdec_ops "$test_name")"
  require_min_native_incdec_jcc_backedges "$out" "$test_name" "$(required_min_native_incdec_jcc_backedges "$test_name")"
  require_max_helper_incdec_reg_calls "$out" "$test_name" "$(required_max_helper_incdec_reg_calls "$test_name")"
  require_min_native_pmem_loads "$out" "$test_name" "$(required_min_native_pmem_loads "$test_name")"
  require_min_native_pmem_stores "$out" "$test_name" "$(required_min_native_pmem_stores "$test_name")"
  require_min_native_imul_ops "$out" "$test_name" "$(required_min_native_imul_ops "$test_name")"
  require_min_native_shift_ops "$out" "$test_name" "$(required_min_native_shift_ops "$test_name")"
  require_min_native_not_ops "$out" "$test_name" "$(required_min_native_not_ops "$test_name")"
  require_min_native_movzx_ops "$out" "$test_name" "$(required_min_native_movzx_ops "$test_name")"
done

echo "x86 JIT smoke gate passed: ${TESTS[*]}"
