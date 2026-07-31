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

BENCH_DIR="$ROOT/am-kernels/benchmarks/fpmemmark"
JIT_DEFCONFIG=riscv64-am-headless-jit_defconfig
STATS_DEFCONFIG=riscv64-am-headless-jit-stats_defconfig
SAMPLE_COUNT=7
MIN_SPEEDUP=8
MIN_NATIVE_EXECUTIONS=1000000
MAX_NATIVE_EXECUTIONS=1001024

fail() {
  echo "RISC-V64 JIT FP-memory performance check failed: $*" >&2
  exit 1
}

run_fpmemmark() {
  local mode=$1
  local out
  local elapsed_us
  local checksum_hi
  local checksum_lo

  out=$(mktemp)

  if [ "$mode" = "jit" ]; then
    env -u NEMU_DISABLE_JIT \
      -u NEMU_JIT_PERFMAP \
      -u NEMU_JIT_STATS \
      -u NEMU_DISABLE_RV64_JIT_DIRECT_LINK \
      -u NEMU_DISABLE_RV64_JIT_RETURN_LINK \
      make -C "$BENCH_DIR" ARCH="$ARCH" run >"$out" 2>&1 || {
      cat "$out" >&2
      rm -f "$out"
      exit 2
    }
  else
    env -u NEMU_JIT_PERFMAP \
      -u NEMU_JIT_STATS \
      -u NEMU_DISABLE_RV64_JIT_DIRECT_LINK \
      -u NEMU_DISABLE_RV64_JIT_RETURN_LINK \
      NEMU_DISABLE_JIT=1 \
      make -C "$BENCH_DIR" ARCH="$ARCH" run >"$out" 2>&1 || {
      cat "$out" >&2
      rm -f "$out"
      exit 2
    }
  fi

  grep -q 'FPMemMark PASS' "$out" || {
    cat "$out" >&2
    rm -f "$out"
    fail "$mode FPMemMark did not pass"
  }

  if [ "$mode" = "jit" ]; then
    grep -q 'jit: RISC-V64 native code arena' "$out" || {
      cat "$out" >&2
      rm -f "$out"
      fail "JIT mode did not initialise the RV64 native code arena"
    }
    if grep -q 'jit: disabled by NEMU_DISABLE_JIT=1' "$out"; then
      cat "$out" >&2
      rm -f "$out"
      fail "JIT mode unexpectedly ran with the JIT disabled"
    fi
  else
    grep -q 'jit: disabled by NEMU_DISABLE_JIT=1' "$out" || {
      cat "$out" >&2
      rm -f "$out"
      fail "interpreter mode did not report the JIT as disabled"
    }
  fi

  elapsed_us=$(sed -n \
    's/.*fpmemmark_us: \([0-9][0-9]*\).*/\1/p' \
    "$out" | tail -n 1)
  checksum_hi=$(sed -n \
    's/.*fpmemmark_checksum_hi: \(0x[0-9a-fA-F][0-9a-fA-F]*\).*/\1/p' \
    "$out" | tail -n 1)
  checksum_lo=$(sed -n \
    's/.*fpmemmark_checksum_lo: \(0x[0-9a-fA-F][0-9a-fA-F]*\).*/\1/p' \
    "$out" | tail -n 1)
  rm -f "$out"

  [ -n "$elapsed_us" ] || fail "could not parse $mode elapsed time"
  [ -n "$checksum_hi" ] || fail "could not parse $mode checksum high half"
  [ -n "$checksum_lo" ] || fail "could not parse $mode checksum low half"
  [ "$elapsed_us" -gt 0 ] ||
    fail "$mode elapsed time must be positive, got $elapsed_us"
  printf "%s %s %s\n" "$elapsed_us" "$checksum_hi" "$checksum_lo"
}

median_samples() {
  printf "%s\n" "$@" | sort -n |
    sed -n "$((SAMPLE_COUNT / 2 + 1))p"
}

run_stats_smoke() {
  local out
  local operation
  local count
  local helper_calls

  make -C "$NEMU_HOME" "$STATS_DEFCONFIG" >/dev/null
  out=$(mktemp)

  NEMU_JIT_STATS=1 make -C "$BENCH_DIR" ARCH="$ARCH" run >"$out" 2>&1 || {
    cat "$out" >&2
    rm -f "$out"
    exit 2
  }

  grep -q 'FPMemMark PASS' "$out" || {
    cat "$out" >&2
    rm -f "$out"
    fail "statistics FPMemMark did not pass"
  }

  for operation in FLW FLD FSW FSD; do
    count=$(sed -n \
      "s/.*native FP memory ${operation} executions = \\([0-9][0-9]*\\).*/\\1/p" \
      "$out" | tail -n 1)
    [ -n "$count" ] ||
      fail "could not parse native $operation executions"
    [ "$count" -ge "$MIN_NATIVE_EXECUTIONS" ] &&
      [ "$count" -le "$MAX_NATIVE_EXECUTIONS" ] ||
      fail "native $operation executions were $count, expected $MIN_NATIVE_EXECUTIONS..$MAX_NATIVE_EXECUTIONS"
  done

  helper_calls=$(sed -n \
    's/.*jit: FP helper sites = [0-9][0-9]*, calls = \([0-9][0-9]*\).*/\1/p' \
    "$out" | tail -n 1)
  [ "$helper_calls" = "0" ] ||
    fail "FPMemMark entered the FP helper $helper_calls times"

  if grep -q 'block end fp-memory = ' "$out"; then
    cat "$out" >&2
    rm -f "$out"
    fail "FPMemMark still compiled an FP-memory block ending"
  fi

  rm -f "$out"
}

cd "$ROOT"
make -C "$NEMU_HOME" "$JIT_DEFCONFIG" >/dev/null

# Untimed cold runs populate build artefacts and establish the checksum oracle.
jit_warm_line=$(run_fpmemmark jit)
read -r _ warm_jit_hi warm_jit_lo <<<"$jit_warm_line"
interp_warm_line=$(run_fpmemmark interpreter)
read -r _ warm_interp_hi warm_interp_lo <<<"$interp_warm_line"

[ "$warm_jit_hi" = "$warm_interp_hi" ] ||
  fail "JIT/interpreter checksum high halves differ"
[ "$warm_jit_lo" = "$warm_interp_lo" ] ||
  fail "JIT/interpreter checksum low halves differ"

jit_samples=()
interp_samples=()

for ((sample = 0; sample < SAMPLE_COUNT; sample++)); do
  if [ $((sample % 2)) -eq 0 ]; then
    jit_line=$(run_fpmemmark jit)
    read -r jit_us jit_hi jit_lo <<<"$jit_line"
    interp_line=$(run_fpmemmark interpreter)
    read -r interp_us interp_hi interp_lo <<<"$interp_line"
  else
    interp_line=$(run_fpmemmark interpreter)
    read -r interp_us interp_hi interp_lo <<<"$interp_line"
    jit_line=$(run_fpmemmark jit)
    read -r jit_us jit_hi jit_lo <<<"$jit_line"
  fi

  [ "$jit_hi" = "$warm_jit_hi" ] &&
    [ "$jit_lo" = "$warm_jit_lo" ] ||
    fail "JIT checksum changed during sample $sample"
  [ "$interp_hi" = "$warm_interp_hi" ] &&
    [ "$interp_lo" = "$warm_interp_lo" ] ||
    fail "interpreter checksum changed during sample $sample"

  jit_samples+=("$jit_us")
  interp_samples+=("$interp_us")
done

jit_median=$(median_samples "${jit_samples[@]}")
interp_median=$(median_samples "${interp_samples[@]}")

[ $((jit_median * MIN_SPEEDUP)) -le "$interp_median" ] ||
  fail "native median is below the ${MIN_SPEEDUP}x speed floor"

speedup=$(awk -v interp="$interp_median" -v jit="$jit_median" \
  'BEGIN { if (jit == 0) print "inf"; else printf "%.2f", interp / jit }')

run_stats_smoke

printf "jit_fpmem_samples_us=%s\n" "${jit_samples[*]}"
printf "interpreter_fpmem_samples_us=%s\n" "${interp_samples[*]}"
printf "jit_fpmem_median_us=%s\n" "$jit_median"
printf "interpreter_fpmem_median_us=%s\n" "$interp_median"
printf "jit_vs_interpreter_speedup=%sx\n" "$speedup"
printf "native_execution_expected_range=%s..%s\n" \
  "$MIN_NATIVE_EXECUTIONS" "$MAX_NATIVE_EXECUTIONS"
printf "checksum_hi=%s checksum_lo=%s\n" \
  "$warm_jit_hi" "$warm_jit_lo"
