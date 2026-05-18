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

BENCH_DIR="$ROOT/am-kernels/benchmarks/wopmark"
JIT_DEFCONFIG=riscv64-am-headless-jit_defconfig
MAX_WOPMARK_US=${MAX_WOPMARK_US:-28000}

fail() {
  echo "RISC-V64 JIT W-op check failed: $*" >&2
  exit 1
}

out=$(mktemp)
trap 'rm -f "$out"' EXIT

make -C "$NEMU_HOME" "$JIT_DEFCONFIG" >/dev/null

make -C "$BENCH_DIR" ARCH="$ARCH" run >"$out" 2>&1 || {
  cat "$out" >&2
  exit 2
}

grep -q 'WOpMark PASS' "$out" || {
  cat "$out" >&2
  fail "WOpMark did not pass"
}

wopmark_us=$(sed -n 's/.*wopmark_total_us: \([0-9][0-9]*\).*/\1/p' "$out" | tail -n 1)
[ -n "$wopmark_us" ] || {
  cat "$out" >&2
  fail "could not parse WOpMark time"
}

if [ "$wopmark_us" -gt "$MAX_WOPMARK_US" ]; then
  cat "$out" >&2
  fail "expected at most $MAX_WOPMARK_US us, got $wopmark_us"
fi

checksum=$(sed -n 's/.*wopmark_checksum: \(0x[0-9a-fA-F][0-9a-fA-F]*\).*/\1/p' "$out" | tail -n 1)
printf "wopmark_us=%s checksum=%s max_us=%s\n" "$wopmark_us" "${checksum:-unknown}" "$MAX_WOPMARK_US"
