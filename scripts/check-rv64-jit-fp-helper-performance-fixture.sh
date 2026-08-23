#!/usr/bin/env bash

# This script also acts as the fake Make executable used by the fixture.  A
# symlink placed first in PATH selects this branch without generating a second
# shell script whose output could drift away from the assertions below.
fake_make() {
  local make_directory=
  local target=
  local image=
  local argument
  local expect_directory=0
  local expect_option_value=0
  local clone_role=
  local clone_root=
  local snapshot_root=
  local expected_guest=
  local run_mode=build
  local total_us=
  local checksum_lo=0x3cbdd8a
  local worktree_status

  for argument in "$@"; do
    if [ "$expect_directory" -eq 1 ]; then
      make_directory=$argument
      expect_directory=0
      continue
    fi
    if [ "$expect_option_value" -eq 1 ]; then
      expect_option_value=0
      continue
    fi

    case "$argument" in
      -C | --directory)
        expect_directory=1
        ;;
      --directory=*)
        make_directory=${argument#*=}
        ;;
      -j)
        expect_option_value=1
        ;;
      IMG=*)
        image=${argument#IMG=}
        ;;
      *=* | -*)
        ;;
      *)
        target=$argument
        ;;
    esac
  done

  case "$make_directory" in
    */baseline/nemu)
      clone_role=baseline
      clone_root=${make_directory%/nemu}
      snapshot_root=${clone_root%/baseline}
      ;;
    */candidate/nemu)
      clone_role=candidate
      clone_root=${make_directory%/nemu}
      snapshot_root=${clone_root%/candidate}
      ;;
    */baseline/am-kernels/benchmarks/fphelpermark)
      clone_role=baseline
      clone_root=${make_directory%/am-kernels/benchmarks/fphelpermark}
      snapshot_root=${clone_root%/baseline}
      ;;
    *)
      printf 'unexpected fake Make directory: %s\n' "$make_directory" >&2
      return 97
      ;;
  esac

  # Requiring real clone metadata keeps this behavioural test coupled to the
  # gate's snapshot path.  A gate that merely synthesises benchmark output
  # cannot satisfy the fixture by calling fake Make in arbitrary directories.
  if [ ! -d "$clone_root/.git" ]; then
    printf 'fake Make did not receive a local Git clone: %s\n' \
      "$clone_root" >&2
    return 98
  fi

  # A normal project clone omits the ignored, pinned SoftFloat checkout.  The
  # gate must seed each isolated NEMU tree from the verified local dependency
  # so a performance comparison never depends on network availability.
  if [ ! -d "$clone_root/nemu/tools/softfloat/repo/.git" ]; then
    printf 'snapshot is missing its local SoftFloat Git dependency\n' >&2
    return 104
  fi
  if [ -n "$(git -C "$clone_root/nemu/tools/softfloat/repo" \
      status --porcelain)" ]; then
    printf 'snapshot SoftFloat dependency is not clean\n' >&2
    return 105
  fi

  if [ "$target" = "riscv64-am-headless-jit_defconfig" ]; then
    worktree_status=$(git -C "$clone_root" status --porcelain \
      --untracked-files=all)
    if [ "$clone_role" = "baseline" ] && [ -n "$worktree_status" ]; then
      printf 'baseline clone is not clean before its build\n' >&2
      return 99
    fi
    if [ "$clone_role" = "candidate" ] && [ -z "$worktree_status" ]; then
      printf 'candidate clone does not contain the working-tree delta\n' >&2
      return 100
    fi

    if [ "${RV64_JIT_FP_HELPER_FIXTURE_STATS_ROLE-}" = "$clone_role" ]; then
      printf '%s\n' 'CONFIG_RV64_JIT_STATS=y' >"$make_directory/.config"
    else
      printf '%s\n' '# CONFIG_RV64_JIT_STATS is not set' \
        >"$make_directory/.config"
    fi
  fi

  # Both NEMU revisions must execute the exact image created in the baseline
  # clone.  In particular, the candidate must not silently rebuild its guest.
  expected_guest="$snapshot_root/baseline/am-kernels/benchmarks/fphelpermark/build/fphelpermark-riscv64-nemu.bin"

  case "$target" in
    image)
      if [ "$clone_role" != "baseline" ]; then
        printf 'FPHelperMark image must be built in the baseline clone\n' >&2
        return 101
      fi
      mkdir -p "${expected_guest%/*}"
      : >"$expected_guest"
      ;;
    run)
      if [ "$make_directory" != "$clone_root/nemu" ]; then
        printf 'FPHelperMark run did not use a clone-specific NEMU tree\n' >&2
        return 102
      fi
      if [ "$image" != "$expected_guest" ] || [ ! -f "$image" ]; then
        printf 'FPHelperMark run did not reuse the baseline guest image\n' >&2
        return 103
      fi

      if [ "$clone_role" = "baseline" ]; then
        run_mode=baseline
        total_us=100
      elif [ "${NEMU_DISABLE_RV64_JIT_FP_GPR_EFFECTS-}" = "1" ]; then
        run_mode=full-sync
        total_us=110
      else
        run_mode=enabled
        total_us=${RV64_JIT_FP_HELPER_FIXTURE_CANDIDATE_US:-104}
        checksum_lo=${RV64_JIT_FP_HELPER_FIXTURE_CANDIDATE_CHECKSUM_LO:-0x3cbdd8a}
      fi

      printf '%s\n' \
        'FPHelperMark PASS' \
        'jit: RISC-V64 native code arena' \
        "fphelpermark_total_us: $total_us" \
        'fphelpermark_checksum_hi: 0x0' \
        "fphelpermark_checksum_lo: $checksum_lo"
      ;;
    *)
      # Empty/default and explicit build targets are intentionally no-ops.
      # The fixture measures gate orchestration rather than the host compiler.
      ;;
  esac

  printf '%s:%s:%s\n' "$clone_role" "${target:-default}" "$run_mode" \
    >>"${RV64_JIT_FP_HELPER_FIXTURE_CALL_LOG:?}"
}

if [ "${RV64_JIT_FP_HELPER_FIXTURE_FAKE_MAKE-}" = "1" ]; then
  fake_make "$@"
  exit $?
fi

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
GATE="$SCRIPT_DIR/check-rv64-jit-fp-helper-performance.sh"

TEST_SANDBOX=$(mktemp -d)
cleanup_test_sandbox() {
  # TEST_SANDBOX is the exact directory returned by mktemp above.
  rm -rf -- "$TEST_SANDBOX"
}
trap cleanup_test_sandbox EXIT

FAKE_BIN="$TEST_SANDBOX/bin"
mkdir -p "$FAKE_BIN"
ln -s "$SCRIPT_DIR/check-rv64-jit-fp-helper-performance-fixture.sh" \
  "$FAKE_BIN/make"

failures=0

record_failure() {
  printf 'FPHelperMark fixture assertion failed: %s\n' "$*" >&2
  failures=$((failures + 1))
}

assert_zero() {
  local label=$1
  local actual=$2
  local output=$3

  if [ "$actual" -ne 0 ]; then
    record_failure "$label: expected status 0, got $actual"
    sed 's/^/  | /' "$output" >&2
  fi
}

assert_nonzero() {
  local label=$1
  local actual=$2

  if [ "$actual" -eq 0 ]; then
    record_failure "$label: expected a non-zero exit status"
  fi
}

assert_output_line() {
  local label=$1
  local expected=$2
  local output=$3

  if ! grep -Fxq "$expected" "$output"; then
    record_failure "$label: missing literal line '$expected'"
  fi
}

assert_output_matches() {
  local label=$1
  local expression=$2
  local output=$3

  if ! grep -Eiq "$expression" "$output"; then
    record_failure "$label: output does not match '$expression'"
  fi
}

assert_directory_empty() {
  local label=$1
  local directory=$2

  if [ -n "$(ls -A -- "$directory")" ]; then
    record_failure "$label: temporary files remain in $directory"
  fi
}

invoke_gate() {
  local result_variable=$1
  local case_tmp=$2
  local output=$3
  local call_log=$4
  local candidate_us=$5
  local candidate_checksum_lo=$6
  local statistics_role=${7-}
  local gate_status

  set +e
  env -u NEMU_DISABLE_JIT \
    -u NEMU_JIT_STATS \
    -u NEMU_JIT_PERFMAP \
    -u NEMU_JIT_KV \
    -u NEMU_DISABLE_RV64_JIT_DIRECT_LINK \
    -u NEMU_DISABLE_RV64_JIT_RETURN_LINK \
    -u NEMU_DISABLE_RV64_JIT_FP_GPR_EFFECTS \
    PATH="$FAKE_BIN:$PATH" \
    TMPDIR="$case_tmp" \
    RV64_JIT_FP_HELPER_FIXTURE=1 \
    RV64_JIT_FP_HELPER_FIXTURE_FAKE_MAKE=1 \
    RV64_JIT_FP_HELPER_FIXTURE_CALL_LOG="$call_log" \
    RV64_JIT_FP_HELPER_FIXTURE_CANDIDATE_US="$candidate_us" \
    RV64_JIT_FP_HELPER_FIXTURE_CANDIDATE_CHECKSUM_LO="$candidate_checksum_lo" \
    RV64_JIT_FP_HELPER_FIXTURE_STATS_ROLE="$statistics_role" \
    RV64_JIT_FP_HELPER_BASE_REF='HEAD^' \
    RV64_JIT_FP_HELPER_SAMPLE_COUNT=3 \
    RV64_JIT_FP_HELPER_MAX_REGRESSION_PERCENT=5 \
    bash "$GATE" >"$output" 2>&1
  gate_status=$?
  set -e

  printf -v "$result_variable" '%s' "$gate_status"
}

run_success_case() {
  local case_root="$TEST_SANDBOX/success"
  local case_tmp="$case_root/tmp"
  local output="$case_root/output.log"
  local call_log="$case_root/make.log"
  local status

  mkdir -p "$case_tmp"
  : >"$call_log"

  invoke_gate status "$case_tmp" "$output" "$call_log" 104 0x3cbdd8a

  assert_zero "accepted 4% candidate regression" "$status" "$output"
  assert_output_line "baseline samples" \
    'baseline_samples_us=100 100 100' "$output"
  assert_output_line "candidate samples" \
    'candidate_samples_us=104 104 104' "$output"
  assert_output_line "candidate full-sync samples" \
    'candidate_full_sync_samples_us=110 110 110' "$output"
  assert_output_line "baseline median" 'baseline_median_us=100' "$output"
  assert_output_line "candidate median" 'candidate_median_us=104' "$output"
  assert_output_line "candidate full-sync median" \
    'candidate_full_sync_median_us=110' "$output"
  assert_output_line "candidate regression" \
    'candidate_vs_baseline_percent=4.00%' "$output"
  assert_output_line "baseline image build" 'baseline:image:build' "$call_log"
  assert_output_matches "baseline clone execution" \
    '^baseline:run:baseline$' "$call_log"
  assert_output_matches "candidate enabled execution" \
    '^candidate:run:enabled$' "$call_log"
  assert_output_matches "candidate full-sync execution" \
    '^candidate:run:full-sync$' "$call_log"
  assert_directory_empty "successful gate cleanup" "$case_tmp"
}

run_bad_checksum_case() {
  local case_root="$TEST_SANDBOX/bad-checksum"
  local case_tmp="$case_root/tmp"
  local output="$case_root/output.log"
  local call_log="$case_root/make.log"
  local status

  mkdir -p "$case_tmp"
  : >"$call_log"

  invoke_gate status "$case_tmp" "$output" "$call_log" \
    104 0xdeadbeef

  assert_nonzero "candidate checksum mismatch" "$status"
  assert_output_matches "candidate checksum diagnostic" 'checksum' "$output"
  assert_directory_empty "checksum-failure cleanup" "$case_tmp"
}

run_regression_case() {
  local case_root="$TEST_SANDBOX/regression"
  local case_tmp="$case_root/tmp"
  local output="$case_root/output.log"
  local call_log="$case_root/make.log"
  local status

  mkdir -p "$case_tmp"
  : >"$call_log"

  invoke_gate status "$case_tmp" "$output" "$call_log" 106 0x3cbdd8a

  assert_nonzero "candidate 6% regression" "$status"
  assert_output_matches "candidate regression diagnostic" \
    'regression|allowance|exceeds' "$output"
  assert_directory_empty "regression-failure cleanup" "$case_tmp"
}

run_statistics_configuration_case() {
  local case_root="$TEST_SANDBOX/statistics-config"
  local case_tmp="$case_root/tmp"
  local output="$case_root/output.log"
  local call_log="$case_root/make.log"
  local status

  mkdir -p "$case_tmp"
  : >"$call_log"

  invoke_gate status "$case_tmp" "$output" "$call_log" \
    104 0x3cbdd8a candidate

  assert_nonzero "statistics-enabled candidate configuration" "$status"
  assert_output_matches "statistics configuration diagnostic" \
    'statistics-free|RV64_JIT_STATS' "$output"
  assert_directory_empty "statistics-configuration cleanup" "$case_tmp"
}

run_success_case
run_bad_checksum_case
run_regression_case
run_statistics_configuration_case

if [ "$failures" -ne 0 ]; then
  printf 'FPHelperMark performance fixture FAILED (%d assertions)\n' \
    "$failures" >&2
  exit 1
fi

printf '%s\n' 'FPHelperMark performance fixture PASS'
