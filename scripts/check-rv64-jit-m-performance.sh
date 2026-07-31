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

BENCH_DIR="$ROOT/am-kernels/benchmarks/mmark"
JIT_DEFCONFIG=riscv64-am-headless-jit_defconfig
SAMPLE_COUNT=7

fail() {
  echo "RISC-V64 JIT M performance check failed: $*" >&2
  exit 1
}

run_mmark() {
  local mode=$1
  local out
  local mul_us
  local div_us
  local total_us
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

  grep -q 'MMark PASS' "$out" || {
    cat "$out" >&2
    rm -f "$out"
    fail "$mode MMark did not pass"
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

  mul_us=$(sed -n \
    's/.*mmark_mul_us: \([0-9][0-9]*\).*/\1/p' \
    "$out" | tail -n 1)
  div_us=$(sed -n \
    's/.*mmark_div_us: \([0-9][0-9]*\).*/\1/p' \
    "$out" | tail -n 1)
  total_us=$(sed -n \
    's/.*mmark_total_us: \([0-9][0-9]*\).*/\1/p' \
    "$out" | tail -n 1)
  checksum_hi=$(sed -n \
    's/.*mmark_checksum_hi: \(0x[0-9a-fA-F][0-9a-fA-F]*\).*/\1/p' \
    "$out" | tail -n 1)
  checksum_lo=$(sed -n \
    's/.*mmark_checksum_lo: \(0x[0-9a-fA-F][0-9a-fA-F]*\).*/\1/p' \
    "$out" | tail -n 1)
  rm -f "$out"

  [ -n "$mul_us" ] || fail "could not parse $mode MMark multiply time"
  [ -n "$div_us" ] || fail "could not parse $mode MMark division time"
  [ -n "$total_us" ] || fail "could not parse $mode MMark time"
  [ -n "$checksum_hi" ] || fail "could not parse $mode checksum high half"
  [ -n "$checksum_lo" ] || fail "could not parse $mode checksum low half"
  [ $((mul_us + div_us)) -eq "$total_us" ] ||
    fail "$mode phase times do not add up to the reported total"
  printf "%s %s %s %s %s\n" \
    "$mul_us" "$div_us" "$total_us" "$checksum_hi" "$checksum_lo"
}

median_samples() {
  printf "%s\n" "$@" | sort -n | sed -n "$((SAMPLE_COUNT / 2 + 1))p"
}

cd "$ROOT"
make -C "$NEMU_HOME" "$JIT_DEFCONFIG" >/dev/null

# Untimed cold runs populate build artefacts and verify both execution modes.
jit_warm_line=$(run_mmark jit)
read -r _ _ _ warm_jit_hi warm_jit_lo <<<"$jit_warm_line"
interp_warm_line=$(run_mmark interpreter)
read -r _ _ _ warm_interp_hi warm_interp_lo <<<"$interp_warm_line"

[ "$warm_jit_hi" = "$warm_interp_hi" ] ||
  fail "JIT/interpreter checksum high halves differ"
[ "$warm_jit_lo" = "$warm_interp_lo" ] ||
  fail "JIT/interpreter checksum low halves differ"

jit_mul_samples=()
jit_div_samples=()
jit_total_samples=()
interp_mul_samples=()
interp_div_samples=()
interp_total_samples=()

for ((sample = 0; sample < SAMPLE_COUNT; sample++)); do
  if [ $((sample % 2)) -eq 0 ]; then
    jit_line=$(run_mmark jit)
    read -r jit_mul_us jit_div_us jit_total_us jit_hi jit_lo <<<"$jit_line"
    interp_line=$(run_mmark interpreter)
    read -r interp_mul_us interp_div_us interp_total_us \
      interp_hi interp_lo <<<"$interp_line"
  else
    interp_line=$(run_mmark interpreter)
    read -r interp_mul_us interp_div_us interp_total_us \
      interp_hi interp_lo <<<"$interp_line"
    jit_line=$(run_mmark jit)
    read -r jit_mul_us jit_div_us jit_total_us jit_hi jit_lo <<<"$jit_line"
  fi

  [ "$jit_hi" = "$warm_jit_hi" ] &&
    [ "$jit_lo" = "$warm_jit_lo" ] ||
    fail "JIT checksum changed during sample $sample"
  [ "$interp_hi" = "$warm_interp_hi" ] &&
    [ "$interp_lo" = "$warm_interp_lo" ] ||
    fail "interpreter checksum changed during sample $sample"

  jit_mul_samples+=("$jit_mul_us")
  jit_div_samples+=("$jit_div_us")
  jit_total_samples+=("$jit_total_us")
  interp_mul_samples+=("$interp_mul_us")
  interp_div_samples+=("$interp_div_us")
  interp_total_samples+=("$interp_total_us")
done

jit_mul_median=$(median_samples "${jit_mul_samples[@]}")
jit_div_median=$(median_samples "${jit_div_samples[@]}")
jit_total_median=$(median_samples "${jit_total_samples[@]}")
interp_mul_median=$(median_samples "${interp_mul_samples[@]}")
interp_div_median=$(median_samples "${interp_div_samples[@]}")
interp_total_median=$(median_samples "${interp_total_samples[@]}")

[ "$jit_mul_median" -lt "$interp_mul_median" ] ||
  fail "native multiply median is not faster than the interpreter"
[ "$jit_div_median" -lt "$interp_div_median" ] ||
  fail "native division median is not faster than the interpreter"
[ "$jit_total_median" -lt "$interp_total_median" ] ||
  fail "native total median is not faster than the interpreter"

speedup=$(awk -v interp="$interp_total_median" -v jit="$jit_total_median" \
  'BEGIN { if (jit == 0) print "inf"; else printf "%.2f", interp / jit }')

printf "jit_mul_samples_us=%s\n" "${jit_mul_samples[*]}"
printf "jit_div_samples_us=%s\n" "${jit_div_samples[*]}"
printf "jit_total_samples_us=%s\n" "${jit_total_samples[*]}"
printf "interpreter_mul_samples_us=%s\n" "${interp_mul_samples[*]}"
printf "interpreter_div_samples_us=%s\n" "${interp_div_samples[*]}"
printf "interpreter_total_samples_us=%s\n" "${interp_total_samples[*]}"
printf "jit_mul_median_us=%s\n" "$jit_mul_median"
printf "jit_div_median_us=%s\n" "$jit_div_median"
printf "jit_total_median_us=%s\n" "$jit_total_median"
printf "interpreter_mul_median_us=%s\n" "$interp_mul_median"
printf "interpreter_div_median_us=%s\n" "$interp_div_median"
printf "interpreter_total_median_us=%s\n" "$interp_total_median"
printf "jit_vs_interpreter_speedup=%sx\n" "$speedup"
printf "checksum_hi=%s checksum_lo=%s\n" "$warm_jit_hi" "$warm_jit_lo"
