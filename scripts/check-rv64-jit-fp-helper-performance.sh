#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
# shellcheck source=scripts/rv64-jit-performance-common.sh
source "$SCRIPT_DIR/rv64-jit-performance-common.sh"

BASE_REF=${RV64_JIT_FP_HELPER_BASE_REF:-HEAD}
SAMPLE_COUNT=${RV64_JIT_FP_HELPER_SAMPLE_COUNT:-9}
MAX_BASELINE_REGRESSION_PERCENT=${RV64_JIT_FP_HELPER_MAX_REGRESSION_PERCENT:-5}
MAX_EFFECTS_INVERSION_PERCENT=5
EXPECTED_CHECKSUM_HI=0x0
EXPECTED_CHECKSUM_LO=0x3cbdd8a
ARCH=riscv64-nemu
ISA=riscv64
JIT_DEFCONFIG=riscv64-am-headless-jit_defconfig

# This gate owns independent clones rather than changing the caller's checkout.
# The shared EXIT trap therefore needs temporary-resource cleanup only; there is
# no production configuration in the source tree to restore.
rv64_jit_perf_enable_cleanup

fail() {
  printf 'RISC-V64 JIT FPHelperMark performance gate failed: %s\n' "$*" >&2
  exit 1
}

require_non_negative_integer() {
  local label=$1
  local value=$2

  case "$value" in
    '' | *[!0-9]*)
      fail "$label must be a non-negative decimal integer, got '$value'"
      ;;
  esac
}

require_non_negative_integer \
  'RV64_JIT_FP_HELPER_SAMPLE_COUNT' "$SAMPLE_COUNT"
require_non_negative_integer \
  'RV64_JIT_FP_HELPER_MAX_REGRESSION_PERCENT' \
  "$MAX_BASELINE_REGRESSION_PERCENT"

# Seven, nine, and eleven samples are the supported normal confidence/cost
# choices.  The deliberately small fixture count keeps the behavioural test
# quick without weakening the real gate's public sampling contract.
case "$SAMPLE_COUNT" in
  7 | 9 | 11)
    ;;
  3)
    [ "${RV64_JIT_FP_HELPER_FIXTURE:-0}" = "1" ] ||
      fail 'three samples are reserved for the behavioural fixture'
    ;;
  *)
    fail 'sample count must be 7, 9, or 11'
    ;;
esac

BUILD_JOBS=${RV64_JIT_FP_HELPER_BUILD_JOBS:-2}
require_non_negative_integer 'RV64_JIT_FP_HELPER_BUILD_JOBS' "$BUILD_JOBS"
[ "$BUILD_JOBS" -gt 0 ] ||
  fail 'RV64_JIT_FP_HELPER_BUILD_JOBS must be greater than zero'

# Optional affinity is useful on noisy hosts, but an invalid or unavailable CPU
# must fail explicitly rather than silently producing incomparable samples.
TASKSET_PREFIX=()
if [ -n "${RV64_JIT_PERF_CPU:-}" ]; then
  require_non_negative_integer 'RV64_JIT_PERF_CPU' "$RV64_JIT_PERF_CPU"
  command -v taskset >/dev/null 2>&1 ||
    fail 'RV64_JIT_PERF_CPU was set, but taskset is unavailable'
  if ! taskset -c "$RV64_JIT_PERF_CPU" true >/dev/null 2>&1; then
    fail "RV64_JIT_PERF_CPU=$RV64_JIT_PERF_CPU is not available to this process"
  fi
  TASKSET_PREFIX=(taskset -c "$RV64_JIT_PERF_CPU")
fi

if ! BASE_SHA=$(
  git -C "$ROOT" rev-parse --verify --end-of-options "${BASE_REF}^{commit}"
); then
  fail "could not resolve baseline revision '$BASE_REF'"
fi
if ! SOURCE_HEAD_SHA=$(git -C "$ROOT" rev-parse --verify HEAD); then
  fail 'could not resolve the source checkout HEAD'
fi

rv64_jit_perf_make_temp_dir SNAPSHOT_ROOT
BASELINE_ROOT="$SNAPSHOT_ROOT/baseline"
CANDIDATE_ROOT="$SNAPSHOT_ROOT/candidate"
DELTA_FILE="$SNAPSHOT_ROOT/candidate.patch"
UNTRACKED_FILE="$SNAPSHOT_ROOT/untracked-files"

# --no-hardlinks makes the snapshots independent even when the source and
# temporary directory share a filesystem.  Both trees begin at the one SHA
# resolved above, so candidate construction cannot observe a moving ref.
if ! git clone --quiet --local --no-hardlinks --no-checkout \
    "$ROOT" "$BASELINE_ROOT"; then
  fail 'could not create the clean baseline clone'
fi
if ! git clone --quiet --local --no-hardlinks --no-checkout \
    "$ROOT" "$CANDIDATE_ROOT"; then
  fail 'could not create the candidate clone'
fi
if ! git -c advice.detachedHead=false -C "$BASELINE_ROOT" \
    checkout --quiet --detach "$BASE_SHA"; then
  fail "could not detach the baseline clone at $BASE_SHA"
fi
if ! git -c advice.detachedHead=false -C "$CANDIDATE_ROOT" \
    checkout --quiet --detach "$BASE_SHA"; then
  fail "could not detach the candidate clone at $BASE_SHA"
fi

# A diff against the resolved baseline includes committed changes after that
# revision together with staged and unstaged tracked changes.  Binary and mode
# metadata are retained so the candidate is a faithful working-tree snapshot.
if ! git -C "$ROOT" diff --binary --full-index "$BASE_SHA" -- \
    >"$DELTA_FILE"; then
  fail 'could not capture the tracked candidate delta'
fi
if [ -s "$DELTA_FILE" ] &&
    ! git -C "$CANDIDATE_ROOT" apply --binary "$DELTA_FILE"; then
  fail 'could not apply the tracked candidate delta to its clone'
fi

# Git diff omits untracked files.  Materialise the NUL-delimited list first so
# a Git failure cannot be hidden by process-substitution semantics.  Consume
# it in this shell, then use cp --parents from the source root to preserve
# unusual names, executable modes, and symlinks without flattening their paths.
if ! git -C "$ROOT" ls-files --others --exclude-standard -z \
    >"$UNTRACKED_FILE"; then
  fail 'could not enumerate untracked candidate files'
fi
while IFS= read -r -d '' candidate_path; do
  case "$candidate_path" in
    /* | ../* | */../*)
      fail "Git returned an unsafe untracked path: $candidate_path"
      ;;
  esac
  if ! (
    cd "$ROOT"
    cp -a --parents -- "$candidate_path" "$CANDIDATE_ROOT"
  ); then
    fail "could not copy untracked candidate path '$candidate_path'"
  fi
done <"$UNTRACKED_FILE"

CANDIDATE_STATUS=$(
  git -C "$CANDIDATE_ROOT" status --porcelain --untracked-files=all
)
if [ -z "$CANDIDATE_STATUS" ] && [ "$BASE_SHA" = "$SOURCE_HEAD_SHA" ]; then
  fail 'candidate delta is empty; select an older base or make a working-tree change'
fi

SOURCE_SOFTFLOAT="$ROOT/nemu/tools/softfloat/repo"
if [ ! -d "$SOURCE_SOFTFLOAT/.git" ]; then
  fail 'the local SoftFloat dependency is not initialised; run make -C nemu/tools/softfloat prepare'
fi
if [ -n "$(git -C "$SOURCE_SOFTFLOAT" status --porcelain \
    --untracked-files=all)" ]; then
  fail 'the local SoftFloat dependency has uncommitted changes'
fi

seed_softfloat_dependency() {
  local clone_root=$1
  local role=$2
  local dependency_root="$clone_root/nemu/tools/softfloat/repo"
  local dependency_makefile="$clone_root/nemu/tools/softfloat/Makefile"
  local required_commit

  required_commit=$(sed -n \
    's/^SOFTFLOAT_REPO_COMMIT[[:space:]]*:=[[:space:]]*\([0-9a-fA-F][0-9a-fA-F]*\)[[:space:]]*$/\1/p' \
    "$dependency_makefile")
  [ -n "$required_commit" ] ||
    fail "could not read the $role SoftFloat revision"
  if ! git -C "$SOURCE_SOFTFLOAT" cat-file -e \
      "${required_commit}^{commit}"; then
    fail "the local SoftFloat dependency does not contain $role revision $required_commit"
  fi

  # Project clones deliberately omit this ignored nested checkout.  Seed an
  # independent local clone at the revision requested by each snapshot rather
  # than allowing Make to fetch from the network during an otherwise local,
  # reproducible comparison.
  if ! git clone --quiet --local --no-hardlinks --no-checkout \
      "$SOURCE_SOFTFLOAT" "$dependency_root"; then
    fail "could not clone the local SoftFloat dependency for $role"
  fi
  if ! git -c advice.detachedHead=false -C "$dependency_root" \
      checkout --quiet --detach "$required_commit"; then
    fail "could not select SoftFloat $required_commit for $role"
  fi
  if [ -n "$(git -C "$dependency_root" status --porcelain \
      --untracked-files=all)" ]; then
    fail "the $role SoftFloat dependency is not clean after checkout"
  fi
}

seed_softfloat_dependency "$BASELINE_ROOT" baseline
seed_softfloat_dependency "$CANDIDATE_ROOT" candidate

configure_and_build_nemu() {
  local clone_root=$1
  local role=$2
  local build_log

  rv64_jit_perf_make_temp_file build_log

  # Each clone receives the production, statistics-free defconfig.  Explicit
  # clone-local homes prevent inherited project paths from configuring or
  # building the caller's checkout by accident.
  if ! env \
      AM_HOME="$clone_root/abstract-machine" \
      NEMU_HOME="$clone_root/nemu" \
      NAVY_HOME="$clone_root/navy-apps" \
      ISA="$ISA" \
      make -C "$clone_root/nemu" "$JIT_DEFCONFIG" \
      >"$build_log" 2>&1; then
    sed 's/^/  | /' "$build_log" >&2
    fail "could not configure the $role NEMU snapshot"
  fi

  # Unsetting the run-time reporting switch is not enough: a binary compiled
  # with RV64_JIT_STATS still executes counter updates in every timed sample.
  # Require the generated clone-local configuration to prove that both sides
  # of the comparison use the genuinely statistics-free production build.
  if ! grep -Fxq '# CONFIG_RV64_JIT_STATS is not set' \
      "$clone_root/nemu/.config"; then
    fail "$role NEMU configuration is not statistics-free (RV64_JIT_STATS)"
  fi

  : >"$build_log"
  if ! env \
      AM_HOME="$clone_root/abstract-machine" \
      NEMU_HOME="$clone_root/nemu" \
      NAVY_HOME="$clone_root/navy-apps" \
      ISA="$ISA" \
      make -C "$clone_root/nemu" -j "$BUILD_JOBS" app \
      >"$build_log" 2>&1; then
    sed 's/^/  | /' "$build_log" >&2
    fail "could not build the $role NEMU snapshot"
  fi
  rm -f -- "$build_log"
}

configure_and_build_nemu "$BASELINE_ROOT" baseline
configure_and_build_nemu "$CANDIDATE_ROOT" candidate

GUEST_DIR="$BASELINE_ROOT/am-kernels/benchmarks/fphelpermark"
GUEST_IMAGE="$GUEST_DIR/build/fphelpermark-$ARCH.bin"

# Build the guest exactly once in the baseline clone.  Every timing run below
# passes this same byte-for-byte image to the selected host NEMU binary, which
# isolates emitter performance from guest-build differences.
if ! env \
    AM_HOME="$BASELINE_ROOT/abstract-machine" \
    NEMU_HOME="$BASELINE_ROOT/nemu" \
    NAVY_HOME="$BASELINE_ROOT/navy-apps" \
    ARCH="$ARCH" ISA="$ISA" \
    make -C "$GUEST_DIR" ARCH="$ARCH" image >/dev/null; then
  fail 'could not build the baseline FPHelperMark guest image'
fi
[ -f "$GUEST_IMAGE" ] ||
  fail "FPHelperMark did not produce the expected guest image: $GUEST_IMAGE"

report_run_failure() {
  local label=$1
  local output_path=$2
  shift 2

  sed 's/^/  | /' "$output_path" >&2
  fail "$label: $*"
}

run_fphelpermark() {
  local result_variable=$1
  local clone_root=$2
  local label=$3
  local full_sync=$4
  local output_path
  local pass_count
  local arena_count
  local total_us
  local checksum_hi
  local checksum_lo
  local -a total_values=()
  local -a checksum_hi_values=()
  local -a checksum_lo_values=()
  local -a clean_command=(
    env
    -u NEMU_DISABLE_JIT
    -u NEMU_JIT_STATS
    -u NEMU_JIT_KV
    -u NEMU_RV64_JIT_STATS_KV
    -u NEMU_JIT_PERFMAP
    -u NEMU_DISABLE_RV64_JIT_DIRECT_LINK
    -u NEMU_DISABLE_RV64_JIT_RETURN_LINK
    -u NEMU_DISABLE_RV64_JIT_FP_GPR_EFFECTS
    -u NEMU_RV64_JIT_TEST_FP_MMIO_BOUNDARY
    -u NEMU_RV64_JIT_TEST_MMIO_BOUNDARIES
    -u NEMU_EXIT_AFTER_INSTR
  )

  rv64_jit_perf_make_temp_file output_path

  # The control differs from the enabled candidate by one variable only.  All
  # statistics, KV, perf-map, link-disable, forced-exit, and test-hook inputs
  # are removed on every invocation so inherited diagnostics cannot bias time.
  if [ "$full_sync" -eq 1 ]; then
    clean_command+=(NEMU_DISABLE_RV64_JIT_FP_GPR_EFFECTS=1)
  fi
  clean_command+=(
    AM_HOME="$clone_root/abstract-machine"
    NEMU_HOME="$clone_root/nemu"
    NAVY_HOME="$clone_root/navy-apps"
    ARCH="$ARCH"
    ISA="$ISA"
    SDL_AUDIODRIVER=dummy
    SDL_VIDEODRIVER=dummy
    make -C "$clone_root/nemu" ISA="$ISA" IMG="$GUEST_IMAGE" run
  )

  if ! "${TASKSET_PREFIX[@]}" "${clean_command[@]}" \
      >"$output_path" 2>&1; then
    report_run_failure "$label" "$output_path" \
      'NEMU did not complete successfully'
  fi

  pass_count=$(grep -Fc 'FPHelperMark PASS' "$output_path" || true)
  [ "$pass_count" -eq 1 ] ||
    report_run_failure "$label" "$output_path" \
      "expected exactly one FPHelperMark PASS marker, found $pass_count"

  arena_count=$(
    grep -Fc 'jit: RISC-V64 native code arena' "$output_path" || true
  )
  [ "$arena_count" -eq 1 ] ||
    report_run_failure "$label" "$output_path" \
      "expected exactly one native-arena marker, found $arena_count"

  if grep -Fq 'jit: disabled' "$output_path"; then
    report_run_failure "$label" "$output_path" \
      'the native JIT reported itself disabled'
  fi
  if grep -Fq 'jit: RV64 JIT statistics' "$output_path"; then
    report_run_failure "$label" "$output_path" \
      'a statistics-enabled NEMU binary was used for timing'
  fi
  if grep -Fq 'jit-kv:' "$output_path"; then
    report_run_failure "$label" "$output_path" \
      'machine-readable JIT statistics appeared during timing'
  fi
  if grep -Fq 'jit: perf map =' "$output_path"; then
    report_run_failure "$label" "$output_path" \
      'perf-map output appeared during authoritative guest timing'
  fi

  mapfile -t total_values < <(sed -n \
    's/.*fphelpermark_total_us: \([0-9][0-9]*\).*/\1/p' \
    "$output_path")
  mapfile -t checksum_hi_values < <(sed -n \
    's/.*fphelpermark_checksum_hi: \(0x[0-9a-fA-F][0-9a-fA-F]*\).*/\1/p' \
    "$output_path")
  mapfile -t checksum_lo_values < <(sed -n \
    's/.*fphelpermark_checksum_lo: \(0x[0-9a-fA-F][0-9a-fA-F]*\).*/\1/p' \
    "$output_path")

  [ "${#total_values[@]}" -eq 1 ] ||
    report_run_failure "$label" "$output_path" \
      "expected exactly one guest time, found ${#total_values[@]}"
  [ "${#checksum_hi_values[@]}" -eq 1 ] ||
    report_run_failure "$label" "$output_path" \
      "expected exactly one checksum high half, found ${#checksum_hi_values[@]}"
  [ "${#checksum_lo_values[@]}" -eq 1 ] ||
    report_run_failure "$label" "$output_path" \
      "expected exactly one checksum low half, found ${#checksum_lo_values[@]}"

  total_us=${total_values[0]}
  checksum_hi=${checksum_hi_values[0]}
  checksum_lo=${checksum_lo_values[0]}
  [ "$total_us" -gt 0 ] ||
    report_run_failure "$label" "$output_path" \
      "guest time must be positive, got $total_us"

  if [ "$((checksum_hi))" -ne "$((EXPECTED_CHECKSUM_HI))" ] ||
      [ "$((checksum_lo))" -ne "$((EXPECTED_CHECKSUM_LO))" ]; then
    report_run_failure "$label" "$output_path" \
      "checksum $checksum_hi:$checksum_lo does not match $EXPECTED_CHECKSUM_HI:$EXPECTED_CHECKSUM_LO"
  fi

  rm -f -- "$output_path"
  printf -v "$result_variable" '%s' "$total_us"
}

median_samples() {
  local result_variable=$1
  shift
  local middle_line=$((($# + 1) / 2))
  local median_value

  median_value=$(printf '%s\n' "$@" | sort -n | sed -n "${middle_line}p")
  printf -v "$result_variable" '%s' "$median_value"
}

signed_percent_change() {
  local result_variable=$1
  local measured=$2
  local reference=$3
  local percentage

  percentage=$(awk -v measured="$measured" -v reference="$reference" \
    'BEGIN { printf "%.2f", (measured - reference) * 100.0 / reference }')
  printf -v "$result_variable" '%s' "$percentage"
}

# Untimed warm-ups validate every mode and absorb one-off host/guest initial
# work before the authoritative rotation begins.
run_fphelpermark warm_time "$BASELINE_ROOT" 'baseline warm-up' 0
run_fphelpermark warm_time "$CANDIDATE_ROOT" 'candidate warm-up' 0
run_fphelpermark warm_time "$CANDIDATE_ROOT" \
  'candidate full-sync warm-up' 1

baseline_samples=()
candidate_samples=()
candidate_full_sync_samples=()

# Rotate B,C,F / C,F,B / F,B,C.  Across the supported odd counts, positions
# are balanced as closely as possible and no mode repeatedly benefits from the
# same thermal or scheduler position.
for ((sample = 0; sample < SAMPLE_COUNT; sample++)); do
  case $((sample % 3)) in
    0)
      run_fphelpermark sample_time "$BASELINE_ROOT" \
        "baseline sample $((sample + 1))" 0
      baseline_samples+=("$sample_time")
      run_fphelpermark sample_time "$CANDIDATE_ROOT" \
        "candidate sample $((sample + 1))" 0
      candidate_samples+=("$sample_time")
      run_fphelpermark sample_time "$CANDIDATE_ROOT" \
        "candidate full-sync sample $((sample + 1))" 1
      candidate_full_sync_samples+=("$sample_time")
      ;;
    1)
      run_fphelpermark sample_time "$CANDIDATE_ROOT" \
        "candidate sample $((sample + 1))" 0
      candidate_samples+=("$sample_time")
      run_fphelpermark sample_time "$CANDIDATE_ROOT" \
        "candidate full-sync sample $((sample + 1))" 1
      candidate_full_sync_samples+=("$sample_time")
      run_fphelpermark sample_time "$BASELINE_ROOT" \
        "baseline sample $((sample + 1))" 0
      baseline_samples+=("$sample_time")
      ;;
    2)
      run_fphelpermark sample_time "$CANDIDATE_ROOT" \
        "candidate full-sync sample $((sample + 1))" 1
      candidate_full_sync_samples+=("$sample_time")
      run_fphelpermark sample_time "$BASELINE_ROOT" \
        "baseline sample $((sample + 1))" 0
      baseline_samples+=("$sample_time")
      run_fphelpermark sample_time "$CANDIDATE_ROOT" \
        "candidate sample $((sample + 1))" 0
      candidate_samples+=("$sample_time")
      ;;
  esac
done

median_samples baseline_median "${baseline_samples[@]}"
median_samples candidate_median "${candidate_samples[@]}"
median_samples candidate_full_sync_median \
  "${candidate_full_sync_samples[@]}"
signed_percent_change candidate_vs_baseline_percent \
  "$candidate_median" "$baseline_median"
signed_percent_change effects_vs_full_sync_percent \
  "$candidate_median" "$candidate_full_sync_median"

printf 'base_revision=%s\n' "$BASE_SHA"
printf 'baseline_samples_us=%s\n' "${baseline_samples[*]}"
printf 'candidate_samples_us=%s\n' "${candidate_samples[*]}"
printf 'candidate_full_sync_samples_us=%s\n' \
  "${candidate_full_sync_samples[*]}"
printf 'baseline_median_us=%s\n' "$baseline_median"
printf 'candidate_median_us=%s\n' "$candidate_median"
printf 'candidate_full_sync_median_us=%s\n' \
  "$candidate_full_sync_median"
printf 'candidate_vs_baseline_percent=%s%%\n' \
  "$candidate_vs_baseline_percent"
printf 'effects_vs_full_sync_percent=%s%%\n' \
  "$effects_vs_full_sync_percent"
printf 'checksum_hi=%s checksum_lo=%s\n' \
  "$EXPECTED_CHECKSUM_HI" "$EXPECTED_CHECKSUM_LO"
printf '%s\n' 'checksum=0x0000000003cbdd8a'

if [ $((candidate_median * 100)) -gt \
    $((baseline_median * (100 + MAX_BASELINE_REGRESSION_PERCENT))) ]; then
  fail "candidate median exceeds the ${MAX_BASELINE_REGRESSION_PERCENT}% baseline regression allowance"
fi

# Full-sync is an ablation control, not a required speedup floor: small effects
# can disappear in host noise.  Only reject an enabled candidate that is more
# than five per cent slower than its deliberately conservative control.
if [ $((candidate_median * 100)) -gt \
    $((candidate_full_sync_median * \
      (100 + MAX_EFFECTS_INVERSION_PERCENT))) ]; then
  fail "enabled FP GPR effects are more than ${MAX_EFFECTS_INVERSION_PERCENT}% slower than the full-sync control"
fi

printf '%s\n' 'FPHelperMark performance gate PASS'
