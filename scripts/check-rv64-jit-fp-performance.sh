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

BENCH_DIR="$ROOT/am-kernels/benchmarks/fpmark"
JIT_DEFCONFIG=riscv64-am-headless-jit_defconfig
SAMPLE_COUNT=7
# The measured baseline is about 24-28x for each phase. Requiring 10x leaves
# ample room for noisy hosts while still rejecting loss of the native fast path.
MIN_PHASE_SPEEDUP=10

fail() {
  echo "RISC-V64 JIT exact-FP performance check failed: $*" >&2
  exit 1
}

run_fpmark() {
  local mode=$1
  local out
  local move_us
  local class_us
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

  grep -q 'FPMark PASS' "$out" || {
    cat "$out" >&2
    rm -f "$out"
    fail "$mode FPMark did not pass"
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

  move_us=$(sed -n \
    's/.*fpmark_move_sign_us: \([0-9][0-9]*\).*/\1/p' \
    "$out" | tail -n 1)
  class_us=$(sed -n \
    's/.*fpmark_class_us: \([0-9][0-9]*\).*/\1/p' \
    "$out" | tail -n 1)
  total_us=$(sed -n \
    's/.*fpmark_total_us: \([0-9][0-9]*\).*/\1/p' \
    "$out" | tail -n 1)
  checksum_hi=$(sed -n \
    's/.*fpmark_checksum_hi: \(0x[0-9a-fA-F][0-9a-fA-F]*\).*/\1/p' \
    "$out" | tail -n 1)
  checksum_lo=$(sed -n \
    's/.*fpmark_checksum_lo: \(0x[0-9a-fA-F][0-9a-fA-F]*\).*/\1/p' \
    "$out" | tail -n 1)
  rm -f "$out"

  [ -n "$move_us" ] || fail "could not parse $mode move/sign time"
  [ -n "$class_us" ] || fail "could not parse $mode classification time"
  [ -n "$total_us" ] || fail "could not parse $mode FPMark time"
  [ -n "$checksum_hi" ] || fail "could not parse $mode checksum high half"
  [ -n "$checksum_lo" ] || fail "could not parse $mode checksum low half"
  [ $((move_us + class_us)) -eq "$total_us" ] ||
    fail "$mode phase times do not add up to the reported total"
  printf "%s %s %s %s %s\n" \
    "$move_us" "$class_us" "$total_us" "$checksum_hi" "$checksum_lo"
}

median_samples() {
  printf "%s\n" "$@" | sort -n |
    sed -n "$((SAMPLE_COUNT / 2 + 1))p"
}

cd "$ROOT"
make -C "$NEMU_HOME" "$JIT_DEFCONFIG" >/dev/null

# Untimed cold runs populate build artefacts and verify both execution modes.
jit_warm_line=$(run_fpmark jit)
read -r _ _ _ warm_jit_hi warm_jit_lo <<<"$jit_warm_line"
interp_warm_line=$(run_fpmark interpreter)
read -r _ _ _ warm_interp_hi warm_interp_lo <<<"$interp_warm_line"

[ "$warm_jit_hi" = "$warm_interp_hi" ] ||
  fail "JIT/interpreter checksum high halves differ"
[ "$warm_jit_lo" = "$warm_interp_lo" ] ||
  fail "JIT/interpreter checksum low halves differ"

jit_move_samples=()
jit_class_samples=()
jit_total_samples=()
interp_move_samples=()
interp_class_samples=()
interp_total_samples=()

for ((sample = 0; sample < SAMPLE_COUNT; sample++)); do
  if [ $((sample % 2)) -eq 0 ]; then
    jit_line=$(run_fpmark jit)
    read -r jit_move_us jit_class_us jit_total_us jit_hi jit_lo \
      <<<"$jit_line"
    interp_line=$(run_fpmark interpreter)
    read -r interp_move_us interp_class_us interp_total_us \
      interp_hi interp_lo <<<"$interp_line"
  else
    interp_line=$(run_fpmark interpreter)
    read -r interp_move_us interp_class_us interp_total_us \
      interp_hi interp_lo <<<"$interp_line"
    jit_line=$(run_fpmark jit)
    read -r jit_move_us jit_class_us jit_total_us jit_hi jit_lo \
      <<<"$jit_line"
  fi

  [ "$jit_hi" = "$warm_jit_hi" ] &&
    [ "$jit_lo" = "$warm_jit_lo" ] ||
    fail "JIT checksum changed during sample $sample"
  [ "$interp_hi" = "$warm_interp_hi" ] &&
    [ "$interp_lo" = "$warm_interp_lo" ] ||
    fail "interpreter checksum changed during sample $sample"

  jit_move_samples+=("$jit_move_us")
  jit_class_samples+=("$jit_class_us")
  jit_total_samples+=("$jit_total_us")
  interp_move_samples+=("$interp_move_us")
  interp_class_samples+=("$interp_class_us")
  interp_total_samples+=("$interp_total_us")
done

jit_move_median=$(median_samples "${jit_move_samples[@]}")
jit_class_median=$(median_samples "${jit_class_samples[@]}")
jit_total_median=$(median_samples "${jit_total_samples[@]}")
interp_move_median=$(median_samples "${interp_move_samples[@]}")
interp_class_median=$(median_samples "${interp_class_samples[@]}")
interp_total_median=$(median_samples "${interp_total_samples[@]}")

[ $((jit_move_median * MIN_PHASE_SPEEDUP)) -le "$interp_move_median" ] ||
  fail "native move/sign median is below the ${MIN_PHASE_SPEEDUP}x speed floor"
[ $((jit_class_median * MIN_PHASE_SPEEDUP)) -le "$interp_class_median" ] ||
  fail "native classification median is below the ${MIN_PHASE_SPEEDUP}x speed floor"
[ $((jit_total_median * MIN_PHASE_SPEEDUP)) -le "$interp_total_median" ] ||
  fail "native total median is below the ${MIN_PHASE_SPEEDUP}x speed floor"

speedup=$(awk -v interp="$interp_total_median" -v jit="$jit_total_median" \
  'BEGIN { if (jit == 0) print "inf"; else printf "%.2f", interp / jit }')

printf "jit_move_sign_samples_us=%s\n" "${jit_move_samples[*]}"
printf "jit_class_samples_us=%s\n" "${jit_class_samples[*]}"
printf "jit_total_samples_us=%s\n" "${jit_total_samples[*]}"
printf "interpreter_move_sign_samples_us=%s\n" \
  "${interp_move_samples[*]}"
printf "interpreter_class_samples_us=%s\n" \
  "${interp_class_samples[*]}"
printf "interpreter_total_samples_us=%s\n" \
  "${interp_total_samples[*]}"
printf "jit_move_sign_median_us=%s\n" "$jit_move_median"
printf "jit_class_median_us=%s\n" "$jit_class_median"
printf "jit_total_median_us=%s\n" "$jit_total_median"
printf "interpreter_move_sign_median_us=%s\n" "$interp_move_median"
printf "interpreter_class_median_us=%s\n" "$interp_class_median"
printf "interpreter_total_median_us=%s\n" "$interp_total_median"
printf "jit_vs_interpreter_speedup=%sx\n" "$speedup"
printf "checksum_hi=%s checksum_lo=%s\n" \
  "$warm_jit_hi" "$warm_jit_lo"
