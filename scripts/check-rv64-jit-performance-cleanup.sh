#!/usr/bin/env bash

# This script doubles as the fake Make executable used by the behavioural
# checks.  A symlink selects this early path, avoiding a separately generated
# helper whose contents could drift away from the assertions below.
fake_make() {
  local make_directory=
  local target=
  local expect_directory=0
  local argument

  for argument in "$@"; do
    if [ "$expect_directory" -eq 1 ]; then
      make_directory=$argument
      expect_directory=0
      continue
    fi

    case "$argument" in
      -C)
        expect_directory=1
        ;;
      *=* | -*)
        ;;
      *)
        target=$argument
        ;;
    esac
  done

  case "$target" in
    *_defconfig)
      printf '%s\n' "$target" >>"${RV64_JIT_PERF_TEST_CONFIG_LOG:?}"

      case "${RV64_JIT_PERF_TEST_CASE:?}:$target" in
        branch:riscv64-am-headless-jit-stats_defconfig)
          return 73
          ;;
        restore-failure:riscv64-am-headless-jit_defconfig)
          return 91
          ;;
      esac
      return 0
      ;;
    run)
      ;;
    *)
      # Build steps are not relevant to this fixture.  Treat any such call as
      # successful so the test remains focused on cleanup and restoration.
      return 0
      ;;
  esac

  case "$make_directory" in
    *branchmark)
      if [ "${NEMU_DISABLE_JIT-}" = "1" ]; then
        printf '%s\n' \
          'BranchMark PASS' \
          'jit: disabled by NEMU_DISABLE_JIT=1' \
          'branchmark_total_us: 100' \
          'branchmark_checksum: 0x1234'
      else
        printf '%s\n' \
          'BranchMark PASS' \
          'jit: RISC-V64 native code arena' \
          'branchmark_total_us: 10' \
          'branchmark_checksum: 0x1234' \
          'JIT instructions = 128000' \
          'executed blocks = 1000'
      fi
      ;;
    *fpmemmark)
      if [ "${NEMU_JIT_STATS-}" = "1" ]; then
        # FLW is deliberately absent.  The production script must fail after
        # entering the statistics configuration, then restore and clean up.
        printf '%s\n' \
          'FPMemMark PASS' \
          'jit: RISC-V64 native code arena' \
          'fpmemmark_us: 10' \
          'fpmemmark_checksum_hi: 0x0' \
          'fpmemmark_checksum_lo: 0x1234' \
          'native FP memory FLD executions = 1000000' \
          'native FP memory FSW executions = 1000000' \
          'native FP memory FSD executions = 1000000' \
          'jit: FP helper sites = 4, calls = 0'
      elif [ "${NEMU_DISABLE_JIT-}" = "1" ]; then
        printf '%s\n' \
          'FPMemMark PASS' \
          'jit: disabled by NEMU_DISABLE_JIT=1' \
          'fpmemmark_us: 100' \
          'fpmemmark_checksum_hi: 0x0' \
          'fpmemmark_checksum_lo: 0x1234'
      else
        printf '%s\n' \
          'FPMemMark PASS' \
          'jit: RISC-V64 native code arena' \
          'fpmemmark_us: 10' \
          'fpmemmark_checksum_hi: 0x0' \
          'fpmemmark_checksum_lo: 0x1234'
      fi
      ;;
    *)
      printf 'unexpected fake Make run directory: %s\n' \
        "$make_directory" >&2
      return 97
      ;;
  esac
}

if [ "${RV64_JIT_PERF_TEST_FAKE_MAKE-}" = "1" ]; then
  fake_make "$@"
  exit $?
fi

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
PRODUCTION_DEFCONFIG=riscv64-am-headless-jit_defconfig
STATS_DEFCONFIG=riscv64-am-headless-jit-stats_defconfig

TEST_SANDBOX=$(mktemp -d)
cleanup_test_sandbox() {
  # TEST_SANDBOX is the exact directory returned by mktemp above.
  rm -rf -- "$TEST_SANDBOX"
}
trap cleanup_test_sandbox EXIT

FAKE_BIN="$TEST_SANDBOX/bin"
mkdir -p "$FAKE_BIN"
ln -s "$SCRIPT_DIR/check-rv64-jit-performance-cleanup.sh" \
  "$FAKE_BIN/make"

failures=0

record_failure() {
  printf 'cleanup fixture assertion failed: %s\n' "$*" >&2
  failures=$((failures + 1))
}

assert_equal() {
  local label=$1
  local expected=$2
  local actual=$3

  if [ "$actual" != "$expected" ]; then
    record_failure "$label: expected '$expected', got '$actual'"
  fi
}

assert_nonzero() {
  local label=$1
  local actual=$2

  if [ "$actual" -eq 0 ]; then
    record_failure "$label: expected a non-zero exit status"
  fi
}

assert_log_contains() {
  local label=$1
  local expected=$2
  local log=$3

  if ! grep -Fxq "$expected" "$log"; then
    record_failure "$label: '$expected' was not recorded"
  fi
}

assert_directory_empty() {
  local label=$1
  local directory=$2

  if [ -n "$(ls -A -- "$directory")" ]; then
    record_failure "$label: temporary files remain in $directory"
  fi
}

last_config() {
  local log=$1

  if [ -s "$log" ]; then
    tail -n 1 "$log"
  fi
}

run_branch_failure_case() {
  local case_root="$TEST_SANDBOX/branch"
  local case_tmp="$case_root/tmp"
  local config_log="$case_root/config.log"
  local output_log="$case_root/output.log"
  local status
  local final_config

  mkdir -p "$case_tmp"
  : >"$config_log"

  set +e
  env -u NEMU_DISABLE_JIT -u NEMU_JIT_STATS \
    PATH="$FAKE_BIN:$PATH" \
    TMPDIR="$case_tmp" \
    RV64_JIT_PERF_TEST_FAKE_MAKE=1 \
    RV64_JIT_PERF_TEST_CASE=branch \
    RV64_JIT_PERF_TEST_CONFIG_LOG="$config_log" \
    bash "$SCRIPT_DIR/check-rv64-jit-performance.sh" \
    >"$output_log" 2>&1
  status=$?
  set -e

  final_config=$(last_config "$config_log")
  assert_equal "BranchMark preserves the configuration failure" 73 "$status"
  assert_log_contains "BranchMark enters the statistics configuration" \
    "$STATS_DEFCONFIG" "$config_log"
  assert_equal "BranchMark restores the production configuration" \
    "$PRODUCTION_DEFCONFIG" "$final_config"
  assert_directory_empty "BranchMark cleanup" "$case_tmp"
}

run_fpmem_failure_case() {
  local case_root="$TEST_SANDBOX/fpmem"
  local case_tmp="$case_root/tmp"
  local config_log="$case_root/config.log"
  local output_log="$case_root/output.log"
  local status
  local final_config

  mkdir -p "$case_tmp"
  : >"$config_log"

  set +e
  env -u NEMU_DISABLE_JIT -u NEMU_JIT_STATS \
    PATH="$FAKE_BIN:$PATH" \
    TMPDIR="$case_tmp" \
    RV64_JIT_PERF_TEST_FAKE_MAKE=1 \
    RV64_JIT_PERF_TEST_CASE=fpmem \
    RV64_JIT_PERF_TEST_CONFIG_LOG="$config_log" \
    bash "$SCRIPT_DIR/check-rv64-jit-fp-memory-performance.sh" \
    >"$output_log" 2>&1
  status=$?
  set -e

  final_config=$(last_config "$config_log")
  assert_nonzero "FPMemMark rejects a missing FLW counter" "$status"
  assert_log_contains "FPMemMark enters the statistics configuration" \
    "$STATS_DEFCONFIG" "$config_log"
  assert_equal "FPMemMark restores the production configuration" \
    "$PRODUCTION_DEFCONFIG" "$final_config"
  assert_directory_empty "FPMemMark cleanup" "$case_tmp"
}

run_restoration_failure_case() {
  local case_root="$TEST_SANDBOX/restore-failure"
  local case_tmp="$case_root/tmp"
  local config_log="$case_root/config.log"
  local output_log="$case_root/output.log"
  local status
  local final_config

  mkdir -p "$case_tmp"
  : >"$config_log"

  set +e
  env -u NEMU_DISABLE_JIT -u NEMU_JIT_STATS \
    PATH="$FAKE_BIN:$PATH" \
    TMPDIR="$case_tmp" \
    RV64_JIT_PERF_TEST_FAKE_MAKE=1 \
    RV64_JIT_PERF_TEST_CASE=restore-failure \
    RV64_JIT_PERF_TEST_CONFIG_LOG="$config_log" \
    RV64_JIT_PERF_TEST_COMMON="$SCRIPT_DIR/rv64-jit-performance-common.sh" \
    RV64_JIT_PERF_TEST_NEMU_HOME="$ROOT/nemu" \
    bash -c '
      set -euo pipefail
      source "$RV64_JIT_PERF_TEST_COMMON"
      rv64_jit_perf_enable_cleanup "$RV64_JIT_PERF_TEST_NEMU_HOME"
      rv64_jit_perf_use_defconfig riscv64-am-headless-jit-stats_defconfig
    ' >"$output_log" 2>&1
  status=$?
  set -e

  final_config=$(last_config "$config_log")
  assert_nonzero "a failed final restoration changes success to failure" "$status"
  assert_log_contains "the restoration case dirties the configuration" \
    "$STATS_DEFCONFIG" "$config_log"
  assert_equal "the restoration case attempts the production configuration" \
    "$PRODUCTION_DEFCONFIG" "$final_config"
  assert_directory_empty "restoration-failure cleanup" "$case_tmp"
}

run_signal_case() {
  local signal_name=$1
  local expected_status=$2
  local case_label=${signal_name,,}
  local case_root="$TEST_SANDBOX/signal-$case_label"
  local case_tmp="$case_root/tmp"
  local config_log="$case_root/config.log"
  local output_log="$case_root/output.log"
  local pid_file="$case_root/child.pid"
  local ready_file="$case_root/ready"
  local signaller_pid
  local signaller_status
  local status
  local final_config

  mkdir -p "$case_tmp"
  : >"$config_log"

  # Keep the benchmark shell in the foreground so SIGINT retains its ordinary
  # disposition.  A small synchronising helper sends the requested signal only
  # after the child has registered a temporary file and dirtied the NEMU
  # configuration, making every cleanup assertion meaningful.
  (
    local attempts=0
    local child_pid

    while [ ! -s "$pid_file" ] || [ ! -e "$ready_file" ]; do
      attempts=$((attempts + 1))
      if [ "$attempts" -ge 500 ]; then
        printf 'signal fixture child did not become ready\n' >&2
        exit 98
      fi
      sleep 0.01
    done

    child_pid=$(<"$pid_file")
    kill -s "$signal_name" "$child_pid"

    # A non-interactive upstream harness can cause SIGINT to arrive ignored.
    # Bound that failure mode so the fixture reports it instead of leaving the
    # foreground child in its deliberate busy loop forever.
    attempts=0
    while kill -0 "$child_pid" 2>/dev/null; do
      attempts=$((attempts + 1))
      if [ "$attempts" -ge 200 ]; then
        kill -s TERM "$child_pid" 2>/dev/null || true
        sleep 0.05
        kill -s KILL "$child_pid" 2>/dev/null || true
        exit 99
      fi
      sleep 0.01
    done
  ) &
  signaller_pid=$!

  set +e
  env -u NEMU_DISABLE_JIT -u NEMU_JIT_STATS \
    PATH="$FAKE_BIN:$PATH" \
    TMPDIR="$case_tmp" \
    RV64_JIT_PERF_TEST_FAKE_MAKE=1 \
    RV64_JIT_PERF_TEST_CASE="signal-$case_label" \
    RV64_JIT_PERF_TEST_CONFIG_LOG="$config_log" \
    RV64_JIT_PERF_TEST_COMMON="$SCRIPT_DIR/rv64-jit-performance-common.sh" \
    RV64_JIT_PERF_TEST_NEMU_HOME="$ROOT/nemu" \
    RV64_JIT_PERF_TEST_PID_FILE="$pid_file" \
    RV64_JIT_PERF_TEST_READY_FILE="$ready_file" \
    bash -c '
      set -euo pipefail
      source "$RV64_JIT_PERF_TEST_COMMON"
      rv64_jit_perf_enable_cleanup "$RV64_JIT_PERF_TEST_NEMU_HOME"
      rv64_jit_perf_make_temp_file signal_temp
      rv64_jit_perf_use_defconfig riscv64-am-headless-jit-stats_defconfig
      printf "%s\n" "$BASHPID" >"$RV64_JIT_PERF_TEST_PID_FILE"
      : >"$RV64_JIT_PERF_TEST_READY_FILE"
      while :; do :; done
    ' >"$output_log" 2>&1
  status=$?
  wait "$signaller_pid"
  signaller_status=$?
  set -e

  final_config=$(last_config "$config_log")
  assert_equal "$signal_name signaller status" 0 "$signaller_status"
  assert_equal "$signal_name preserves its derived status" \
    "$expected_status" "$status"
  assert_log_contains "$signal_name enters the statistics configuration" \
    "$STATS_DEFCONFIG" "$config_log"
  assert_equal "$signal_name restores the production configuration" \
    "$PRODUCTION_DEFCONFIG" "$final_config"
  assert_directory_empty "$signal_name cleanup" "$case_tmp"
}

run_branch_failure_case
run_fpmem_failure_case
run_restoration_failure_case
run_signal_case INT 130
run_signal_case TERM 143

if [ "$failures" -ne 0 ]; then
  printf 'RISC-V64 JIT performance cleanup fixture: %d assertion(s) failed\n' \
    "$failures" >&2
  exit 1
fi

printf '%s\n' 'RISC-V64 JIT performance cleanup fixture: PASS'
