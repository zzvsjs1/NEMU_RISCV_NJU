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
MIN_IOMARK_SPEEDUP=${MIN_IOMARK_SPEEDUP:-2.0}
# A calibrated CI runner can still impose an absolute limit.  WSL and local
# hosts default to a same-machine comparison because wall-clock microseconds
# vary with the scheduler and virtualisation layer.
MAX_IOMARK_US=${MAX_IOMARK_US:-}

fail() {
  echo "RISC-V64 JIT IO check failed: $*" >&2
  exit 1
}

run_iomark() {
  local mode=$1
  local out=$2

  if [ "$mode" = "interpreter" ]; then
    NEMU_DISABLE_JIT=1 make -C "$BENCH_DIR" ARCH="$ARCH" run >"$out" 2>&1
  else
    make -C "$BENCH_DIR" ARCH="$ARCH" run >"$out" 2>&1
  fi
}

jit_out=$(mktemp)
interp_out=$(mktemp)
trap 'rm -f "$jit_out" "$interp_out"' EXIT

make -C "$NEMU_HOME" "$JIT_DEFCONFIG" >/dev/null

run_iomark jit "$jit_out" || {
  cat "$jit_out" >&2
  exit 2
}

run_iomark interpreter "$interp_out" || {
  cat "$interp_out" >&2
  exit 2
}

grep -q 'IOMark PASS' "$jit_out" || {
  cat "$jit_out" >&2
  fail "IOMark did not pass"
}
grep -q 'IOMark PASS' "$interp_out" || {
  cat "$interp_out" >&2
  fail "interpreter IOMark did not pass"
}

jit_us=$(sed -n 's/.*iomark_total_us: \([0-9][0-9]*\).*/\1/p' "$jit_out" | tail -n 1)
interp_us=$(sed -n 's/.*iomark_total_us: \([0-9][0-9]*\).*/\1/p' "$interp_out" | tail -n 1)
[ -n "$jit_us" ] || {
  cat "$jit_out" >&2
  fail "could not parse JIT IOMark time"
}
[ -n "$interp_us" ] || {
  cat "$interp_out" >&2
  fail "could not parse interpreter IOMark time"
}

speedup=$(awk -v interp="$interp_us" -v jit="$jit_us" 'BEGIN {
  if (jit <= 0) {
    print "inf";
  } else {
    printf "%.2f", interp / jit;
  }
}')

if ! awk -v speedup="$speedup" -v minimum="$MIN_IOMARK_SPEEDUP" \
    'BEGIN { exit !(speedup >= minimum) }'; then
  cat "$jit_out" >&2
  cat "$interp_out" >&2
  fail "expected at least ${MIN_IOMARK_SPEEDUP}x speedup, got ${speedup}x"
fi

if [ -n "$MAX_IOMARK_US" ] && [ "$jit_us" -gt "$MAX_IOMARK_US" ]; then
  cat "$jit_out" >&2
  fail "expected at most $MAX_IOMARK_US us, got $jit_us"
fi

iters=$(sed -n 's/.*iomark_iters: \([0-9][0-9]*\).*/\1/p' "$jit_out" | tail -n 1)
checksum=$(sed -n 's/.*iomark_checksum: \(0x[0-9a-fA-F][0-9a-fA-F]*\).*/\1/p' "$jit_out" | tail -n 1)
printf "iomark_us=%s interpreter_us=%s speedup=%sx iters=%s checksum=%s" \
  "$jit_us" "$interp_us" "$speedup" "${iters:-unknown}" "${checksum:-unknown}"
if [ -n "$MAX_IOMARK_US" ]; then
  printf " max_us=%s" "$MAX_IOMARK_US"
fi
printf "\n"
