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
RV32D_DEFCONFIG=riscv32-am-headless-d_defconfig
RV32D_FALLBACK_TEST=riscv32-d-jit-fallback
RV32D_TRAP_TEST=riscv32-d-traps
RV32F_FALLBACK_TEST=riscv32-fpu-jit-fallback
RV32F_TRAP_TEST=riscv32-fpu-traps
TESTS=(
  "$RV32F_FALLBACK_TEST"
  "$RV32F_TRAP_TEST"
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

require_good_trap() {
  local log=$1
  local test_name=$2

  # A normal process exit is not proof that the CPU test reached its
  # architectural success trap. An early quit can occur after plausible JIT
  # statistics have already accumulated.
  if ! grep -q 'HIT GOOD TRAP' "$log"; then
    echo "Expected $test_name to reach HIT GOOD TRAP" >&2
    cat "$log" >&2
    exit 1
  fi
}

require_count_expectation() {
  local actual=$1
  local expectation=$2
  local counter_name=$3
  local test_name=$4
  local log=$5

  case "$expectation" in
    any)
      return
      ;;
    positive)
      if [ "$actual" -gt 0 ]; then
        return
      fi
      ;;
    zero)
      if [ "$actual" -eq 0 ]; then
        return
      fi
      ;;
    *)
      if [[ "$expectation" =~ ^[0-9]+$ ]] && [ "$actual" -eq "$expectation" ]; then
        return
      fi
      ;;
  esac

  echo "Expected FP $counter_name for $test_name to be $expectation, got $actual" >&2
  cat "$log" >&2
  exit 1
}

require_fp_helper_stats() {
  local log=$1
  local test_name=$2
  local expected_sites=$3
  local expected_calls=$4
  local expected_continuations=$5
  local expected_trap_exits=$6
  local expected_memory_exits=$7
  local summary
  local sites
  local calls
  local continuations
  local trap_exits
  local memory_exits
  local classified_calls

  summary=$(extract_last_stat \
    's/.*jit: FP helper sites = \([0-9][0-9]*\), calls = \([0-9][0-9]*\), continuations = \([0-9][0-9]*\), trap exits = \([0-9][0-9]*\), memory exits = \([0-9][0-9]*\).*/\1 \2 \3 \4 \5/p' \
    "$log")

  if [ -z "$summary" ]; then
    echo "Failed to find exact FP helper stats summary for $test_name" >&2
    cat "$log" >&2
    exit 2
  fi

  read -r sites calls continuations trap_exits memory_exits <<<"$summary"

  classified_calls=$((continuations + trap_exits + memory_exits))
  if [ "$calls" -ne "$classified_calls" ]; then
    echo "FP helper calls for $test_name were not fully classified: calls=$calls" \
      "continuations=$continuations trap_exits=$trap_exits memory_exits=$memory_exits" >&2
    cat "$log" >&2
    exit 1
  fi

  # Exact integer expectations are the hook for a focused guest whose stable
  # instruction mix covers all seven FP major-opcode families. Aggregate
  # positive counts alone cannot prove per-family coverage.
  require_count_expectation "$sites" "$expected_sites" "helper sites" "$test_name" "$log"
  require_count_expectation "$calls" "$expected_calls" "helper calls" "$test_name" "$log"
  require_count_expectation "$continuations" "$expected_continuations" "helper continuations" "$test_name" "$log"
  require_count_expectation "$trap_exits" "$expected_trap_exits" "helper trap exits" "$test_name" "$log"
  require_count_expectation "$memory_exits" "$expected_memory_exits" "helper memory exits" "$test_name" "$log"
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

  require_good_trap "$out" "$test_name"
  require_positive_jit_instructions "$out" "$test_name"

  if [ "$test_name" = "$RV32F_FALLBACK_TEST" ]; then
    require_fp_helper_stats "$out" "$test_name" positive 38 21 zero 17
  fi

  if [ "$test_name" = "$RV32F_TRAP_TEST" ]; then
    require_fp_helper_stats "$out" "$test_name" positive 43 29 14 zero
  fi

  if [ "$test_name" = "riscv32-jit-system-fence" ]; then
    require_positive_unsupported_hits "$out" "$test_name"
  fi

  if [ "$test_name" = "jit-smc" ]; then
    require_positive_invalidated_blocks "$out" "$test_name"
  fi
done

rv32d_out=$(mktemp)
tmp_files+=("$rv32d_out")

# Select D explicitly before asking the AM launcher to validate its defconfig
# stamp. The F phase above intentionally invokes NEMU's defconfig target
# directly, so a stamp left by an earlier D run must not suppress this switch.
make -C "$NEMU_HOME" "$RV32D_DEFCONFIG" >/dev/null

# The guest-level checks cover the conservative FP-memory block boundary,
# while the host statistics prove the run emitted and executed helper sites.
if ! NEMU_JIT_STATS=1 make -C am-kernels/tests/cpu-tests ARCH="$ARCH" \
    NEMU_DEFCONFIG="$RV32D_DEFCONFIG" ALL="$RV32D_FALLBACK_TEST" run \
    >"$rv32d_out" 2>&1; then
  echo "$RV32D_FALLBACK_TEST failed" >&2
  cat "$rv32d_out" >&2
  exit 2
fi

require_good_trap "$rv32d_out" "$RV32D_FALLBACK_TEST"
require_positive_jit_instructions "$rv32d_out" "$RV32D_FALLBACK_TEST"
require_fp_helper_stats "$rv32d_out" "$RV32D_FALLBACK_TEST" positive 44 18 zero 26

rv32d_trap_out=$(mktemp)
tmp_files+=("$rv32d_trap_out")

if ! NEMU_JIT_STATS=1 make -C am-kernels/tests/cpu-tests ARCH="$ARCH" \
    NEMU_DEFCONFIG="$RV32D_DEFCONFIG" ALL="$RV32D_TRAP_TEST" run \
    >"$rv32d_trap_out" 2>&1; then
  echo "$RV32D_TRAP_TEST failed" >&2
  cat "$rv32d_trap_out" >&2
  exit 2
fi

require_good_trap "$rv32d_trap_out" "$RV32D_TRAP_TEST"
require_positive_jit_instructions "$rv32d_trap_out" "$RV32D_TRAP_TEST"
require_fp_helper_stats "$rv32d_trap_out" "$RV32D_TRAP_TEST" positive 45 zero 16 29

scripts/check-rv32-jit-branch-chain.sh

# Leave the workspace on the ordinary RV32F JIT configuration used by the
# first phase. Removing the AM-owned stamp makes the next explicitly selected
# defconfig revalidate instead of trusting the D phase's stale marker.
make -C "$NEMU_HOME" riscv32-am-headless-jit_defconfig >/dev/null
rm -f "$NEMU_HOME/.config.defconfig"

echo "RISC-V32 JIT correctness gate passed: ${TESTS[*]} $RV32D_FALLBACK_TEST $RV32D_TRAP_TEST"
