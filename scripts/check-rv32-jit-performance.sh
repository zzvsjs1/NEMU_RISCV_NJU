#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
export AM_HOME="$ROOT/abstract-machine"
export NEMU_HOME="$ROOT/nemu"
export NAVY_HOME="$ROOT/navy-apps"
export ISA=riscv32
export ARCH=riscv32-nemu
export SDL_AUDIODRIVER=dummy
export SDL_VIDEODRIVER=dummy

JITBENCH_ALU_MAX_US=${JITBENCH_ALU_MAX_US:-22000}
MICROBENCH_MIN_MARKS=${MICROBENCH_MIN_MARKS:-15000}

cd "$ROOT"

jitbench_out=$(mktemp)
microbench_out=$(mktemp)
trap 'rm -f "$jitbench_out" "$microbench_out"' EXIT

if ! make -C am-kernels/benchmarks/jitbench ARCH=riscv32-nemu run >"$jitbench_out" 2>&1; then
  echo "JITBench failed" >&2
  cat "$jitbench_out" >&2
  exit 2
fi

if ! make -C am-kernels/benchmarks/microbench ARCH=riscv32-nemu run >"$microbench_out" 2>&1; then
  echo "MicroBench failed" >&2
  cat "$microbench_out" >&2
  exit 2
fi

alu_us=$(sed -n 's/^ALU hot loop: \([0-9][0-9]*\)\.\([0-9][0-9][0-9]\) ms$/\1\2/p' "$jitbench_out" | tail -n 1)
marks=$(sed -n 's/^MicroBench PASS[[:space:]]*\([0-9][0-9]*\) Marks$/\1/p' "$microbench_out" | tail -n 1)

if [ -z "$alu_us" ]; then
  echo "Failed to parse JITBench ALU time" >&2
  cat "$jitbench_out" >&2
  exit 2
fi

if [ -z "$marks" ]; then
  echo "Failed to parse MicroBench marks" >&2
  cat "$microbench_out" >&2
  exit 2
fi

if [ "$alu_us" -gt "$JITBENCH_ALU_MAX_US" ]; then
  echo "JITBench ALU too slow: ${alu_us}us > ${JITBENCH_ALU_MAX_US}us" >&2
  exit 1
fi

if [ "$marks" -lt "$MICROBENCH_MIN_MARKS" ]; then
  echo "MicroBench score too low: ${marks} < ${MICROBENCH_MIN_MARKS}" >&2
  exit 1
fi

echo "RISC-V32 JIT performance gate passed: ALU=${alu_us}us MicroBench=${marks}"
