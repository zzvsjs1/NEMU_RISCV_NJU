#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
export AM_HOME="$ROOT/abstract-machine"
export NEMU_HOME="$ROOT/nemu"
export ISA=riscv64
export ARCH=riscv64-nemu
export SDL_AUDIODRIVER=dummy
export SDL_VIDEODRIVER=dummy

BENCH_DIR="$ROOT/am-kernels/benchmarks/iomark"
JIT_DEFCONFIG=riscv64-am-headless-jit_defconfig
JIT_STATS_DEFCONFIG=riscv64-am-headless-jit-stats_defconfig
IOMARK_SAMPLES=7
IOMARK_EXPECTED_ITERS=1000000
IOMARK_EXPECTED_CHECKSUM=0x214f3b42
IOMARK_STATS_SLACK=${IOMARK_STATS_SLACK:-1024}
MIN_IOMARK_SPEEDUP=${MIN_IOMARK_SPEEDUP:-20.0}
# A calibrated CI runner can still impose an absolute limit.  WSL and local
# hosts default to a same-machine comparison because wall-clock microseconds
# vary with the scheduler and virtualisation layer.
MAX_IOMARK_US=${MAX_IOMARK_US:-}

fail() {
  echo "RISC-V64 JIT IO check failed: $*" >&2
  exit 1
}

logs=()

cleanup() {
  local log

  for log in "${logs[@]}"; do
    rm -f -- "$log"
  done

  # Leave the repository in the ordinary non-statistics configuration even if
  # a parser or performance threshold stops the script early.
  make -C "$NEMU_HOME" "$JIT_DEFCONFIG" >/dev/null 2>&1 || true
}

trap cleanup EXIT

run_iomark() {
  local mode=$1
  local out=$2

  if [ "$mode" = "interpreter" ]; then
    NEMU_DISABLE_JIT=1 make -C "$BENCH_DIR" ARCH="$ARCH" run >"$out" 2>&1
  else
    make -C "$BENCH_DIR" ARCH="$ARCH" run >"$out" 2>&1
  fi
}

require_iomark_result() {
  local log=$1
  local label=$2
  local mode=$3
  local pass_count
  local fail_count
  local good_trap_count
  local bad_trap_count
  local native_banner_count
  local disabled_banner_count
  local -a time_values
  local -a iter_values
  local -a checksum_values
  local time_us
  local iters
  local checksum

  pass_count=$(grep -Ec '^IOMark PASS$' "$log" || true)
  fail_count=$(grep -Ec '^IOMark FAIL$' "$log" || true)
  good_trap_count=$(grep -c 'HIT GOOD TRAP' "$log" || true)
  bad_trap_count=$(grep -c 'HIT BAD TRAP' "$log" || true)

  if [ "$pass_count" -ne 1 ] ||
      [ "$fail_count" -ne 0 ] ||
      [ "$good_trap_count" -ne 1 ] ||
      [ "$bad_trap_count" -ne 0 ]; then
    cat "$log" >&2
    fail "$label did not produce one unambiguous successful execution"
  fi

  mapfile -t time_values < <(
    sed -n 's/^iomark_total_us: \([0-9][0-9]*\)$/\1/p' "$log"
  )
  mapfile -t iter_values < <(
    sed -n 's/^iomark_iters: \([0-9][0-9]*\)$/\1/p' "$log"
  )
  mapfile -t checksum_values < <(
    sed -n \
      's/^iomark_checksum: \(0x[0-9a-fA-F][0-9a-fA-F]*\)$/\1/p' \
      "$log"
  )

  if [ "${#time_values[@]}" -ne 1 ] ||
      [ "${#iter_values[@]}" -ne 1 ] ||
      [ "${#checksum_values[@]}" -ne 1 ]; then
    cat "$log" >&2
    fail "$label had missing, duplicate, or malformed result fields"
  fi

  time_us=${time_values[0]}
  iters=${iter_values[0]}
  checksum=${checksum_values[0]}

  if [[ ! "$time_us" =~ ^[1-9][0-9]*$ ]]; then
    cat "$log" >&2
    fail "$label reported a missing or non-positive time"
  fi

  if [ "$iters" != "$IOMARK_EXPECTED_ITERS" ]; then
    cat "$log" >&2
    fail "$label expected $IOMARK_EXPECTED_ITERS iterations, got ${iters:-missing}"
  fi

  if [ "${checksum,,}" != "$IOMARK_EXPECTED_CHECKSUM" ]; then
    cat "$log" >&2
    fail "$label expected checksum $IOMARK_EXPECTED_CHECKSUM, got ${checksum:-missing}"
  fi

  native_banner_count=$(grep -c 'jit: RISC-V64 native code arena =' "$log" || true)
  disabled_banner_count=$(grep -c 'jit: disabled by NEMU_DISABLE_JIT=1' "$log" || true)

  if [ "$mode" = "interpreter" ]; then
    if [ "$disabled_banner_count" -ne 1 ] ||
        [ "$native_banner_count" -ne 0 ]; then
      cat "$log" >&2
      fail "$label did not execute exclusively in interpreter mode"
    fi
  elif [ "$disabled_banner_count" -ne 0 ] ||
      [ "$native_banner_count" -ne 1 ]; then
    cat "$log" >&2
    fail "$label did not execute with the RV64 JIT enabled"
  fi

  printf "%s\n" "$time_us"
}

median_of_odd_samples() {
  local middle=$((($# + 1) / 2))

  printf "%s\n" "$@" | sort -n | sed -n "${middle}p"
}

parse_single_count() {
  local log=$1
  local expression=$2
  local label=$3
  local -a values

  mapfile -t values < <(sed -n "$expression" "$log")
  if [ "${#values[@]}" -ne 1 ]; then
    cat "$log" >&2
    fail "expected exactly one $label"
  fi

  printf "%s\n" "${values[0]}"
}

require_route_stats() {
  local log=$1
  local stats_header_count
  local -a load_summaries
  local -a store_summaries
  local -a helper_summaries
  local -a bare_store_summaries
  local -a invalidation_summaries
  local load_hits
  local load_misses
  local load_fills
  local store_hits
  local store_misses
  local store_fills
  local direct_loads
  local direct_stores
  local direct_load_sites
  local direct_store_sites
  local helper_loads
  local helper_stores
  local bare_loads
  local bare_stores
  local bare_continuations
  local bare_boundaries
  local zero_side_exits
  local invalidation_requests
  local invalidated_blocks
  local arena_resets
  local minimum_direct_loads
  local maximum_direct_loads
  local minimum_direct_stores
  local maximum_direct_stores
  local minimum_route_load_hits
  local minimum_route_store_hits
  local routed_loads
  local routed_stores
  local uncached_direct_loads
  local uncached_direct_stores
  local load_rate
  local store_rate

  stats_header_count=$(grep -c 'jit: RV64 JIT statistics' "$log" || true)
  if [ "$stats_header_count" -ne 1 ]; then
    cat "$log" >&2
    fail "statistics smoke run did not contain exactly one JIT report"
  fi

  mapfile -t load_summaries < <(
    sed -n \
      's/.*direct MMIO load routes: warm hits = \([0-9][0-9]*\), misses = \([0-9][0-9]*\), fills = \([0-9][0-9]*\).*/\1 \2 \3/p' \
      "$log"
  )
  mapfile -t store_summaries < <(
    sed -n \
      's/.*direct MMIO store routes: warm hits = \([0-9][0-9]*\), misses = \([0-9][0-9]*\), fills = \([0-9][0-9]*\).*/\1 \2 \3/p' \
      "$log"
  )

  if [ "${#load_summaries[@]}" -ne 1 ] ||
      [ "${#store_summaries[@]}" -ne 1 ]; then
    cat "$log" >&2
    fail "expected exactly one direct-MMIO route summary per direction"
  fi

  read -r load_hits load_misses load_fills <<<"${load_summaries[0]}"
  read -r store_hits store_misses store_fills <<<"${store_summaries[0]}"

  direct_loads=$(parse_single_count \
    "$log" \
    's/.*inline direct MMIO load hits = \([0-9][0-9]*\).*/\1/p' \
    "inline direct-MMIO load count")
  direct_stores=$(parse_single_count \
    "$log" \
    's/.*inline direct MMIO store hits = \([0-9][0-9]*\).*/\1/p' \
    "inline direct-MMIO store count")
  direct_load_sites=$(parse_single_count \
    "$log" \
    's/.*inline direct MMIO load sites = \([0-9][0-9]*\).*/\1/p' \
    "inline direct-MMIO load-site count")
  direct_store_sites=$(parse_single_count \
    "$log" \
    's/.*inline direct MMIO store sites = \([0-9][0-9]*\).*/\1/p' \
    "inline direct-MMIO store-site count")

  mapfile -t helper_summaries < <(
    sed -n \
      's/.*helper loads = \([0-9][0-9]*\), helper stores = \([0-9][0-9]*\).*/\1 \2/p' \
      "$log"
  )
  bare_loads=$(parse_single_count \
    "$log" \
    's/.*bare MMIO load calls = \([0-9][0-9]*\).*/\1/p' \
    "bare-MMIO load helper count")
  mapfile -t bare_store_summaries < <(
    sed -n \
      's/.*bare MMIO store calls = \([0-9][0-9]*\), continuations = \([0-9][0-9]*\), boundary exits = \([0-9][0-9]*\).*/\1 \2 \3/p' \
      "$log"
  )

  if [ "${#helper_summaries[@]}" -ne 1 ] ||
      [ "${#bare_store_summaries[@]}" -ne 1 ]; then
    cat "$log" >&2
    fail "expected exactly one MMIO helper-routing summary"
  fi

  read -r helper_loads helper_stores <<<"${helper_summaries[0]}"
  read -r bare_stores bare_continuations bare_boundaries \
    <<<"${bare_store_summaries[0]}"

  zero_side_exits=$(parse_single_count \
    "$log" \
    's/.*zero side exits = \([0-9][0-9]*\).*/\1/p' \
    "zero-progress side-exit count")
  mapfile -t invalidation_summaries < <(
    sed -n \
      's/.*invalidation requests = \([0-9][0-9]*\), invalidated blocks = \([0-9][0-9]*\), arena resets = \([0-9][0-9]*\).*/\1 \2 \3/p' \
      "$log"
  )
  if [ "${#invalidation_summaries[@]}" -ne 1 ]; then
    cat "$log" >&2
    fail "expected exactly one JIT invalidation summary"
  fi
  read -r invalidation_requests invalidated_blocks arena_resets \
    <<<"${invalidation_summaries[0]}"

  minimum_direct_loads=$((2 * IOMARK_EXPECTED_ITERS - IOMARK_STATS_SLACK))
  maximum_direct_loads=$((2 * IOMARK_EXPECTED_ITERS + IOMARK_STATS_SLACK))
  minimum_direct_stores=$((IOMARK_EXPECTED_ITERS - IOMARK_STATS_SLACK))
  maximum_direct_stores=$((IOMARK_EXPECTED_ITERS + IOMARK_STATS_SLACK))
  minimum_route_load_hits=$((2 * IOMARK_EXPECTED_ITERS -
                             2 * IOMARK_STATS_SLACK))
  minimum_route_store_hits=$((IOMARK_EXPECTED_ITERS -
                              2 * IOMARK_STATS_SLACK))

  if [ "$direct_loads" -lt "$minimum_direct_loads" ] ||
      [ "$direct_loads" -gt "$maximum_direct_loads" ] ||
      [ "$direct_stores" -lt "$minimum_direct_stores" ] ||
      [ "$direct_stores" -gt "$maximum_direct_stores" ]; then
    cat "$log" >&2
    fail "direct-MMIO coverage moved outside the IOMark hot-loop bounds"
  fi

  routed_loads=$((load_hits + load_fills))
  routed_stores=$((store_hits + store_fills))
  uncached_direct_loads=$((direct_loads - routed_loads))
  uncached_direct_stores=$((direct_stores - routed_stores))

  if [ "$uncached_direct_loads" -lt 0 ] ||
      [ "$uncached_direct_loads" -gt "$IOMARK_STATS_SLACK" ] ||
      [ "$uncached_direct_stores" -lt 0 ] ||
      [ "$uncached_direct_stores" -gt "$IOMARK_STATS_SLACK" ]; then
    cat "$log" >&2
    fail "too many direct-MMIO operations bypassed specialised routes"
  fi

  if [ "$load_fills" -gt 256 ] ||
      [ "$store_fills" -gt 64 ] ||
      [ "$load_misses" -lt "$load_fills" ] ||
      [ "$store_misses" -lt "$store_fills" ] ||
      [ "$load_misses" -gt $((load_fills + bare_loads)) ] ||
      [ "$store_misses" -gt $((store_fills + bare_stores)) ]; then
    cat "$log" >&2
    fail "direct-MMIO route fill/miss invariants changed"
  fi

  if [ "$load_hits" -lt "$minimum_route_load_hits" ] ||
      [ "$store_hits" -lt "$minimum_route_store_hits" ]; then
    cat "$log" >&2
    fail "route cache did not cover the translated IOMark hot loop"
  fi

  if ! awk \
      -v load_hits="$load_hits" -v load_misses="$load_misses" \
      -v store_hits="$store_hits" -v store_misses="$store_misses" \
      'BEGIN {
        load_total = load_hits + load_misses;
        store_total = store_hits + store_misses;
        exit !(load_total > 0 && store_total > 0 &&
               load_hits / load_total >= 0.99 &&
               store_hits / store_total >= 0.99);
      }'; then
    cat "$log" >&2
    fail "expected at least a 99% warm-route hit rate in both directions"
  fi

  # Timer, startup, and output accesses may shift slightly when block layout
  # changes. Keep them bounded while requiring every helper route and completed
  # store outcome to balance exactly.
  if [ "$helper_loads" -ne "$bare_loads" ] ||
      [ "$helper_stores" -ne "$bare_stores" ] ||
      [ "$bare_loads" -le 0 ] || [ "$bare_loads" -gt 128 ] ||
      [ "$bare_stores" -le 0 ] || [ "$bare_stores" -gt 1024 ] ||
      [ "$bare_stores" -ne $((bare_continuations + bare_boundaries)) ]; then
    cat "$log" >&2
    fail "IOMark helper routing or continuation accounting changed"
  fi

  if [ "$zero_side_exits" -ne 0 ] ||
      [ "$direct_load_sites" -le 0 ] ||
      [ "$direct_store_sites" -le 0 ] ||
      [ "$invalidation_requests" -ne 0 ] ||
      [ "$invalidated_blocks" -ne 0 ] ||
      [ "$arena_resets" -ne 0 ] ||
      grep -Eq 'side exit store-guard = [1-9][0-9]*' "$log"; then
    cat "$log" >&2
    fail "IOMark JIT lifecycle or side-exit invariants changed"
  fi

  load_rate=$(awk -v hits="$load_hits" -v misses="$load_misses" \
    'BEGIN { printf "%.4f", 100.0 * hits / (hits + misses) }')
  store_rate=$(awk -v hits="$store_hits" -v misses="$store_misses" \
    'BEGIN { printf "%.4f", 100.0 * hits / (hits + misses) }')

  printf \
    "route_load_hits=%s route_load_misses=%s route_load_fills=%s route_load_hit_rate=%s%% uncached_direct_loads=%s " \
    "$load_hits" "$load_misses" "$load_fills" "$load_rate" \
    "$uncached_direct_loads"
  printf \
    "route_store_hits=%s route_store_misses=%s route_store_fills=%s route_store_hit_rate=%s%% uncached_direct_stores=%s\n" \
    "$store_hits" "$store_misses" "$store_fills" "$store_rate" \
    "$uncached_direct_stores"
}

jit_times=()
interp_times=()

make -C "$NEMU_HOME" "$JIT_DEFCONFIG" >/dev/null

for ((sample = 1; sample <= IOMARK_SAMPLES; sample++)); do
  modes=(jit interpreter)
  if [ $((sample % 2)) -eq 0 ]; then
    modes=(interpreter jit)
  fi

  for mode in "${modes[@]}"; do
    log=$(mktemp)
    logs+=("$log")

    if ! run_iomark "$mode" "$log"; then
      cat "$log" >&2
      exit 2
    fi

    time_us=$(require_iomark_result \
      "$log" "$mode sample $sample" "$mode")
    if [ "$mode" = "jit" ]; then
      jit_times+=("$time_us")
    else
      interp_times+=("$time_us")
    fi
  done
done

jit_us=$(median_of_odd_samples "${jit_times[@]}")
interp_us=$(median_of_odd_samples "${interp_times[@]}")
speedup=$(awk -v interp="$interp_us" -v jit="$jit_us" 'BEGIN {
  if (jit <= 0) {
    print "inf";
  } else {
    printf "%.2f", interp / jit;
  }
}')

if ! awk -v speedup="$speedup" -v minimum="$MIN_IOMARK_SPEEDUP" \
    'BEGIN { exit !(speedup >= minimum) }'; then
  fail "expected at least ${MIN_IOMARK_SPEEDUP}x speedup, got ${speedup}x"
fi

if [ -n "$MAX_IOMARK_US" ] && [ "$jit_us" -gt "$MAX_IOMARK_US" ]; then
  fail "expected median at most $MAX_IOMARK_US us, got $jit_us"
fi

stats_out=$(mktemp)
logs+=("$stats_out")
make -C "$NEMU_HOME" "$JIT_STATS_DEFCONFIG" >/dev/null

if ! NEMU_JIT_STATS=1 make -C "$BENCH_DIR" \
    ARCH="$ARCH" run >"$stats_out" 2>&1; then
  cat "$stats_out" >&2
  exit 2
fi

require_iomark_result \
  "$stats_out" "statistics JIT smoke run" jit >/dev/null
route_summary=$(require_route_stats "$stats_out")

printf "iomark_jit_median_us=%s interpreter_median_us=%s speedup=%sx samples=%s iters=%s checksum=%s" \
  "$jit_us" "$interp_us" "$speedup" "$IOMARK_SAMPLES" \
  "$IOMARK_EXPECTED_ITERS" "$IOMARK_EXPECTED_CHECKSUM"
if [ -n "$MAX_IOMARK_US" ]; then
  printf " max_us=%s" "$MAX_IOMARK_US"
fi

printf "\n"
printf "jit_samples_us=%s\n" "$(IFS=,; echo "${jit_times[*]}")"
printf "interpreter_samples_us=%s\n" "$(IFS=,; echo "${interp_times[*]}")"
printf "%s\n" "$route_summary"
