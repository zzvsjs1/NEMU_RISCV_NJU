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
MAX_IOMARK_US=${MAX_IOMARK_US:-45000}

fail() {
  echo "RISC-V64 JIT IO check failed: $*" >&2
  exit 1
}

out=$(mktemp)
trap 'rm -f "$out"' EXIT

make -C "$NEMU_HOME" "$JIT_DEFCONFIG" >/dev/null

make -C "$BENCH_DIR" ARCH="$ARCH" run >"$out" 2>&1 || {
  cat "$out" >&2
  exit 2
}

grep -q 'IOMark PASS' "$out" || {
  cat "$out" >&2
  fail "IOMark did not pass"
}

iomark_us=$(sed -n 's/.*iomark_total_us: \([0-9][0-9]*\).*/\1/p' "$out" | tail -n 1)
[ -n "$iomark_us" ] || {
  cat "$out" >&2
  fail "could not parse IOMark time"
}

if [ "$iomark_us" -gt "$MAX_IOMARK_US" ]; then
  cat "$out" >&2
  fail "expected at most $MAX_IOMARK_US us, got $iomark_us"
fi

iters=$(sed -n 's/.*iomark_iters: \([0-9][0-9]*\).*/\1/p' "$out" | tail -n 1)
checksum=$(sed -n 's/.*iomark_checksum: \(0x[0-9a-fA-F][0-9a-fA-F]*\).*/\1/p' "$out" | tail -n 1)
printf "iomark_us=%s iters=%s checksum=%s max_us=%s\n" \
  "$iomark_us" "${iters:-unknown}" "${checksum:-unknown}" "$MAX_IOMARK_US"
