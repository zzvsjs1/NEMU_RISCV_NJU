#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
STATS_HELPER="$SCRIPT_DIR/rv64-jit-stats.sh"
TEMP_FILES=()

cleanup() {
  local path

  for path in "${TEMP_FILES[@]}"; do
    rm -f "$path"
  done
}

trap cleanup EXIT

fail() {
  echo "RV64 JIT keyed-statistics parser check failed: $*" >&2
  exit 1
}

new_fixture() {
  local destination=$1
  local path

  path=$(mktemp)
  TEMP_FILES+=("$path")
  # Assign in the current shell. Returning the path through command
  # substitution would run this function in a subshell and silently discard
  # the TEMP_FILES update, leaving every fixture behind in /tmp.
  printf -v "$destination" '%s' "$path"
}

assert_value() {
  local expected=$1
  local key=$2
  local test_name=$3
  local log=$4
  local actual

  if ! actual=$(rv64_jit_stat "$key" "$test_name" "$log"); then
    fail "lookup for $key unexpectedly failed"
  fi

  if [ "$actual" != "$expected" ]; then
    fail "expected $key=$expected, got $actual"
  fi
}

# The helper is a deliberately small public contract between the C report and
# the Bash correctness gate. Keeping this check outside the full emulator build
# makes malformed, missing, and duplicate records cheap to test directly.
# shellcheck source=scripts/rv64-jit-stats.sh
source "$STATS_HELPER"

new_fixture valid_log
printf '%s\n' \
  'jit: wording and field order may change freely' \
  'jit-kv: indirect_pic.jalr.secondary_hits=2000' \
  'unrelated guest output' \
  'jit-kv: direct_jalr_link.misses=2' \
  'jit-kv: indirect_jump_cache.sites=1' \
  'jit-kv: indirect_pic.jalr.hits=1400' >"$valid_log"
# Log() colour output can be captured in a CRLF-normalised file. Exercise the
# combined suffix because stripping only one of its two parts is insufficient.
printf 'jit-kv: indirect_pic.return.hits=7\033[0m\r\n' >>"$valid_log"

load_rv64_jit_stats "$valid_log" parser-valid ||
  fail "a valid reordered fixture was rejected"
assert_value 1 indirect_jump_cache.sites parser-valid "$valid_log"
assert_value 1400 indirect_pic.jalr.hits parser-valid "$valid_log"
assert_value 2000 indirect_pic.jalr.secondary_hits parser-valid "$valid_log"
assert_value 2 direct_jalr_link.misses parser-valid "$valid_log"
assert_value 7 indirect_pic.return.hits parser-valid "$valid_log"

if rv64_jit_stat indirect_pic.return.misses parser-valid "$valid_log" \
    >/dev/null 2>&1; then
  fail "a missing key was accepted"
fi

# A later validator must not reuse plausible values from a previously loaded
# guest. Both the test identity and the exact log path are part of the binding.
if rv64_jit_stat indirect_jump_cache.sites parser-stale "$valid_log" \
    >/dev/null 2>&1; then
  fail "statistics loaded for another test were reused"
fi

new_fixture unloaded_log
printf '%s\n' 'jit-kv: indirect_jump_cache.sites=99' >"$unloaded_log"
if rv64_jit_stat indirect_jump_cache.sites parser-valid "$unloaded_log" \
    >/dev/null 2>&1; then
  fail "statistics loaded from another log were reused"
fi

new_fixture duplicate_log
printf '%s\n' \
  'jit-kv: indirect_jump_cache.hits=10' \
  'jit-kv: indirect_jump_cache.hits=11' >"$duplicate_log"
if load_rv64_jit_stats "$duplicate_log" parser-duplicate >/dev/null 2>&1; then
  fail "a duplicate key was accepted"
fi

new_fixture malformed_log
printf '%s\n' 'jit-kv: indirect_jump_cache.hits' >"$malformed_log"
if load_rv64_jit_stats "$malformed_log" parser-malformed >/dev/null 2>&1; then
  fail "a malformed machine record was accepted"
fi

new_fixture non_decimal_log
printf '%s\n' 'jit-kv: indirect_jump_cache.hits=not-a-number' \
  >"$non_decimal_log"
if load_rv64_jit_stats "$non_decimal_log" parser-non-decimal \
    >/dev/null 2>&1; then
  fail "a non-decimal value was accepted"
fi

echo "RV64 JIT keyed-statistics parser check passed"
