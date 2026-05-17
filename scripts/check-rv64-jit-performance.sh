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

BENCH_DIR="$ROOT/am-kernels/benchmarks/branchmark"
INTERP_DEFCONFIG=riscv64-am-headless_defconfig
JIT_DEFCONFIG=riscv64-am-headless-jit_defconfig

fail() {
  echo "RISC-V64 JIT performance check failed: $*" >&2
  exit 1
}

run_branchmark() {
  local label=$1
  local defconfig=$2
  local stats_env=$3
  local out

  out=$(mktemp)
  make -C "$NEMU_HOME" "$defconfig" >/dev/null

  if [ "$stats_env" = "jit" ]; then
    NEMU_JIT_STATS=1 make -C "$BENCH_DIR" ARCH="$ARCH" run >"$out" 2>&1 || {
      cat "$out" >&2
      rm -f "$out"
      exit 2
    }
  else
    NEMU_DISABLE_JIT=1 make -C "$BENCH_DIR" ARCH="$ARCH" run >"$out" 2>&1 || {
      cat "$out" >&2
      rm -f "$out"
      exit 2
    }
  fi

  local guest_us
  guest_us=$(sed -n 's/.*branchmark_total_us: \([0-9][0-9]*\).*/\1/p' "$out" | tail -n 1)
  [ -n "$guest_us" ] || {
    cat "$out" >&2
    rm -f "$out"
    fail "could not parse BranchMark time for $label"
  }

  local checksum
  checksum=$(sed -n 's/.*branchmark_checksum: \(0x[0-9a-fA-F][0-9a-fA-F]*\).*/\1/p' "$out" | tail -n 1)
  [ -n "$checksum" ] || {
    cat "$out" >&2
    rm -f "$out"
    fail "could not parse BranchMark checksum for $label"
  }

  local jit_insns=0
  if [ "$stats_env" = "jit" ]; then
    jit_insns=$(sed -n 's/.*JIT instructions = \([0-9][0-9]*\).*/\1/p' "$out" | tail -n 1)
    [ -n "$jit_insns" ] || {
      cat "$out" >&2
      rm -f "$out"
      fail "could not parse JIT instruction count"
    }
  fi

  printf "%s guest_us=%s checksum=%s jit_insns=%s\n" "$label" "$guest_us" "$checksum" "$jit_insns"
  rm -f "$out"
}

cd "$ROOT"

interp_line=$(run_branchmark interpreter "$INTERP_DEFCONFIG" nojit)
jit_line=$(run_branchmark jit "$JIT_DEFCONFIG" jit)

interp_us=$(printf "%s\n" "$interp_line" | sed -n 's/.*guest_us=\([0-9][0-9]*\).*/\1/p')
jit_us=$(printf "%s\n" "$jit_line" | sed -n 's/.*guest_us=\([0-9][0-9]*\).*/\1/p')
jit_insns=$(printf "%s\n" "$jit_line" | sed -n 's/.*jit_insns=\([0-9][0-9]*\).*/\1/p')

[ -n "$interp_us" ] || fail "missing interpreter time"
[ -n "$jit_us" ] || fail "missing JIT time"
[ -n "$jit_insns" ] || fail "missing JIT instruction count"
[ "$jit_insns" -gt 0 ] || fail "expected positive JIT instruction count"

speedup=$(awk -v interp="$interp_us" -v jit="$jit_us" 'BEGIN {
  if (jit <= 0) {
    print "inf";
  } else {
    printf "%.2f", interp / jit;
  }
}')

printf "%s\n" "$interp_line"
printf "%s\n" "$jit_line"
printf "speedup=%sx\n" "$speedup"
